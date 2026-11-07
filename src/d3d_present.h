#ifndef D3D_PRESENT_H
#define D3D_PRESENT_H
#include <windows.h>
#include <stdint.h>
/* Direct3D 11 presenter: uploads the 320x200 framebuffer (R in the low byte) and draws it letterboxed with either sharp-bilinear
 * scaling (mode 0) or the CRT shader (mode 1). d3d_init returns 0 when Direct3D is unavailable; the caller then keeps using GDI. */
int d3d_init(HWND hw, int srcw, int srch);
void d3d_resize(int w, int h);
void d3d_present(const uint32_t *fb, int mode);
void d3d_shutdown(void);
#endif
