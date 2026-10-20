/* The display's gamma ramp.
 *
 * Xbox D3D (SetGammaRamp; SSX 3's sub_00239B76) loads 256 R, G and B entries
 * into the NV2A's VGA palette (PRMDIO 0x6813C8 index, 0x6813C9 data, three
 * writes per entry), and the RAMDAC looks every scanned-out pixel up in it.
 * The emulated MMIO keeps no such table, so a title calls
 * xbox_FramebufferSetGammaRamp with the ramp it uploads and the presenters
 * apply it to the finished frame -- without it the picture is shown with a
 * linear ramp, lighter and flatter than on the console.
 * RECOMP_GAMMA=0 ignores the title's ramp. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t s_lut[3][256];
static volatile int s_lut_on;
static volatile unsigned s_lut_serial;     /* +1 per change, for the GPU presenter */

void xbox_FramebufferSetGammaRamp(const uint8_t *r, const uint8_t *g, const uint8_t *b)
{
    static int allowed = -1, said;
    int i, identity = 1;
    if (allowed < 0) {
        const char *e = getenv("RECOMP_GAMMA");
        allowed = !(e && *e == '0');
    }
    for (i = 0; i < 256; i++)
        if (r[i] != i || g[i] != i || b[i] != i)
            identity = 0;
    if (!allowed || identity) {
        s_lut_on = 0;
        s_lut_serial++;
        return;
    }
    memcpy(s_lut[0], r, 256);
    memcpy(s_lut[1], g, 256);
    memcpy(s_lut[2], b, 256);
    s_lut_on = 1;
    s_lut_serial++;
    if (said < 4) {
        said++;
        fprintf(stderr, "[GAMMA] ramp loaded: r 0->%u 64->%u 128->%u 192->%u 255->%u"
                " (g 128->%u, b 128->%u)\n", r[0], r[64], r[128], r[192], r[255], g[128], b[128]);
    }
}

/* Apply the ramp to n X8R8G8B8 pixels in place (no-op without one). */
void xbox_FramebufferApplyGamma(uint32_t *px, size_t n)
{
    size_t i;
    if (!s_lut_on)
        return;
    for (i = 0; i < n; i++) {
        uint32_t c = px[i];
        px[i] = (c & 0xFF000000u) | ((uint32_t)s_lut[0][(c >> 16) & 0xFF] << 16)
              | ((uint32_t)s_lut[1][(c >> 8) & 0xFF] << 8) | s_lut[2][c & 0xFF];
    }
}

/* The ramp as 256 B8G8R8A8 texels (identity without one), for a presenter
 * that applies it on the GPU; returns its change serial. */
unsigned xbox_FramebufferGammaTexels(uint32_t *out256)
{
    int i;
    for (i = 0; i < 256; i++)
        out256[i] = s_lut_on ? 0xFF000000u | (uint32_t)s_lut[0][i] << 16
                               | (uint32_t)s_lut[1][i] << 8 | s_lut[2][i]
                             : 0xFF000000u | (uint32_t)i * 0x010101u;
    return s_lut_serial;
}

unsigned xbox_FramebufferGammaSerial(void)
{
    return s_lut_serial;
}
