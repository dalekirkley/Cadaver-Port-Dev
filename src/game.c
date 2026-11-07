#include "game.h"
#include "sound_table.h"
#include "sound_seq.h"
#define SND_TICK_SEC 0.005      /* seconds per script tick: the original rate is not known (0.02 would be one 50 Hz frame); tune by ear */
#define SND_MAX_SEC 4.0         /* a script never runs longer than this */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MARGIN 6                 /* min-corner limit ($89b/$89c) */
#define HERO_FX 7
#define HERO_FY 7
#define HERO_FZ 30
#define ENERGY_MAX 100
#define SCROLL_TICKS 75                    /* the examine scroll stays up 3 s (150 video frames in the original), input and movement frozen */
#define MAX_PUSH_DEPTH 0x1f
#define WALK_TICKS_PER_FRAME 3   /* measured in the original: 6 video frames per walk frame */

typedef struct { int len; int8_t *data; } Sfx;
typedef struct { int sfx; double pos, step; int vol; double left, total; } Voice;
typedef struct { const SndEv *ev; int n, i, mv, snd; double t, el; } SeqVoice;   /* left = seconds before the voice is cut (0 = play to the end) */
typedef struct { int state, w, h; int16_t f[5]; int push; int16_t fx, fy, fz, name; uint8_t c, b16, b17, b1d; uint8_t *px; int aw, ah, ax, ay; uint8_t *apx; int nfr; struct { int w, h, dx, dy; uint8_t *px; } fr[16]; } Sprite;
typedef struct { int16_t hx, hy; int w, h; uint8_t *px; } Frame;
typedef struct { int x, y, z, slot, flags, pid, xflags, anim, atick, home, stage, vx, vy, life, hp, lift, mpc, mwait, mstop, mc[3], mdir, skey, macc; } Obj;   /* life > 0: a missile in flight; hp: remaining health of a creature (0 = not set yet) */
#define MISSILE_SLOT 31          /* the thrown-rock image (16x7) */
#define MISSILE_PID 9000
#define MISSILE_SPEED 3
#define CREATURE_HP 15           /* guess: three hits of the stone bag's power 5 */
#define ROCK_POWER 5       /* x,y = max corner, inclusive box */
typedef struct { int x, y, orient; int box[4]; int slot, z, wx, wy; int idx, flags, b7; } Wall;
typedef struct {
    int valid, x0, y0, sx, sy, basez, org, name, prefix;
    uint8_t *bg;                      /* 320*200*4 */
    int nwalls; Wall *walls;
    int nobj, ocap; Obj *objs;              /* live state, persists while the program runs */
} Room;

#define MAX_WALLIDX 4096
struct Game {
    uint8_t pal[16][3];
    int16_t sub[64][2];
    Sprite spr[255];
    int nframes; Frame *frames;
    Room rooms[100];
    int cur;
    Obj hero;                         /* hero.slot = 8 */
    int facing, anim_t, moving;
    uint8_t font[46 * 24], fontmap[32]; char **text; int ntext; int day; int energy; int jump_t, jump_z0;
    int has_scroll; uint8_t scroll_tiles[18 * 32], scroll_map[0x30], font5[46 * 20]; int nexam, nuse, ntake; uint16_t take_pid[512]; int16_t take_gold[512], take_xp[512]; uint8_t counter[256], gflag[256]; uint32_t gbits; int nhid; struct { int room; Obj o; } hid[128]; int hurt_cd; int want_level; int pull_pid, pull_room; int spawn_pid; int rs_period[100], rs_nb[100]; uint8_t *rs_blk[100][16]; int tm_room, tm_left; uint8_t mv_prog[512][96]; uint8_t mv_len[512], mv_step[512][3], mv_pat[512]; int lvl1; int nch; uint16_t ch_pid[64], ch_cont[64], ch_key[64]; uint8_t ch_flags[64], ch_trap[64]; int dbg, dbg_verb, dbg_pid, dbg_ran, dbg_execs; unsigned dbg_log[4][3]; unsigned char dbg_tr[48]; int dbg_ntr; int nvm; uint16_t vm_pid[256]; uint8_t vm_len[256], vm_code[256][255]; uint16_t exam_pid[512], exam_text[512], use_pid[512], use_text[512];   /* examine scroll */
    int scroll_t; char scroll_str[256]; long score; int map_open, map_px, map_py, status_open; long gold, xp, level; uint8_t visited[100];
    int has_icons; uint8_t icons[20 * 112]; uint16_t slotpos[8][4]; uint8_t verbtab[16];
    int target, nverbs, verbs[8], menu, sel, prev_joy, msg, msg_t, ninv, inv[64], inv_slot[64], inv_mode, inv_sel, inv_hold;     /* verb menu state */
    uint16_t wallflag[MAX_WALLIDX]; uint8_t walldoor[MAX_WALLIDX]; int bump_timer;    /* wall-record flags, shared by the two rooms a wall joins */
    unsigned tickno; int walk_ticks, walk_step, start_room; Obj start_hero;
    Sfx sfx[65]; Voice voice[4]; SeqVoice sq[8];
    uint8_t *hud;                      /* 320*200*4 bottom panel frame, or NULL */
};

/* ------------------------------------------------------------------ loading */
static int rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }
static int rd8(FILE *f) { uint8_t v; return rd(f, &v, 1) ? v : -1; }
static int rd16(FILE *f) { uint8_t b[2]; if (!rd(f, b, 2)) return 0; return b[0] | b[1] << 8; }
static int rds16(FILE *f) { return (int16_t)rd16(f); }
static uint32_t rd32(FILE *f) { uint8_t b[4]; if (!rd(f, b, 4)) return 0; return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

Game *game_load(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    char magic[4]; if (!rd(f, magic, 4) || memcmp(magic, "CADL", 4)) { fclose(f); return NULL; }
    rd32(f);
    Game *g = (Game *)calloc(1, sizeof *g);
    rd(f, g->pal, 48);
    for (int i = 0; i < 64; i++) { g->sub[i][0] = (int16_t)rds16(f); g->sub[i][1] = (int16_t)rds16(f); }
    uint32_t ns = rd32(f);
    for (uint32_t s = 0; s < ns && s < 255; s++) {
        Sprite *sp = &g->spr[s]; sp->state = rd8(f);
        if (!sp->state) continue;
        for (int k = 0; k < 5; k++) sp->f[k] = (int16_t)rds16(f);
        sp->push = rd8(f); sp->fx = rds16(f); sp->fy = rds16(f); sp->fz = rds16(f); sp->name = rds16(f); sp->c = rd8(f); sp->b16 = rd8(f); sp->b17 = rd8(f); sp->b1d = rd8(f);
        if (sp->state == 1) { sp->w = rd16(f); sp->h = rd16(f); sp->px = malloc((size_t)sp->w * sp->h * 4); rd(f, sp->px, (size_t)sp->w * sp->h * 4); }
    }
    g->nframes = rd32(f); g->frames = calloc(g->nframes, sizeof(Frame));
    for (int n = 0; n < g->nframes; n++) {
        Frame *fr = &g->frames[n]; fr->hx = rds16(f); fr->hy = rds16(f); fr->w = rd16(f); fr->h = rd16(f);
        fr->px = malloc((size_t)fr->w * fr->h * 4); rd(f, fr->px, (size_t)fr->w * fr->h * 4);
    }
    uint32_t nr = rd32(f);
    for (uint32_t n = 0; n < nr && n < 100; n++) {
        Room *r = &g->rooms[n]; r->valid = rd8(f); if (!r->valid) continue;
        r->x0 = rd8(f); r->y0 = rd8(f); r->sx = rd8(f); r->sy = rd8(f); r->basez = rd8(f); r->org = rd16(f);
        r->bg = malloc(SCREEN_W * SCREEN_H * 4); rd(f, r->bg, SCREEN_W * SCREEN_H * 4);
        r->nwalls = rd16(f); r->walls = calloc(r->nwalls ? r->nwalls : 1, sizeof(Wall));
        for (int k = 0; k < r->nwalls; k++) {
            Wall *w = &r->walls[k]; w->x = rds16(f); w->y = rds16(f); w->orient = rd8(f);
            for (int b = 0; b < 4; b++) w->box[b] = rds16(f);
            w->slot = rd16(f); w->z = rd8(f); w->wx = rds16(f); w->wy = rds16(f);
            w->idx = rd16(f); w->flags = rd16(f); w->b7 = rd8(f); if (w->idx < MAX_WALLIDX) { g->wallflag[w->idx] = (uint16_t)w->flags; if (w->flags) g->walldoor[w->idx] = 1; }
        }
        int no = rd16(f); r->objs = calloc((size_t)no + 32, sizeof(Obj)); r->nobj = 0; r->ocap = no + 32;
        for (int k = 0; k < no; k++) {
            Obj o; memset(&o, 0, sizeof o); o.pid = rd16(f); o.x = rd8(f); o.y = rd8(f); o.z = rd8(f); o.flags = rd8(f); o.slot = rd16(f); o.xflags = rd8(f); o.anim = 0; o.atick = 0; o.home = (int)n; o.stage = 0;
            if (o.flags & 0x80) { if (g->nhid < 128) { g->hid[g->nhid].room = (int)n; g->hid[g->nhid++].o = o; } continue; }   /* not spawned yet: a script can bring it in */
            if (o.slot == 8) { g->hero = o; g->start_hero = o; g->start_room = (int)n; continue; }       /* the player placement */
            r->objs[r->nobj++] = o;
        }
    }
    { uint32_t nsfx = rd32(f);                 /* optional: sound effect pool (absent in old data files) */
      for (uint32_t i = 0; i < nsfx && i < 65; i++) {
          g->sfx[i].len = (int)rd32(f); g->sfx[i].data = malloc(g->sfx[i].len ? g->sfx[i].len : 1);
          rd(f, g->sfx[i].data, g->sfx[i].len);
      } }
    if (rd8(f) == 1) { g->hud = malloc(SCREEN_W * SCREEN_H * 4); if (!rd(f, g->hud, SCREEN_W * SCREEN_H * 4)) { free(g->hud); g->hud = NULL; } }
    if (rd8(f) == 1) {                                                 /* optional: font, strings, room names */
        rd(f, g->font, sizeof g->font); rd(f, g->fontmap, sizeof g->fontmap);
        g->ntext = rd16(f); g->text = calloc(g->ntext ? g->ntext : 1, sizeof(char *));
        for (int i = 0; i < g->ntext; i++) { int ln = rd16(f); g->text[i] = malloc(ln + 1); rd(f, g->text[i], ln); g->text[i][ln] = 0; }
        for (int n = 0; n < 100; n++) { g->rooms[n].name = rd8(f); g->rooms[n].prefix = (int16_t)rd16(f); }
    }
    if (rd8(f) == 1) {                                                 /* optional: verb icons */
        rd(f, g->icons, sizeof g->icons);
        for (int i = 0; i < 8; i++) for (int k = 0; k < 4; k++) { int hi = rd8(f), lo = rd8(f); g->slotpos[i][k] = (uint16_t)((hi << 8) | lo); }   /* explicit order: C leaves operand order unspecified */
        rd(f, g->verbtab, sizeof g->verbtab); g->has_icons = 1;
    }
    if (rd8(f) == 1) {                                                 /* optional: scroll panel tiles, layout, small font, examine texts */
        rd(f, g->scroll_tiles, sizeof g->scroll_tiles); rd(f, g->scroll_map, sizeof g->scroll_map); rd(f, g->font5, sizeof g->font5);
        g->nexam = rd16(f); if (g->nexam > 512) g->nexam = 512;
        for (int i = 0; i < g->nexam; i++) { g->exam_pid[i] = (uint16_t)rd16(f); g->exam_text[i] = (uint16_t)rd16(f); }
        g->nuse = rd16(f); if (g->nuse < 0 || g->nuse > 512) g->nuse = 0;
        for (int i = 0; i < g->nuse; i++) { g->use_pid[i] = (uint16_t)rd16(f); g->use_text[i] = (uint16_t)rd16(f); }
        g->nvm = rd16(f); if (g->nvm < 0 || g->nvm > 256) g->nvm = 0;
        for (int i = 0; i < g->nvm; i++) { g->vm_pid[i] = (uint16_t)rd16(f); g->vm_len[i] = (uint8_t)rd8(f); rd(f, g->vm_code[i], g->vm_len[i]); }
        g->ntake = rd16(f); if (g->ntake < 0 || g->ntake > 512) g->ntake = 0;
        for (int i = 0; i < g->ntake; i++) { g->take_pid[i] = (uint16_t)rd16(f); g->take_gold[i] = (int16_t)rd16(f); g->take_xp[i] = (int16_t)rd16(f); }
        { int na = rd16(f); if (na < 0 || na > 255) na = 0;          /* second sprite image (lever pulled) */
          for (int i = 0; i < na; i++) { int sl = rd8(f); Sprite *sp = &g->spr[sl]; sp->aw = rd16(f); sp->ah = rd16(f); sp->apx = malloc((size_t)sp->aw * sp->ah * 4); rd(f, sp->apx, (size_t)sp->aw * sp->ah * 4); } }
        { int na = rd16(f); if (na < 0 || na > 255) na = 0; for (int i = 0; i < na; i++) { int sl = rd8(f); int ax = rds16(f), ay = rds16(f); g->spr[sl].ax = ax; g->spr[sl].ay = ay; } }   /* alt image offsets (optional) */
        { int nr = rd16(f); if (nr < 0 || nr > 100) nr = 0;            /* room scripts (timed events) */
          for (int i = 0; i < nr; i++) { int rn = rd8(f), per = rd8(f), nb = rd8(f); if (rn >= 100) rn = 0; g->rs_period[rn] = per; g->rs_nb[rn] = 0;
              for (int b = 0; b < nb; b++) { int ln = rd8(f); uint8_t *p = malloc(ln ? ln : 1); if (ln > 0) rd(f, p, (size_t)ln);
                  if (g->rs_nb[rn] < 16) g->rs_blk[rn][g->rs_nb[rn]++] = p; else free(p); } } }
        { int ns = rd16(f); if (ns < 0 || ns > 30) ns = 0;              /* multi-frame creature images */
          for (int i = 0; i < ns; i++) { int sl = rd8(f), nf = rd8(f); Sprite *sp = &g->spr[sl]; sp->nfr = 0;
              for (int k = 0; k < nf; k++) { int w = rd16(f), h = rd16(f), dx = rds16(f), dy = rds16(f); uint8_t *px = malloc((size_t)w * h * 4); rd(f, px, (size_t)w * h * 4);
                  if (k < 16) { sp->fr[k].w = w; sp->fr[k].h = h; sp->fr[k].dx = dx; sp->fr[k].dy = dy; sp->fr[k].px = px; sp->nfr = k + 1; } else free(px); } } }
        { int nc = rd16(f); if (nc < 0 || nc > 64) nc = 0; g->nch = nc;         /* chest data: contents, key, flags, trap */
          for (int i = 0; i < nc; i++) { g->ch_pid[i] = (uint16_t)rd16(f); g->ch_cont[i] = (uint16_t)rd16(f); g->ch_key[i] = (uint16_t)rd16(f); g->ch_flags[i] = (uint8_t)rd8(f); g->ch_trap[i] = (uint8_t)rd8(f); } }
        { int nm = rd16(f); if (nm < 0 || nm > 512) nm = 0;                    /* creature movement programs (optional, after the chests) */
          for (int i = 0; i < nm; i++) { int pid = rd16(f), s0 = rd8(f), s1 = rd8(f), s2 = rd8(f), pat = rd8(f), ln = rd8(f); uint8_t tmp[256];
              if (ln < 0 || ln > 255) ln = 0; if (ln > 0) rd(f, tmp, (size_t)ln);
              if (pid >= 0 && pid < 512 && ln > 0) { memcpy(g->mv_prog[pid], tmp, (size_t)(ln > 96 ? 96 : ln)); g->mv_len[pid] = (uint8_t)(ln > 96 ? 96 : ln); g->mv_step[pid][0] = (uint8_t)s0; g->mv_step[pid][1] = (uint8_t)s1; g->mv_step[pid][2] = (uint8_t)s2; g->mv_pat[pid] = (uint8_t)pat; } } }
        g->has_scroll = 1;
    }
    fclose(f);
    g->target = -1; g->msg = -1;
    g->cur = g->start_room; g->hero.x += 1; g->start_hero = g->hero;                                /* level start nudges the hero +1 in x */
    g->lvl1 = strstr(path, "level1") != NULL; g->facing = JOY_RIGHT; g->day = 1; g->energy = ENERGY_MAX; g->level = 1;
    { const char *q = strstr(path, "level"); if (q && q[5] >= '1' && q[5] <= '9') g->day = q[5] - '0'; }   /* the day counter is the level number (level2.bin shows DAY 2) */
    return g;
}

void game_free(Game *g) {
    if (!g) return;
    for (int s = 0; s < 255; s++) { free(g->spr[s].px); free(g->spr[s].apx); for (int k = 0; k < g->spr[s].nfr; k++) free(g->spr[s].fr[k].px); }
    for (int n = 0; n < g->nframes; n++) free(g->frames[n].px);
    for (int n = 0; n < 100; n++) { free(g->rooms[n].bg); free(g->rooms[n].walls); free(g->rooms[n].objs); }
    free(g->hud);
    for (int i = 0; i < g->ntext; i++) free(g->text[i]);
    free(g->text);
    for (int i = 0; i < 65; i++) free(g->sfx[i].data);
    free(g->frames); free(g);
}

int game_room(const Game *g) { return g->cur; }
void game_hero(const Game *g, int *x, int *y, int *z) { *x = g->hero.x; *y = g->hero.y; *z = g->hero.z; }

/* ---------------------------------------------------------------- sound */
/* Original Paula clock: 3546895 / period = sample rate; volume is Paula's 0..64. */
static int play_voice(Game *g, int id, int period, int vol, double seconds) {
    if (id < 0 || id >= 74 || (id < 65 && !g->sfx[id].len)) return -1;
    int v = 0; for (int i = 0; i < 4; i++) if (!g->voice[i].step) { v = i; break; }
    g->voice[v].sfx = id; g->voice[v].pos = 0; g->voice[v].step = 3546895.0 / period; g->voice[v].vol = vol; g->voice[v].left = seconds; g->voice[v].total = seconds; return v;
}

/* Play a game sound id with its real per-voice samples, periods and volumes. */
static void play_sound(Game *g, int snd) {
    if (snd < 0 || snd >= 69) return;
    for (int i = 0; i < 4; i++) {
        int n = SND_IDX[snd][i][1]; if (!n) continue;
        int k = -1; for (int q = 0; q < 8; q++) if (!g->sq[q].ev) { k = q; break; }
        if (k < 0) k = 0;
        g->sq[k].ev = &SND_EV[SND_IDX[snd][i][0]]; g->sq[k].n = n; g->sq[k].i = 0; g->sq[k].t = 0; g->sq[k].mv = -1; g->sq[k].snd = snd; g->sq[k].el = 0;
    }
}

void game_audio_mix(Game *g, int16_t *out, int n, int rate) {
    for (int i = 0; i < n; i++) {
        int acc = 0;
        for (int q = 0; q < 8; q++) {                                           /* note sequencers */
            SeqVoice *sq = &g->sq[q]; if (!sq->ev) continue;
            if ((sq->t -= 1.0 / rate) > 0) continue;
            if (sq->i >= sq->n) { sq->ev = NULL; continue; }
            const SndEv *e = &sq->ev[sq->i]; int last = sq->i == sq->n - 1;
            if (sq->mv >= 0) g->voice[sq->mv].step = 0;
            if (e->sample >= 0) sq->mv = play_voice(g, e->sample, e->period, e->vol, (last && e->sample < 65) ? (sq->snd == 0 ? 0.1 : 0.6) : e->ticks * SND_TICK_SEC); else sq->mv = -1;
            sq->t += e->ticks * SND_TICK_SEC; sq->i++; sq->el += e->ticks * SND_TICK_SEC; if (sq->el > SND_MAX_SEC) sq->i = sq->n;
        }
        for (int v = 0; v < 4; v++) {
            Voice *vo = &g->voice[v]; if (!vo->step) continue;
            int syn = vo->sfx >= 65, p = (int)vo->pos, len, sv;
            if (syn) { len = SYN_LEN[vo->sfx - 65]; if (!len) { vo->step = 0; continue; } p %= len; sv = SYN_DATA[vo->sfx - 65][p]; if (vo->left > 0 && vo->total > 0) sv = (int)(sv * (vo->left / vo->total)); }   /* synth note: looped cycle with a linear fade */
            else { const Sfx *s = &g->sfx[vo->sfx]; if (p >= s->len) { vo->step = 0; continue; } sv = s->data[p]; }
            acc += sv * vo->vol; vo->pos += vo->step / rate;
            if (syn && vo->pos >= 1e9) vo->pos = 0;
            if (vo->left > 0 && (vo->left -= 1.0 / rate) <= 0) vo->step = 0;
        }
        acc = acc * 4; if (acc > 32767) acc = 32767; if (acc < -32768) acc = -32768;
        out[i] = (int16_t)acc;
    }
}

/* Original $10836: add a signed amount to energy; heal plays sound $19, damage $1b; at zero the hero dies (sound $1c, $15) and the game restarts. */
static void change_energy(Game *g, int delta) {
    play_sound(g, delta < 0 ? 0x1b : 0x19);
    g->energy += delta;
    if (g->energy > ENERGY_MAX) g->energy = ENERGY_MAX;
    if (g->energy <= 0) {
        play_sound(g, 0x1c);
        g->energy = ENERGY_MAX; g->hero = g->start_hero; g->cur = g->start_room; g->jump_t = 0;
    }
}

/* ---------------------------------------------------------------- collision */
typedef struct { int x0, y0, x1, y1, z0, z1; } Box;
static Box box_of(const Game *g, const Obj *o) {
    const Sprite *s = &g->spr[o->slot]; int fx = s->fx, fy = s->fy, fz = s->fz;
    if (o->slot == 8) { fx = HERO_FX; fy = HERO_FY; fz = HERO_FZ; }
    Box b = { o->x - fx + 1, o->y - fy + 1, o->x, o->y, o->z, o->z + fz - 1 };
    return b;
}
static int overlap(Box a, Box b) {
    return !(a.x1 < b.x0 || b.x1 < a.x0 || a.y1 < b.y0 || b.y1 < a.y0 || a.z1 < b.z0 || b.z1 < a.z0);
}
static int flat(const Game *g, const Obj *o) { const Sprite *s = &g->spr[o->slot]; return o->slot != 8 && !s->push && s->fz <= 2; }   /* floor decals: walk-over, drawn under everything */
/* Walking pushes only light objects (type byte +$c bit 1, the TAKE bit); bit 0 alone = heavy furniture (altars, pillars) that only the MOVE verb could shift. */
static int has_contact_script(const Game *g, const Obj *o) { for (int k = 0; k < g->nvm; k++) if (g->vm_pid[k] == (o->pid | 0x8000)) return 1; return 0; }
static int pushable(const Game *g, const Obj *o) {
    if (o->slot == 8) return 0;
    const Sprite *s = &g->spr[o->slot];
    if (s->push & 2) return 1;                                  /* light objects (TAKE bit) */
    if ((s->c & 5) == 5 && !has_contact_script(g, o)) return 1;      /* chests, crates: MOVE icon + c bit 2 (creatures also have the bits but fight back) */
    /* barrels and jars: MOVE-icon objects with a small footprint and moderate height; altars, pillars and tables are bigger or taller */
    return (s->c & 1) && !(o->xflags & 4) && s->fx <= 7 && s->fy <= 7 && s->fz >= 10 && s->fz <= 24;
}

/* Try to move o by (dx,dy); every overlapping object must itself be pushable and movable. */
static int try_move(Game *g, Room *r, Obj *o, int dx, int dy, int depth) {
    int nx = o->x + dx, ny = o->y + dy;
    Box self = box_of(g, o); int fx = self.x1 - self.x0 + 1, fy = self.y1 - self.y0 + 1;
    if (nx >= r->sx * 8 || ny >= r->sy * 8 || nx - fx + 1 < MARGIN || ny - fy + 1 < MARGIN || depth > MAX_PUSH_DEPTH) return 0;
    Obj ghost = *o; ghost.x = nx; ghost.y = ny; Box gb = box_of(g, &ghost);
    Obj *hits[64]; int nh = 0;
    for (int i = 0; i < r->nobj && nh < 64; i++) { Obj *q = &r->objs[i]; if (q == o || flat(g, q)) continue; if (overlap(gb, box_of(g, q))) hits[nh++] = q; }
    if (o != &g->hero && overlap(gb, box_of(g, &g->hero))) return 0;     /* pushed objects stop at the hero */
    for (int i = 0; i < nh; i++) if (!pushable(g, hits[i])) return 0;
    int sx[64], sy[64];
    for (int i = 0; i < nh; i++) {
        sx[i] = hits[i]->x; sy[i] = hits[i]->y;
        if (!try_move(g, r, hits[i], dx, dy, depth + 1)) {
            for (int k = 0; k < i; k++) { hits[k]->x = sx[k]; hits[k]->y = sy[k]; }
            return 0;
        }
    }
    o->x = nx; o->y = ny; return 1;
}

/* Index of the object the hero is touching in his facing direction (original $9090: probe one unit ahead with $8264). */
static int facing_target(const Game *g) {
    const Room *r = &g->rooms[g->cur];
    int dx = ((g->facing & JOY_RIGHT) ? 1 : 0) - ((g->facing & JOY_LEFT) ? 1 : 0);
    int dy = ((g->facing & JOY_DOWN) ? 1 : 0) - ((g->facing & JOY_UP) ? 1 : 0);
    if (!dx && !dy) return -1;
    Obj ghost = g->hero; ghost.x += dx; ghost.y += dy; Box gb = box_of(g, &ghost);
    for (int i = 0; i < r->nobj; i++) {
        const Obj *q = &r->objs[i]; if (flat(g, q)) continue;
        if (overlap(gb, box_of(g, q))) return i;
    }
    return -1;
}

static int neighbour(const Game *g, int wx, int wy, int horiz) {
    for (int n = 0; n < 100; n++) {
        const Room *r = &g->rooms[n]; if (!r->valid || n == g->cur) continue;
        if (horiz ? (r->x0 <= wx && wx < r->x0 + r->sx && r->y0 - 1 <= wy && wy <= r->y0 + r->sy)
                  : (r->x0 - 1 <= wx && wx <= r->x0 + r->sx && r->y0 <= wy && wy < r->y0 + r->sy)) return n;
    }
    return -1;
}

/* Wall record flags word (original $6e2c): 0 = open passage, 0xffff = permanently blocked, anything else = locked,
   needs the key item with that id (inventory not implemented yet, so such doors stay shut). */
static int wall_closed(const Game *g, const Wall *w) { return w->idx < MAX_WALLIDX ? g->wallflag[w->idx] != 0 : w->flags != 0; }

/* Placement-script interpreter (event 5 = USE/switch scripts), following the original dispatcher ($fab0, ops via table $fcca).
   A condition op sets g->cond (the original's $8de counter: true = nonzero); 0e tests it:  0e len <then ops> 16 [0f len <else ops> 16].
   Ops handled: 1e self state bit, 10 other object's state bit, 22 item in rucksack, 28 counter compare, 11/12/13 set/clear/toggle other's bit,
   1f/20/21 same on self, 27 counter add, 1b/0a wall record lock/unlock, 1c text, 2d energy change, 46 sound, 1d/16/17 end markers. Unknown ops stop. */
static void change_energy(Game *g, int delta);
static Obj *find_obj(Game *g, int pid) {
    for (int rm = 0; rm < 100; rm++) { Room *r = &g->rooms[rm]; if (!r->valid) continue; for (int i = 0; i < r->nobj; i++) if (r->objs[i].pid == pid) return &r->objs[i]; }
    return NULL;
}
typedef struct { Game *g; Obj *self; const uint8_t *c; int len, p, cond, stop; } Vm;
static int vm_word(Vm *m) { int v = (m->c[m->p] << 8) | m->c[m->p + 1]; m->p += 2; return v; }
static void vm_op(Vm *m, int op);
static void vm_block(Vm *m) {                                         /* run ops until the 16 that closes the block */
    while (!m->stop && m->p < m->len) { int op = m->c[m->p++]; if (op == 0x16) return; vm_op(m, op); }
    m->stop = 1;
}
static void vm_op(Vm *m, int op) {
    Game *g = m->g; Obj *o;
    if (g->dbg_ntr < 46) { g->dbg_tr[g->dbg_ntr++] = (unsigned char)op; g->dbg_tr[g->dbg_ntr++] = (unsigned char)(m->cond | (m->self->flags & 1) << 4); }
    switch (op) {
    case 0x17: m->stop = 1; break;
    case 0x16: case 0x1d: break;
    case 0x1e: m->cond = (m->self->flags & 1) ? m->cond + 1 : 0; break;
    case 0x10: o = find_obj(g, vm_word(m)); m->cond = (o && (o->flags & 1)) ? m->cond + 1 : 0; break;
    case 0x11: o = find_obj(g, vm_word(m)); if (o) o->flags |= 1; break;
    case 0x12: o = find_obj(g, vm_word(m)); if (o) o->flags &= ~1; break;
    case 0x13: o = find_obj(g, vm_word(m)); if (o) o->flags ^= 1; break;
    case 0x01: { int id = vm_word(m);                                   /* bring a not-yet-spawned object into its room */
        for (int i = 0; i < g->nhid; i++) if (g->hid[i].o.pid == id) {
            Room *r = &g->rooms[g->hid[i].room]; Obj o = g->hid[i].o; o.flags &= ~0x80;
            r->objs[r->nobj++] = o; g->hid[i] = g->hid[--g->nhid]; break; }
        break; }
    case 0x05: { int d = m->c[m->p]; m->p++; g->gold += d; if (g->gold < 0) g->gold = 0; play_sound(g, d < 0 ? 0x24 : 0x1a); break; }   /* add gold ($ffa0) */
    case 0x33: { int n = m->c[m->p]; m->p++; if (n <= 9) g->want_level = n + 2; m->stop = 1; break; }              /* go to level n+2 ($10098) */
    case 0x24: { int id = vm_word(m); int x = m->c[m->p], y = m->c[m->p + 1], z = m->c[m->p + 2]; m->p += 4;   /* move/spawn a placement into the current room ($104b4) */
        Room *cr = &g->rooms[g->cur]; Obj o2; int got = 0;
        int skey = (x << 8 | y) + 1;                                     /* one placement can be spawned several times at different spots (3 bats in room 38); the same spot again is a repeat */
        for (int i = 0; i < cr->nobj; i++) if (cr->objs[i].pid == id) { if (cr->objs[i].skey == skey || cr->objs[i].skey == 0 || cr->nobj >= cr->ocap - 1) return; if (!got) { o2 = cr->objs[i]; got = 1; } }
        if (got) { o2.skey = skey; o2.flags &= ~0x80; o2.x = x; o2.y = y; o2.z = (z >= 0x40 && !(g->lvl1 && o2.slot == 119)) ? 0 : z; o2.stage = 0; o2.mpc = o2.mwait = o2.mstop = 0; memset(o2.mc, 0, sizeof o2.mc); cr->objs[cr->nobj++] = o2; break; }
        for (int i = 0; i < g->nhid && !got; i++) if (g->hid[i].o.pid == id) { o2 = g->hid[i].o; g->hid[i] = g->hid[--g->nhid]; got = 1; }
        for (int rm = 0; rm < 100 && !got; rm++) { Room *r2 = &g->rooms[rm]; if (!r2->valid || rm == g->cur) continue;
            for (int i = 0; i < r2->nobj; i++) if (r2->objs[i].pid == id) { o2 = r2->objs[i]; r2->objs[i] = r2->objs[--r2->nobj]; got = 1; break; } }
        if (!got) return;
        o2.flags &= ~0x80; o2.x = x; o2.y = y; o2.z = (z >= 0x40 && !(g->lvl1 && o2.slot == 119)) ? 0 : z; o2.anim = g->spr[o2.slot].nfr >= 10 ? 8 : 0; o2.atick = 8; o2.stage = 0; o2.skey = skey; cr->objs[cr->nobj++] = o2; break; }
    case 0x25: { int n = m->c[m->p], dx = m->c[m->p + 1], dy = m->c[m->p + 2], dz = m->c[m->p + 3]; m->p += 4;   /* teleport the hero into room n at an offset from its origin ($10546) */
        if (n < 100 && g->rooms[n].valid) { g->cur = n; g->hero.x = MARGIN + HERO_FX + dx; g->hero.y = MARGIN + HERO_FY + dy; g->hero.z = dz; }
        break; }
    case 0x29: { int id = vm_word(m); int rm = m->c[m->p], x = m->c[m->p + 1], y = m->c[m->p + 2], z = m->c[m->p + 3]; m->p += 4;   /* move an object (or rucksack item) to room rm ($10668) */
        Obj o2; int got = 0;
        for (int i = 0; i < g->ninv && !got; i++) if (g->inv[i] == id) {
            memset(&o2, 0, sizeof o2); o2.pid = id; o2.slot = g->inv_slot[i];
            if (g->inv_hold == id) g->inv_hold = -1;
            memmove(&g->inv[i], &g->inv[i + 1], (size_t)(g->ninv - i - 1) * sizeof(int)); memmove(&g->inv_slot[i], &g->inv_slot[i + 1], (size_t)(g->ninv - i - 1) * sizeof(int)); g->ninv--;
            if (g->inv_sel >= g->ninv) g->inv_sel = g->ninv ? g->ninv - 1 : 0;
            got = 1; }
        for (int i = 0; i < g->nhid && !got; i++) if (g->hid[i].o.pid == id) { o2 = g->hid[i].o; g->hid[i] = g->hid[--g->nhid]; got = 1; }
        for (int r2 = 0; r2 < 100 && !got; r2++) { Room *q = &g->rooms[r2]; if (!q->valid) continue;
            for (int i = 0; i < q->nobj; i++) if (q->objs[i].pid == id) { o2 = q->objs[i]; q->objs[i] = q->objs[--q->nobj]; got = 1; break; } }
        if (got && rm < 100 && g->rooms[rm].valid && g->rooms[rm].nobj < 4000) { o2.x = x; o2.y = y; o2.z = z; o2.flags &= ~0x80; g->rooms[rm].objs[g->rooms[rm].nobj++] = o2; }
        break; }
    case 0x26: { int n = m->c[m->p], v = m->c[m->p + 1]; m->p += 2; g->counter[n] = (uint8_t)v; break; }              /* set counter n ($1078c) */
    case 0x30: m->cond = m->cond == 0; vm_op(m, 0x0e); break;                                              /* block runs when the condition FAILED ($10222) */
    case 0x31: { int i = m->c[m->p], v = m->c[m->p + 1]; m->p += 2; g->gflag[i] = (uint8_t)v; break; }              /* global flag byte ($108d8) */
    case 0x4c: { int n = m->c[m->p++]; m->cond = (g->gbits >> (n & 31) & 1) ? m->cond + 1 : 0; break; }            /* global bit n set ($108cc) */
    case 0x2f: { int id = vm_word(m); Obj *q = id >= 0x8000 ? m->self : find_obj(g, id); m->cond = q ? m->cond + 1 : 0; break; }   /* object exists ($108bc) */
    case 0x2b: { int id = vm_word(m); int rm = m->c[m->p++]; if (rm == 0xfe) rm = g->cur; int in = 0;                /* object is in room rm ($103a8) */
        for (int r2 = 0; r2 < 100 && !in; r2++) { Room *q = &g->rooms[r2]; if (!q->valid) continue; for (int i = 0; i < q->nobj; i++) if (q->objs[i].pid == id) { in = (r2 == rm); r2 = 100; break; } }
        for (int i = 0; i < g->nhid && !in; i++) if (g->hid[i].o.pid == id && g->hid[i].room == rm) in = 1;
        m->cond = in ? m->cond + 1 : 0; break; }
    case 0x15: { int id = vm_word(m); Obj *q = id >= 0x8000 ? m->self : find_obj(g, id); if (q) q->flags &= ~2; break; }   /* clear placement flag bit 1 ($1030c) */
    case 0x32: { int id = vm_word(m); Obj *q = id >= 0x8000 ? m->self : find_obj(g, id);                            /* post the destroy event (0x17) to an object ($1002c): it is removed */
        if (q && q != &g->hero) q->life = -1; break; }
    case 0x06: { int n = m->c[m->p++]; g->xp += n; g->gold += n >> 2; play_sound(g, 3); break; }                       /* experience (and a quarter of it as gold) ($ffdc) */
    case 0x4a: { int id = vm_word(m);                                                                              /* reveal a hidden object ($10068) */
        for (int i = 0; i < g->nhid; i++) if (g->hid[i].o.pid == id) {
            Room *r2 = &g->rooms[g->hid[i].room]; Obj o2 = g->hid[i].o; o2.flags &= ~0x80;
            r2->objs[r2->nobj++] = o2; g->hid[i] = g->hid[--g->nhid]; break; }
        break; }
    case 0x1a: { int id = vm_word(m); Obj *q = id >= 0x8000 ? m->self : find_obj(g, id); if (q && q != &g->hero) q->life = -1; break; }   /* despawn an object ($fea4) */
    case 0x3a: { int n = m->c[m->p];                                  /* run the block only if exactly n conditions held ($10236); layout as 0e */
        m->cond = (m->cond == n) ? 1 : 0; m->p++; vm_op(m, 0x0e); break; }
    case 0x1f: m->self->flags &= ~1; break;
    case 0x20: m->self->flags |= 1; break;
    case 0x21: m->self->flags ^= 1; break;
    case 0x22: { int id = vm_word(m), has = 0; for (int i = 0; i < g->ninv; i++) if (g->inv[i] == id) has = 1; m->cond = has ? m->cond + 1 : 0; break; }
    case 0x27: { int n = m->c[m->p]; int add = m->c[m->p + 1]; m->p += 2; g->counter[n] = (uint8_t)(g->counter[n] + add); break; }
    case 0x28: { int n = m->c[m->p], mode = m->c[m->p + 1], v = m->c[m->p + 2], x = g->counter[n]; m->p += 3;
        int ok = mode == 0 ? x > v : mode == 1 ? x < v : mode == 2 ? x == v : x != v;      /* modes as in $107aa (cmp d2,d3 = counter - v): 0 gt, 1 lt, 2 eq, 3 ne */
        m->cond = ok ? m->cond + 1 : 0; break; }
    case 0x1b: { int w = m->c[m->p++]; int v = vm_word(m); if (w < MAX_WALLIDX) { g->wallflag[w] = (uint16_t)v; if (v) g->walldoor[w] = 1; } break; }
    case 0x0a: { int w = m->c[m->p++]; if (w < MAX_WALLIDX && g->wallflag[w]) { g->wallflag[w] = 0; play_sound(g, 0x12); } break; }
    case 0x1c: { int t = vm_word(m); if (t < g->ntext && g->text[t][0]) { snprintf(g->scroll_str, sizeof g->scroll_str, "%s", g->text[t]); g->scroll_t = SCROLL_TICKS; } break; }
    case 0x2d: { int d = vm_word(m); if (d >= 32768) d -= 65536; change_energy(g, d); break; }
    case 0x46: { int snd = m->c[m->p]; m->p++; play_sound(g, snd); break; }
    case 0x0e: {
        if (m->cond) {                                                /* run the then part, then skip an else part */
            m->cond = 0; m->p++; vm_block(m);
            if (!m->stop && m->p < m->len && m->c[m->p] == 0x0f) { m->p++; m->p += m->c[m->p]; }
        } else {
            m->cond = 0; int l = m->c[m->p]; int p0 = m->p; m->p += l;
            if (m->p < m->len && m->c[m->p] == 0x0f) { m->p += 2; vm_block(m); }
        }
        break; }
    default: m->stop = 1; break;                                      /* not decoded yet */
    }
}
static void run_script(Game *g, Obj *self, const uint8_t *c, int len) {
    Vm m = { g, self, c, len, 0, 0, 0 };
    while (!m.stop && m.p < m.len) { int op = c[m.p]; m.p++; vm_op(&m, op); }   /* no side effect inside the call's argument: MSVC evaluated the argument after the callee read p */
}

static void say(Game *g, int text_id);

/* Rucksack: examine / drop an item by index. */
static void inv_examine(Game *g, int idx) {
    int pid = g->inv[idx];
    for (int i = 0; i < g->nexam; i++) if (g->exam_pid[i] == pid && g->exam_text[i] < g->ntext && g->text[g->exam_text[i]][0]) {
        snprintf(g->scroll_str, sizeof g->scroll_str, "%s.", g->text[g->exam_text[i]]); g->scroll_t = SCROLL_TICKS; return; }
    int nm = g->spr[g->inv_slot[idx]].name; if (nm >= 0 && nm < g->ntext) { snprintf(g->scroll_str, sizeof g->scroll_str, "%s.", g->text[nm]); g->scroll_t = SCROLL_TICKS; }
}
static void inv_remove(Game *g, int idx) {
    memmove(&g->inv[idx], &g->inv[idx + 1], (size_t)(g->ninv - idx - 1) * sizeof(int)); memmove(&g->inv_slot[idx], &g->inv_slot[idx + 1], (size_t)(g->ninv - idx - 1) * sizeof(int)); g->ninv--;
    if (g->inv_sel >= g->ninv) g->inv_sel = g->ninv ? g->ninv - 1 : 0;
}
static void inv_drop(Game *g, int idx) {
    Room *r = &g->rooms[g->cur]; Obj o; memset(&o, 0, sizeof o); o.pid = g->inv[idx]; o.slot = g->inv_slot[idx]; o.x = g->hero.x; o.y = g->hero.y; o.z = g->hero.z;
    if (g->inv_hold == o.pid) g->inv_hold = -1;
    if (r->nobj < 4000) r->objs[r->nobj++] = o;                          /* objs arrays have 32 spare entries; stay conservative below */
    inv_remove(g, idx); play_sound(g, 0);
}
/* Leaving the room through an exit wall; returns 1 if the room changed. */
static int try_exit(Game *g, int dx, int dy) {
    Room *r = &g->rooms[g->cur]; Obj *h = &g->hero;
    int nx = h->x + dx, ny = h->y + dy;
    for (int k = 0; k < r->nwalls; k++) {
        Wall *w = &r->walls[k];
        if (dy != 0 && w->orient == 0) continue;
        if (dx != 0 && dy == 0 && w->orient != 0) continue;
        if (dy < 0 && w->box[1] >= r->sy * 4) continue;           /* must be the wall on the side we are leaving through */
        if (dy > 0 && w->box[1] < r->sy * 4) continue;
        if (dy == 0 && dx < 0 && w->box[0] >= r->sx * 4) continue;
        if (dy == 0 && dx > 0 && w->box[0] < r->sx * 4) continue;
        if (dy != 0) { if (!(w->box[0] <= nx - HERO_FX + 1 && nx <= w->box[2])) continue; }
        else         { if (!(w->box[1] <= ny - HERO_FY + 1 && ny <= w->box[3])) continue; }
        { int level = (w->z - r->basez) * 16; if (h->z < level || h->z >= level + 32) continue; }   /* the exit sits at its own height: stairs lead up to it */
        int n = neighbour(g, w->x, w->y, w->orient != 0); if (n < 0) continue;
        if (wall_closed(g, w) && w->idx < MAX_WALLIDX && g->wallflag[w->idx] != 0xffff) {
            int key = g->wallflag[w->idx];                        /* locked door: the key item's placement id is the flag value */
            for (int i = 0; i < g->ninv; i++) if (g->inv[i] == key) {
                if (w->b7 & 2) { memmove(&g->inv[i], &g->inv[i + 1], (size_t)(g->ninv - i - 1) * sizeof(int)); memmove(&g->inv_slot[i], &g->inv_slot[i + 1], (size_t)(g->ninv - i - 1) * sizeof(int)); g->ninv--; }   /* key consumed */
                g->wallflag[w->idx] = 0;                              /* the door stays unlocked after the key is used (a consumed key could never reopen it) */
                play_sound(g, 18); say(g, 1); key = 0; break;
            }
            if (key) { if (g->bump_timer <= 0) { play_sound(g, 60); g->bump_timer = 32; } return 0; }
        } else if (wall_closed(g, w)) {                           /* shut door: bump, with a sound at most every 32 ticks ($9be counter) */
            if (g->bump_timer <= 0) { play_sound(g, 60); g->bump_timer = 32; }
            return 0;
        }
        Room *nr = &g->rooms[n]; int x, y;
        if (w->orient != 0) {
            x = (w->x + 1 - nr->x0) * 8 + 4;
            y = (((ny - MARGIN) & ~7) == 0) ? nr->sy * 8 - 1 : MARGIN + HERO_FY;
        } else {
            y = (w->y + 1 - nr->y0) * 8 + 4;
            x = (((nx - MARGIN) & ~7) == 0) ? nr->sx * 8 - 1 : MARGIN + HERO_FX;
        }
        h->z = (((h->z >> 4) + r->basez - nr->basez) << 4) | (h->z & 15);
        h->x = x; h->y = y; g->cur = n; if (w->slot < 4) play_sound(g, 0x38); return 1;                 /* door sound only for header doorways (wall slots 0-3; the lever clunk stands in) */
    }
    return 0;
}

/* Walking animation scripts (frame bank indices), from the original tables at $562e/$5694. */
static const int walk_up[4] = {12, 13, 14, 15}, walk_down[4] = {11, 10, 9, 8}, walk_left[4] = {7, 6, 5, 4}, walk_right[4] = {0, 1, 2, 3};

/* Verb list for the object being touched, in the order of the original builder ($8e44..$8f46). Verb id = icon id. */
static void build_verbs(Game *g) {
    Room *r = &g->rooms[g->cur]; g->nverbs = 0;
    if (g->target < 0 || !g->has_icons) return;
    const Obj *o = &r->objs[g->target]; const Sprite *s = &g->spr[o->slot]; int *v = g->verbs, n = 0;
    if ((s->c & 2) && !(o->xflags & 4)) v[n++] = 2;                                   /* take */
    if (s->b17 && !(o->xflags & 0x40) && g->verbtab[s->b17 & 15] < 20) v[n++] = g->verbtab[s->b17 & 15];
    if (s->b1d == 0x12) v[n++] = 0xe;
    if ((s->c & 1) && !(o->xflags & 4)) v[n++] = 0xa;
    if (s->b17 != 6 && !(s->b16 & 0x80)) v[n++] = 0xb;
    v[n++] = 6;
    g->nverbs = n;
}

static void say(Game *g, int text_id) { g->msg = text_id; g->msg_t = 50; }

static void exec_verb(Game *g, int verb) {
    if (verb == 0xa && g->target >= 0) { g->pull_pid = g->rooms[g->cur].objs[g->target].pid; g->pull_room = g->cur; return; }   /* MOVE: walking away drags this object behind the hero */
    Room *r = &g->rooms[g->cur];
    if (g->target < 0) return;
    g->dbg_verb = verb; g->dbg_pid = r->objs[g->target].pid; g->dbg_ran = 0; g->dbg_ntr = 0; g->dbg_log[g->dbg_execs & 3][0] = g->tickno; g->dbg_log[g->dbg_execs & 3][1] = (unsigned)verb; g->dbg_log[g->dbg_execs & 3][2] = (unsigned)g->prev_joy; g->dbg_execs++;
    if (verb == 2 && g->ninv >= 32) {                                                /* the rucksack holds 32 things */
        int tre = 0; for (int i = 0; i < g->ntake; i++) if (g->take_pid[i] == r->objs[g->target].pid) tre = 1;
        if (!tre) { say(g, 53); g->menu = 0; return; }
    }
    if (verb == 2 && g->ninv < 64) {                                                 /* take: the object goes to the rucksack */
        { int pid = r->objs[g->target].pid;                                          /* treasure: gold and XP from the take script ($ffdc/$ffa0) */
          for (int i = 0; i < g->ntake; i++) if (g->take_pid[i] == pid) {
              if (g->take_gold[i]) { g->gold += g->take_gold[i]; play_sound(g, 3); }
              if (g->take_xp[i]) { g->xp += g->take_xp[i]; if (g->xp < 0) g->xp = 0; if (!g->take_gold[i]) play_sound(g, 0x1a); }
              break; } }
        g->inv_slot[g->ninv] = r->objs[g->target].slot; g->inv[g->ninv++] = r->objs[g->target].pid;
        if (g->spr[r->objs[g->target].slot].name >= 0) say(g, g->spr[r->objs[g->target].slot].name);   /* original prints the item name */
        memmove(&r->objs[g->target], &r->objs[g->target + 1], (size_t)(r->nobj - g->target - 1) * sizeof(Obj)); r->nobj--;
        g->target = -1; g->nverbs = 0; return;
    }
    if (verb == 8 && g->spr[r->objs[g->target].slot].b16 == 7 && g->has_scroll) { g->status_open = 1; return; }     /* the diary: health, gold, XP, level, % complete */
    if (verb == 0xb && g->has_scroll) {                                               /* examine: the object's description on a parchment scroll */
        int pid = r->objs[g->target].pid;
        for (int i = 0; i < g->nexam; i++) if (g->exam_pid[i] == pid && g->exam_text[i] < g->ntext && g->text[g->exam_text[i]][0]) {
            snprintf(g->scroll_str, sizeof g->scroll_str, "%s.", g->text[g->exam_text[i]]); g->scroll_t = SCROLL_TICKS; return;
        }
    }
    if (verb == 3) {                                                                  /* open a chest: lid up (state bit), its contents are spawned by its script */
        Obj *ch = &r->objs[g->target];
        int ci = -1; for (int i = 0; i < g->nch; i++) if (g->ch_pid[i] == ch->pid) ci = i;
        if (ci >= 0 && (ch->flags & 1)) { if (g->ch_flags[ci] & 0x20) { ch->flags &= ~1; play_sound(g, 0x38); } return; }          /* open: only some chests close again */
        if (ci >= 0 && g->ch_trap[ci]) { change_energy(g, -g->ch_trap[ci]); g->ch_trap[ci] = 0; say(g, 50); play_sound(g, 0x24); return; }   /* a trap goes off first (once) */
        if (ci >= 0 && g->ch_key[ci]) {                                                /* locked: the key must be in the rucksack */
            int ki = -1; for (int i = 0; i < g->ninv; i++) if (g->inv[i] == g->ch_key[ci]) ki = i;
            if (ki < 0) { play_sound(g, 0x3b); say(g, 48); return; }
            if (g->ch_flags[ci] & 2) inv_remove(g, ki);                                /* the key is used up */
            g->ch_key[ci] = 0; play_sound(g, 0x12);
        }
        if (ci >= 0 && g->ch_cont[ci]) {                                               /* contents appear where the chest stands */
            int id = g->ch_cont[ci], got = 0; Obj o2;
            for (int i = 0; i < g->nhid && !got; i++) if (g->hid[i].o.pid == id) { o2 = g->hid[i].o; g->hid[i] = g->hid[--g->nhid]; got = 1; }
            for (int rm = 0; rm < 100 && !got; rm++) { Room *r2 = &g->rooms[rm]; if (!r2->valid || rm == g->cur) continue;
                for (int i = 0; i < r2->nobj; i++) if (r2->objs[i].pid == id) { o2 = r2->objs[i]; r2->objs[i] = r2->objs[--r2->nobj]; got = 1; break; } }
            if (got) {                                                                  /* pop out beside the chest (far side from the hero first), else on its lid */
                o2.flags &= ~0x80; const Sprite *cs = &g->spr[ch->slot], *is = &g->spr[o2.slot];
                int fdx = (g->facing & JOY_RIGHT) ? 1 : (g->facing & JOY_LEFT) ? -1 : 0, fdy = (g->facing & JOY_DOWN) ? 1 : (g->facing & JOY_UP) ? -1 : 0;
                int cand[4][2] = { { fdx, fdy }, { fdy, fdx }, { -fdy, -fdx }, { -fdx, -fdy } }, placed = 0;
                for (int c = 0; c < 4 && !placed; c++) {
                    if (!cand[c][0] && !cand[c][1]) continue;
                    Obj t = o2; t.z = ch->z;
                    t.x = ch->x + (cand[c][0] > 0 ? is->fx : cand[c][0] < 0 ? -cs->fx : 0); t.y = ch->y + (cand[c][1] > 0 ? is->fy : cand[c][1] < 0 ? -cs->fy : 0);
                    if (t.x >= r->sx * 8 || t.y >= r->sy * 8 || t.x - is->fx + 1 < MARGIN || t.y - is->fy + 1 < MARGIN) continue;
                    Box tb = box_of(g, &t); int hit = overlap(tb, box_of(g, &g->hero));
                    for (int i = 0; i < r->nobj && !hit; i++) { if (&r->objs[i] == ch || flat(g, &r->objs[i])) continue; if (overlap(tb, box_of(g, &r->objs[i]))) hit = 1; }
                    if (!hit) { o2 = t; placed = 1; } }
                if (!placed) { o2.x = ch->x; o2.y = ch->y; o2.z = ch->z + cs->fz; }
                r->objs[r->nobj++] = o2; ch = &r->objs[g->target]; }
            g->ch_cont[ci] = 0;
        }
        if (!(ch->flags & 1)) {
            ch->flags |= 1; play_sound(g, 0x0a);
            for (int i = 0; i < g->nvm; i++) if (g->vm_pid[i] == ch->pid) run_script(g, ch, g->vm_code[i], g->vm_len[i]);
        } else say(g, 4);
        return;
    }
    if (verb == 7 || verb == 4 || (verb == 6 && g->has_scroll)) {                                  /* use / pull a lever: event-5 script (a lever also clicks, sound $38) */
        int pid = r->objs[g->target].pid;
        { int ran = 0; int pidf = r->objs[g->target].flags; for (int i = 0; i < g->nvm; i++) if (g->vm_pid[i] == pid) { if (!ran && (verb == 7 || verb == 4)) play_sound(g, 0x38); ran = 1; g->dbg_ran++; run_script(g, &r->objs[g->target], g->vm_code[i], g->vm_len[i]); } if (ran && verb == 7 && r->objs[g->target].flags == pidf) r->objs[g->target].flags ^= 1; if (ran) return; }   /* a lever whose script leaves its own state alone still flips to the pulled picture */
    }
    if (verb == 6 && g->has_scroll) {                                                 /* use: event 5 message scripts (inscriptions, switches) */
        int pid = r->objs[g->target].pid;
        for (int i = 0; i < g->nuse; i++) if (g->use_pid[i] == pid && g->use_text[i] < g->ntext && g->text[g->use_text[i]][0]) {
            snprintf(g->scroll_str, sizeof g->scroll_str, "%s", g->text[g->use_text[i]]); g->scroll_t = SCROLL_TICKS; return;
        }
    }
    say(g, 4);                                                                        /* "NOTHING HAPPENS" until the other verbs exist */
}

static int move_sel(const Game *g, int dirs) {
    int cx = g->slotpos[g->sel][0], cy = g->slotpos[g->sel][1], best = g->sel, bd = 1 << 30;
    for (int i = 0; i < g->nverbs; i++) {
        if (i == g->sel) continue;
        int dx = g->slotpos[i][0] - cx, dy = g->slotpos[i][1] - cy;
        int ok = ((dirs & JOY_LEFT) && dx < 0) || ((dirs & JOY_RIGHT) && dx > 0) || ((dirs & JOY_UP) && dy < 0) || ((dirs & JOY_DOWN) && dy > 0);
        if (!ok) continue;
        int d = dx * dx + dy * dy; if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* Step-up onto a low object (original $6b44: type bit 3, climb under $14 units): retry the move with the hero raised to the top. */
static int try_step_up(Game *g, Room *r, int dx, int dy) {
    Obj *h = &g->hero; Obj ghost = *h; ghost.x += dx; ghost.y += dy;
    Box gb = box_of(g, &ghost); int top = -1;
    for (int i = 0; i < r->nobj; i++) {
        Obj *q = &r->objs[i]; if (flat(g, q)) continue;
        Box qb = box_of(g, q); if (!overlap(gb, qb)) continue;
        if (pushable(g, q) || !(g->spr[q->slot].c & 8)) return 0;
        if (qb.z1 + 1 > top) top = qb.z1 + 1;
    }
    if (top < 0 || top - h->z >= 0x14) return 0;
    ghost.z = top; gb = box_of(g, &ghost);
    for (int i = 0; i < r->nobj; i++) { Obj *q = &r->objs[i]; if (!flat(g, q) && overlap(gb, box_of(g, q))) return 0; }
    h->x = ghost.x; h->y = ghost.y; h->z = top; return 1;
}

/* Gravity: the hero rests on the highest object top under his feet (or the floor) and drops a few units per tick. */
/* Jump profile measured in the original (hero z above the take-off height after each logic tick; 20 ticks, apex 35). */
static const uint8_t JUMP_Z[20] = {5, 10, 14, 18, 22, 26, 29, 32, 34, 35, 34, 32, 29, 26, 22, 18, 14, 10, 5, 0};
static int hero_grounded(const Game *g, const Room *r) {
    const Obj *h = &g->hero; Box hb = box_of(g, h); int rest = 0;
    for (int i = 0; i < r->nobj; i++) {
        const Obj *q = &r->objs[i]; if (flat(g, q)) continue;
        Box qb = box_of(g, q);
        if (qb.x1 < hb.x0 || hb.x1 < qb.x0 || qb.y1 < hb.y0 || hb.y1 < qb.y0) continue;
        if (qb.z1 + 1 <= h->z && qb.z1 + 1 > rest) rest = qb.z1 + 1;
    }
    return h->z <= rest;
}
static void hero_gravity(Game *g, Room *r) {
    Obj *h = &g->hero; Box hb = box_of(g, h); int rest = 0;
    for (int i = 0; i < r->nobj; i++) {
        Obj *q = &r->objs[i]; if (flat(g, q)) continue;
        Box qb = box_of(g, q);
        if (qb.x1 < hb.x0 || hb.x1 < qb.x0 || qb.y1 < hb.y0 || hb.y1 < qb.y0) continue;
        if (qb.z1 + 1 <= h->z && qb.z1 + 1 > rest) rest = qb.z1 + 1;
    }
    if (h->z > rest) { h->z -= 4; if (h->z < rest) h->z = rest; }
}


/* Ranged weapons: fire with a class-4 item held (the stone bag) throws a rock in the facing direction (original $ee28: sound $14, missile = placement 121 = image slot 31, power 5, no ammo needed). */
static int held_ranged(const Game *g) {
    if (g->inv_hold < 0) return 0;
    for (int i = 0; i < g->ninv; i++) if (g->inv[i] == g->inv_hold) return g->spr[g->inv_slot[i]].b16 == 4;
    return 0;
}
static int fire_weapon(Game *g) {
    Room *r = &g->rooms[g->cur]; Obj *h = &g->hero;
    int dx = ((g->facing & JOY_RIGHT) ? 1 : 0) - ((g->facing & JOY_LEFT) ? 1 : 0), dy = ((g->facing & JOY_DOWN) ? 1 : 0) - ((g->facing & JOY_UP) ? 1 : 0);
    if ((!dx && !dy) || r->nobj >= 4000) return 0;
    Obj m; memset(&m, 0, sizeof m); m.slot = MISSILE_SLOT; m.pid = MISSILE_PID + (g->spawn_pid++ & 255);
    m.x = h->x + dx * 4; m.y = h->y + dy * 4; m.z = h->z + 10; m.vx = dx; m.vy = dy; m.life = 40;
    r->objs[r->nobj++] = m; play_sound(g, 0x14); return 1;
}
/* Missiles fly MISSILE_SPEED units a tick in single steps; they stop at the room edge or on the first solid thing, which a creature loses health to. */
static void update_missiles(Game *g) {
    Room *r = &g->rooms[g->cur];
    for (int i = 0; i < r->nobj; i++) {
        Obj *m = &r->objs[i]; if (m->life <= 0) continue;
        int dead = 0;
        for (int st = 0; st < MISSILE_SPEED && !dead; st++) {
            m->x += m->vx; m->y += m->vy;
            if (m->x >= r->sx * 8 || m->y >= r->sy * 8 || m->x < MARGIN || m->y < MARGIN) { dead = 1; break; }
            Box mb = box_of(g, m);
            for (int k = 0; k < r->nobj; k++) {
                Obj *q = &r->objs[k]; if (k == i || q->life != 0 || flat(g, q)) continue;
                if (!overlap(mb, box_of(g, q))) continue;
                if (has_contact_script(g, q)) {
                    if (q->hp <= 0) q->hp = CREATURE_HP;
                    q->hp -= ROCK_POWER;
                    { static const uint8_t hit_snd[19] = { 9, 9, 9, 13, 13, 9, 19, 9, 9, 9, 19, 9, 13, 9, 9, 13, 13, 19, 9 };    /* victim's hit sound, table $5bf8 indexed by type byte +$1d */
                      int ty = g->spr[q->slot].b1d; play_sound(g, ty >= 0 && ty < 19 ? hit_snd[ty] : 0x1a); }
                    if (q->hp <= 0) q->life = -1;                      /* killed: removed below */
                }
                dead = 1; break;
            }
        }
        if (--m->life <= 0) dead = 1;
        if (dead) m->life = -1;
    }
    int w = 0; for (int i = 0; i < r->nobj; i++) if (r->objs[i].life >= 0) r->objs[w++] = r->objs[i];
    r->nobj = w;
}
/* Creature path programs (original movement executor $f3f2/$f606): one step per tick along each non-zero axis counter.
   op 0 a b c = load counters, 1 d = jump by d bytes from the op, 2 = stop, 3 n = wait n ticks, 4 n = (event) skipped. A blocked step reverses the path (the original posts event $b). */
static int creature_free(Game *g, Room *r, Obj *o, int nx, int ny) {
    Obj ghost = *o; ghost.x = nx; ghost.y = ny; Box gb = box_of(g, &ghost);
    int fx = gb.x1 - gb.x0 + 1, fy = gb.y1 - gb.y0 + 1;
    if (nx >= r->sx * 8 || ny >= r->sy * 8 || nx - fx + 1 < MARGIN || ny - fy + 1 < MARGIN) return 0;
    return 1;                                                    /* static furniture does not stop them in the emulator captures (room 38); only the room edge does */
}
static void creature_move(Game *g, Room *r, Obj *c) {
    int pid = c->pid; if (pid < 0 || pid >= 512 || !g->mv_len[pid] || c->mstop || c->life > 0) return;
    if (c->mwait > 0) { c->mwait--; return; }
    const uint8_t *pr = g->mv_prog[pid]; int len = g->mv_len[pid];
    for (int guard = 0; guard < 8 && !(c->mc[0] | c->mc[1] | c->mc[2]) && !c->mwait && !c->mstop; guard++) {
        if (c->mpc < 0 || c->mpc + 1 >= len) { c->mstop = 1; break; }
        int op = pr[c->mpc];
        if (op == 0 && c->mpc + 3 < len) { c->mc[0] = (int8_t)pr[c->mpc + 1]; c->mc[1] = (int8_t)pr[c->mpc + 2]; c->mc[2] = (int8_t)pr[c->mpc + 3]; c->mpc += 4; }
        else if (op == 1) c->mpc += (int8_t)pr[c->mpc + 1];
        else if (op == 3) { c->mwait = pr[c->mpc + 1]; c->mpc += 2; }
        else if (op == 4) c->mpc += 2;
        else c->mstop = 1;
    }
    if (!(c->mc[0] | c->mc[1] | c->mc[2])) return;
    c->macc += g->mv_pat[pid] ? 2 : 5; if (c->macc < 5) return; c->macc -= 5;   /* tick pattern byte: 0 = a step every tick (room 38 bats); 0x11 = about 2 steps in 5 ticks (room 52, measured). Other values unmeasured: treated like 0x11 */
    int d[3] = { c->mc[0] > 0 ? 1 : c->mc[0] < 0 ? -1 : 0, c->mc[1] > 0 ? 1 : c->mc[1] < 0 ? -1 : 0, c->mc[2] > 0 ? 1 : c->mc[2] < 0 ? -1 : 0 };
    int st0 = g->mv_step[pid][0] ? g->mv_step[pid][0] : 1, st1 = g->mv_step[pid][1] ? g->mv_step[pid][1] : 1, st2 = g->mv_step[pid][2] ? g->mv_step[pid][2] : 1;
    int nx = c->x + d[0] * st0, ny = c->y + d[1] * st1;
    if (!creature_free(g, r, c, nx, ny)) { c->mc[0] = -c->mc[0]; c->mc[1] = -c->mc[1]; c->mc[2] = -c->mc[2]; return; }
    c->x = nx; c->y = ny; c->z += d[2] * st2;
    for (int i = 0; i < 3; i++) if (c->mc[i]) c->mc[i] += c->mc[i] > 0 ? -1 : 1;
    if (d[0] > 0) c->mdir = 0; else if (d[0] < 0) c->mdir = 3; else if (d[1] > 0) c->mdir = 1; else if (d[1] < 0) c->mdir = 2;
}

void game_tick(Game *g, int joy) {
    Room *r = &g->rooms[g->cur]; Obj *h = &g->hero;
    int fire_edge = (joy & JOY_FIRE) && !(g->prev_joy & JOY_FIRE), new_dirs = (joy & 15) & ~(g->prev_joy & 15);
    if (fire_edge && !g->menu) g->pull_pid = 0;                 /* fire ends pull mode (choosing MOVE itself happens inside the menu below) */
    int prev = g->prev_joy; g->prev_joy = joy;
    int first_visit = !g->visited[g->cur]; g->visited[g->cur] = 1;
    if (g->tm_room != g->cur + 1) { g->tm_room = g->cur + 1; g->tm_left = g->rs_period[g->cur] * 17;          /* room timer restarts on entry */
        for (int ev = first_visit ? 0x1c : 0x06; ev >= 0x06; ev -= 0x16) {                              /* entry events: 0x1c on the first visit ($e51c), then 0x06 on every entry ($e74e); they spawn most monsters */
            int room = g->cur;
            for (int b = 0; b < g->rs_nb[room]; b++) { const uint8_t *c = g->rs_blk[room][b]; if ((c[1] & 0x7f) != ev) continue;
                Vm m = { g, &g->hero, c, c[0], 2, 0, 0 }; vm_block(&m); }
        }
        { int n0 = g->rooms[g->cur].nobj;                    /* event 0xd scripts (posted by an object's animation program; here run once on entry): the dragon spawns its flame this way */
          for (int i = 0; i < n0; i++) { Obj self = g->rooms[g->cur].objs[i];
              for (int k = 0; k < g->nvm; k++) if (g->vm_pid[k] == (self.pid | 0x4000)) run_script(g, &self, g->vm_code[k], g->vm_len[k]); } }
        if (g->lvl1 && g->cur == 36) { static const uint8_t flame[] = { 0x24, 0x01, 0xe5, 51, 51, 38, 0xfe, 0x24, 0x01, 0xe5, 54, 51, 38, 0xfe, 0x24, 0x01, 0xe5, 57, 51, 38, 0xfe, 0x17 };   /* dragon flame (pid 485): three flames measured from the emulator (around (54,51,38)); the original spawns it from the dragon's animation event, not decoded yet */
            run_script(g, &g->hero, flame, (int)sizeof flame); }
        r = &g->rooms[g->cur]; }
    else if (g->tm_left > 0 && --g->tm_left == 0) {
        g->tm_left = g->rs_period[g->cur] * 17;
        for (int b = 0; b < g->rs_nb[g->cur]; b++) { const uint8_t *c = g->rs_blk[g->cur][b]; if ((c[1] & 0x7f) != 0x0e) continue;
            Vm m = { g, &g->hero, c, c[0], 2, 0, 0 }; vm_block(&m); }
    }
    if (g->status_open) { if ((joy & 0x1F) && !(prev & 0x1F)) g->status_open = 0; return; }              /* diary panel: waits for a key ($9d68) */
    if ((joy & JOY_DEBUG) && !(prev & JOY_DEBUG)) g->dbg ^= 1;
    { int ed = (joy & ~prev);                                                                  /* rucksack: Space = single item, Return = grid, H = hold */
      if ((ed & JOY_INV) && g->ninv) { g->inv_mode = g->inv_mode == 1 ? 0 : 1; g->menu = 0; if (g->inv_sel >= g->ninv) g->inv_sel = 0; }
      else if ((ed & JOY_LIST) && g->ninv) { g->inv_mode = g->inv_mode == 2 ? 1 : 2; g->menu = 0; if (g->inv_sel >= g->ninv) g->inv_sel = 0; }
      if (g->inv_mode && !g->ninv) g->inv_mode = 0;
      if (g->inv_mode && g->scroll_t <= 0) {
          int nd = (joy & 15) & ~(prev & 15), n = g->ninv, step = g->inv_mode == 2 ? 4 : 1;
          if (nd & JOY_RIGHT) g->inv_sel = (g->inv_sel + 1) % n;
          if (nd & JOY_LEFT) g->inv_sel = (g->inv_sel + n - 1) % n;
          if (g->inv_mode == 2) { if ((nd & JOY_DOWN) && g->inv_sel + step < n) g->inv_sel += step; if ((nd & JOY_UP) && g->inv_sel - step >= 0) g->inv_sel -= step; }
          if (ed & JOY_HOLD) g->inv_hold = (g->inv_hold == g->inv[g->inv_sel]) ? -1 : g->inv[g->inv_sel];
          if (ed & JOY_FIRE) inv_examine(g, g->inv_sel);
          if ((ed & JOY_DOWN) && g->inv_mode == 1) { inv_drop(g, g->inv_sel); }
      }
      if (g->inv_mode == 1) { g->nverbs = 2; g->verbs[0] = 0xb; g->verbs[1] = 5; } else if (g->inv_mode == 2) g->nverbs = 0;
      if (g->inv_mode && g->scroll_t <= 0) return;
    }
    if ((joy & JOY_MAP) && !(prev & JOY_MAP) && g->has_scroll) { g->map_open ^= 1; g->map_px = g->map_py = 0; }     /* F1: map of the rooms entered so far */
    if (g->map_open) {                                                                          /* the game waits; the joystick pans the view (1 cell per tick), fire closes it ($a3e8..$a4dc) */
        g->map_px += ((joy & JOY_RIGHT) ? 1 : 0) - ((joy & JOY_LEFT) ? 1 : 0); g->map_py += ((joy & JOY_DOWN) ? 1 : 0) - ((joy & JOY_UP) ? 1 : 0);
        if ((joy & JOY_FIRE) && !(prev & JOY_FIRE)) g->map_open = 0;
        return;
    }
    if (g->scroll_t > 0) { g->scroll_t--; return; }
    if (g->msg_t > 0) g->msg_t--;
    if (g->menu) {                                                                     /* verb menu: the hero stands still */
        if (g->target < 0 || g->nverbs == 0) g->menu = 0;
        else { if (fire_edge) { g->menu = 0; exec_verb(g, g->verbs[g->sel]); } else if (new_dirs) g->sel = move_sel(g, new_dirs); joy &= ~15; }
    } else if (fire_edge && held_ranged(g)) { fire_weapon(g); joy &= ~15;
    } else if (fire_edge && g->target >= 0 && g->nverbs > 0) { g->menu = 1; g->sel = 0; joy &= ~15; }
    else if (fire_edge && g->target < 0 && !g->jump_t && hero_grounded(g, r)) { g->jump_t = 1; g->jump_z0 = h->z; }    /* fire with nothing in front: jump */
    int dx = ((joy & JOY_RIGHT) ? 1 : 0) - ((joy & JOY_LEFT) ? 1 : 0);
    int dy = ((joy & JOY_DOWN) ? 1 : 0) - ((joy & JOY_UP) ? 1 : 0);
    g->tickno++;
    if (g->hurt_cd > 0) g->hurt_cd--;
    for (int i = 0; i < r->nobj; i++) {                      /* water lurker (slot 116): ripples, surfaces, opens its mouth, sinks, then returns to the pool until the next timer. Frame order and timing are read off the emulator, not decoded */
        Obj *c = &r->objs[i];
        if (g->lvl1 && c->slot == 111 && g->spr[111].nfr >= 2) { c->anim = g->tickno % 36 < 5 ? 1 : 0; continue; }   /* dragon head: mouth open for 5 of every 36 ticks (emulator, room 36; phase unknown) */
        if (g->lvl1 && c->slot == 121 && g->spr[121].nfr >= 4) { static const int fl[4] = { 0, 3, 2, 1 };
            if (!c->vx) c->vx = c->x;                       /* remember the resting x (the leftmost flame is the one the dragon shoots out) */
            c->anim = fl[(c->stage++ / 3 + c->vx) % 4];
            if (c->vx == 51) {   /* launched flame: screen offsets from its rest position, every 2 ticks from tick 2 of the 36-tick cycle (emulator, room 36) */
                static const int8_t off[15][2] = { {-29,-22}, {-26,-12}, {-25,1}, {-22,10}, {-21,13}, {-18,9}, {-17,10}, {-14,7}, {-13,8}, {-10,6}, {-9,7}, {-6,4}, {-5,5}, {-2,1}, {-1,2} };
                int k = (g->tickno % 36 + 34) % 36 / 2, dx = 0, dy = 0; if (k < 15) { dx = off[k][0]; dy = off[k][1]; }
                c->x = c->vx + dx / 2; c->lift = dx / 2 - dy; }   /* screen: dx = 2*dx_world, dy = dx_world - dz */
            continue; }   /* dragon flame: 4-frame loop, 3 ticks per frame (emulator, room 36) */
        if (g->spr[c->slot].nfr < 10) continue;
        if (g->lvl1 && c->slot == 112 && g->spr[112].nfr >= 16) {          /* worm: 78-tick cycle read off the emulator: hop (frame 15, arc), crawl (frames 12/13 alternating), rest (frame 14); gone after 3 cycles */
            static const int arc[10] = { 8, 18, 33, 46, 54, 52, 42, 28, 12, 0 };
            int t = (c->stage + 60) % 78; c->stage = c->home == g->cur ? t - 59 : c->stage + 1;   /* a fresh worm starts in its rest phase */
            if (c->home != g->cur && c->stage >= 78 * 3) { Obj back = *c; back.stage = 0; back.lift = 0; back.anim = 8; back.atick = 8; Room *hr = &g->rooms[back.home];
                r->objs[i] = r->objs[--r->nobj]; if (hr->valid) hr->objs[hr->nobj++] = back; i--; continue; }
            if (t < 30) { c->anim = 15; c->lift = arc[(t + 1) / 3 > 9 ? 9 : (t + 1) / 3]; }
            else if (t < 60) { c->anim = ((t - 30) / 3) % 2 ? 13 : 12; c->lift = 0; }
            else { c->anim = 14; c->lift = 0; }
            continue; }
        if (c->home == g->cur) continue;
        static const struct { int f, t; } seq[] = { {8, 8}, {9, 8}, {0, 6}, {3, 6}, {4, 16}, {3, 6}, {0, 6}, {9, 8}, {8, 8} };
        if (--c->atick > 0) continue;
        c->stage++;
        if (c->stage >= (int)(sizeof seq / sizeof seq[0])) {
            Obj back = *c; back.stage = 0; back.anim = 8; back.atick = 8; Room *hr = &g->rooms[back.home];
            r->objs[i] = r->objs[--r->nobj]; if (hr->valid) hr->objs[hr->nobj++] = back; i--; continue; }
        c->anim = seq[c->stage].f; c->atick = seq[c->stage].t;
    }
    for (int i = 0; i < r->nobj; i++) { Obj *c = &r->objs[i]; int sl = c->slot; if (g->lvl1 ? (sl < 113 || sl > 119 || sl == 116) : (sl < 112 || sl > 123)) continue;      /* path-following creatures */
        creature_move(g, r, c);
        /* walk frames per facing (0 = +x, 1 = +y, 2 = -y, 3 = -x), from the type record programs (sections in the order -x, +y, -y, +x); slots 113 and 119 verified against the emulator for +x/-x/+y */
        static const struct { int slot, nf, ticks, base[4]; } walk[] = { { 113, 3, 3, { 0, 3, 6, 9 } }, { 117, 3, 3, { 3, 0, 9, 6 } }, { 119, 2, 6, { 2, 0, 4, 6 } } };
        for (int k = 0; g->lvl1 && k < (int)(sizeof walk / sizeof walk[0]); k++) if (walk[k].slot == sl && g->spr[sl].nfr >= walk[k].base[3] + walk[k].nf)
            c->anim = walk[k].base[c->mdir] + (g->tickno / walk[k].ticks) % walk[k].nf; }
    if (g->hurt_cd == 0) {                                  /* contact scripts (event 9 with the hero): creatures, fires, spikes. Original posts it every tick of contact; a 1 s repeat is a guess */
        Box hb = box_of(g, h);
        for (int i = 0; i < r->nobj && g->hurt_cd == 0; i++) {
            Obj *q = &r->objs[i]; int has = 0;
            for (int k = 0; k < g->nvm; k++) if (g->vm_pid[k] == (q->pid | 0x8000)) { has = 1; break; }
            if (!has || (q->slot == 116 && q->anim >= 8)) continue;
            Box qb = box_of(g, q);
            if (qb.x1 + 1 < hb.x0 || hb.x1 + 1 < qb.x0 || qb.y1 + 1 < hb.y0 || hb.y1 + 1 < qb.y0 || qb.z1 < hb.z0 || hb.z1 < qb.z0) continue;
            for (int k = 0; k < g->nvm; k++) if (g->vm_pid[k] == (q->pid | 0x8000)) { run_script(g, q, g->vm_code[k], g->vm_len[k]); g->hurt_cd = 25; }
        }
    }
    if (g->pull_pid && g->pull_room != g->cur) g->pull_pid = 0;
    update_missiles(g);
    g->moving = 0;
    if (dx || dy) {
        g->facing = joy & 15; g->moving = 1;
        Obj *pulled = NULL;                                                      /* MOVE (pull mode): the chosen object follows when the hero walks away from it */
        if (g->pull_pid && g->pull_room == g->cur) {
            for (int i = 0; i < r->nobj; i++) if (r->objs[i].pid == g->pull_pid) { pulled = &r->objs[i]; break; }
            if (pulled) { Obj gh = *h; gh.x -= dx; gh.y -= dy; if (!overlap(box_of(g, &gh), box_of(g, pulled))) pulled = NULL; }
        }
        if (try_move(g, r, h, dx, dy, 0)) { if (pulled) try_move(g, r, pulled, dx, dy, 0); }
        else {
            int nx = h->x + dx, ny = h->y + dy;
            int out = nx >= r->sx * 8 || ny >= r->sy * 8 || nx - HERO_FX + 1 < MARGIN || ny - HERO_FY + 1 < MARGIN;
            if (out) try_exit(g, dx, dy); else try_step_up(g, r, dx, dy);
        }
    }
    if (g->moving && (dx || dy)) {   /* small objects being pushed slide in line with the hero's feet instead of riding on the edge of his box */
        Box hb = box_of(g, h);
        for (int i = 0; i < r->nobj; i++) {
            Obj *q = &r->objs[i]; if (flat(g, q) || !pushable(g, q)) continue;
            Box qb = box_of(g, q); int qw = dx ? qb.y1 - qb.y0 + 1 : qb.x1 - qb.x0 + 1, hw = dx ? hb.y1 - hb.y0 + 1 : hb.x1 - hb.x0 + 1;
            if (qw >= hw || qb.z0 > hb.z1 || qb.z1 < hb.z0) continue;
            int front = dx ? (dx > 0 ? qb.x0 == hb.x1 + 1 : qb.x1 == hb.x0 - 1) && !(qb.y1 < hb.y0 || hb.y1 < qb.y0)
                           : (dy > 0 ? qb.y0 == hb.y1 + 1 : qb.y1 == hb.y0 - 1) && !(qb.x1 < hb.x0 || hb.x1 < qb.x0);
            if (!front) continue;
            int hc = dx ? hb.y0 + hb.y1 : hb.x0 + hb.x1, qc = dx ? qb.y0 + qb.y1 : qb.x0 + qb.x1;
            if (abs(hc - qc) < 2) continue;
            int s = hc > qc ? 1 : -1; try_move(g, r, q, dx ? 0 : s, dx ? s : 0, 0);
        }
    }
    {   /* objects interpenetrating the hero (arrival in a room, spawns): push pushable ones out along the dominant axis, as walking into them would */
        Box hb = box_of(g, h); int hcx = hb.x0 + hb.x1, hcy = hb.y0 + hb.y1;
        for (int i = 0; i < r->nobj; i++) {
            Obj *q = &r->objs[i]; if (flat(g, q) || !pushable(g, q)) continue;
            Box qb = box_of(g, q); if (!overlap(hb, qb)) continue;
            int ex = (qb.x0 + qb.x1) - hcx, ey = (qb.y0 + qb.y1) - hcy, sx = 0, sy = 0;
            if (abs(ex) >= abs(ey)) sx = ex >= 0 ? 1 : -1; else sy = ey >= 0 ? 1 : -1;
            int hz = h->z; h->z = hz + 1000;                                     /* lets try_move ignore the hero while separating */
            if (!try_move(g, r, q, sx, sy, 0)) { int ox = sx, oy = sy; sx = oy ? (ex >= 0 ? 1 : -1) : 0; sy = ox ? (ey >= 0 ? 1 : -1) : 0; if (sx || sy) try_move(g, r, q, sx, sy, 0); }
            h->z = hz;
        }
    }
    if (g->jump_t) {
        int prevz = h->z, nz = g->jump_z0 + JUMP_Z[g->jump_t - 1], land = -1;
        if (g->jump_t > 10 || nz < prevz) {                                   /* descending: land on the highest object top passed through */
            Box hb = box_of(g, h);
            for (int i = 0; i < r->nobj; i++) {
                const Obj *q = &r->objs[i]; if (flat(g, q)) continue;
                Box qb = box_of(g, q);
                if (qb.x1 < hb.x0 || hb.x1 < qb.x0 || qb.y1 < hb.y0 || hb.y1 < qb.y0) continue;
                int top = qb.z1 + 1; if (prevz >= top && nz <= top && top > land) land = top;
            }
        }
        if (land >= 0) { h->z = land; g->jump_t = 0; }
        else { h->z = nz; if (++g->jump_t > 20) g->jump_t = 0; }
    }
    else hero_gravity(g, r);
    /* Walking animation + footsteps, timed from an emulator trace of the original: each of the 4 walk frames is shown
       for 3 logic ticks; a footstep (sample 22, period 320, volume 8, cut after 4 video frames = 80 ms) sounds when the
       script reaches its 2nd and 4th frame (the `$fc 00` entries of the walk scripts). */
    if (g->moving) {
        int step = g->walk_ticks / WALK_TICKS_PER_FRAME;
        if (g->walk_ticks % WALK_TICKS_PER_FRAME == 0 && (step & 1) && !g->jump_t && hero_grounded(g, r)) play_sound(g, 0);   /* no footsteps in the air */
        g->walk_step = step & 3; g->walk_ticks++;
    } else g->walk_ticks = 0;
    g->anim_t++;
    if (g->bump_timer > 0) g->bump_timer--;
    { int old = g->target; r = &g->rooms[g->cur]; g->target = facing_target(g); if (g->target != old) { g->menu = 0; g->msg_t = 0; } build_verbs(g); }
}

/* ---------------------------------------------------------------- rendering */
static void project(const Game *g, int org, int x, int y, int z, const int16_t *F, int *px, int *py) {
    int ci = x >> 3, cj = y >> 3, xs = x & 7, ys = y & 7;
    int base = org + 322 * ci + 318 * cj;
    int bl = base / 40, bb = base % 40; if (bb < 0) { bb += 40; bl--; }
    int off = g->sub[ys * 8 + xs][0], sh = g->sub[ys * 8 + xs][1];
    int ol = (int)lrint((double)off / 40.0), ob = off - 40 * ol;
    *px = (bb + ob) * 8 + sh - 16 * (F[1] - 1);
    *py = bl + ol - (z - F[0] + F[2] + F[3] + F[4]);
}

/* Energy bar (HUD box x277..297, y143..147): colours sampled from the original; filled left to right, the rest is black. */
static const uint32_t ENERGY_BAR[5][21] = {
    {0x222222,0x662200,0x222222,0x884400,0x444422,0x884400,0x444422,0xaa6622,0x666666,0xaa6622,0x666666,0xcc8844,0x888888,0xcc8844,0x888888,0xeeaa44,0xaaaaaa,0xeeaa44,0xaaaaaa,0xeeaa44,0xaaaaaa},
    {0x662200,0x222222,0x662200,0x444422,0x884400,0x444422,0x884400,0x666666,0xaa6622,0x666666,0xaa6622,0x888888,0xcc8844,0x888888,0xcc8844,0xaaaaaa,0xeeaa44,0xaaaaaa,0xeeaa44,0xaaaaaa,0xeeaa44},
    {0x222222,0x662200,0x222222,0x884400,0x444422,0x884400,0x444422,0xaa6622,0x666666,0xaa6622,0x666666,0xcc8844,0x888888,0xcc8844,0x888888,0xeeaa44,0xaaaaaa,0xeeaa44,0xaaaaaa,0xeeaa44,0xaaaaaa},
    {0x662200,0x222222,0x662200,0x444422,0x884400,0x444422,0x884400,0x666666,0xaa6622,0x666666,0xaa6622,0x888888,0xcc8844,0x888888,0xcc8844,0xaaaaaa,0xeeaa44,0xaaaaaa,0xeeaa44,0xaaaaaa,0xeeaa44},
    {0x222222,0x662200,0x222222,0x884400,0x444422,0x884400,0x444422,0xaa6622,0x666666,0xaa6622,0x666666,0xcc8844,0x888888,0xcc8844,0x888888,0xeeaa44,0xaaaaaa,0xeeaa44,0xaaaaaa,0xeeaa44,0xaaaaaa}
};
static void draw_energy(const Game *g, uint32_t *fb) {
    int n = (g->energy * 21 + ENERGY_MAX - 1) / ENERGY_MAX; if (g->energy <= 0) n = 0;
    for (int y = 0; y < 5; y++) for (int x = 0; x < 21; x++) {
        uint32_t c = x < n ? ENERGY_BAR[y][x] : 0; fb[(143 + y) * SCREEN_W + 277 + x] = (c >> 16 & 0xFF) | (c & 0xFF00) | (c & 0xFF) << 16 | 0xFF000000u;
    }
}
static void blit(uint32_t *fb, const uint8_t *px, int w, int h, int x0, int y0) {
    for (int y = 0; y < h; y++) {
        int Y = y0 + y; if (Y < 0 || Y >= SCREEN_H) continue;
        for (int x = 0; x < w; x++) {
            int X = x0 + x; if (X < 0 || X >= SCREEN_W) continue;
            const uint8_t *p = px + ((size_t)y * w + x) * 4;
            if (p[3]) fb[Y * SCREEN_W + X] = p[0] | p[1] << 8 | p[2] << 16 | 0xFF000000u;
        }
    }
}

/* Door pixels covered by the wall jamb, chosen per pixel against the original on 37 Level 1 rooms (bit x of row y).
   DOOR_HIDE: always covered. DOOR_BG: covered only where the room background is wall (a narrow wall leaves the black gap, so the door shows). */
static const unsigned long long DOOR_HIDE[2][57] = {
{0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL},
{0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL}};
static const unsigned long long DOOR_BG[2][57] = {
{0x000c00000000ULL,0x000700000000ULL,0x0001c0000000ULL,0x0000f0000000ULL,0x0000fc000000ULL,0x0000ff000000ULL,0x0000ffc00000ULL,0x0000fff00000ULL,0x0000fffc0000ULL,0x0000ffff0000ULL,0x000027ffc000ULL,0x00003ffff000ULL,0x00000ffffc00ULL,0x000001ffff00ULL,0x000000ffff00ULL,0x0000003fff00ULL,0x0000000fff00ULL,0x00000003ff00ULL,0x00000000ff00ULL,0x000000007f00ULL,0x000000007f00ULL,0x000000007f00ULL,0x000000007f00ULL,0x000000007f00ULL,0x000000007f00ULL,0x000000007f00ULL,0x000000007f00ULL,0x000000007f00ULL,0x000000007f00ULL,0x000000007f00ULL,0x000000003f00ULL,0x000000003f00ULL,0x000000003f00ULL,0x000000003f00ULL,0x000000003f00ULL,0x000000003f00ULL,0x000000007f00ULL,0x000000007f00ULL,0x000000007f00ULL,0x000000007f00ULL,0x000000007f00ULL,0x000000007f00ULL,0x000000007f00ULL,0x000000007f00ULL,0x000000005f00ULL,0x000000005f00ULL,0x000000003f00ULL,0x000000003f00ULL,0x000000007f00ULL,0x000000007f00ULL,0x000000007f00ULL,0x000000003f00ULL,0x000000000f00ULL,0x000000000300ULL,0x000000000000ULL,0x000000000000ULL,0x000000000000ULL},
{0x000000018000ULL,0x000000078000ULL,0x0000001f0000ULL,0x0000007f0000ULL,0x000001fe0000ULL,0x000007f80000ULL,0x00001fe00000ULL,0x00007f800000ULL,0x0001fe000000ULL,0x0007f8000000ULL,0x001fe0000000ULL,0x007f80000000ULL,0x01ff00000000ULL,0x07ff00000000ULL,0x07ff00000000ULL,0x07ff00000000ULL,0x07ff00000000ULL,0x07ff00000000ULL,0x07ff00000000ULL,0x07fe00000000ULL,0x07ff00000000ULL,0x07fd00000000ULL,0x07ff00000000ULL,0x07fe00000000ULL,0x07fc00000000ULL,0x07fc00000000ULL,0x07fc00000000ULL,0x07fc00000000ULL,0x07fc00000000ULL,0x07fc00000000ULL,0x07fc00000000ULL,0x07fc00000000ULL,0x07fc00000000ULL,0x07fd00000000ULL,0x07ff00000000ULL,0x07ff00000000ULL,0x07ff00000000ULL,0x07fb00000000ULL,0x07fe00000000ULL,0x07ff00000000ULL,0x07ff00000000ULL,0x07fb00000000ULL,0x07fd00010000ULL,0x07ff00000000ULL,0x07ff00000000ULL,0x07ff00000000ULL,0x07ff00000000ULL,0x07fd00000000ULL,0x07fd00000000ULL,0x07fd00000000ULL,0x07ff00000000ULL,0x07fc00000000ULL,0x07f000000000ULL,0x07c000000000ULL,0x078000000000ULL,0x078000000000ULL,0x020000000000ULL}};
static void blit_door(uint32_t *fb, const Sprite *s, int slot, int x0, int y0) {
    for (int y = 0; y < s->h && y < 57; y++) {
        int Y = y0 + y; if (Y < 0 || Y >= SCREEN_H) continue;
        for (int x = 0; x < s->w && x < 48; x++) {
            int X = x0 + x; if (X < 0 || X >= SCREEN_W) continue;
            if (DOOR_HIDE[slot][y] >> x & 1) continue;
            if ((DOOR_BG[slot][y] >> x & 1) && (fb[Y * SCREEN_W + X] & 0xFFFFFFu) != 0) continue;
            const uint8_t *p = s->px + ((size_t)y * s->w + x) * 4;
            if (p[3]) fb[Y * SCREEN_W + X] = p[0] | p[1] << 8 | p[2] << 16 | 0xFF000000u;
        }
    }
}
typedef struct { int key, x, y, z, slot; int hero; Box b; int solid; int alt; int fr; } DrawItem;
static int cmp_item(const void *a, const void *b) {
    const DrawItem *p = a, *q = b;
    if (p->key != q->key) return p->key - q->key;
    if (p->x != q->x) return p->x - q->x;
    if (p->y != q->y) return p->y - q->y;
    return p->z - q->z;
}

static int hero_frame(const Game *g) {
    int f = g->facing;
    const int *a; int idle_frame;
    if (f & JOY_RIGHT) { a = walk_right; idle_frame = 16; }
    else if (f & JOY_LEFT) { a = walk_left; idle_frame = 21; }
    else if (f & JOY_DOWN) { a = walk_down; idle_frame = 26; }
    else { a = walk_up; idle_frame = 31; }
    return g->moving ? a[g->walk_step] : idle_frame;
}

static void draw_text(const Game *g, uint32_t *fb, const char *str, int x, int y, int width);
/* Original text printer ($10f82): 8x6 glyphs, advance 7, centred in a box; glyph pixel value = palette index (0 = clear). */
static void draw_text_clip(const Game *g, uint32_t *fb, const char *str, int x, int y, int width, int clip0, int clip1, int scroll) {
    int len = (int)strlen(str); x += (width - len * 7) / 2 - scroll;
    for (const char *c = str; *c; c++, x += 7) {
        int ch = (unsigned char)*c, gi;
        if (ch == ' ' || ch < 0x21) continue;
        gi = ch >= 0x41 ? ch - 0x41 : g->fontmap[ch - 0x21];
        if (gi >= 46) continue;
        const uint8_t *gl = g->font + gi * 24;
        for (int row = 0; row < 6; row++) for (int col = 0; col < 8; col++) {
            int v = 0; for (int p = 0; p < 4; p++) v |= ((gl[row * 4 + p] >> (7 - col)) & 1) << p;
            int X = x + col, Y = y + row; if (!v || X < clip0 || X >= clip1 || X < 0 || X >= SCREEN_W || Y < 0 || Y >= SCREEN_H) continue;
            fb[Y * SCREEN_W + X] = g->pal[v][0] | g->pal[v][1] << 8 | g->pal[v][2] << 16 | 0xFF000000u;
        }
    }
}
static void draw_text(const Game *g, uint32_t *fb, const char *str, int x, int y, int width) { draw_text_clip(g, fb, str, x, y, width, 0, SCREEN_W, 0); }

/* Rucksack display (verified in the original for the single-item view): a 34x34 box at (266,142), background colour 14, the item's sprite centred in it, the item name in the bottom strip. */
static void blit_px(uint32_t *fb, const uint8_t *px, int w, int h, int x0, int y0) {
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        const uint8_t *p = px + (y * w + x) * 4; if (!p[3]) continue; int X = x0 + x, Y = y0 + y;
        if (X < 0 || Y < 0 || X >= SCREEN_W || Y >= SCREEN_H) continue;
        fb[Y * SCREEN_W + X] = p[0] | p[1] << 8 | p[2] << 16 | 0xFF000000u; }
}
static void fill_box(const Game *g, uint32_t *fb, int x0, int y0, int w, int h, int idx) {
    uint32_t c = g->pal[idx][0] | g->pal[idx][1] << 8 | g->pal[idx][2] << 16 | 0xFF000000u;
    for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) if (x >= 0 && y >= 0 && x < SCREEN_W && y < SCREEN_H) fb[y * SCREEN_W + x] = c;
}
static void draw_rucksack(const Game *g, uint32_t *fb) {
    int sel = g->inv_sel; const Sprite *s = &g->spr[g->inv_slot[sel]];
    if (g->inv_mode == 2) {                                                                   /* whole rucksack: grid of item boxes over the room */
        int cols = 4, x0 = 96, y0 = 24;
        fill_box(g, fb, x0 - 6, y0 - 6, (g->ninv < cols ? g->ninv : cols) * 36 + 12, ((g->ninv + cols - 1) / cols) * 36 + 12, 0);
        for (int i = 0; i < g->ninv; i++) {
            int bx = x0 + (i % cols) * 36, by = y0 + (i / cols) * 36; const Sprite *q = &g->spr[g->inv_slot[i]];
            fill_box(g, fb, bx, by, 34, 34, i == sel ? 10 : 14);
            fill_box(g, fb, bx + 1, by + 1, 32, 32, 14);
            if (q->state == 1 && q->px) blit_px(fb, q->px, q->w, q->h, bx + 17 - q->w / 2, by + 17 - q->h / 2);
            if (g->inv[i] == g->inv_hold) fill_box(g, fb, bx + 1, by + 1, 4, 4, 10);
        }
    }
    fill_box(g, fb, 266, 142, 34, 34, 0); fill_box(g, fb, 267, 143, 32, 32, 14);
    if (s->state == 1 && s->px) blit_px(fb, s->px, s->w, s->h, 283 - s->w / 2, 159 - s->h / 2);
    if (g->inv[sel] == g->inv_hold) fill_box(g, fb, 268, 144, 4, 4, 10);
    if (s->name >= 0 && s->name < g->ntext) { fill_box(g, fb, 205, 185, 108, 8, 0); draw_text(g, fb, g->text[s->name], 205, 186, 108); }
}

/* ---- examine scroll: parchment built from 8x8 tiles (layout table at $55e8), text in the 5-row font, word-wrapped like the original ($dc98). */
static void scroll_tile(const Game *g, uint32_t *fb, int idx, int x0, int y0) {
    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) {
        int v = 0; for (int p = 0; p < 4; p++) v |= ((g->scroll_tiles[idx * 32 + r * 4 + p] >> (7 - c)) & 1) << p;
        int X = x0 + c, Y = y0 + r; if (X < 0 || X >= SCREEN_W || Y < 0 || Y >= SCREEN_H) continue;
        fb[Y * SCREEN_W + X] = g->pal[v][0] | g->pal[v][1] << 8 | g->pal[v][2] << 16 | 0xFF000000u;
    }
}
static void draw_panel(const Game *g, uint32_t *fb, int x0, int y0, int wt, int body) {
    const uint8_t *T = g->scroll_map; int rowy = y0;
    #define TROW(l, m, r) do { scroll_tile(g, fb, (l), x0, rowy); for (int k = 0; k < wt; k++) scroll_tile(g, fb, (m), x0 + 8 + 8 * k, rowy); scroll_tile(g, fb, (r), x0 + 8 + 8 * wt, rowy); rowy += 8; } while (0)
    TROW(T[0], T[1], T[2]); TROW(T[3], T[4], T[5]);
    for (int b = 0; b < body; b++) { unsigned h = (unsigned)(b * 2654435761u) >> 7; TROW(T[6 + (h & 7)], 0xd, T[14 + ((h >> 3) & 7)]); }
    TROW(T[22], T[23], T[24]); TROW(T[25], T[26], T[27]);
    #undef TROW
}
static void put5(const Game *g, uint32_t *fb, const char *s, int x, int y) {
    for (const char *c = s; *c; c++, x += 6) {
        int ch = (unsigned char)*c, gi; if (ch == ' ' || ch < 0x21) continue;
        gi = ch >= 0x41 ? ch - 0x41 : g->fontmap[ch - 0x21]; if (gi >= 46) continue;
        for (int row = 0; row < 5; row++) for (int col = 0; col < 8; col++) {
            int v = 0; for (int p = 0; p < 4; p++) v |= ((g->font5[gi * 20 + row * 4 + p] >> (7 - col)) & 1) << p;
            int X = x + col, Y = y + row; if (!v || X < 0 || X >= SCREEN_W || Y < 0 || Y >= SCREEN_H) continue;
            fb[Y * SCREEN_W + X] = g->pal[v][0] | g->pal[v][1] << 8 | g->pal[v][2] << 16 | 0xFF000000u;
        }
    }
}
/* F1 map: 2 px per world cell, centred on the current room; entered rooms are outlined (current one in yellow). */
static void draw_map(const Game *g, uint32_t *fb) {
    draw_panel(g, fb, 96, 30, 14, 9);
    const Room *cur = &g->rooms[g->cur]; int n = 0;
    uint32_t dark = g->pal[5][0] | g->pal[5][1] << 8 | g->pal[5][2] << 16 | 0xFF000000u, bright = g->pal[10][0] | g->pal[10][1] << 8 | g->pal[10][2] << 16 | 0xFF000000u;
    for (int pass = 0; pass < 2; pass++) for (int i = 0; i < 100; i++) {
        const Room *r = &g->rooms[i]; if (!r->valid || !g->visited[i] || (i == g->cur) != (pass == 1)) continue;
        if (!pass) n++; else n++;
        int x = 160 + 2 * r->x0 - (2 * cur->x0 + cur->sx) - 2 * g->map_px, y = 78 + 2 * r->y0 - (2 * cur->y0 + cur->sy) - 2 * g->map_py, w = 2 * r->sx, h = 2 * r->sy;
        uint32_t col = pass ? bright : dark;
        for (int k = 0; k <= w; k++) for (int e = 0; e < 2; e++) { int X = x + k, Y = y + (e ? h : 0); if (X >= 104 && X < 216 && Y >= 38 && Y < 126) fb[Y * SCREEN_W + X] = col; }
        for (int k = 0; k <= h; k++) for (int e = 0; e < 2; e++) { int X = x + (e ? w : 0), Y = y + k; if (X >= 104 && X < 216 && Y >= 38 && Y < 126) fb[Y * SCREEN_W + X] = col; }
    }
    char buf[64]; snprintf(buf, sizeof buf, "%s%d", g->ntext > 42 ? g->text[42] : "ROOMS ENTERED ", n); put5(g, fb, buf, 107, 116);
}
/* Diary panel ($9c7c): 6 lines of text 13 on the left, the values at x+0x42; the title comes from the gold table (only the first rank is known). */
static void draw_status(const Game *g, uint32_t *fb) {
    draw_panel(g, fb, 68, 10, 16, 4);
    char line[16][48]; int nl = 0; const char *t = g->ntext > 13 ? g->text[13] : "";
    for (const char *p = t; *p && nl < 16;) { int n = 0; while (p[n] && p[n] != '\n' && p[n] != '\r') n++; if (n > 47) n = 47; memcpy(line[nl], p, (size_t)n); line[nl][n] = 0; nl++; p += n; if (*p) p++; }
    for (int i = 0; i < nl; i++) put5(g, fb, line[i], 78, 23 + 6 * i);
    int rooms = 0, seen = 0; for (int i = 0; i < 100; i++) if (g->rooms[i].valid) { rooms++; seen += g->visited[i]; }
    char b[48];
    snprintf(b, sizeof b, "%d/%d", g->energy, ENERGY_MAX); put5(g, fb, b, 134, 23);
    if (g->gold) { snprintf(b, sizeof b, "%ld", g->gold); put5(g, fb, b, 134, 29); }
    if (g->xp) { snprintf(b, sizeof b, "%ld", g->xp); put5(g, fb, b, 134, 35); }
    snprintf(b, sizeof b, "%ld", g->level); put5(g, fb, b, 134, 41);
    snprintf(b, sizeof b, "%d%%", rooms ? seen * 100 / rooms : 0); put5(g, fb, b, 134, 47);
    if (g->ntext > 63) put5(g, fb, g->text[63], 134, 53);
}
static void draw_scroll(const Game *g, uint32_t *fb) {
    const char *str = g->scroll_str; int len = (int)strlen(str);
    int maxw = 12, d0 = len - 48; if (d0 >= 0) { do { maxw++; d0 -= maxw; } while (d0 > 0); }        /* chars per line grows with the text */
    char lines[16][48]; int nl = 0;
    for (const char *p = str; *p && nl < 16;) {
        int n = 0, best = 0, k = 0;                                                    /* longest run of whole words that fits */
        for (;;) {
            while (p[k] && p[k] != ' ') k++;
            if (k > maxw) break;
            best = k; if (!p[k]) break; k++;
            (void)n;
        }
        if (best == 0) { best = k > 47 ? 47 : k; }                                     /* a word longer than the line: break it */
        memcpy(lines[nl], p, (size_t)best); lines[nl][best] = 0; nl++;
        p += best; if (*p == ' ') p++;
    }
    int wt = maxw * 6 / 8, body = nl * 6 / 8, x0 = 32, y0 = 20, W = (wt + 2) * 8;
    draw_panel(g, fb, x0, y0, wt, body);
    for (int l = 0; l < nl; l++) {
        int x = x0 + (W - (int)strlen(lines[l]) * 6) / 2, y = y0 + 13 + 6 * l;
        for (const char *c = lines[l]; *c; c++, x += 6) {
            int ch = (unsigned char)*c, gi; if (ch == ' ' || ch < 0x21) continue;
            gi = ch >= 0x41 ? ch - 0x41 : g->fontmap[ch - 0x21]; if (gi >= 46) continue;
            for (int row = 0; row < 5; row++) for (int col = 0; col < 8; col++) {
                int v = 0; for (int p = 0; p < 4; p++) v |= ((g->font5[gi * 20 + row * 4 + p] >> (7 - col)) & 1) << p;
                int X = x + col, Y = y + row; if (!v || X < 0 || X >= SCREEN_W || Y < 0 || Y >= SCREEN_H) continue;
                fb[Y * SCREEN_W + X] = g->pal[v][0] | g->pal[v][1] << 8 | g->pal[v][2] << 16 | 0xFF000000u;
            }
        }
    }
}

void game_render(const Game *g, uint32_t *fb) {
    const Room *r = &g->rooms[g->cur];
    memcpy(fb, r->bg, SCREEN_W * SCREEN_H * 4);
    DrawItem items[256]; int n = 0;
    for (int i = 0; i < r->nobj && n < 255; i++) {
        const Obj *o = &r->objs[i];
        items[n++] = (DrawItem){ flat(g, o) ? -1000 : o->x + o->y, o->x, o->y, o->z + o->lift, o->slot, 0, box_of(g, o), !flat(g, o), (o->flags & 1) && g->spr[o->slot].apx != NULL, g->spr[o->slot].nfr ? o->anim : -1 };
    }
    for (int i = 0; i < r->nwalls && n < 255; i++) {
        const Wall *w = &r->walls[i]; const Sprite *s = &g->spr[w->slot];
        if (s->state == 1 && s->h > 1) items[n++] = (DrawItem){ -999, w->wx + s->fx - 1, w->wy + s->fy - 1, (w->z - r->basez) * 16, w->slot, 0, {0}, 0, 0, -1 };   /* door slabs sit in the far walls: always behind the hero and objects */
    }
    items[n++] = (DrawItem){ g->hero.x + g->hero.y, g->hero.x, g->hero.y, g->hero.z, 8, 1, box_of(g, &g->hero), 1 };
    qsort(items, n, sizeof items[0], cmp_item);
    {   /* depth order: an item that is strictly behind (smaller x or y) or underneath another one is drawn first, so things standing on a table
           are not hidden by it; the x+y sort above is only the tie-break. */
        DrawItem ord[256]; char done[256] = {0}; int m = 0;
        while (m < n) {
            int pick = -1;
            for (int i = 0; i < n && pick < 0; i++) {
                if (done[i]) continue;
                int blocked = 0;
                for (int j = 0; j < n && !blocked; j++) {
                    if (done[j] || j == i || !items[i].solid || !items[j].solid) continue;
                    const Box *A = &items[j].b, *B = &items[i].b;               /* does j have to come before i? */
                    int xy = !(A->x1 < B->x0 || B->x1 < A->x0 || A->y1 < B->y0 || B->y1 < A->y0);
                    if (xy) {
                        if (A->z1 < B->z0) blocked = 1;                         /* j is entirely underneath i */
                        else if (B->z1 < A->z0) blocked = 0;                    /* i is entirely underneath j */
                        else blocked = (A->x0 + A->x1 + A->y0 + A->y1) < (B->x0 + B->x1 + B->y0 + B->y1);   /* interpenetrating (e.g. the hero standing on a coin): the one whose centre is further back goes first */
                    }
                    else { int bh = A->x1 < B->x0 || A->y1 < B->y0, rv = B->x1 < A->x0 || B->y1 < A->y0; blocked = bh && !rv; }
                }
                if (!blocked) pick = i;
            }
            if (pick < 0) for (int i = 0; i < n; i++) if (!done[i]) { pick = i; break; }
            done[pick] = 1; ord[m++] = items[pick];
        }
        memcpy(items, ord, sizeof(DrawItem) * (size_t)n);
    }
    for (int i = 0; i < n; i++) {
        const DrawItem *d = &items[i]; const Sprite *s = &g->spr[d->slot];
        int px, py; project(g, r->org, d->x, d->y, d->z, s->f, &px, &py);
        if (d->hero) {
            const Frame *fr = &g->frames[hero_frame(g) % g->nframes];
            blit(fb, fr->px, fr->w, fr->h, px + 2 + fr->hx, py + fr->hy);
        } else if (d->fr >= 0 && d->fr < s->nfr) { blit(fb, s->fr[d->fr].px, s->fr[d->fr].w, s->fr[d->fr].h, px + s->fr[d->fr].dx, py + s->fr[d->fr].dy);
        } else if (d->alt) blit(fb, s->apx, s->aw, s->ah, px + s->ax, py + s->ay);
        else if (s->state == 1 && d->key == -999 && d->slot < 2) blit_door(fb, s, d->slot, px, py);
        else if (s->state == 1) blit(fb, s->px, s->w, s->h, px, py);
    }
    if (g->hud) { blit(fb, g->hud, SCREEN_W, SCREEN_H, 0, 0); }     /* bottom panel frame over the room */
    if (g->text) {
        char buf[96];
        snprintf(buf, sizeof buf, "%s %d", g->ntext > 29 ? g->text[29] : "DAY", g->day);
        if (!g->inv_mode) draw_text(g, fb, buf, 252, 158, 60);
        buf[0] = 0;
        if (r->prefix >= 0 && r->prefix < g->ntext) { strncat(buf, g->text[r->prefix], 40); strcat(buf, " "); }
        if (r->name < 19 && 126 + r->name < g->ntext) strncat(buf, g->text[126 + r->name], 40);
        int nm = g->msg_t > 0 ? g->msg : (g->target >= 0 ? g->spr[r->objs[g->target].slot].name : -1);
        int longmsg = nm >= 0 && nm < g->ntext && (int)strlen(g->text[nm]) * 7 > 84;      /* too wide for the item box: use the room-name strip */
        if (longmsg) {
            int w = (int)strlen(g->text[nm]) * 7, sc = 0;
            if (w > 108) { int t = 50 - g->msg_t; sc = t * (w - 108) / 40; if (sc > w - 108) sc = w - 108; if (sc < 0) sc = 0; }   /* scroll when wider than the strip */
            draw_text_clip(g, fb, g->text[nm], 202, 186, w > 108 ? w : 108, 202, 311, sc);
        } else {
            draw_text(g, fb, buf, 205, 186, 108);
            if (!g->inv_mode && nm >= 0 && nm < g->ntext) draw_text(g, fb, g->text[nm], 229, 172, 84);
        }
        if (g->has_icons) for (int i = 0; i < g->nverbs; i++) {                       /* verb icons in the six HUD slots */
            int ic = g->verbs[i], sx = g->slotpos[i][0], sy = g->slotpos[i][1];
            if (ic < 0 || ic >= 20 || sx < 1 || sy < 1 || sx + 17 > SCREEN_W || sy + 15 > SCREEN_H) continue;
            const uint8_t *src = g->icons + ic * 112;
            for (int y = 0; y < 14; y++) for (int x = 0; x < 16; x++) {
                int v = 0; for (int pl = 0; pl < 4; pl++) { int w = (src[pl * 28 + y * 2] << 8) | src[pl * 28 + y * 2 + 1]; v |= ((w >> (15 - x)) & 1) << pl; }
                if (!v) continue;
                fb[(sy + y) * SCREEN_W + sx + x] = g->pal[v][0] | g->pal[v][1] << 8 | g->pal[v][2] << 16 | 0xFF000000u;
            }
            if (g->menu && i == g->sel) {                                                /* highlight: 2px ring (colour 10 on black, 14 on the slot background) */
                for (int y = sy - 1; y <= sy + 14; y++) for (int x = sx - 1; x <= sx + 15; x++) {
                    int edge = (y == sy - 1 || y == sy + 14 || x == sx - 1 || x == sx + 15), inner = (y == sy || y == sy + 13 || x == sx || x == sx + 14);
                    if (!edge && !inner) continue;
                    uint32_t *px = &fb[y * SCREEN_W + x]; uint32_t bg = g->pal[9][0] | g->pal[9][1] << 8 | g->pal[9][2] << 16 | 0xFF000000u;
                    int idx = edge ? 10 : 14;
                    if (edge ? (*px == 0xFF000000u || x == sx + 15) : (*px == bg)) *px = g->pal[idx][0] | g->pal[idx][1] << 8 | g->pal[idx][2] << 16 | 0xFF000000u;
                }
            }
        }   /* measured against the original: 3 px right of the nominal box */
    }
    if (g->inv_mode && g->ninv) draw_rucksack(g, fb);
    else if (g->hud) draw_energy(g, fb);
    if (g->scroll_t > 0) draw_scroll(g, fb);
    if (g->map_open && g->has_scroll) draw_map(g, fb);
    if (g->status_open && g->has_scroll) draw_status(g, fb);
}

/* Debug aid for the headless driver: put the hero somewhere. */
void game_warp(Game *g, int room, int x, int y, int z) { if (room >= 0 && room < 100 && g->rooms[room].valid) { g->cur = room; g->hero.x = x; g->hero.y = y; g->hero.z = z; } }

/* Public hook for damage/healing sources (creatures, traps, food) as they get decoded. */
void game_change_energy(Game *g, int delta) { change_energy(g, delta); }

/* Debug text for the window title (F12): data counts, target, last verb, door state. Returns 0 when the overlay is off. */
int game_debug_text(const Game *g, char *buf, int n) {
    if (!g->dbg) return 0;
    int len = snprintf(buf, (size_t)n, "T");
    for (int k = 0; k + 1 < g->dbg_ntr && len < n - 12; k += 2) len += snprintf(buf + len, (size_t)(n - len), " %02x/%02x", g->dbg_tr[k], g->dbg_tr[k + 1]);
    len += snprintf(buf + len, (size_t)(n - len), " | EX %d RAN %d V%d P%d WALL51 %d", g->dbg_execs, g->dbg_ran, g->dbg_verb, g->dbg_pid, g->wallflag[51]);
    if (g->target >= 0 && g->target < g->rooms[g->cur].nobj) len += snprintf(buf + len, (size_t)(n - len), " TFLAGS %02x", g->rooms[g->cur].objs[g->target].flags);
    for (int k = 0; k < g->dbg_execs && k < 4 && len < n - 30; k++) len += snprintf(buf + len, (size_t)(n - len), " | tick %u v%u", g->dbg_log[k][0], g->dbg_log[k][1]);
    return 1;
}

int game_level_request(Game *g) { int l = g->want_level; g->want_level = 0; return l; }

/* Debug: step to the next/previous valid room and stand in its middle (PageUp / PageDown in the window). */
void game_cycle_room(Game *g, int dir) {
    int n = g->cur;
    for (int i = 0; i < 100; i++) { n = (n + dir + 100) % 100; if (g->rooms[n].valid) break; }
    if (!g->rooms[n].valid) return;
    g->cur = n; g->hero.x = g->rooms[n].sx * 4; g->hero.y = g->rooms[n].sy * 4; g->hero.z = 0; g->menu = 0; g->jump_t = 0; g->visited[n] = 1;
}

void game_play_sound(Game *g, int snd) { play_sound(g, snd); }   /* debug/test: trigger a sound id */
