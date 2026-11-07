#ifndef INTRO_H
#define INTRO_H
#include <stdint.h>
typedef struct Intro Intro;
Intro *intro_load(const char *path);             /* data/intro.bin from tools/export_intro.py; NULL if missing */
void intro_free(Intro *in);
int intro_tick(Intro *in, int joy);              /* one 25 Hz tick; returns 1 when the intro is over (any key / fire skips it) */
void intro_render(const Intro *in, uint32_t *fb); /* 320x200 */
void intro_audio_mix(Intro *in, int16_t *out, int n, int rate);   /* mono title tune */
#endif
