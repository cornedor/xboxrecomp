/**
 * NV2A vertex program interpreter. See nv2a_vsh_interp.h for why it exists.
 *
 * Encoding. An instruction is four dwords; dword 0 is unused. Fields, as
 * (dword, lowest bit, width):
 *
 *   ILU op        1 25 3      MAC op        1 21 4
 *   const index   1 13 8      input index   1  9 4   (shared by A, B, C)
 *   A negate      1  8 1      A swizzle     1  0 8   (x y z w, 2 bits each,
 *   A temp        2 28 4      A mux         2 26 2    x highest)
 *   B negate      2 25 1      B swizzle     2 17 8
 *   B temp        2 13 4      B mux         2 11 2
 *   C negate      2 10 1      C swizzle     2  2 8
 *   C temp        2 0 2 (high) : 3 30 2 (low)          C mux 3 28 2
 *   MAC temp mask 3 24 4      temp dest     3 20 4
 *   ILU temp mask 3 16 4      out mask      3 12 4
 *   out to o[]    3 11 1      out address   3  3 8   (0: to c[])
 *   out from ILU  3  2 1      a0.x relative 3  1 1   final 3 0 1
 *
 * Masks are x=8 y=4 z=2 w=1. A source mux of 1 reads a temp, 2 an input,
 * 3 a constant. Temp 12 is oPos. When an instruction carries both a MAC and
 * an ILU op, the ILU's temp write goes to R1 whatever the temp field says,
 * and both units read their sources before either writes.
 *
 * The layout is checked against real programs rather than taken on trust:
 * RECOMP_VSH_DUMP disassembles each program the title uploads, and an Xbox
 * D3D program is recognisable -- DP4s against the matrix constants, ending in
 * the runtime's screen-space epilogue (RCC of R12.w, then a MAD into oPos).
 */
#include "nv2a_vsh_interp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_program[NV2A_VSH_SLOTS][4];
static float    s_const[NV2A_VSH_CONSTANTS][4];
static uint32_t s_load_slot, s_load_word, s_start_slot;
static uint32_t s_const_load, s_const_word;
static uint32_t s_cxt_write;
static int      s_dirty = 1;           /* s_program changed since decode */

static uint32_t field(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

/* ---- uploads ------------------------------------------------------------ */

void nv2a_vsh_set_load_slot(uint32_t slot)
{
    s_load_slot = slot;
    s_load_word = 0;
}

void nv2a_vsh_program_word(uint32_t word)
{
    if (s_load_slot < NV2A_VSH_SLOTS) {
        s_program[s_load_slot][s_load_word] = word;
        s_dirty = 1;
    }
    if (++s_load_word == 4) {
        s_load_word = 0;
        s_load_slot++;
    }
}

void nv2a_vsh_set_start_slot(uint32_t slot) { s_start_slot = slot; }
void nv2a_vsh_set_cxt_write(uint32_t enable) { s_cxt_write = enable; }
int  nv2a_vsh_cxt_write(void) { return s_cxt_write != 0; }

void nv2a_vsh_set_constant_load(uint32_t index)
{
    s_const_load = index;
    s_const_word = 0;
}

/* RECOMP_C57_TRACE: every write that lands in constant 57 (and where from). */
static void c57_trace(const char *src, uint32_t comp, uint32_t word)
{
    static int on = -1;
    static long n;
    union { uint32_t u; float f; } v;
    if (on < 0)
        on = getenv("RECOMP_C57_TRACE") != NULL;
    if (!on || n > 4000)
        return;
    n++;
    v.u = word;
    fprintf(stderr, "[C57] %s .%c = %g\n", src, "xyzw"[comp & 3], v.f);
}

void nv2a_vsh_constant_word(uint32_t word)
{
    if (s_const_load == 57 && memcmp(&s_const[57][s_const_word], &word, 4))
        c57_trace("upload", s_const_word, word);
    if (s_const_load < NV2A_VSH_CONSTANTS)
        memcpy(&s_const[s_const_load][s_const_word], &word, 4);
    if (++s_const_word == 4) {
        s_const_word = 0;
        s_const_load++;
    }
}

void nv2a_vsh_set_constant(uint32_t index, const float v[4])
{
    if (index < NV2A_VSH_CONSTANTS)
        memcpy(s_const[index], v, sizeof s_const[index]);
}

void nv2a_vsh_set_instruction(uint32_t slot, const uint32_t words[4])
{
    if (slot < NV2A_VSH_SLOTS) {
        memcpy(s_program[slot], words, sizeof s_program[slot]);
        s_dirty = 1;
    }
}

/* ---- disassembly (RECOMP_VSH_DUMP) --------------------------------------- */

static const char *const k_mac[16] = {
    "nop", "mov", "mul", "add", "mad", "dp3", "dph", "dp4",
    "dst", "min", "max", "slt", "sge", "arl", "mac14", "mac15"
};
static const char *const k_ilu[8] = {
    "nop", "mov", "rcp", "rcc", "rsq", "exp", "log", "lit"
};

static void dis_src(char *b, size_t n, const uint32_t *ins, int which)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;

    if (which == 0) {
        neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        mux = field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        mux = field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
    } else {
        neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        mux = field(ins, 3, 28, 2);
        reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }
    if (mux == 1)
        snprintf(b, n, "%sR%u", neg ? "-" : "", reg);
    else if (mux == 2)
        snprintf(b, n, "%sv%u", neg ? "-" : "", field(ins, 1, 9, 4));
    else
        snprintf(b, n, "%sc[%s%u]", neg ? "-" : "",
                 field(ins, 3, 1, 1) ? "a0.x+" : "", field(ins, 1, 13, 8));
    {
        size_t l = strlen(b);
        if (l + 6 < n)
            snprintf(b + l, n - l, ".%c%c%c%c", sw[(swz >> 6) & 3],
                     sw[(swz >> 4) & 3], sw[(swz >> 2) & 3], sw[swz & 3]);
    }
}

static void dump_program(uint32_t start)
{
    uint32_t s;
    fprintf(stderr, "  [VSH] program at slot %u:\n", start);
    for (s = start; s < NV2A_VSH_SLOTS; s++) {
        const uint32_t *ins = s_program[s];
        char a[40], bb[40], c[40];
        dis_src(a, sizeof a, ins, 0);
        dis_src(bb, sizeof bb, ins, 1);
        dis_src(c, sizeof c, ins, 2);
        fprintf(stderr, "  [VSH] %3u %-4s %-4s A=%s B=%s C=%s  R%u mac%X ilu%X"
                " out%s%u m%X%s%s\n", s,
                k_mac[field(ins, 1, 21, 4)], k_ilu[field(ins, 1, 25, 3)],
                a, bb, c, field(ins, 3, 20, 4), field(ins, 3, 24, 4),
                field(ins, 3, 16, 4), field(ins, 3, 11, 1) ? "o" : "c",
                field(ins, 3, 3, 8), field(ins, 3, 12, 4),
                field(ins, 3, 2, 1) ? " ilu" : " mac",
                field(ins, 3, 0, 1) ? " FINAL" : "");
        if (field(ins, 3, 0, 1))
            break;
    }
    fflush(stderr);
}

/* The running program to a file, raw words first (for traces). */
void nv2a_vsh_dump_to(FILE *f)
{
    uint32_t s;
    for (s = s_start_slot; s < NV2A_VSH_SLOTS; s++) {
        const uint32_t *ins = s_program[s];
        char a[40], bb[40], c[40];
        dis_src(a, sizeof a, ins, 0);
        dis_src(bb, sizeof bb, ins, 1);
        dis_src(c, sizeof c, ins, 2);
        fprintf(f, "  [VSH] %3u %08X %08X %08X %-4s %-4s A=%s B=%s C=%s  R%u mac%X ilu%X"
                " out%s%u m%X%s%s\n", s, ins[1], ins[2], ins[3],
                k_mac[field(ins, 1, 21, 4)], k_ilu[field(ins, 1, 25, 3)],
                a, bb, c, field(ins, 3, 20, 4), field(ins, 3, 24, 4),
                field(ins, 3, 16, 4), field(ins, 3, 11, 1) ? "o" : "c",
                field(ins, 3, 3, 8), field(ins, 3, 12, 4),
                field(ins, 3, 2, 1) ? " ilu" : " mac",
                field(ins, 3, 0, 1) ? " FINAL" : "");
        if (field(ins, 3, 0, 1))
            break;
    }
}

const float *nv2a_vsh_constant(uint32_t index)
{
    return s_const[index < NV2A_VSH_CONSTANTS ? index : 0];
}

void nv2a_vsh_constant_component(uint32_t index, uint32_t comp, uint32_t word)
{
    if (index == 57 && comp < 4 && memcmp(&s_const[57][comp], &word, 4))
        c57_trace("method", comp, word);
    if (index < NV2A_VSH_CONSTANTS && comp < 4)
        memcpy(&s_const[index][comp], &word, 4);
}

/* ---- execution ----------------------------------------------------------- */

typedef struct { float v[4]; } vec4;

static void write_masked(float *dst, const vec4 *src, uint32_t mask)
{
    if (mask & 8) dst[0] = src->v[0];
    if (mask & 4) dst[1] = src->v[1];
    if (mask & 2) dst[2] = src->v[2];
    if (mask & 1) dst[3] = src->v[3];
}

static vec4 splat(float f)
{
    vec4 r;
    r.v[0] = r.v[1] = r.v[2] = r.v[3] = f;
    return r;
}

static vec4 run_mac(uint32_t op, vec4 a, vec4 b, vec4 c, int *a0)
{
    vec4 r = a;
    int i;

    switch (op) {
    case 1: break;                                            /* mov */
    case 2: for (i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i]; break;
    case 3: for (i = 0; i < 4; i++) r.v[i] = a.v[i] + c.v[i]; break;
    case 4: for (i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i] + c.v[i]; break;
    case 5: r = splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]); break;
    case 6: r = splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]
                      + b.v[3]); break;                        /* dph */
    case 7: r = splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]
                      + a.v[3]*b.v[3]); break;
    case 8: r.v[0] = 1.0f; r.v[1] = a.v[1] * b.v[1];            /* dst */
            r.v[2] = a.v[2]; r.v[3] = b.v[3]; break;
    case 9: for (i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; break;
    case 10: for (i = 0; i < 4; i++) r.v[i] = a.v[i] >= b.v[i] ? a.v[i] : b.v[i]; break;
    case 11: for (i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i] ? 1.0f : 0.0f; break;
    case 12: for (i = 0; i < 4; i++) r.v[i] = a.v[i] >= b.v[i] ? 1.0f : 0.0f; break;
    case 13: *a0 = (int)floorf(a.v[0] + 0.001f); break;         /* arl */
    default: break;
    }
    return r;
}

static vec4 run_ilu(uint32_t op, vec4 c)
{
    float x = c.v[0];
    vec4 r = c;

    switch (op) {
    case 1: break;                                            /* mov */
    case 2: r = splat(x == 0.0f ? INFINITY : 1.0f / x); break;  /* rcp */
    case 3: {                                                  /* rcc */
        float f = 1.0f / x;
        if (fabsf(f) < 5.42101e-20f) f = f < 0 ? -5.42101e-20f : 5.42101e-20f;
        if (fabsf(f) > 1.884467e19f) f = f < 0 ? -1.884467e19f : 1.884467e19f;
        r = splat(f);
        break;
    }
    case 4: r = splat(1.0f / sqrtf(fabsf(x))); break;           /* rsq */
    case 5: {                                                  /* exp */
        float fl = floorf(x);
        r.v[0] = exp2f(fl); r.v[1] = x - fl; r.v[2] = exp2f(x); r.v[3] = 1.0f;
        break;
    }
    case 6: {                                                  /* log */
        float ax = fabsf(x);
        if (ax == 0.0f) {
            r.v[0] = r.v[2] = -INFINITY; r.v[1] = 1.0f;
        } else {
            float e = floorf(log2f(ax));
            r.v[0] = e; r.v[1] = ax / exp2f(e); r.v[2] = log2f(ax);
        }
        r.v[3] = 1.0f;
        break;
    }
    case 7: {                                                  /* lit */
        float nl = c.v[0] > 0.0f ? c.v[0] : 0.0f;
        float nh = c.v[1] > 0.0f ? c.v[1] : 0.0f;
        float p = c.v[3] < -127.9961f ? -127.9961f
                : (c.v[3] > 127.9961f ? 127.9961f : c.v[3]);
        r.v[0] = 1.0f; r.v[1] = nl;
        r.v[2] = c.v[0] > 0.0f ? powf(nh, p) : 0.0f; r.v[3] = 1.0f;
        break;
    }
    default: break;
    }
    return r;
}

/* Content hash of the program that runs now (start slot to FINAL), and a dump
 * of it -- for attributing bad output to a program. */
uint32_t nv2a_vsh_program_hash(void)
{
    uint32_t h = 2166136261u, t;
    int i;
    for (t = s_start_slot; t < NV2A_VSH_SLOTS; t++) {
        for (i = 0; i < 4; i++)
            h = (h ^ s_program[t][i]) * 16777619u;
        if (field(s_program[t], 3, 0, 1))
            break;
    }
    return h;
}

void nv2a_vsh_dump_current(void)
{
    dump_program(s_start_slot);
}

/* Decoded program: the fields of every slot, extracted once per upload
 * rather than per vertex and instruction. */
typedef struct {
    uint8_t mux, reg, in, neg, rel, swz[4];
    uint16_t ci;
} DecSrc;
typedef struct {
    DecSrc src[3];
    uint8_t mac, ilu, mac_mask, ilu_mask, tdst, omask, oaddr;
    uint8_t from_ilu, to_out, final;
} DecIns;
static DecIns s_dec[NV2A_VSH_SLOTS];
static int s_exp_valid;           /* nv2a_vsh_export's copy: 0 stale, 1 ok, -1 no FINAL */

/* Decode the program memory if it changed. Called before vertices are run on
 * several threads, so they only ever read s_dec. */
void nv2a_vsh_prepare(void)
{
    uint32_t t;
    int w, k;
    if (!s_dirty)
        return;
    for (t = 0; t < NV2A_VSH_SLOTS; t++) {
        const uint32_t *ins = s_program[t];
        DecIns *d = &s_dec[t];
        for (w = 0; w < 3; w++) {
            DecSrc *x = &d->src[w];
            uint32_t neg, swz, mux, reg;
            if (w == 0) {
                neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
                mux = field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
            } else if (w == 1) {
                neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
                mux = field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
            } else {
                neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
                mux = field(ins, 3, 28, 2);
                reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
            }
            x->mux = (uint8_t)mux;
            x->reg = (uint8_t)(reg <= 12 ? reg : 0);  /* 12 = oPos, see run */
            x->in = (uint8_t)field(ins, 1, 9, 4);
            x->neg = (uint8_t)neg;
            x->ci = (uint16_t)field(ins, 1, 13, 8);
            x->rel = (uint8_t)field(ins, 3, 1, 1);
            for (k = 0; k < 4; k++)
                x->swz[k] = (uint8_t)((swz >> (6 - 2 * k)) & 3);
        }
        d->mac = (uint8_t)field(ins, 1, 21, 4);
        d->ilu = (uint8_t)field(ins, 1, 25, 3);
        d->mac_mask = (uint8_t)field(ins, 3, 24, 4);
        d->ilu_mask = (uint8_t)field(ins, 3, 16, 4);
        d->tdst = (uint8_t)field(ins, 3, 20, 4);
        d->omask = (uint8_t)field(ins, 3, 12, 4);
        d->oaddr = (uint8_t)field(ins, 3, 3, 8);
        d->from_ilu = (uint8_t)field(ins, 3, 2, 1);
        d->to_out = (uint8_t)field(ins, 3, 11, 1);
        d->final = (uint8_t)field(ins, 3, 0, 1);
    }
    s_dirty = 0;
    s_exp_valid = 0;
}

/* nv2a_vsh_export: rebuilt when the decoded program or start slot changes. */
static uint32_t s_exp[NV2A_VSH_SLOTS][8];
static Nv2aVshProgram s_exp_p;
static uint32_t s_exp_start = 0xFFFFFFFFu;

int nv2a_vsh_export(Nv2aVshProgram *p)
{
    uint32_t t, n = 0, k;
    int w, valid = 0;
    nv2a_vsh_prepare();             /* clears s_dirty: rebuilds below if it did */
    if (!s_exp_valid || s_exp_start != s_start_slot) {
        uint32_t h = 2166136261u;
        memset(&s_exp_p, 0, sizeof s_exp_p);
        memset(s_exp_p.in_slot, 0xFF, sizeof s_exp_p.in_slot);
        for (t = s_start_slot; t < NV2A_VSH_SLOTS; t++) {
            const DecIns *d = &s_dec[t];
            uint32_t *o = s_exp[n++];
            o[0] = d->mac | (uint32_t)d->ilu << 4 | (uint32_t)d->mac_mask << 8
                 | (uint32_t)d->ilu_mask << 12 | (uint32_t)d->tdst << 16
                 | (uint32_t)d->omask << 20 | (uint32_t)d->from_ilu << 24
                 | (uint32_t)d->to_out << 25 | (uint32_t)d->final << 26;
            o[1] = d->oaddr;
            for (w = 0; w < 3; w++) {
                const DecSrc *x = &d->src[w];
                uint32_t slot = 0;
                if (x->mux == 2) {
                    if (s_exp_p.in_slot[x->in] == 0xFF)
                        s_exp_p.in_slot[x->in] = (uint8_t)s_exp_p.nin++;
                    slot = s_exp_p.in_slot[x->in];
                }
                o[2 + w] = x->mux | (uint32_t)x->reg << 2 | slot << 6 | (uint32_t)x->neg << 10
                         | (uint32_t)x->rel << 11 | (uint32_t)x->swz[0] << 12
                         | (uint32_t)x->swz[1] << 14 | (uint32_t)x->swz[2] << 16
                         | (uint32_t)x->swz[3] << 18 | (uint32_t)x->ci << 20;
            }
            o[5] = o[6] = o[7] = 0;
            for (k = 0; k < 5; k++)
                h = (h ^ o[k]) * 16777619u;
            if (d->final) {
                valid = 1;
                break;
            }
        }
        s_exp_p.nins = n;
        s_exp_p.ins = (const uint32_t (*)[8])s_exp;
        s_exp_p.hash = h;
        s_exp_valid = valid ? 1 : -1;
        s_exp_start = s_start_slot;
    }
    s_exp_p.consts = (const float (*)[4])s_const;
    *p = s_exp_p;
    return s_exp_valid == 1;
}

static inline vec4 dec_src(const DecSrc *x, const float in[NV2A_VSH_INPUTS][4],
                           const vec4 *reg, int a0)
{
    const float *s;
    vec4 r;
    if (x->mux == 1)
        s = reg[x->reg].v;
    else if (x->mux == 2)
        s = in[x->in];
    else {
        int ci = x->ci + (x->rel ? a0 : 0);
        if (ci < 0 || ci >= NV2A_VSH_CONSTANTS)
            ci = 0;
        s = s_const[ci];
    }
    r.v[0] = s[x->swz[0]]; r.v[1] = s[x->swz[1]];
    r.v[2] = s[x->swz[2]]; r.v[3] = s[x->swz[3]];
    if (x->neg) {
        r.v[0] = -r.v[0]; r.v[1] = -r.v[1];
        r.v[2] = -r.v[2]; r.v[3] = -r.v[3];
    }
    return r;
}

int nv2a_vsh_run(const float in[NV2A_VSH_INPUTS][4], Nv2aVshOutput *out)
{
    static int dump = -1;
    static uint32_t dumped[128];
    static int ndumped;
    vec4 reg[13];                  /* R0..R11, and R12 = oPos */
    float outregs[13][4];
    int a0 = 0;
    uint32_t s;

    if (dump < 0) {
        /* RECOMP_VSH_DUMP=N: the first N distinct programs (1 or any
         * non-number: 16). */
        const char *e = getenv("RECOMP_VSH_DUMP");
        dump = e ? (atoi(e) > 1 ? atoi(e) : 16) : 0;
    }
    if (dump && ndumped < dump && ndumped < (int)(sizeof dumped / sizeof dumped[0])) {
        /* By content: titles reload different programs into the same slot. */
        uint32_t h = 2166136261u, t;
        int i, seen = 0;
        for (t = s_start_slot; t < NV2A_VSH_SLOTS; t++) {
            for (i = 0; i < 4; i++)
                h = (h ^ s_program[t][i]) * 16777619u;
            if (field(s_program[t], 3, 0, 1))
                break;
        }
        for (i = 0; i < ndumped; i++)
            if (dumped[i] == h) seen = 1;
        if (!seen) {
            dumped[ndumped++] = h;
            dump_program(s_start_slot);
        }
    }

    nv2a_vsh_prepare();
    memset(reg, 0, sizeof reg);
    memset(outregs, 0, sizeof outregs);
    outregs[3][3] = outregs[4][3] = 1.0f;      /* colours default opaque */
    /* Unwritten texture coordinates are (0,0,0,1), as xemu initialises them:
     * a projective stage divides by q, and q = 0 would put every texel of an
     * unwritten stage at infinity. */
    outregs[9][3] = outregs[10][3] = outregs[11][3] = outregs[12][3] = 1.0f;

    for (s = s_start_slot; s < NV2A_VSH_SLOTS; s++) {
        const DecIns *d = &s_dec[s];
        vec4 a = dec_src(&d->src[0], in, reg, a0);
        vec4 b = dec_src(&d->src[1], in, reg, a0);
        vec4 c = dec_src(&d->src[2], in, reg, a0);
        vec4 mres = {{0, 0, 0, 0}}, ires = {{0, 0, 0, 0}};

        if (d->mac)
            mres = run_mac(d->mac, a, b, c, &a0);
        if (d->ilu)
            ires = run_ilu(d->ilu, c);

        if (d->mac && d->mac != 13 && d->mac_mask && d->tdst <= 12)
            write_masked(reg[d->tdst].v, &mres, d->mac_mask);
        if (d->ilu && d->ilu_mask) {
            /* Paired with any MAC op, the ILU can only write R1 -- even when
             * the MAC half writes no temp at all. The D3D epilogue is exactly
             * that: `mul o[0].xyz` beside `rcc R1.x`, then `mad` with R1.x.
             * Sending the rcc to the temp field (R7 there) left R1.x at 0 and
             * put every vertex of every 3D batch at c[59]. */
            uint32_t it = d->mac ? 1u : d->tdst;
            if (it <= 12)
                write_masked(reg[it].v, &ires, d->ilu_mask);
        }
        if (d->omask) {
            const vec4 *src = d->from_ilu ? &ires : &mres;
            if (!d->to_out) {                             /* to c[] */
                if (s_cxt_write && d->oaddr < NV2A_VSH_CONSTANTS)
                    write_masked(s_const[d->oaddr], src, d->omask);
            } else if (d->oaddr == 0) {
                write_masked(reg[12].v, src, d->omask);
            } else if (d->oaddr < 13) {
                write_masked(outregs[d->oaddr], src, d->omask);
            }
        }
        if (d->final) {
            memcpy(out->pos, reg[12].v, sizeof out->pos);
            /* NV2A clamps the colour outputs to 0..1 before they are
             * interpolated (as xemu does); lit riders exceed 1 a lot. */
            for (int k = 0; k < 4; k++) {
                float c0 = outregs[3][k], c1 = outregs[4][k];
                out->d0[k] = c0 > 1.0f ? 1.0f : (c0 >= 0.0f ? c0 : 0.0f);
                out->d1[k] = c1 > 1.0f ? 1.0f : (c1 >= 0.0f ? c1 : 0.0f);
            }
            memcpy(out->fog, outregs[5], sizeof out->fog);
            memcpy(out->tex[0], outregs[9], sizeof out->tex[0]);
            memcpy(out->tex[1], outregs[10], sizeof out->tex[1]);
            memcpy(out->tex[2], outregs[11], sizeof out->tex[2]);
            memcpy(out->tex[3], outregs[12], sizeof out->tex[3]);
            return 1;
        }
    }
    return 0;
}
