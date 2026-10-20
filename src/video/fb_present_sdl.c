/**
 * POSIX counterpart of fb_present.c: show the guest framebuffer in an SDL2
 * window. Same contract (RECOMP_FB_WINDOW, flip-time copy, BMP dump, snap
 * trigger, keyboard state), minus the D3D11 presenter.
 */
#include <stdint.h>

#if !defined(_WIN32)
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>
void xbox_FramebufferApplyGamma(uint32_t *px, size_t n);   /* fb_gamma.c */
#include <unistd.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

static volatile int  s_fb_running;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;
static uint32_t     *s_present[2];
static volatile int  s_present_idx = -1;
static volatile unsigned char s_key_down[256];
static char          s_title[48] = "Xbox Recomp";
static volatile int  s_flips, s_frame_draws;

/* The window's client area in pixels (see fb_present.c). */
static volatile int s_client_w, s_client_h;

int xbox_FramebufferClientSize(int *w, int *h)
{
    int cw = __atomic_load_n(&s_client_w, __ATOMIC_RELAXED);
    int ch = __atomic_load_n(&s_client_h, __ATOMIC_RELAXED);
    if (cw <= 0 || ch <= 0)
        return 0;
    *w = cw; *h = ch;
    return 1;
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

static void fb_convert_row(const uint8_t *row, uint32_t *dst, uint32_t bpp)
{
    uint32_t x;

    if (bpp == 4) {
        memcpy(dst, row, (size_t)s_fb_width * 4);
    } else if (bpp == 2) {
        const uint16_t *p = (const uint16_t *)row;
        for (x = 0; x < s_fb_width; x++) {
            uint16_t v = p[x];
            uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
            uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
            uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
            dst[x] = (r << 16) | (g << 8) | b;
        }
    } else {
        memset(dst, 0, (size_t)s_fb_width * 4);
    }
}

/* A GPU backend's own full-resolution frame (Vulkan, RECOMP_RES): shown
 * instead of the guest surface while it keeps coming. */
static pthread_mutex_t s_hi_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t *s_hi;
static uint32_t  s_hi_w, s_hi_h;
static int       s_hi_new;
static double    s_hi_at;

static double now_ms(void);

void xbox_FramebufferPublishImage(const uint32_t *px, uint32_t w, uint32_t h)
{
    if (!px || !w || !h)
        return;
    pthread_mutex_lock(&s_hi_lock);
    if (w != s_hi_w || h != s_hi_h) {
        free(s_hi);
        s_hi = (uint32_t *)malloc((size_t)w * h * 4);
        s_hi_w = s_hi ? w : 0;
        s_hi_h = s_hi ? h : 0;
    }
    if (s_hi) {
        memcpy(s_hi, px, (size_t)w * h * 4);
        s_hi_new = 1;
        s_hi_at = now_ms();
    }
    pthread_mutex_unlock(&s_hi_lock);
}

/* Called by the pushbuffer executor when the title flips. */

void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    int next;
    uint32_t bpp, y;

    if (!s_fb_running || !fb_va || !pitch)
        return;
    if (getenv("RECOMP_FB_VA"))
        return;
    next = (s_present_idx == 0) ? 1 : 0;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);
        if (!s_present[next])
            return;
    }
    bpp = pitch / s_fb_width;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < s_fb_height; y++)
        fb_convert_row(src + (size_t)y * pitch,
                       s_present[next] + (size_t)y * s_fb_width, bpp);
    __atomic_store_n(&s_present_idx, next, __ATOMIC_RELEASE);
}

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;

    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
        s_title[i] = (char)name[i];
    if (i)
        s_title[i] = 0;
}

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* RECOMP_FRAME_STATS=1: [FRAMES] line once a second, as on Windows. */
void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    static int on = -1;
    static double last, window, dt[2048];
    static int n;
    double t;

    __atomic_fetch_add(&s_flips, 1, __ATOMIC_RELAXED);
    s_frame_draws = (int)draws;
    if (on < 0) {
        const char *e = getenv("RECOMP_FRAME_STATS");
        on = e && *e && *e != '0';
    }
    if (!on)
        return;
    t = now_ms();
    if (last && n < (int)(sizeof dt / sizeof dt[0]))
        dt[n++] = t - last;
    last = t;
    if (!window)
        window = t;
    if (t - window >= 1000.0 && n) {
        double secs = (t - window) / 1000.0;
        qsort(dt, (size_t)n, sizeof dt[0], cmp_double);
        fprintf(stderr, "[FRAMES] n %d fps %.1f p50 %.1f p99 %.1f max %.1f ms draws %u\n",
                n, n / secs, dt[n / 2],
                dt[(n * 99) / 100 < n ? (n * 99) / 100 : n - 1], dt[n - 1], draws);
        n = 0;
        window = t;
    }
}

int xbox_FramebufferKeyDown(int vk)
{
    if ((unsigned)vk > 255)
        return 0;
    return s_key_down[vk] != 0;
}

/* SDL keycode -> Windows virtual-key code, for the keyboard pad stand-in. */
static int sdl_to_vk(SDL_Keycode k)
{
    if (k >= 'a' && k <= 'z') return k - 'a' + 0x41;
    if (k >= '0' && k <= '9') return k;
    if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70 + (k - SDLK_F1);
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return 0x61 + (k - SDLK_KP_1);
    switch (k) {
    case SDLK_KP_0: return 0x60;
    case SDLK_RETURN: return 0x0D;   case SDLK_ESCAPE: return 0x1B;
    case SDLK_SPACE: return 0x20;    case SDLK_TAB: return 0x09;
    case SDLK_BACKSPACE: return 0x08;
    case SDLK_LEFT: return 0x25;     case SDLK_UP: return 0x26;
    case SDLK_RIGHT: return 0x27;    case SDLK_DOWN: return 0x28;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10;
    case SDLK_LCTRL: case SDLK_RCTRL: return 0x11;
    case SDLK_LALT: case SDLK_RALT: return 0x12;
    case SDLK_COMMA: return 0xBC;    case SDLK_PERIOD: return 0xBE;
    case SDLK_MINUS: return 0xBD;    case SDLK_EQUALS: return 0xBB;
    case SDLK_DELETE: return 0x2E;   case SDLK_HOME: return 0x24;
    case SDLK_END: return 0x23;      case SDLK_PAGEUP: return 0x21;
    case SDLK_PAGEDOWN: return 0x22; case SDLK_INSERT: return 0x2D;
    default: return 0;
    }
}

static int s_dump_quiet;

static int write_bmp(const char *path, const uint32_t *px, uint32_t w, uint32_t h)
{
    FILE *f;
    uint32_t row = ((w * 3u) + 3u) & ~3u;
    uint32_t img = row * h, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);
    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < h; y++) {
        const uint32_t *src = px + (size_t)(h - 1 - y) * w;
        for (x = 0; x < w; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    return 0;
}

int xbox_FramebufferDumpBmp(const char *path)
{
    if (!s_rgb || !s_fb_va || write_bmp(path, s_rgb, s_fb_width, s_fb_height) != 0)
        return -1;
    if (!s_dump_quiet)
        fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
                path, s_fb_width, s_fb_height, s_fb_va);
    return 0;
}

/* RECOMP_SNAP_TRIGGER=<file>: when it appears, dump the shown frame to the BMP
 * path it contains and delete it. hi: the GPU backend's full-resolution frame
 * when that is what the window shows (s_rgb is not updated then). */
static void snap_trigger_poll(const uint32_t *hi, uint32_t hi_w, uint32_t hi_h)
{
    static int checked;
    static const char *trig;
    FILE *f;
    char path[1024];
    size_t n;

    if (!checked) {
        checked = 1;
        trig = getenv("RECOMP_SNAP_TRIGGER");
    }
    if (!trig || access(trig, F_OK) != 0)
        return;
    f = fopen(trig, "rb");
    if (!f)
        return;
    n = fread(path, 1, sizeof path - 1, f);
    fclose(f);
    while (n && (path[n - 1] == '\n' || path[n - 1] == '\r' || path[n - 1] == ' '))
        n--;
    path[n] = 0;
    unlink(trig);
    if (n && hi) {
        write_bmp(path, hi, hi_w, hi_h);
    } else if (n) {
        s_dump_quiet = 1;
        xbox_FramebufferDumpBmp(path);
        s_dump_quiet = 0;
    }
}

static void *fb_thread(void *unused)
{
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex, *hi_tex = NULL;
    uint32_t *hi_rgb = NULL, hi_w = 0, hi_h = 0;
    double t0 = 0;
    int f0 = 0;

    (void)unused;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "  [FBWIN] SDL video init failed: %s\n", SDL_GetError());
        s_fb_running = 0;
        return NULL;
    }
    win = SDL_CreateWindow("Xbox Recomp - Framebuffer",
                           SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                           (int)s_fb_width, (int)s_fb_height,
                           SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC) : NULL;
    tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  (int)s_fb_width, (int)s_fb_height) : NULL;
    if (!tex) {
        fprintf(stderr, "  [FBWIN] window setup failed: %s\n", SDL_GetError());
        s_fb_running = 0;
        return NULL;
    }
    SDL_RenderSetLogicalSize(ren, (int)s_fb_width, (int)s_fb_height);
    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);
    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u)\n",
            s_fb_width, s_fb_height);

    while (s_fb_running) {
        SDL_Event e;
        int idx;

        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                /* Window close, and SDL turns SIGINT/SIGTERM into this too.
                 * Closing only the window left the game running and the
                 * next flip reopened it. */
                fflush(NULL);
                _exit(0);
            } else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                int vk = sdl_to_vk(e.key.keysym.sym);
                if (vk)
                    s_key_down[vk] = e.type == SDL_KEYDOWN;
            } else if (e.type == SDL_WINDOWEVENT &&
                       e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                memset((void *)s_key_down, 0, sizeof s_key_down);
            }
        }
        if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED)) {
            int cw, ch;
            SDL_GetWindowSize(win, &cw, &ch);
            __atomic_store_n(&s_client_w, cw, __ATOMIC_RELAXED);
            __atomic_store_n(&s_client_h, ch, __ATOMIC_RELAXED);
        }
        pthread_mutex_lock(&s_hi_lock);
        if (s_hi && now_ms() - s_hi_at < 250.0) {
            int fresh = s_hi_new;
            if (s_hi_w != hi_w || s_hi_h != hi_h) {
                if (hi_tex) SDL_DestroyTexture(hi_tex);
                free(hi_rgb);
                hi_w = s_hi_w; hi_h = s_hi_h;
                hi_rgb = (uint32_t *)malloc((size_t)hi_w * hi_h * 4);
                hi_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                           SDL_TEXTUREACCESS_STREAMING, (int)hi_w, (int)hi_h);
                SDL_RenderSetLogicalSize(ren, (int)hi_w, (int)hi_h);
                fresh = 1;
            }
            if (fresh && hi_rgb)
                memcpy(hi_rgb, s_hi, (size_t)hi_w * hi_h * 4);
            s_hi_new = 0;
            pthread_mutex_unlock(&s_hi_lock);
            if (hi_tex && hi_rgb) {
                if (fresh) {
                    xbox_FramebufferApplyGamma(hi_rgb, (size_t)hi_w * hi_h);
                    SDL_UpdateTexture(hi_tex, NULL, hi_rgb, (int)(hi_w * 4));
                }
                SDL_RenderClear(ren);
                SDL_RenderCopy(ren, hi_tex, NULL, NULL);
                SDL_RenderPresent(ren);
                snap_trigger_poll(hi_rgb, hi_w, hi_h);
                goto shown;
            }
        } else {
            pthread_mutex_unlock(&s_hi_lock);
            if (hi_tex) {
                SDL_DestroyTexture(hi_tex);
                hi_tex = NULL;
                hi_w = hi_h = 0;
                SDL_RenderSetLogicalSize(ren, (int)s_fb_width, (int)s_fb_height);
            }
        }
        idx = __atomic_load_n(&s_present_idx, __ATOMIC_ACQUIRE);
        if (idx >= 0 && s_present[idx])
            memcpy(s_rgb, s_present[idx], (size_t)s_fb_width * s_fb_height * 4);
        else if (s_fb_va && s_fb_pitch) {
            const uint8_t *src = (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            uint32_t y;
            for (y = 0; y < s_fb_height; y++)
                fb_convert_row(src + (size_t)y * s_fb_pitch,
                               s_rgb + (size_t)y * s_fb_width,
                               s_fb_pitch / s_fb_width);
        }
        xbox_FramebufferApplyGamma(s_rgb, (size_t)s_fb_width * s_fb_height);
        SDL_UpdateTexture(tex, NULL, s_rgb, (int)(s_fb_width * 4));
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        snap_trigger_poll(NULL, 0, 0);
shown:
        {
            double now = now_ms();
            if (now - t0 >= 1000.0) {
                int f = s_flips;
                char tb[128];
                snprintf(tb, sizeof tb, "%s | FPS: %.1f | draws: %d", s_title,
                         t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0, s_frame_draws);
                SDL_SetWindowTitle(win, tb);
                t0 = now; f0 = f;
            }
        }
    }
    if (hi_tex) SDL_DestroyTexture(hi_tex);
    free(hi_rgb);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    return NULL;
}

void xbox_FramebufferWindowStart(void)
{
    pthread_t th;

    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    if (__sync_val_compare_and_swap(&s_fb_running, 0, 1) != 0)
        return;
    if (pthread_create(&th, NULL, fb_thread, NULL) == 0)
        pthread_detach(th);
    else
        s_fb_running = 0;
}
#endif /* !_WIN32 */
