/* Cadaver native - platform independent game core (no OS calls). */
#ifndef CADAVER_GAME_H
#define CADAVER_GAME_H
#include <stdint.h>

#define SCREEN_W 320
#define SCREEN_H 200
#define DISPLAY_H 240          /* the picture is shown 4:3 (320x240 shape): Amiga pixels were taller than wide */
#define TICK_HZ 25              /* original logic rate */

/* joystick bits as in the original ($8c3): */
#define JOY_UP 1
#define JOY_DOWN 2
#define JOY_LEFT 4
#define JOY_RIGHT 8
#define JOY_DEBUG 0x80                     /* F12: debug overlay */
#define JOY_MAP 0x40                       /* map key (F1): toggles the visited-rooms panel */
#define JOY_FIRE 0x10
#define JOY_INV 0x20                       /* Space: single item view of the rucksack */
#define JOY_LIST 0x100                     /* Return: whole rucksack grid */
#define JOY_MENU 0x400                     /* Esc: in-game options menu */
#define JOY_HOLD 0x200                     /* H: pick up / put down the selected hand item */

typedef struct Game Game;
Game *game_load(const char *path);          /* returns NULL on error */
void game_free(Game *g);
void game_tick(Game *g, int joy);            /* one 25 Hz logic step */
void game_render(const Game *g, uint32_t *rgba_out); /* 320x200, 0xAABBGGRR little endian = R,G,B,A bytes */
/* Mix mono signed 16-bit audio at `rate` Hz (call from the audio thread / pump). */
void game_audio_mix(Game *g, int16_t *out, int n, int rate);
void game_play_sound(Game *g, int snd);   /* test: trigger a sound id */
int game_room(const Game *g);
void game_warp(Game *g, int room, int x, int y, int z);
int game_debug_text(const Game *g, char *buf, int n);
void game_change_energy(Game *g, int delta);
void game_cycle_room(Game *g, int dir);   /* debug: PageUp/PageDown */
int game_level_request(Game *g);   /* returns and clears a requested level number (0 = none) */
void game_carry(Game *dst, const Game *src);   /* copy the hero's character to the game of the next level */
int game_menu_request(Game *g);   /* returns and clears the menu's pending window actions: 1 quit, 2 toggle CRT filter, 4 toggle fullscreen */
void game_menu_status(Game *g, int crt, int fullscreen);   /* the window tells the menu the current CRT / fullscreen state */
void game_hero(const Game *g, int *x, int *y, int *z);
#endif
