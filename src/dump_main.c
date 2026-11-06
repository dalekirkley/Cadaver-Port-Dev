/* Headless test driver: ./dump level1.bin "R45,U12,D12" out_prefix
 * Each token = direction letter (U D L R, combos like UR) + tick count. Writes <prefix>_final.ppm and prints state. */
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
static void ppm(const char *path, const uint32_t *fb) {
    FILE *f = fopen(path, "wb"); fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) { unsigned v = fb[i]; fputc(v & 255, f); fputc(v >> 8 & 255, f); fputc(v >> 16 & 255, f); }
    fclose(f);
}
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: dump level.bin script prefix\n"); return 1; }
    Game *g = game_load(argv[1]); if (!g) { fprintf(stderr, "load failed\n"); return 1; }
    { const char *e = getenv("WARP"); int a, b, c, d; if (e && sscanf(e, "%d,%d,%d,%d", &a, &b, &c, &d) == 4) game_warp(g, a, b, c, d); }   /* WARP=room,x,y,z */
    char *s = strdup(argv[2]); char *tok = strtok(s, ",");
    while (tok) {
        int joy = 0; char *p = tok;
        for (; *p && isalpha((unsigned char)*p); p++) joy |= *p == 'U' ? JOY_UP : *p == 'D' ? JOY_DOWN : *p == 'L' ? JOY_LEFT : *p == 'R' ? JOY_RIGHT : *p == 'F' ? JOY_FIRE : *p == 'M' ? JOY_MAP : 0;
        int n = atoi(p);
        for (int i = 0; i < n; i++) game_tick(g, joy);
        int x, y, z; game_hero(g, &x, &y, &z); printf("%s -> room %d hero (%d,%d,%d)\n", tok, game_room(g), x, y, z);
        tok = strtok(NULL, ",");
    }
    static uint32_t fb[SCREEN_W * SCREEN_H]; game_render(g, fb);
    char name[512]; snprintf(name, sizeof name, "%s_final.ppm", argv[3]); ppm(name, fb);
    game_free(g); return 0;
}
