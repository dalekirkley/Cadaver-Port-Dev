/* Title sequence: logos, then the parchment with two flickering candles and the credits text rising into place (observed from the
 * original at 50 Hz; one native tick = 2 original frames). Assets come from tools/export_intro.py. */
#include "intro.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int x, y, w, h; uint8_t *px; } Img;                       /* RGBA */
typedef struct { Img base; int n, nseq; Img *im; uint8_t *seq; } Flame;
struct Intro {
    Img logos, title, text; Flame fl[2]; int nfl;
    int clip, rest, first, rest_abs;
    int frame;                                                              /* 50 Hz frames since the start */
    int held;                                                               /* a key was still down at the start: wait for release */
};
#define LOGO_FRAMES 100                                                      /* 2 s: the original shows them while loading */
#define END_FRAMES 150                                                       /* stays on the finished title 3 s, then moves on */

static int rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }
static int rd_img(FILE *f, Img *im, int has_pos) {
    int16_t x = 0, y = 0; uint16_t w, h;
    if (has_pos) { if (!rd(f, &x, 2) || !rd(f, &y, 2)) return 0; }
    if (!rd(f, &w, 2) || !rd(f, &h, 2)) return 0;
    im->x = x; im->y = y; im->w = w; im->h = h; im->px = malloc((size_t)w * h * 4); return im->px && rd(f, im->px, (size_t)w * h * 4);
}
Intro *intro_load(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    char mg[4]; Intro *in = calloc(1, sizeof *in); int ok = in && rd(f, mg, 4) && !memcmp(mg, "INTR", 4);
    ok = ok && rd_img(f, &in->logos, 1) && rd_img(f, &in->title, 1);
    uint16_t nf = 0; ok = ok && rd(f, &nf, 2) && nf <= 2; if (ok) in->nfl = nf;
    for (int i = 0; ok && i < in->nfl; i++) {
        Flame *fl = &in->fl[i]; uint16_t x, y, w, h, n, ns;
        ok = rd(f, &x, 2) && rd(f, &y, 2) && rd(f, &w, 2) && rd(f, &h, 2) && rd(f, &n, 2);
        if (!ok) break;
        fl->base.x = x; fl->base.y = y; fl->base.w = w; fl->base.h = h; fl->n = n; fl->im = calloc(n ? n : 1, sizeof(Img));
        for (int k = 0; ok && k < n; k++) { fl->im[k].x = x; fl->im[k].y = y; fl->im[k].w = w; fl->im[k].h = h; fl->im[k].px = malloc((size_t)w * h * 4); ok = fl->im[k].px && rd(f, fl->im[k].px, (size_t)w * h * 4); }
        ok = ok && rd(f, &ns, 2); if (!ok) break;
        fl->nseq = ns; fl->seq = malloc(ns ? ns : 1); ok = fl->seq && rd(f, fl->seq, ns);
    }
    uint16_t pr[4]; ok = ok && rd_img(f, &in->text, 1) && rd(f, pr, 8);
    fclose(f);
    if (!ok) { intro_free(in); return NULL; }
    in->clip = pr[0]; in->rest = pr[1]; in->first = pr[2]; in->rest_abs = pr[3];
    return in;
}
void intro_free(Intro *in) {
    if (!in) return;
    free(in->logos.px); free(in->title.px); free(in->text.px);
    for (int i = 0; i < 2; i++) { for (int k = 0; k < in->fl[i].n; k++) free(in->fl[i].im[k].px); free(in->fl[i].im); free(in->fl[i].seq); }
    free(in);
}
int intro_tick(Intro *in, int joy) {
    if (!in) return 1;
    if (in->frame == 0) in->held = joy != 0;
    if (!joy) in->held = 0;
    in->frame += 2;
    if (joy && !in->held && in->frame > 4) return 1;                         /* any key or fire skips */
    return in->frame > LOGO_FRAMES + in->rest_abs + END_FRAMES;
}
static void put(uint32_t *fb, const Img *im, int ox, int oy, int clip_y) {
    for (int y = 0; y < im->h; y++) {
        int sy = oy + y; if (sy < 0 || sy >= SCREEN_H || sy >= clip_y) continue;
        for (int x = 0; x < im->w; x++) {
            int sx = ox + x; if (sx < 0 || sx >= SCREEN_W) continue;
            const uint8_t *p = im->px + ((size_t)y * im->w + x) * 4; if (!p[3]) continue;
            fb[sy * SCREEN_W + sx] = p[0] | p[1] << 8 | p[2] << 16 | 0xFF000000u;
        }
    }
}
void intro_render(const Intro *in, uint32_t *fb) {
    memset(fb, 0, sizeof(uint32_t) * SCREEN_W * SCREEN_H);
    if (in->frame < LOGO_FRAMES) { put(fb, &in->logos, 0, 0, SCREEN_H); return; }
    int t = in->frame - LOGO_FRAMES;                                          /* title frames, the first title frame is original frame 15 */
    put(fb, &in->title, 0, 0, SCREEN_H);
    for (int i = 0; i < in->nfl; i++) { const Flame *fl = &in->fl[i]; if (!fl->nseq) continue;
        int k = t < fl->nseq ? t : fl->nseq / 2 + (t - fl->nseq / 2) % (fl->nseq - fl->nseq / 2); int id = fl->seq[k]; if (id < fl->n) put(fb, &fl->im[id], fl->im[id].x, fl->im[id].y, SCREEN_H); }
    int off = in->rest > t ? (in->rest - t) / 2 : 0;
    put(fb, &in->text, in->text.x, in->text.y + off, in->clip);
}
