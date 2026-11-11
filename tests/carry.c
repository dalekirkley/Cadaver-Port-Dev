/* Level change carries the hero's stats. Usage: carry <levelA.bin> <levelB.bin> */
#include "../src/game.c"
int main(int c, char **v) {
    if (c < 3) return 2;
    Game *a = game_load(v[1]), *b = game_load(v[2]); if (!a || !b) return 2;
    a->energy = 55; a->gold = 123; a->xp = 9; game_carry(b, a);
    if (b->energy != 55 || b->gold != 123 || b->xp != 9) { puts("FAIL carry"); return 1; }
    Game *a2 = game_load(v[1]), *b2 = game_load(v[2]);                          /* rucksack: level 1 pid 82 (runic stone) becomes 726, pid 87 becomes 812; an unlisted item (pid 1) is dropped */
    a2->ninv = 3; a2->inv[0] = 82; a2->inv[1] = 1; a2->inv[2] = 87; a2->inv_slot[0] = a2->inv_slot[1] = a2->inv_slot[2] = 0; a2->spl_chg[87] = 7;
    game_carry(b2, a2);
    if (a2->ncarry != 17 || b2->ninv != 2 || b2->inv[0] != 726 || b2->inv[1] != 812 || b2->spl_chg[812] != 7) { printf("FAIL rucksack ncarry %d ninv %d\n", a2->ncarry, b2->ninv); return 1; }
    puts("ok carry"); return 0;
}
