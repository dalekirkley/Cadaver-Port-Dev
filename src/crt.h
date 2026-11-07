#ifndef CRT_H
#define CRT_H
#include <stdint.h>
/* Software CRT filter (a CPU approximation of the crt-royale look: curved screen, gaussian scanline beams that widen with
 * brightness, RGB aperture-grille mask, halation glow, gamma-correct blending). src = SCREEN_W*SCREEN_H pixels (R in the low byte),
 * dst = dw*dh pixels written as 0x00RRGGBB. */
void crt_render(uint32_t *dst, int dw, int dh, const uint32_t *src, int sw, int sh);
#endif
