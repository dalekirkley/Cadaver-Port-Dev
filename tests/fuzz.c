/* Random-input fuzz: plays 20000 ticks of random joystick input with random room warps.
   Build with ASAN/UBSAN (see run.sh). Usage: fuzz <levelN.bin> <seed> */
#include "../src/game.c"
int main(int c, char **v) {
    if (c < 3) { fprintf(stderr, "usage: fuzz level.bin seed\n"); return 2; }
    Game *g = game_load(v[1]); if (!g) { fprintf(stderr, "cannot load %s\n", v[1]); return 2; }
    static uint32_t fb[64000]; unsigned s = (unsigned)atoi(v[2]); int joy = 0;
    for (int t = 0; t < 20000; t++) {
        s = s * 1103515245u + 12345u; if ((s >> 16) % 7 == 0) joy = (s >> 8) & 0x371;
        game_tick(g, joy); if (t % 50 == 0) game_render(g, fb);
        if (g->want_level) g->want_level = 0;
        if ((s >> 20) % 997 == 0) { int r = (s >> 4) % 100; if (g->rooms[r].valid) game_warp(g, r, 30, 30, 0); }
    }
    printf("ok room %d energy %d\n", g->cur, g->energy); return 0;
}
