/* D3D11 presentation for the framebuffer window (see gpu_present.c). */
#ifndef XBOX_GPU_PRESENT_H
#define XBOX_GPU_PRESENT_H
#if defined(_WIN32)
#include <windows.h>
#include <stdint.h>

int  gpu_present_enabled(void);                 /* RECOMP_D3D_PRESENT=1 */
int  gpu_present_init(HWND hwnd, uint32_t width, uint32_t height);
int  gpu_present_frame_gamma(const uint32_t *pixels, uint32_t width, uint32_t height, int gamma); /* gamma: apply the title's ramp on the GPU */
int  gpu_present_frame(const uint32_t *pixels); /* width*height B8G8R8A8; 0 = not shown (occluded) */
int  gpu_present_frame_size(const uint32_t *pixels, uint32_t width, uint32_t height);
int  gpu_present_active(void);
void gpu_present_capture(const char *path);   /* next presented image -> BMP */
void gpu_present_shutdown(void);
#endif
#endif
