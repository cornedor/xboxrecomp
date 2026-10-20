/**
 * Show the guest framebuffer in a window.
 *
 * The title renders into its own framebuffer in guest RAM and tells the kernel
 * where it is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out. Nothing here scans anything out, so however much of the GPU is
 * implemented, none of it is observable. This is the other half: a window that
 * reads that memory and puts it on screen.
 *
 * Deliberately plain GDI rather than the D3D8 layer. The point is to display
 * whatever the guest actually wrote, so the fewer stages between guest memory
 * and the screen the better -- and it must keep working while the D3D8 layer
 * is busy with something else, such as the FMV player's own window.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#include <stdint.h>
#include <stddef.h>
void xbox_FramebufferApplyGamma(uint32_t *px, size_t n);   /* fb_gamma.c */

#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu_present.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

static volatile LONG s_fb_running;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;           /* converted 32-bit copy for GDI */

/* A finished frame, taken at the flip and shown until the next one.
 *
 * The window used to convert straight out of guest memory every 16 ms. Even
 * pointed at the buffer the title had just finished, that races the executor
 * drawing the next frame into the other one and, whenever the two swap, puts
 * a half-drawn image on the screen -- which is the flicker. Copying the
 * finished frame once per flip means the window never reads memory the
 * rasteriser is writing, so what it shows cannot be half of anything.
 *
 * Two buffers and an index, swapped after the copy completes, so the window
 * thread is never reading the one being filled. */
static uint32_t     *s_present[2];
static volatile LONG s_present_idx = -1;   /* -1 until the first flip */

/* The window's client area in pixels, kept by the window thread (0 until
 * the window exists, or while minimised): a renderer can match its frame's
 * shape to it. */
static volatile LONG s_client_w, s_client_h;

int xbox_FramebufferClientSize(int *w, int *h)
{
    LONG cw = s_client_w, ch = s_client_h;
    if (cw <= 0 || ch <= 0)
        return 0;
    *w = (int)cw; *h = (int)ch;
    return 1;
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the window to one guest address instead of following
     * whichever surface is being drawn into. A black window cannot distinguish
     * "the read path is broken" from "the title rendered black", and pointing
     * it at memory known to have content settles that. */
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    LONG next;
    uint32_t bpp, x, y;

    if (!s_fb_running || !fb_va || !pitch)
        return;
    if (getenv("RECOMP_FB_VA"))
        return;                       /* pinned: leave the old path alone */
    next = (s_present_idx == 0) ? 1 : 0;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height,
                                             4);
        if (!s_present[next])
            return;
    }
    bpp = pitch / s_fb_width;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch;
        uint32_t *dst = s_present[next] + (size_t)y * s_fb_width;

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
    /* Published only once it is whole. */
    InterlockedExchange(&s_present_idx, next);
}

/* A finished frame straight from a GPU backend, at its own (possibly higher)
 * resolution: shown instead of the guest surface while they keep coming
 * (the guest copy still feeds screenshots and the GDI fallback). Same
 * two-buffer publication as above. */
/* Three buffers, so neither side ever waits or copies for the other: the
 * flip thread writes into its back buffer and swaps it with the middle
 * ("latest", flag 4 = not shown yet); the window thread swaps its front
 * buffer with the middle when it holds a new frame, then uploads straight
 * from the front buffer. (It was two buffers and a 29 MB copy per window
 * pass at 5120x1440, plus a CPU gamma pass -- 8 ms of work, ~90 passes a
 * second, presenting each frame up to a pass late: uneven, choppy motion
 * at a steady 60 FPS.) */
static uint32_t     *s_hi[3];
static uint32_t      s_hi_w[3], s_hi_h[3];
static size_t        s_hi_cap[3];
static LONG          s_hi_back = 0;        /* flip thread's */
static LONG          s_hi_front = 2;       /* window thread's */
static int           s_hi_have;            /* the front buffer holds a frame */
static volatile LONG s_hi_mid = 1;         /* index | 4 when new */
static volatile LONG s_hi_tick;           /* tick of the last one */
static volatile LONG s_hi_serial;         /* +1 per published frame */
static HANDLE        s_hi_event;          /* set per published frame */

void xbox_FramebufferPublishImage(const uint32_t *px, uint32_t w, uint32_t h)
{
    LONG b = s_hi_back;
    size_t n = (size_t)w * h;
    if (!s_fb_running || !px || !n)
        return;
    if (s_hi_cap[b] < n) {
        free(s_hi[b]);
        s_hi[b] = (uint32_t *)malloc(n * 4);
        s_hi_cap[b] = s_hi[b] ? n : 0;
        if (!s_hi[b])
            return;
    }
    memcpy(s_hi[b], px, n * 4);
    s_hi_w[b] = w; s_hi_h[b] = h;
    s_hi_back = InterlockedExchange(&s_hi_mid, b | 4) & 3;
    InterlockedExchange(&s_hi_tick, (LONG)GetTickCount());
    InterlockedIncrement(&s_hi_serial);
    if (s_hi_event)
        SetEvent(s_hi_event);
}

/* Window thread: take the newest frame if there is one (1), else keep the
 * front buffer (0). */
static int hi_acquire(void)
{
    if (!(s_hi_mid & 4))
        return 0;
    s_hi_front = InterlockedExchange(&s_hi_mid, s_hi_front) & 3;
    s_hi_have = 1;
    return 1;
}

/* Which keys are down, for the pad stand-in in src/input.
 *
 * GetAsyncKeyState looked like the cheaper way to ask and does not work
 * here: it reads a state Wine keeps for the X server, and a guest process
 * drawing through GDI never sees it change. The window that has the focus
 * is the thing that receives the keys, so that is what has to remember
 * them.
 *
 * Reading this needs no lock. Each entry is written only by the window
 * thread and read only by the USB thread, one byte at a time, and a press
 * seen a frame late is indistinguishable from one made a frame later. */
static volatile unsigned char s_key_down[256];

/* Title bar, the way ps3recomp's window shows it. Written by the flip,
 * read once a second by the window thread; a torn read shows one stale
 * number for a second, which nobody can tell apart from a real one. */
static wchar_t       s_title[48] = L"Xbox Recomp";
static volatile LONG s_flips;
static volatile LONG s_frame_draws;

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;
    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
        s_title[i] = (wchar_t)name[i];
    if (i)
        s_title[i] = 0;
}

/* RECOMP_FRAME_STATS=1: frame times from the flips themselves, once a second:
 *   [FRAMES] n 11 fps 10.9 p50 88.1 p99 120.4 max 131.0 ms draws 612
 * The title-bar FPS is a 1 s average; a 60 FPS target is about the slow
 * frames, so this reports the distribution (tools/bench.ps1 parses it). */
static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void frame_stats_note(uint32_t draws)
{
    static int on = -1;
    static LARGE_INTEGER freq, last, window;
    static double dt[2048];
    static int n;
    LARGE_INTEGER now;

    if (on < 0) {
        const char *e = getenv("RECOMP_FRAME_STATS");
        on = e && *e && *e != '0';
        QueryPerformanceFrequency(&freq);
    }
    if (!on)
        return;
    QueryPerformanceCounter(&now);
    {
        /* A slow frame (> 22 ms between flips): how much of it the GPU
         * executor spent executing -- busy close to the whole interval means
         * the frame's GPU work did not fit, little means the title was late
         * handing it over. Up to 10 a second. */
        extern double xbox_Nv2aExecBusyMs(void);
        static double busy_last;
        static int slow_said;
        double busy = xbox_Nv2aExecBusyMs();
        if (last.QuadPart) {
            double d = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
            if (d > 22.0 && slow_said < 10) {
                slow_said++;
                fprintf(stderr, "[SLOWFRAME] %.1f ms, executor busy %.1f ms, draws %u\n",
                        d, busy - busy_last, draws);
            }
        }
        busy_last = busy;
        if (!n)
            slow_said = 0;
    }
    if (last.QuadPart && n < (int)(sizeof dt / sizeof dt[0]))
        dt[n++] = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
    last = now;
    if (!window.QuadPart)
        window = now;
    if ((double)(now.QuadPart - window.QuadPart) / (double)freq.QuadPart >= 1.0 && n) {
        double secs = (double)(now.QuadPart - window.QuadPart) / (double)freq.QuadPart;
        qsort(dt, (size_t)n, sizeof dt[0], cmp_double);
        fprintf(stderr, "[FRAMES] n %d fps %.1f p50 %.1f p99 %.1f max %.1f ms draws %u\n",
                n, n / secs, dt[n / 2], dt[(n * 99) / 100 < n ? (n * 99) / 100 : n - 1],
                dt[n - 1], draws);
        fflush(stderr);
        n = 0;
        window = now;
    }
}

/* For the FPS overlay: the longest gap between flips, this second. */
static volatile LONG s_flip_gap_max_us;

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    {
        static LARGE_INTEGER freq, last;
        LARGE_INTEGER now;
        if (!freq.QuadPart)
            QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        if (last.QuadPart) {
            LONG us = (LONG)((now.QuadPart - last.QuadPart) * 1000000 / freq.QuadPart);
            if (us > s_flip_gap_max_us)
                InterlockedExchange(&s_flip_gap_max_us, us);
        }
        last = now;
    }
    InterlockedIncrement(&s_flips);
    InterlockedExchange(&s_frame_draws, (LONG)draws);
    frame_stats_note(draws);
}

int xbox_FramebufferKeyDown(int vk)
{
    if ((unsigned)vk > 255)
        return 0;
    return s_key_down[vk] != 0;
}

/* Alt+Enter / F11: borderless fullscreen on the window's monitor, and back.
 * (DXGI's own Alt+Enter, exclusive mode at the frame's 640x480, is off --
 * gpu_present.c; the presenter letterboxes to any size.) */
static void fb_toggle_fullscreen(HWND h)
{
    static int full;
    static RECT saved;
    static LONG_PTR saved_style;
    if (!full) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        GetWindowRect(h, &saved);
        saved_style = GetWindowLongPtrA(h, GWL_STYLE);
        SetWindowLongPtrA(h, GWL_STYLE, (saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        full = 1;
    } else {
        SetWindowLongPtrA(h, GWL_STYLE, saved_style);
        SetWindowPos(h, NULL, saved.left, saved.top, saved.right - saved.left,
                     saved.bottom - saved.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
        full = 0;
    }
}

/* FPS overlay (F3, or RECOMP_FPS_OVERLAY=1 to start with it on): drawn
 * into the presented image's top-left corner, once a second refreshed:
 *   FPS 60  SHOWN 60  MAX 18MS
 * FPS = frames the game flipped, SHOWN = new frames that reached the window
 * (fewer: the window loop missed some), MAX = the longest gap between two
 * flips. A 5x7 pixel font, scaled with the image. */
static volatile LONG s_overlay = -1;
static LONG s_pres_shown, s_pres_loops;  /* window thread only */
static double s_pres_work, s_pres_work_max, s_pres_present;

static double pres_now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
static char s_overlay_text[48];

static void fps_overlay_toggle(void)
{
    if (s_overlay < 0) {
        const char *e = getenv("RECOMP_FPS_OVERLAY");
        s_overlay = e && *e && *e != '0';
    }
    InterlockedExchange(&s_overlay, !s_overlay);
}

static int fps_overlay_on(void)
{
    if (s_overlay < 0) {
        const char *e = getenv("RECOMP_FPS_OVERLAY");
        s_overlay = e && *e && *e != '0';
    }
    return s_overlay > 0;
}

/* 5x7 glyphs, one byte per row, bit 4 = leftmost column. */
static const unsigned char *glyph(char ch)
{
    static const unsigned char g_digits[10][7] = {
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
        {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
        {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
        {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
        {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}};
    static const struct { char c; unsigned char rows[7]; } g_letters[] = {
        {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}}, {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
        {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}}, {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
        {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'W', {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},
        {'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11}}, {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
        {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}}, {'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
        {'.', {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}}};
    size_t i;
    if (ch >= '0' && ch <= '9')
        return g_digits[ch - '0'];
    for (i = 0; i < sizeof g_letters / sizeof g_letters[0]; i++)
        if (g_letters[i].c == ch)
            return g_letters[i].rows;
    return NULL;                        /* space */
}

static void fps_overlay_draw(uint32_t *px, uint32_t w, uint32_t h)
{
    char text[48];
    int sc = (int)(h / 240u), len, i, x0, y0, bw, bh, x, y;
    if (!fps_overlay_on() || !px || w < 64 || h < 32)
        return;
    memcpy(text, s_overlay_text, sizeof text);
    text[sizeof text - 1] = 0;
    len = (int)strlen(text);
    if (!len)
        return;
    if (sc < 2) sc = 2;
    x0 = sc * 3; y0 = sc * 3;
    bw = (len * 6 + 2) * sc; bh = 10 * sc;
    if (x0 + bw > (int)w) bw = (int)w - x0;
    for (y = y0; y < y0 + bh && y < (int)h; y++)      /* darkened box */
        for (x = x0; x < x0 + bw; x++) {
            uint32_t p = px[(size_t)y * w + x];
            px[(size_t)y * w + x] = (p >> 2) & 0x3F3F3F;
        }
    for (i = 0; i < len; i++) {
        const unsigned char *g = glyph(text[i]);
        int r, cx, gx = x0 + (i * 6 + 1) * sc, gy = y0 + sc + sc / 2;
        if (!g)
            continue;
        for (r = 0; r < 7; r++)
            for (cx = 0; cx < 5; cx++)
                if (g[r] & (0x10 >> cx))
                    for (y = gy + r * sc; y < gy + (r + 1) * sc && y < (int)h; y++)
                        for (x = gx + cx * sc; x < gx + (cx + 1) * sc && x < (int)w; x++)
                            px[(size_t)y * w + x] = 0xFFFFFF;
    }
}

/* Once a second from the window thread: the overlay's numbers, and with
 * RECOMP_FRAME_STATS the window loop's own (loops, new frames, time spent
 * copying + gamma + upload before Present, and in Present). */
static void present_second(LONG flips_delta, LONG shown, LONG loops, double work_ms,
                           double work_max, double present_ms)
{
    LONG gap = InterlockedExchange(&s_flip_gap_max_us, 0);
    snprintf(s_overlay_text, sizeof s_overlay_text, "FPS %ld  SHOWN %ld  MAX %ldMS",
             (long)flips_delta, (long)shown, (long)((gap + 500) / 1000));
    if (getenv("RECOMP_FRAME_STATS")) {
        fprintf(stderr, "[PRESENT] loops %ld new %ld | work avg %.1f max %.1f ms, present avg %.1f ms\n",
                (long)loops, (long)shown, loops ? work_ms / loops : 0.0, work_max,
                loops ? present_ms / loops : 0.0);
    }
}

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if ((m == WM_SYSKEYDOWN && w == VK_RETURN && (l & (1 << 29)) && !(l & (1 << 30)))
        || (m == WM_KEYDOWN && w == VK_F11 && !(l & (1 << 30)))) {
        fb_toggle_fullscreen(h);
        return 0;                       /* not an Enter for the game */
    }
    if (m == WM_KEYDOWN && w == VK_F3 && !(l & (1 << 30))) {
        fps_overlay_toggle();
        return 0;
    }
    if (m == WM_SYSKEYDOWN && w == VK_RETURN)
        return 0;                       /* auto-repeat of Alt+Enter */
    switch (m) {
    case WM_CLOSE:
    case WM_DESTROY:
        /* Closing the window quits the game. Only stopping this thread left
         * every other thread running -- the audio played on and the process
         * had to be ended in Task Manager. The game has no clean exit path
         * of its own (the console never quits), so end the process here;
         * RECOMP_CLOSE_KEEPS_RUNNING=1 keeps the old behaviour. */
        InterlockedExchange(&s_fb_running, 0);
        if (!getenv("RECOMP_CLOSE_KEEPS_RUNNING")) {
            extern void xbox_BeforeExit(void);
            fprintf(stderr, "  [FBWIN] window closed: exiting\n");
            fflush(stderr);
            fflush(stdout);
            xbox_BeforeExit();
            TerminateProcess(GetCurrentProcess(), 0);
        }
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if ((unsigned)w < 256)
            s_key_down[w] = 1;
        /* RECOMP_KEY_TRACE: each key as it arrives, edge-triggered.
         *
         * The obvious diagnostic -- sampling which keys are held, once a
         * second, from the input path -- cannot tell a key that was never
         * pressed from one that was tapped: a 100 ms press is caught about
         * one time in ten. That ambiguity is expensive when the only way
         * to test is to ask someone to press a key and describe what
         * happened. This answers "did it arrive" on its own. */
        if (getenv("RECOMP_KEY_TRACE")) {
            static unsigned n;
            if (n++ < 40) {
                fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)w);
                fflush(stderr);
            }
        }
        /* System keys still go to Windows, or Alt+F4 stops closing us. */
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        if ((unsigned)w < 256)
            s_key_down[w] = 0;
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        memset((void *)s_key_down, 0, sizeof s_key_down);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* The display formats an Xbox front buffer is actually set to. The pitch says
 * how wide a row is in bytes, so pitch/width gives the pixel size; the exact
 * component layout only matters for 16-bit, where 5:6:5 and 1:5:5:5 differ. */
static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    uint32_t x, y;

    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * s_fb_pitch;
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

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
}

/* Write what the window is currently showing to a 24-bit BMP.
 *
 * A black window is ambiguous: it means either that the read path is wrong or
 * that the title really did render black. Dumping the same converted pixels
 * the window draws settles which, and does it without a screenshot. */
static int s_dump_quiet;

int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    const uint32_t *pix = s_rgb;
    uint32_t *hi = NULL;
    uint32_t w = s_fb_width, h = s_fb_height;
    uint32_t row, img, total, y, x;
    uint8_t hdr[54], *line;

    if (!s_rgb || !s_fb_va)
        return -1;
    {
        /* A backend publishing its own full-resolution frames: dump that,
         * gamma applied as on screen. */
        /* (called on the window thread, which owns the front buffer) */
        LONG k = s_hi_front;
        if (s_hi_have && (LONG)GetTickCount() - s_hi_tick < 500) {
            size_t n = (size_t)s_hi_w[k] * s_hi_h[k];
            hi = (uint32_t *)malloc(n * 4);
            if (hi) {
                memcpy(hi, s_hi[k], n * 4);
                xbox_FramebufferApplyGamma(hi, n);
                pix = hi; w = s_hi_w[k]; h = s_hi_h[k];
            }
        }
    }
    row = ((w * 3u) + 3u) & ~3u;
    img = row * h; total = 54u + img;
    f = fopen(path, "wb");
    if (!f) {
        free(hi);
        return -1;
    }
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
        const uint32_t *src = pix + (size_t)(h - 1 - y) * w;
        for (x = 0; x < w; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    free(hi);
    fclose(f);
    if (!s_dump_quiet)
        fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
                path, w, h, s_fb_va);
    return 0;
}

/* RECOMP_SNAP_TRIGGER=<file>: when the file appears, write the frame being
 * shown to the BMP path it contains and delete it. Screenshots taken from
 * inside work whatever presents the frame -- PrintWindow cannot read a D3D11
 * swap chain -- and with the window covered (tools\live.ps1 snap uses it). */
static void snap_trigger_poll(void)
{
    static int checked;
    static const char *trig;
    FILE *f;
    char path[MAX_PATH];
    size_t n;

    if (!checked) {
        checked = 1;
        trig = getenv("RECOMP_SNAP_TRIGGER");
    }
    if (!trig || GetFileAttributesA(trig) == INVALID_FILE_ATTRIBUTES)
        return;
    f = fopen(trig, "rb");
    if (!f)
        return;
    n = fread(path, 1, sizeof path - 1, f);
    fclose(f);
    while (n && (path[n - 1] == '\n' || path[n - 1] == '\r' || path[n - 1] == ' '))
        n--;
    path[n] = 0;
    DeleteFileA(trig);
    if (n) {
        if (getenv("RECOMP_SNAP_PRESENTED") && gpu_present_active()) {
            gpu_present_capture(path);    /* what the window shows */
            return;
        }
        s_dump_quiet = 1;
        xbox_FramebufferDumpBmp(path);
        s_dump_quiet = 0;
    }
}

/* GDI fallback: the frame letterboxed into the client area, like the D3D11
 * presenter; the borders are cleared when the size changes. */
static void gdi_blit(HWND hwnd, HDC hdc, const BITMAPINFO *bi)
{
    static LONG lw, lh;
    RECT c;
    LONG cw, ch, w, h;
    GetClientRect(hwnd, &c);
    cw = c.right - c.left; ch = c.bottom - c.top;
    if (cw <= 0 || ch <= 0)
        return;
    if (cw != lw || ch != lh) {
        FillRect(hdc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
        lw = cw; lh = ch;
    }
    if ((long long)cw * s_fb_height <= (long long)ch * s_fb_width) {
        w = cw; h = (LONG)((long long)cw * s_fb_height / s_fb_width);
    } else {
        h = ch; w = (LONG)((long long)ch * s_fb_width / s_fb_height);
    }
    SetStretchBltMode(hdc, HALFTONE);
    StretchDIBits(hdc, (cw - w) / 2, (ch - h) / 2, w, h,
                  0, 0, (int)s_fb_width, (int)s_fb_height,
                  s_rgb, bi, DIB_RGB_COLORS, SRCCOPY);
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;

    (void)unused;

    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "XboxRecompFramebuffer";
        RegisterClassA(&wc);
    }
    {
        /* RECOMP_WINDOW_SCALE (default 2): the client area is the frame times
         * this, made smaller until the window fits the desktop work area;
         * centred on it. The window can be resized freely afterwards. */
        /* Default: the internal resolution's scale (RECOMP_RES, e.g. 1080 =
         * x2.25), else 2 -- stepped down by quarters until it fits. */
        const char *e = getenv("RECOMP_WINDOW_SCALE"), *res = getenv("RECOMP_RES");
        double scale = 2.0, rv = res ? atof(res) : 0;
        RECT work;
        if (e && atof(e) > 0)
            scale = atof(e);
        else if (rv >= 240)
            scale = rv / 480.0;
        else if (rv >= 1.0 && rv <= 8.0)
            scale = rv;
        if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
            work.left = work.top = 0; work.right = 1920; work.bottom = 1080;
        }
        for (;; scale -= 0.25) {
            r.left = 0; r.top = 0;
            {   /* RECOMP_ASPECT: the window has the renderer's frame shape */
                const char *ws = getenv("RECOMP_ASPECT");
                double aspect_x = (ws && atof(ws) > 4.0 / 3.0 + 0.01) ? atof(ws) * 3.0 / 4.0 : 1.0;
                r.right = (LONG)((double)s_fb_width * aspect_x * (scale > 1.0 ? scale : 1.0) + 0.5);
            }
            r.bottom = (LONG)((double)s_fb_height * (scale > 1.0 ? scale : 1.0) + 0.5);
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            if (scale <= 1.0 || (r.right - r.left <= work.right - work.left
                                 && r.bottom - r.top <= work.bottom - work.top))
                break;
        }
        hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               work.left + ((work.right - work.left) - (r.right - r.left)) / 2,
                               work.top + ((work.bottom - work.top) - (r.bottom - r.top)) / 2,
                               r.right - r.left, r.bottom - r.top,
                               NULL, NULL, GetModuleHandleA(NULL), NULL);
    }
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    hdc = GetDC(hwnd);
    {
        /* RECOMP_FULLSCREEN=1: start in borderless fullscreen (F11 /
         * Alt+Enter still toggle) -- the launcher's "Fullscreen" box. */
        const char *fs = getenv("RECOMP_FULLSCREEN");
        if (fs && *fs && *fs != '0')
            fb_toggle_fullscreen(hwnd);
    }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_fb_width;
    bi.bmiHeader.biHeight      = -(LONG)s_fb_height;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u)\n",
            s_fb_width, s_fb_height);
    if (gpu_present_enabled())
        gpu_present_init(hwnd, s_fb_width, s_fb_height);
    if (!s_hi_event)
        s_hi_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        {
            RECT cr;
            if (!IsIconic(hwnd) && GetClientRect(hwnd, &cr)) {
                InterlockedExchange(&s_client_w, cr.right - cr.left);
                InterlockedExchange(&s_client_h, cr.bottom - cr.top);
            }
        }
        if (gpu_present_active() && (s_hi_mid & 4 || s_hi_have)
            && (LONG)GetTickCount() - s_hi_tick < 500) {
            /* The backend's own image (full resolution), each new frame
             * presented once, as soon as it is published: wait for the next
             * (or for window messages) instead of re-presenting the same
             * one. Gamma is applied by the presenter's shader. */
            if (!hi_acquire()) {
                MsgWaitForMultipleObjects(1, &s_hi_event, FALSE, 20, QS_ALLINPUT);
                if (!hi_acquire())
                    goto window_tail;
            }
            {
                LONG k = s_hi_front;
                double tw0 = pres_now_ms(), tw1;
                fps_overlay_draw(s_hi[k], s_hi_w[k], s_hi_h[k]);
                tw1 = pres_now_ms();
                s_pres_shown++;
                if (!gpu_present_frame_gamma(s_hi[k], s_hi_w[k], s_hi_h[k], 1))
                    Sleep(16);
                s_pres_loops++;
                s_pres_work += tw1 - tw0;
                if (tw1 - tw0 > s_pres_work_max) s_pres_work_max = tw1 - tw0;
                s_pres_present += pres_now_ms() - tw1;
            }
            if (s_present_idx >= 0 && s_rgb && s_present[s_present_idx])
                memcpy(s_rgb, s_present[s_present_idx], (size_t)s_fb_width * s_fb_height * 4);
        } else if (s_present_idx >= 0 && s_rgb) {
            /* A finished frame, published by the flip. Copied into s_rgb so
             * the dump path and GDI see one consistent image even if the
             * next flip lands mid-blit. */
            LONG idx = s_present_idx;
            if (s_present[idx])
                memcpy(s_rgb, s_present[idx],
                       (size_t)s_fb_width * s_fb_height * 4);
            xbox_FramebufferApplyGamma(s_rgb, (size_t)s_fb_width * s_fb_height);
            if (gpu_present_active()) {
                fps_overlay_draw(s_rgb, s_fb_width, s_fb_height);
                if (!gpu_present_frame_size(s_rgb, s_fb_width, s_fb_height))  /* vsync-paced, unless occluded */
                    Sleep(16);
            }
            else
                gdi_blit(hwnd, hdc, &bi);
        } else if (s_fb_va && s_fb_pitch && s_rgb) {
            /* No flip yet, or pinned with RECOMP_FB_VA: read guest memory as
             * before, which is also what a title that never flips needs. */
            const uint8_t *src =
                (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            fb_convert(src, s_fb_pitch / s_fb_width);
            xbox_FramebufferApplyGamma(s_rgb, (size_t)s_fb_width * s_fb_height);
            if (gpu_present_active()) {
                if (!gpu_present_frame_size(s_rgb, s_fb_width, s_fb_height))
                    Sleep(16);
            } else
                gdi_blit(hwnd, hdc, &bi);
        }
    window_tail:
        {
            /* One dump a few seconds in, so the title has had time to render
             * something rather than catching the first blank frame.
             *
             * RECOMP_FB_WINDOW_DUMP_EVERY=<frames> dumps repeatedly instead.
             * This window follows the address AvSetDisplayMode gave, which is
             * what the CRTC scans and therefore what a person sees; the
             * pushbuffer executor's own dump follows its draw surface. With
             * double buffering those are different buffers, and measuring
             * progress from the executor's dump reports a blank screen while
             * the window is showing the title's logo. Ask the window. */
            const char *dump = getenv("RECOMP_FB_DUMP");
            const char *every = getenv("RECOMP_FB_WINDOW_DUMP_EVERY");
            static int frames;
            int period = every ? atoi(every) : 0;
            frames++;
            if (dump && period > 0) {
                if (frames % period == 0)
                    xbox_FramebufferDumpBmp(dump);
            } else if (dump && frames == 600) {
                xbox_FramebufferDumpBmp(dump);
            }
        }
        {
            static DWORD t0;
            static LONG f0;
            DWORD now = GetTickCount();
            if (now - t0 >= 1000) {
                LONG f = s_flips;
                wchar_t tb[128];
                present_second(t0 ? f - f0 : 0, s_pres_shown, s_pres_loops, s_pres_work,
                               s_pres_work_max, s_pres_present);
                s_pres_shown = s_pres_loops = 0;
                s_pres_work = s_pres_work_max = s_pres_present = 0;
                _snwprintf(tb, 127, L"%ls | FPS: %.1f | draws: %ld", s_title,
                           t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0,
                           (long)s_frame_draws);
                tb[127] = 0;
                SetWindowTextW(hwnd, tb);
                t0 = now; f0 = f;
            }
        }
        snap_trigger_poll();
        if (!gpu_present_active())
            Sleep(16);                    /* D3D11 present waits for vsync */
    }

    gpu_present_shutdown();
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(s_rgb);
    s_rgb = NULL;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;

    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&s_fb_running, 0);
}

#endif
