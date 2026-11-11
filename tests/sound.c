/* Render one sound id through the port's mixer to a mono WAV: sound <level.bin> <id> <seconds> <out.wav> (compared against the emulator's own audio by tests/sound_cmp.py) */
#include "../src/game.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
void game_play_sound_test(Game *g, int id);
int main(int argc, char **argv) {
    if (argc < 5) return 1;
    Game *g = game_load(argv[1]); if (!g) return 2;
    int id = atoi(argv[2]), n = (int)(atof(argv[3]) * 44100);
    game_play_sound_test(g, id);
    int16_t *buf = malloc(n * 2); for (int o = 0; o < n; o += 441) game_audio_mix(g, buf + o, n - o < 441 ? n - o : 441, 44100);
    FILE *f = fopen(argv[4], "wb"); fwrite(buf, 2, n, f); fclose(f);     /* raw s16le mono */
    return 0;
}
