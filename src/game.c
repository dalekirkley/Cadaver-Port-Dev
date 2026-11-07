#include "game.h"
#include "sound_table.h"
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
typedef struct { int sfx; double pos, step; int vol; double left; } Voice;   /* left = seconds before the voice is cut (0 = play to the end) */
typedef struct { int state, w, h; int16_t f[5]; int push; int16_t fx, fy, fz, name; uint8_t c, b16, b17, b1d; uint8_t *px; int aw, ah, ax, ay; uint8_t *apx; } Sprite;
typedef struct { int16_t hx, hy; int w, h; uint8_t *px; } Frame;
typedef struct { int x, y, z, slot, flags, pid, xflags; } Obj;       /* x,y = max corner, inclusive box */
typedef struct { int x, y, orient; int box[4]; int slot, z, wx, wy; int idx, flags, b7; } Wall;
typedef struct {
    int valid, x0, y0, sx, sy, basez, org, name, prefix;
    uint8_t *bg;                      /* 320*200*4 */
    int nwalls; Wall *walls;
    int nobj; Obj *objs;              /* live state, persists while the program runs */
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
    int has_scroll; uint8_t scroll_tiles[18 * 32], scroll_map[0x30], font5[46 * 20]; int nexam, nuse, ntake; uint16_t take_pid[512]; int16_t take_gold[512], take_xp[512]; uint8_t counter[256]; int nhid; struct { int room; Obj o; } hid[128]; int hurt_cd; int dbg, dbg_verb, dbg_pid, dbg_ran, dbg_execs; unsigned dbg_log[4][3]; unsigned char dbg_tr[48]; int dbg_ntr; int nvm; uint16_t vm_pid[256]; uint8_t vm_len[256], vm_code[256][255]; uint16_t exam_pid[512], exam_text[512], use_pid[512], use_text[512];   /* examine scroll */
    int scroll_t; char scroll_str[256]; long score; int map_open, map_px, map_py, status_open; long gold, xp, level; uint8_t visited[100];
    int has_icons; uint8_t icons[20 * 112]; uint16_t slotpos[8][4]; uint8_t verbtab[16];
    int target, nverbs, verbs[8], menu, sel, prev_joy, msg, msg_t, ninv, inv[64];     /* verb menu state */
    uint16_t wallflag[MAX_WALLIDX]; uint8_t walldoor[MAX_WALLIDX]; int bump_timer;    /* wall-record flags, shared by the two rooms a wall joins */
    unsigned tickno; int walk_ticks, walk_step, start_room; Obj start_hero;
    Sfx sfx[65]; Voice voice[4];
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
        int no = rd16(f); r->objs = calloc((size_t)no + 32, sizeof(Obj)); r->nobj = 0;
        for (int k = 0; k < no; k++) {
            Obj o; o.pid = rd16(f); o.x = rd8(f); o.y = rd8(f); o.z = rd8(f); o.flags = rd8(f); o.slot = rd16(f); o.xflags = rd8(f);
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
        g->has_scroll = 1;
    }
    fclose(f);
    g->target = -1; g->msg = -1;
    g->cur = g->start_room; g->hero.x += 1; g->start_hero = g->hero;                                /* level start nudges the hero +1 in x */
    g->facing = JOY_RIGHT; g->day = 1; g->energy = ENERGY_MAX; g->level = 1;
    { const char *q = strstr(path, "level"); if (q && q[5] >= '1' && q[5] <= '9') g->day = q[5] - '0'; }   /* the day counter is the level number (level2.bin shows DAY 2) */
    return g;
}

void game_free(Game *g) {
    if (!g) return;
    for (int s = 0; s < 255; s++) { free(g->spr[s].px); free(g->spr[s].apx); }
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
static void play_voice(Game *g, int id, int period, int vol, double seconds) {
    if (id < 0 || id >= 65 || !g->sfx[id].len) return;
    int v = 0; for (int i = 0; i < 4; i++) if (!g->voice[i].step) { v = i; break; }
    g->voice[v].sfx = id; g->voice[v].pos = 0; g->voice[v].step = 3546895.0 / period; g->voice[v].vol = vol; g->voice[v].left = seconds;
}

/* Play a game sound id with its real per-voice samples, periods and volumes. */
static void play_sound(Game *g, int snd) {
    if (snd < 0 || snd >= 69) return;
    for (int i = 0; i < 4; i++) {
        const SndVoice *sv = &SND_TABLE[snd][i];
        if (sv->sample >= 0) play_voice(g, sv->sample, sv->period, sv->vol, snd == 0 ? 0.1 : 0.6);
    }
}

void game_audio_mix(Game *g, int16_t *out, int n, int rate) {
    for (int i = 0; i < n; i++) {
        int acc = 0;
        for (int v = 0; v < 4; v++) {
            Voice *vo = &g->voice[v]; if (!vo->step) continue;
            const Sfx *s = &g->sfx[vo->sfx]; int p = (int)vo->pos;
            if (p >= s->len) { vo->step = 0; continue; }
            acc += s->data[p] * vo->vol; vo->pos += vo->step / rate;
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
static int pushable(const Game *g, const Obj *o) { return o->slot != 8 && (g->spr[o->slot].push & 2) != 0; }

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
    case 0x1f: m->self->flags &= ~1; break;
    case 0x20: m->self->flags |= 1; break;
    case 0x21: m->self->flags ^= 1; break;
    case 0x22: { int id = vm_word(m), has = 0; for (int i = 0; i < g->ninv; i++) if (g->inv[i] == id) has = 1; m->cond = has ? m->cond + 1 : 0; break; }
    case 0x27: { int n = m->c[m->p]; int add = m->c[m->p + 1]; m->p += 2; g->counter[n] = (uint8_t)(g->counter[n] + add); break; }
    case 0x28: { int n = m->c[m->p], mode = m->c[m->p + 1], v = m->c[m->p + 2], x = g->counter[n]; m->p += 3;
        int ok = mode == 0 ? x <= v : mode == 1 ? x >= v : mode == 2 ? x != v : x == v;      /* modes as in $107aa: 0 le, 1 ge, 2 ne, 3 eq */
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
                if (w->b7 & 2) { memmove(&g->inv[i], &g->inv[i + 1], (size_t)(g->ninv - i - 1) * sizeof(int)); g->ninv--; }   /* key consumed */
                if (w->b7 & 4) g->wallflag[w->idx] = 0;           /* unlocked for good */
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
        h->x = x; h->y = y; g->cur = n; return 1;
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
    Room *r = &g->rooms[g->cur];
    if (g->target < 0) return;
    g->dbg_verb = verb; g->dbg_pid = r->objs[g->target].pid; g->dbg_ran = 0; g->dbg_ntr = 0; g->dbg_log[g->dbg_execs & 3][0] = g->tickno; g->dbg_log[g->dbg_execs & 3][1] = (unsigned)verb; g->dbg_log[g->dbg_execs & 3][2] = (unsigned)g->prev_joy; g->dbg_execs++;
    if (verb == 2 && g->ninv < 64) {                                                 /* take: the object goes to the rucksack */
        { int pid = r->objs[g->target].pid;                                          /* treasure: gold and XP from the take script ($ffdc/$ffa0) */
          for (int i = 0; i < g->ntake; i++) if (g->take_pid[i] == pid) {
              if (g->take_gold[i]) { g->gold += g->take_gold[i]; play_sound(g, 3); }
              if (g->take_xp[i]) { g->xp += g->take_xp[i]; if (g->xp < 0) g->xp = 0; if (!g->take_gold[i]) play_sound(g, 0x1a); }
              break; } }
        g->inv[g->ninv++] = r->objs[g->target].pid;
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
        if (!(ch->flags & 1)) {
            ch->flags |= 1; play_sound(g, 0x0a);
            for (int i = 0; i < g->nvm; i++) if (g->vm_pid[i] == ch->pid) run_script(g, ch, g->vm_code[i], g->vm_len[i]);
        } else say(g, 4);
        return;
    }
    if (verb == 7 || verb == 4 || (verb == 6 && g->has_scroll)) {                                  /* use / pull a lever: event-5 script (a lever also clicks, sound $38) */
        int pid = r->objs[g->target].pid;
        { int ran = 0; for (int i = 0; i < g->nvm; i++) if (g->vm_pid[i] == pid) { if (!ran && (verb == 7 || verb == 4)) play_sound(g, 0x38); ran = 1; g->dbg_ran++; run_script(g, &r->objs[g->target], g->vm_code[i], g->vm_len[i]); } if (ran) return; }
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

void game_tick(Game *g, int joy) {
    Room *r = &g->rooms[g->cur]; Obj *h = &g->hero;
    int fire_edge = (joy & JOY_FIRE) && !(g->prev_joy & JOY_FIRE), new_dirs = (joy & 15) & ~(g->prev_joy & 15);
    int prev = g->prev_joy; g->prev_joy = joy;
    g->visited[g->cur] = 1;
    if (g->status_open) { if ((joy & 0xCF) && !(prev & 0xCF)) g->status_open = 0; return; }              /* diary panel: waits for a key ($9d68) */
    if ((joy & JOY_DEBUG) && !(prev & JOY_DEBUG)) g->dbg ^= 1;
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
    } else if (fire_edge && g->target >= 0 && g->nverbs > 0) { g->menu = 1; g->sel = 0; joy &= ~15; }
    else if (fire_edge && g->target < 0 && !g->jump_t && hero_grounded(g, r)) { g->jump_t = 1; g->jump_z0 = h->z; }    /* fire with nothing in front: jump */
    int dx = ((joy & JOY_RIGHT) ? 1 : 0) - ((joy & JOY_LEFT) ? 1 : 0);
    int dy = ((joy & JOY_DOWN) ? 1 : 0) - ((joy & JOY_UP) ? 1 : 0);
    g->tickno++;
    if (g->hurt_cd > 0) g->hurt_cd--;
    if (g->hurt_cd == 0) {                                  /* contact scripts (event 9 with the hero): creatures, fires, spikes. Original posts it every tick of contact; a 1 s repeat is a guess */
        Box hb = box_of(g, h);
        for (int i = 0; i < r->nobj && g->hurt_cd == 0; i++) {
            Obj *q = &r->objs[i]; int has = 0;
            for (int k = 0; k < g->nvm; k++) if (g->vm_pid[k] == (q->pid | 0x8000)) { has = 1; break; }
            if (!has) continue;
            Box qb = box_of(g, q);
            if (qb.x1 + 1 < hb.x0 || hb.x1 + 1 < qb.x0 || qb.y1 + 1 < hb.y0 || hb.y1 + 1 < qb.y0 || qb.z1 < hb.z0 || hb.z1 < qb.z0) continue;
            for (int k = 0; k < g->nvm; k++) if (g->vm_pid[k] == (q->pid | 0x8000)) { run_script(g, q, g->vm_code[k], g->vm_len[k]); g->hurt_cd = 25; }
        }
    }
    g->moving = 0;
    if (dx || dy) {
        g->facing = joy & 15; g->moving = 1;
        if (!try_move(g, r, h, dx, dy, 0)) {
            int nx = h->x + dx, ny = h->y + dy;
            int out = nx >= r->sx * 8 || ny >= r->sy * 8 || nx - HERO_FX + 1 < MARGIN || ny - HERO_FY + 1 < MARGIN;
            if (out) try_exit(g, dx, dy); else try_step_up(g, r, dx, dy);
        }
    }
    if (g->jump_t) { h->z = g->jump_z0 + JUMP_Z[g->jump_t - 1]; if (++g->jump_t > 20) g->jump_t = 0; }
    else hero_gravity(g, r);
    /* Walking animation + footsteps, timed from an emulator trace of the original: each of the 4 walk frames is shown
       for 3 logic ticks; a footstep (sample 22, period 320, volume 8, cut after 4 video frames = 80 ms) sounds when the
       script reaches its 2nd and 4th frame (the `$fc 00` entries of the walk scripts). */
    if (g->moving) {
        int step = g->walk_ticks / WALK_TICKS_PER_FRAME;
        if (g->walk_ticks % WALK_TICKS_PER_FRAME == 0 && (step & 1)) play_sound(g, 0);
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

typedef struct { int key, x, y, z, slot; int hero; Box b; int solid; int alt; } DrawItem;
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
        items[n++] = (DrawItem){ flat(g, o) ? -1000 : o->x + o->y, o->x, o->y, o->z, o->slot, 0, box_of(g, o), !flat(g, o), (o->flags & 1) && g->spr[o->slot].apx != NULL };
    }
    for (int i = 0; i < r->nwalls && n < 255; i++) {
        const Wall *w = &r->walls[i]; const Sprite *s = &g->spr[w->slot];
        if (s->state == 1 && s->h > 1 && (wall_closed(g, w) || (w->idx < MAX_WALLIDX && g->walldoor[w->idx]))) items[n++] = (DrawItem){ w->wx + s->fx + w->wy + s->fy - 2, w->wx + s->fx - 1, w->wy + s->fy - 1, (w->z - r->basez) * 16, w->slot, 0, {0}, 0 };
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
                    if (xy) blocked = A->z1 < B->z0;
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
        } else if (d->alt) blit(fb, s->apx, s->aw, s->ah, px + s->ax, py + s->ay);
        else if (s->state == 1) blit(fb, s->px, s->w, s->h, px, py);
    }
    if (g->hud) { blit(fb, g->hud, SCREEN_W, SCREEN_H, 0, 0); }     /* bottom panel frame over the room */
    if (g->text) {
        char buf[96];
        snprintf(buf, sizeof buf, "%s %d", g->ntext > 29 ? g->text[29] : "DAY", g->day);
        draw_text(g, fb, buf, 252, 158, 60);
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
            if (nm >= 0 && nm < g->ntext) draw_text(g, fb, g->text[nm], 229, 172, 84);
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
    if (g->hud) draw_energy(g, fb);
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
