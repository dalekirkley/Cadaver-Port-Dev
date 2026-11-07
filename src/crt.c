/* CRT look in software; see crt.h. Everything is in linear light. */
#include "crt.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static float lin_lut[256];
static int lut_ready;
static float *lin, *bloom; static int cap_w, cap_h;

static void prep(int sw, int sh) {
    if (!lut_ready) { for (int i = 0; i < 256; i++) lin_lut[i] = powf(i / 255.0f, 2.4f); lut_ready = 1; }
    if (cap_w != sw || cap_h != sh) { free(lin); free(bloom); lin = malloc((size_t)sw * sh * 3 * sizeof(float)); bloom = malloc((size_t)sw * sh * 3 * sizeof(float)); cap_w = sw; cap_h = sh; }
}
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float outc(float v) { if (v <= 0) return 0; if (v >= 1) return 255; return 255.0f * powf(v, 1.0f / 2.2f); }

void crt_render(uint32_t *dst, int dw, int dh, const uint32_t *src, int sw, int sh) {
    if (dw < 8 || dh < 8) return;
    prep(sw, sh);
    for (int i = 0; i < sw * sh; i++) { uint32_t v = src[i]; lin[i * 3] = lin_lut[v & 255]; lin[i * 3 + 1] = lin_lut[(v >> 8) & 255]; lin[i * 3 + 2] = lin_lut[(v >> 16) & 255]; }
    /* halation: two separable box blurs (radius 2) of the linear image */
    static float *tmp; static int tmp_n; if (tmp_n < sw * sh * 3) { free(tmp); tmp = malloc((size_t)sw * sh * 3 * sizeof(float)); tmp_n = sw * sh * 3; }
    for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) for (int c = 0; c < 3; c++) {
        float a = 0; for (int k = -3; k <= 3; k++) a += lin[(y * sw + clampi(x + k, 0, sw - 1)) * 3 + c]; tmp[(y * sw + x) * 3 + c] = a / 7.0f; }
    for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) for (int c = 0; c < 3; c++) {
        float a = 0; for (int k = -3; k <= 3; k++) a += tmp[(clampi(y + k, 0, sh - 1) * sw + x) * 3 + c]; bloom[(y * sw + x) * 3 + c] = a / 7.0f; }

        for (int py = 0; py < dh; py++) {
        float v = (py + 0.5f) / dh * 2.0f - 1.0f;
        for (int px = 0; px < dw; px++) {
            float u = (px + 0.5f) / dw * 2.0f - 1.0f;
            float cu = u, cv = v;                                                        /* flat screen: no curvature */
            float sx = (cu * 0.5f + 0.5f) * sw - 0.5f, sy = (cv * 0.5f + 0.5f) * sh - 0.5f;
            int iy = clampi((int)floorf(sy + 0.5f), 0, sh - 1); float dy = sy - iy;           /* distance from the scanline centre, in source rows */
            int ix = (int)floorf(sx); float fx = sx - ix; int x0 = clampi(ix, 0, sw - 1), x1 = clampi(ix + 1, 0, sw - 1);
            float rgb[3];
            for (int c = 0; c < 3; c++) {
                float a = lin[(iy * sw + x0) * 3 + c] * (1 - fx) + lin[(iy * sw + x1) * 3 + c] * fx;
                float b = bloom[(iy * sw + x0) * 3 + c] * (1 - fx) + bloom[(iy * sw + x1) * 3 + c] * fx;
                float s = 0.26f + 0.17f * a;                                                    /* beam widens with brightness */
                float beam = expf(-(dy * dy) / (2.0f * s * s)) * 1.18f;
                rgb[c] = a * beam + 0.22f * b;
            }
            /* RGB aperture grille: one phosphor stripe per channel every 3 output pixels, soft edges */
            int ph = px % 3; static const float M[3][3] = {{1.0f, 0.62f, 0.62f}, {0.62f, 1.0f, 0.62f}, {0.62f, 0.62f, 1.0f}};
            for (int c = 0; c < 3; c++) rgb[c] *= M[ph][c] * 1.28f;
            uint32_t R = (uint32_t)outc(rgb[0]), G = (uint32_t)outc(rgb[1]), B = (uint32_t)outc(rgb[2]);
            dst[py * dw + px] = R << 16 | G << 8 | B;
        }
    }
}
