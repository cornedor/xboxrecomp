#include "kernel.h"
/*
 * Read-only survey of the pushbuffer a title submits.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; nothing
 * here executes them, so the framebuffer stays black however far the game
 * gets. Before any of that can be made to draw, the question is what it
 * actually asks for -- which methods, on which object classes, how many of
 * them -- because that is the difference between "the existing PGRAPH
 * translator nearly covers this" and "this needs a real one".
 *
 * Purely a reader: it walks the buffer and counts, and never writes to guest
 * memory or to the GPU state. Enabled with RECOMP_PB_SCAN.
 *
 * Pushbuffer encoding (NV20/NV2A), one dword per command header:
 *   (w & 0xE0030003) == 0x00000000  increasing methods
 *   (w & 0xE0030003) == 0x40000000  non-increasing (same method, count params)
 *   (w & 0x00000003) == 0x00000001  jump
 *   (w & 0x00000003) == 0x00000002  call
 *   (w & 0xFFFF0003) == 0x00020000  return
 * For a method header: count = (w >> 18) & 0x7FF, subchannel = (w >> 13) & 7,
 * method = w & 0x1FFC.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>
#include <string.h>

#ifndef XBOX_CONTIG_BASE
#define XBOX_CONTIG_BASE 0x80000000u   /* physical P is at this + P */
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define PB_MAX_METHODS 4096

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;
static uint32_t s_tot_calls;

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_report(void);
static int s_exec_enabled = -1;

/* (subchannel, method) -> index into s_seen + 1, so counting a word is O(1):
 * the linear scan this replaced was a third of the executor's time. Methods
 * are 13 bits, word aligned; subchannels 3 bits. */
static uint16_t s_seen_idx[8 * 2048];

static void note(uint32_t subch, uint32_t method)
{
    uint16_t *slot = &s_seen_idx[((subch & 7u) << 11) | ((method >> 2) & 0x7FFu)];
    if (*slot && s_seen[*slot - 1].method == method && s_seen[*slot - 1].subch == subch) {
        s_seen[*slot - 1].count++;
        return;
    }
    if (s_seen_count >= PB_MAX_METHODS) {
        /* Silently dropping past the cap is how a truncated inventory reads as
         * "the title never does that" -- exactly the wrong conclusion when the
         * inventory is being used to decide what to implement. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[PB] method table full at %d -- inventory is"
                            " truncated\n", PB_MAX_METHODS);
        }
    }
    if (s_seen_count < PB_MAX_METHODS) {
        s_seen[s_seen_count].method = method;
        s_seen[s_seen_count].subch  = subch;
        s_seen[s_seen_count].count  = 1;
        s_seen_count++;
        *slot = (uint16_t)s_seen_count;
    }
}

/* NV097 (Kelvin 3D class) methods worth naming. The point of the survey is to
 * decide what a translator has to implement, and a bare method number does not
 * answer that -- "0x1818 x412" only means something once it reads
 * INLINE_ARRAY. Unnamed ones still get counted. */
static const struct { uint32_t m; const char *name; } NV097_NAMES[] = {
    { 0x0000, "SET_OBJECT" },
    { 0x0100, "NO_OPERATION" },
    { 0x0104, "SET_WARNING_ENABLE" },
    { 0x0130, "SET_FLIP_READ" },
    { 0x0200, "SET_SURFACE_CLIP_HORIZONTAL" },
    { 0x0204, "SET_SURFACE_CLIP_VERTICAL" },
    { 0x0208, "SET_SURFACE_FORMAT" },
    { 0x020C, "SET_SURFACE_PITCH" },
    { 0x0210, "SET_SURFACE_COLOR_OFFSET" },
    { 0x0214, "SET_SURFACE_ZETA_OFFSET" },
    { 0x0300, "SET_ALPHA_TEST_ENABLE" },
    { 0x0304, "SET_BLEND_ENABLE" },
    { 0x030C, "SET_DEPTH_TEST_ENABLE" },
    { 0x0310, "SET_DITHER_ENABLE" },
    { 0x0314, "SET_LIGHTING_ENABLE" },
    { 0x033C, "SET_CULL_FACE_ENABLE" },
    { 0x0340, "SET_DEPTH_MASK" },
    { 0x0350, "SET_CLEAR_DEPTH_VALUE" },
    { 0x1D8C, "SET_CLEAR_DEPTH" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D6C, "SET_ZSTENCIL_CLEAR" },
    { 0x0B80, "SET_TRANSFORM_PROGRAM" },
    { 0x0B00, "SET_TRANSFORM_CONSTANT" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "ARRAY_ELEMENT32" },
    { 0x1818, "INLINE_ARRAY" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B14, "SET_TEXTURE_IMAGE_RECT" },
    { 0x0FD8, "SET_COMBINER_*" },
    { 0x0000, NULL },
};

static const char *nv097_name(uint32_t m)
{
    int i;
    for (i = 0; NV097_NAMES[i].name; i++)
        if (NV097_NAMES[i].m == m)
            return NV097_NAMES[i].name;
    return "";
}

void nv2a_pb_scan_report(void)
{
    int i;

    if (s_exec_enabled > 0) {
        nv2a_pb_exec_report();
        fprintf(stderr, "[PB] %u pushbuffer calls followed\n", s_tot_calls);
    }
    if (!s_seen_count || !getenv("RECOMP_PB_SCAN"))
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u calls,"
                    " %u unrecognised -- %d distinct (subchannel, method)"
                    " pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_calls,
            s_tot_unknown, s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

/* Walk commands from va to end_va. Returns the words consumed.
 *
 * A CALL runs a subroutine -- a pushbuffer somewhere else in memory -- up to
 * its RETURN, then carries on after the CALL. This used to skip CALLs, which
 * was invisible until a title used them for real: RenderWare on the Xbox
 * compiles static geometry into pushbuffers and draws it with a CALL, so the
 * whole race track went missing from Burnout 3's main view while the
 * geometry it submits inline (environment maps, the HUD) drew fine.
 * The NV2A has one level of subroutine, so a CALL inside a call is not
 * followed. Addresses are physical, like PUT's, and reach guest memory the
 * same way: through the contiguous window. */
static uint32_t pb_walk(uint32_t va, uint32_t end_va, int in_call,
                        uint32_t *jumps, uint32_t *unknown)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t words = 0;

    while (va < end_va && words < 0x100000u) {
        uint32_t w = *(const uint32_t *)(mem + va);
        va += 4;
        words++;

        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            uint32_t target = (w & 3u) == 1u ? (w & 0xFFFFFFFCu)
                                             : (w & 0x1FFFFFFCu);
            (*jumps)++;
            if (!in_call)
                break;                        /* the ring: a jump ends it */
            va = XBOX_CONTIG_BASE | (target & 0x0FFFFFFFu);
            end_va = va + 0x400000u;
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {  /* RETURN */
            if (in_call)
                break;
            continue;
        }
        if ((w & 3u) == 2u) {                    /* CALL */
            if (!in_call) {
                uint32_t sub = XBOX_CONTIG_BASE | ((w & 0xFFFFFFFCu) & 0x0FFFFFFFu);
                s_tot_calls++;
                words += pb_walk(sub, sub + 0x400000u, 1, jumps, unknown);
            }
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            for (uint32_t i = 0; i < count && va < end_va; i++) {
                uint32_t m = noninc ? method : method + i * 4;
                note(subch, m);
                /* Same walk, two consumers: the survey counts, the executor
                 * acts. Keeping them on one decode means they can never
                 * disagree about what the stream said. */
                if (s_exec_enabled)
                    nv2a_pb_exec_method(subch, m,
                                        *(const uint32_t *)(mem + va));
                va += 4;
                words++;
            }
            continue;
        }
        (*unknown)++;
    }
    return words;
}

/* Execute the main pushbuffer from GET up to PUT the way the GPU's DMA engine
 * does, and return the new GET.
 *
 * nv2a_pb_scan walks a [start, end) range and stops at the first JUMP, which
 * leaves the caller to guess where the ring wraps (the lowest and highest PUT
 * it has seen). That holds while the title waits for every batch to be drawn;
 * once the GPU runs asynchronously (RECOMP_NV2A_ACK_THREAD) the title fills
 * the ring at its own pace, the guessed end goes stale, and the walk read the
 * previous lap's leftovers -- NOPs with parameters like 106955041 among them.
 * Following each JUMP to its target and stopping exactly at PUT needs no
 * guess. */
uint32_t nv2a_pb_run(uint32_t get_va, uint32_t put_va, volatile uint32_t *get_reg)
{
    /* The DMA pusher's method state, kept across kicks: D3D may set PUT in
     * the middle of a method's data, and resuming there as if at a header
     * read data words as commands -- one of them looked like a JUMP out of
     * RAM, the walk was resynchronised past ~700 KB of commands, and the
     * fence semaphores in them never executed. */
    static uint32_t c_left, c_method, c_subch, c_noninc;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t va = get_va, words = 0, jumps = 0, unknown = 0;

    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    while (va != put_va) {
        uint32_t w;
        /* Never walk out of RAM or forever: a GET that cannot reach PUT (the
         * title restarted the ring without a JUMP, or a jump target was not
         * what it looked like) resynchronises at PUT instead of reading on
         * through memory until it faults. */
        if ((va & 0x0FFFFFFFu) >= 0x04000000u || words >= 0x200000u) {
            static int said;
            if (said++ < 5)
                fprintf(stderr, "  [PB] GET 0x%08X cannot reach PUT 0x%08X (%s); resync\n",
                        va, put_va, words >= 0x200000u ? "too long" : "out of RAM");
            c_left = 0;
            return put_va;
        }
        if (c_left) {                            /* method data */
            note(c_subch, c_method);
            if (s_exec_enabled)
                nv2a_pb_exec_method(c_subch, c_method,
                                    *(const uint32_t *)(mem + va));
            va += 4;
            words++;
            if (!c_noninc)
                c_method += 4;
            c_left--;
            continue;
        }
        /* GET moves as the commands are consumed, as on hardware -- not once
         * the whole segment is done. D3D's blocking fence wait decides from
         * GET whether the GPU is far enough behind to patch a wake-up NOP in
         * at the fence's position; with GET held back for the length of a
         * walk it patched positions this walk had already passed, the NOP
         * never ran, and the title slept forever on its event. */
        if (get_reg)
            *get_reg = va & 0x0FFFFFFFu;
        w = *(const uint32_t *)(mem + va);
        va += 4;
        words++;

        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            uint32_t target = (w & 3u) == 1u ? (w & 0xFFFFFFFCu)
                                             : (w & 0x1FFFFFFCu);
            jumps++;
            va = XBOX_CONTIG_BASE | (target & 0x0FFFFFFFu);
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u)    /* RETURN outside a call */
            continue;
        if ((w & 3u) == 2u) {                    /* CALL */
            uint32_t sub = XBOX_CONTIG_BASE | ((w & 0xFFFFFFFCu) & 0x0FFFFFFFu);
            s_tot_calls++;
            words += pb_walk(sub, sub + 0x400000u, 1, &jumps, &unknown);
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            c_left   = (w >> 18) & 0x7FFu;
            c_subch  = (w >> 13) & 7u;
            c_method =  w & 0x1FFCu;
            c_noninc = (w & 0xE0000000u) == 0x40000000u;
            continue;
        }
        unknown++;
    }
    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
    return va;
}

static void nv2a_pb_scan_impl(uint32_t start_va, uint32_t end_va);

/* RECOMP_EXEC_STATS: how much wall time execution takes. Every 5 s prints the
 * share of time spent inside the executor, the number of batches and bytes --
 * which says whether a slow frame is the rasteriser or something waiting. */
void nv2a_pb_scan(uint32_t start_va, uint32_t end_va)
{
    static int stats = -1;
    static LARGE_INTEGER freq, window_start;
    static double busy_s;
    static unsigned long calls;
    static unsigned long long bytes;
    LARGE_INTEGER t0, t1;

    if (stats < 0) {
        stats = getenv("RECOMP_EXEC_STATS") != NULL;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&window_start);
    }
    if (!stats) {
        nv2a_pb_scan_impl(start_va, end_va);
        return;
    }
    QueryPerformanceCounter(&t0);
    nv2a_pb_scan_impl(start_va, end_va);
    QueryPerformanceCounter(&t1);
    busy_s += (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    calls++;
    bytes += end_va > start_va ? end_va - start_va : 0;
    {
        double win = (double)(t1.QuadPart - window_start.QuadPart) / (double)freq.QuadPart;
        if (win >= 5.0) {
            extern void nv2a_exec_phase_take(double *, double *, double *);
            double vsh, setup, raster;
            nv2a_exec_phase_take(&vsh, &setup, &raster);
            {
                extern void nv2a_exec_flush_take(unsigned long *, unsigned long *,
                                                 unsigned long long *, unsigned long long *);
                unsigned long fp, fs; unsigned long long pp, ps;
                nv2a_exec_flush_take(&fp, &fs, &pp, &ps);
                fprintf(stderr, "  [EXEC] flushes: %lu parallel (%llu Kpx bbox), %lu serial (%llu Kpx bbox)\n",
                        fp, pp / 1000, fs, ps / 1000);
            }
            fprintf(stderr, "  [EXEC] %.0f%% of %.1f s executing; %lu batches, %llu KB, %.1f ms/batch"
                            " | vsh %.0f%% setup %.0f%% raster %.0f%% other %.0f%%\n",
                    100.0 * busy_s / win, win, calls, bytes / 1024,
                    calls ? 1000.0 * busy_s / calls : 0.0,
                    100.0 * vsh / busy_s, 100.0 * setup / busy_s,
                    100.0 * raster / busy_s,
                    100.0 * (busy_s - vsh - setup - raster) / busy_s);
            busy_s = 0; calls = 0; bytes = 0;
            window_start = t1;
        }
    }
}

static void nv2a_pb_scan_impl(uint32_t start_va, uint32_t end_va)
{
    uint32_t words, jumps = 0, unknown = 0;

    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    static int scan = -1;
    if (scan < 0)
        scan = getenv("RECOMP_PB_SCAN") != NULL;
    if (!(scan || s_exec_enabled) || end_va <= start_va)
        return;
    if (end_va - start_va > 0x400000u)        /* a sane single-frame bound */
        end_va = start_va + 0x400000u;

    words = pb_walk(start_va, end_va, 0, &jumps, &unknown);

    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
}
