#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MARGIN 6                 /* min-corner limit ($89b/$89c) */
#define HERO_FX 7
#define HERO_FY 7
#define HERO_FZ 30
#define MAX_PUSH_DEPTH 0x1f
#define WALK_TICKS_PER_FRAME 3   /* measured in the original: 6 video frames per walk frame */

typedef struct { int len; int8_t *data; } Sfx;
typedef struct { int sfx; double pos, step; int vol; double left; } Voice;   /* left = seconds before the voice is cut (0 = play to the end) */
typedef struct { int state, w, h; int16_t f[5]; int push; int16_t fx, fy, fz, name; uint8_t c, b16, b17, b1d; uint8_t *px; } Sprite;
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
    uint8_t font[46 * 24], fontmap[32]; char **text; int ntext; int day;
    int has_icons; uint8_t icons[20 * 112]; uint16_t slotpos[8][4]; uint8_t verbtab[16];
    int target, nverbs, verbs[8], menu, sel, prev_joy, msg, msg_t, ninv, inv[64];     /* verb menu state */
    uint16_t wallflag[MAX_WALLIDX]; int bump_timer;    /* wall-record flags, shared by the two rooms a wall joins */
    unsigned tickno; int walk_ticks, walk_step, start_room;
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
            w->idx = rd16(f); w->flags = rd16(f); w->b7 = rd8(f); if (w->idx < MAX_WALLIDX) g->wallflag[w->idx] = (uint16_t)w->flags;
        }
        int no = rd16(f); r->objs = calloc(no ? no : 1, sizeof(Obj)); r->nobj = 0;
        for (int k = 0; k < no; k++) {
            Obj o; o.pid = rd16(f); o.x = rd8(f); o.y = rd8(f); o.z = rd8(f); o.flags = rd8(f); o.slot = rd16(f); o.xflags = rd8(f);
            if (o.flags & 0x80) continue;                    /* destroyed / not spawned */
            if (o.slot == 8) { g->hero = o; g->start_room = (int)n; continue; }       /* the player placement */
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
    fclose(f);
    g->target = -1; g->msg = -1;
    g->cur = g->start_room; g->hero.x += 1;                                /* level start nudges the hero +1 in x */
    g->facing = JOY_RIGHT; g->day = 1;
    { const char *q = strstr(path, "level"); if (q && q[5] >= '1' && q[5] <= '9') g->day = q[5] - '0'; }   /* the day counter is the level number (level2.bin shows DAY 2) */
    return g;
}

void game_free(Game *g) {
    if (!g) return;
    for (int s = 0; s < 255; s++) free(g->spr[s].px);
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
static void play_sound(Game *g, int id, int period, int vol, double seconds) {
    if (id < 0 || id >= 65 || !g->sfx[id].len) return;
    int v = 0; for (int i = 0; i < 4; i++) if (!g->voice[i].step) { v = i; break; }
    g->voice[v].sfx = id; g->voice[v].pos = 0; g->voice[v].step = 3546895.0 / period; g->voice[v].vol = vol; g->voice[v].left = seconds;
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
static int pushable(const Game *g, const Obj *o) { return o->slot != 8 && g->spr[o->slot].push != 0; }

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
                play_sound(g, 18, 428, 24, 0.5); say(g, 1); key = 0; break;
            }
            if (key) { if (g->bump_timer <= 0) { play_sound(g, 60, 428, 24, 0.5); g->bump_timer = 32; } return 0; }
        } else if (wall_closed(g, w)) {                           /* shut door: bump, with a sound at most every 32 ticks ($9be counter) */
            if (g->bump_timer <= 0) { play_sound(g, 60, 428, 24, 0.5); g->bump_timer = 32; }
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
    if (verb == 2 && g->ninv < 64) {                                                 /* take: the object goes to the rucksack */
        g->inv[g->ninv++] = r->objs[g->target].pid;
        if (g->spr[r->objs[g->target].slot].name >= 0) say(g, g->spr[r->objs[g->target].slot].name);   /* original prints the item name */
        memmove(&r->objs[g->target], &r->objs[g->target + 1], (size_t)(r->nobj - g->target - 1) * sizeof(Obj)); r->nobj--;
        g->target = -1; g->nverbs = 0; return;
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
    g->prev_joy = joy;
    if (g->msg_t > 0) g->msg_t--;
    if (g->menu) {                                                                     /* verb menu: the hero stands still */
        if (g->target < 0 || g->nverbs == 0) g->menu = 0;
        else { if (fire_edge) { g->menu = 0; exec_verb(g, g->verbs[g->sel]); } else if (new_dirs) g->sel = move_sel(g, new_dirs); joy &= ~15; }
    } else if (fire_edge && g->target >= 0 && g->nverbs > 0) { g->menu = 1; g->sel = 0; joy &= ~15; }
    int dx = ((joy & JOY_RIGHT) ? 1 : 0) - ((joy & JOY_LEFT) ? 1 : 0);
    int dy = ((joy & JOY_DOWN) ? 1 : 0) - ((joy & JOY_UP) ? 1 : 0);
    g->tickno++;
    g->moving = 0;
    if (dx || dy) {
        g->facing = joy & 15; g->moving = 1;
        if (!try_move(g, r, h, dx, dy, 0)) {
            int nx = h->x + dx, ny = h->y + dy;
            int out = nx >= r->sx * 8 || ny >= r->sy * 8 || nx - HERO_FX + 1 < MARGIN || ny - HERO_FY + 1 < MARGIN;
            if (out) try_exit(g, dx, dy); else try_step_up(g, r, dx, dy);
        }
    }
    hero_gravity(g, r);
    /* Walking animation + footsteps, timed from an emulator trace of the original: each of the 4 walk frames is shown
       for 3 logic ticks; a footstep (sample 22, period 320, volume 8, cut after 4 video frames = 80 ms) sounds when the
       script reaches its 2nd and 4th frame (the `$fc 00` entries of the walk scripts). */
    if (g->moving) {
        int step = g->walk_ticks / WALK_TICKS_PER_FRAME;
        if (g->walk_ticks % WALK_TICKS_PER_FRAME == 0 && (step & 1)) play_sound(g, 22, 320, 8, 0.08);
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

typedef struct { int key, x, y, z, slot; int hero; Box b; int solid; } DrawItem;
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

void game_render(const Game *g, uint32_t *fb) {
    const Room *r = &g->rooms[g->cur];
    memcpy(fb, r->bg, SCREEN_W * SCREEN_H * 4);
    DrawItem items[256]; int n = 0;
    for (int i = 0; i < r->nobj && n < 255; i++) {
        const Obj *o = &r->objs[i];
        items[n++] = (DrawItem){ flat(g, o) ? -1000 : o->x + o->y, o->x, o->y, o->z, o->slot, 0, box_of(g, o), !flat(g, o) };
    }
    for (int i = 0; i < r->nwalls && n < 255; i++) {
        const Wall *w = &r->walls[i]; const Sprite *s = &g->spr[w->slot];
        if (s->state == 1 && s->h > 1 && wall_closed(g, w)) items[n++] = (DrawItem){ w->wx + s->fx + w->wy + s->fy - 2, w->wx + s->fx - 1, w->wy + s->fy - 1, (w->z - r->basez) * 16, w->slot, 0, {0}, 0 };
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
        } else if (s->state == 1) blit(fb, s->px, s->w, s->h, px, py);
    }
    if (g->hud) blit(fb, g->hud, SCREEN_W, SCREEN_H, 0, 0);     /* bottom panel frame over the room */
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
}

/* Debug aid for the headless driver: put the hero somewhere. */
void game_warp(Game *g, int room, int x, int y, int z) { if (room >= 0 && room < 100 && g->rooms[room].valid) { g->cur = room; g->hero.x = x; g->hero.y = y; g->hero.z = z; } }
