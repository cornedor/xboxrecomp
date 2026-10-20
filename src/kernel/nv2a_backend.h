/**
 * Pluggable GPU backend for the pushbuffer executor.
 *
 * The executor parses the pushbuffer, tracks state and runs vertex programs.
 * With a backend registered it hands the rasterising to it instead of
 * drawing pixels itself. The backend is optional: with none set, nothing
 * here is used.
 */
#ifndef XBOXRECOMP_NV2A_BACKEND_H
#define XBOXRECOMP_NV2A_BACKEND_H

#include <stdint.h>
#include <stdio.h>
#include "nv2a_vsh_interp.h"
#include "nv2a_combiner.h"

typedef struct {
    uint32_t color_offset;          /* guest address of the colour surface */
    uint32_t pitch;
    uint32_t x, y, width, height;   /* clip rectangle */
    uint32_t bpp;                   /* 2 or 4 */
    int      swizzled;              /* render target stored Morton-swizzled */
    uint32_t zeta_offset;
    uint32_t zmax;                  /* 0xFFFF (Z16) or 0xFFFFFF (Z24S8) */
    int      depth_test, depth_write;
    uint32_t depth_func;            /* GL enum, as the NV2A takes it */
    int      w_buffer;              /* CONTROL0 Z_PERSPECTIVE: depth is w, not z */
    /* Stencil (Z24S8 only): STENCIL_TEST_ENABLE and 0x0360..0x0378 -- write
     * mask, func (GL enum), ref, read mask, op fail / zfail / zpass (GL
     * enums: KEEP 0x1E00, ZERO 0, REPLACE 0x1E01, INCRSAT 0x1E02, DECRSAT
     * 0x1E03, INVERT 0x150A, INCR 0x8507, DECR 0x8508); stencil_clear is
     * the ZSTENCIL_CLEAR_VALUE's stencil byte. */
    int      stencil_test;
    uint32_t stencil_write_mask, stencil_func, stencil_ref, stencil_read_mask;
    uint32_t stencil_fail, stencil_zfail, stencil_zpass, stencil_clear;
    /* Per-pixel state (GL enums as the NV2A takes them). */
    int      blend_enable;
    uint32_t blend_sfactor, blend_dfactor, blend_equation, blend_color; /* ARGB */
    uint32_t color_mask;            /* A<<24 R<<16 G<<8 B, non-zero byte = write */
    int      alpha_test;
    uint32_t alpha_func, alpha_ref;
    Nv2aCombiner rc;
    int      rc_seen;               /* combiners programmed by the title */
    uint32_t fog_enable, fog_mode, fog_color;   /* fog_color: FOG_COLOR as written (R in bits 0-7) */
    float    fog_param[2];
    /* Points: size in pixels (SET_POINT_SIZE / 8, 1 if unset), used unless
     * point_params (then the program's oPts.x); point_sprite = stage 3
     * coordinates come from the position inside the point. */
    float    point_size;
    int      point_params, point_sprite;
} Nv2aTarget;

typedef struct {
    int      valid;
    uint32_t offset, width, height;
    uint32_t addr_u, addr_v;        /* 1 wrap 2 mirror 3 clamp 4 border 5 clamp-to-edge */
    int      linear_filter;         /* magnification */
    int      normalised;            /* swizzled/DXT: coords normalised; else texels */
    uint32_t format;                /* NV097 colour format code */
    uint32_t stage_program;         /* SET_SHADER_STAGE_PROGRAM, 5 bits per stage */
    uint64_t hash;                  /* of the base level's bytes (this frame) */
    int      cube;                  /* cube map: decoded as six faces stacked, +X -X +Y -Y +Z -Z */
} Nv2aTexInfo;

/* Texture stage description, and mip 0 decoded to A8R8G8B8 (width*height
 * dwords; a cube map width*height*6, the faces one below the other). Both
 * return 0 if the stage is unusable. */
int nv2a_exec_tex_info(int stage, Nv2aTexInfo *info);
int nv2a_exec_tex_decode(int stage, uint32_t *out);

/* Trace aid: the current batch's program, constants and vertex inputs. */
void nv2a_exec_dump_batch(FILE *f, uint32_t max_vertices);

#define NV2A_CLEAR_Z      0x01
#define NV2A_CLEAR_STENCIL 0x02
#define NV2A_CLEAR_COLOR  0xF0

typedef struct {
    /* One draw call after the vertex program: n vertices, assembled
     * according to prim (NV097 primitive code). Return 1 if consumed. */
    int  (*draw)(const Nv2aTarget *t, uint32_t prim, const Nv2aVshOutput *v, uint32_t n);
    /* CLEAR_SURFACE; argb and depth are already decoded (depth 0..1). */
    /* Return 1 if consumed (else the executor clears guest memory itself). */
    int  (*clear)(const Nv2aTarget *t, uint32_t flags, uint32_t argb, float depth);
    /* A finished frame is being presented from t: the backend must have
     * written it into guest memory (A8R8G8B8 / R5G6B5 at color_offset). */
    void (*flip)(const Nv2aTarget *t);
    /* Optional: the same draw with the vertex program left to the backend
     * (run on the GPU). in: n vertices of p->nin float4 inputs each (packed
     * by p->in_slot), in draw order. Return 1 if consumed; 0 and the
     * executor runs the program itself and calls draw. Not offered when the
     * program writes constants (vertex order matters then). */
    int  (*draw_vsh)(const Nv2aTarget *t, uint32_t prim, const float *in, uint32_t n,
                     const Nv2aVshProgram *p);
} Nv2aBackend;

void nv2a_exec_set_backend(const Nv2aBackend *b);

#endif
