/* Level change carries the hero's stats. Usage: carry <levelA.bin> <levelB.bin> */
#include "../src/game.c"
int main(int c, char **v) {
    if (c < 3) return 2;
    Game *a = game_load(v[1]), *b = game_load(v[2]); if (!a || !b) return 2;
    a->energy = 55; a->gold = 123; a->xp = 9; game_carry(b, a);
    if (b->energy != 55 || b->gold != 123 || b->xp != 9) { puts("FAIL carry"); return 1; }
    puts("ok carry"); return 0;
}
