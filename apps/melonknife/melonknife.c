/* Melon Knife: a spinning watermelon, a stack of knives. Tap to throw one: it sticks into the melon. Hit a knife
 * that's already in it and it's over. Clear the knives for a level and the melon bursts; the next one spins
 * harder, turns round and starts with knives already in it. Our own game with our own art, drawn in code.
 *
 * Whole numbers only (the apps have no floating point): angles are 1/100 of a degree, and sine and cosine come
 * from a table made at the start. To stay smooth on the phone: the background (with its glow) and the melon's
 * still parts are drawn once into pictures of their own; each frame copies the box the melon and knives move in
 * and draws what moves (knives are filled four-sided shapes, quick, unlike thick lines); the score and the
 * knives left are redrawn only when they change; and only the parts that changed are shown. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"
#include "sdk/audio.h"
#include "sdk/sfx.h"
#include "sdk/kern86.h"

#define SAVE_FILE "melonknife.dat"
#define SAVE_MAGIC 0x4d4b4e32u
#define LEVELS 100
#define PER_WORLD 25                    /* levels in each of the four worlds */
#define MAX_OBS 4
#define OBS_DEG 760                     /* a knife this close to an obstacle (1/100 degree) hits it */
#define FRAME_MS 33
#define MELON_R 170                     /* the melon's radius, px */
#define SPR (2 * MELON_R + 48)          /* the melon's still picture: a square this big */
#define MAX_STUCK 40
#define PART_MAX 48
#define HIT_DEG 800                     /* two knives closer than this (1/100 degree) hit each other */
#define THROW_SPEED 2600                /* px a second */
#define BLADE 104
#define HANDLE 72
#define BOX 350                         /* the box the melon and its knives move in: this far from its middle, each way */

#define C_TEXT 0xffffffu
#define C_DIM 0xaec4cau
#define C_GOLD 0xffd34fu
#define C_FLESH 0xf0455au
#define C_STEEL 0xe8eef6u
#define C_STEEL_DK 0x8d9bb0u
#define C_HANDLE 0x7a4a2au
#define C_BTN 0x2a4651u
#define C_BTN_PRIMARY 0x35a85eu

enum screen { S_MENU, S_PLAY, S_CLEAR, S_FAIL, S_OVER, S_PAUSE };

/* The four worlds: the melon, the backdrop and the obstacles on the melon all change with them. */
struct world {
    const char *name;
    uint32_t bg0, bg1, glow0, glow1, dot, rind0, rind1, ring0, ring1, fl0, fl1, stripe, seed, ob0, ob1, burst0, burst1;
};
static const struct world themes[4] = {
    { "Garden", 0x1f4d59u, 0x153944u, 0x214f5bu, 0x3b8789u, 0x5fb09bu, 0x1b6b30u, 0x2e9d46u, 0xf1f6e8u, 0xdfeacfu, 0xff93a0u, 0xe0304au, 0x74d476u, 0x2a1418u, 0x2f6b24u, 0xa6e86eu, 0xf0455au, 0x4fc060u },
    { "Frost",  0x1d3a5cu, 0x122a47u, 0x1f4468u, 0x5a9bc8u, 0xe6f4ffu, 0x2f6a9au, 0x5aa7d6u, 0xeaf6ffu, 0xcfe6f5u, 0xd6f1ffu, 0x4a9ee0u, 0xbfe8ffu, 0x10263au, 0x5fb2e6u, 0xeaf8ffu, 0x7fd0ffu, 0xeaf8ffu },
    { "Lava",   0x4d221cu, 0x32140fu, 0x5a2a1cu, 0xc2562au, 0xff8a3cu, 0x2b2022u, 0x5a3a36u, 0xffd08au, 0xf0a95au, 0xffe27au, 0xe8501cu, 0xe0652au, 0x2a0d08u, 0x3a2f33u, 0xff7a2au, 0xffb03au, 0xff5a1cu },
    { "Galaxy", 0x2a2150u, 0x1a1438u, 0x30275eu, 0x7a5ac8u, 0xfff2b0u, 0x3d2a8au, 0x6a4fd0u, 0xe8e0ffu, 0xcfc4f5u, 0xf3a6ffu, 0x8a3ad8u, 0xa58cffu, 0x1a0f33u, 0xb06affu, 0xffd0ffu, 0xd77affu, 0xffe27au },
};

struct save {
    uint32_t magic, best;                /* best: the furthest level reached */
    uint32_t run_valid, run_level, run_score;       /* the game to carry on: it is saved when each level starts */
};

struct particle {
    int x, y, vx, vy, life, r;
    uint32_t col;
};

static struct k86_window win;
static struct gfx_surface canvas, bgpic, spr;
static char home[K86_PATH_MAX];
static int vol = -1;
static struct save sv;

static int sin_t[362], cos_t[362];      /* *1024, per degree */

static enum screen screen = S_MENU;
static uint32_t now_ms, last_frame, screen_at, rng_state;
static int cx, cy;                      /* the melon's middle */
static struct gfx_rect box;             /* what the melon, its knives and the juice are drawn in */
static int level, score, score0, left, in_level_ms, won, cur_theme = -1;
static const struct world *th = &themes[0];
static int obs[MAX_OBS], nobs;          /* obstacles on the melon: angles, 1/100 degree */
static int mel_ang;                     /* 1/100 degree */
static int stuck[MAX_STUCK], nstuck;    /* each knife's angle on the melon, 1/100 degree (0 = right, 90 = down) */
static int flying, fly_y;               /* a knife on its way: where its tip is */
static int fall_y, fall_x, fall_vx, fall_rot;   /* the knife that hit one: it drops */
static struct particle parts[PART_MAX];
static int pulse;                       /* the melon's bump when a knife lands, px */
static int shake;                       /* the melon's shake, px */
static int hud_dirty = 1, foot_dirty = 1;
static struct gfx_rect btn_play, btn_menu, btn_pause, chip[4];
static int pressed;                     /* 1 first button, 2 second, 3.. a world */
static int hidden;                      /* the menu is not in front: no sound, no timer, until a touch */
static int fail_obs;                    /* the knife that failed the level hit an obstacle */
static uint32_t perf_sum;
static int perf_n, perf_max;

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iabs(int a) { return a < 0 ? -a : a; }

static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static uint32_t lerp(uint32_t a, uint32_t b, int t, int of)    /* a at 0, b at `of` */
{
    int r = (int)(a >> 16 & 255) + ((int)(b >> 16 & 255) - (int)(a >> 16 & 255)) * t / of;
    int g = (int)(a >> 8 & 255) + ((int)(b >> 8 & 255) - (int)(a >> 8 & 255)) * t / of;
    int bl = (int)(a & 255) + ((int)(b & 255) - (int)(a & 255)) * t / of;
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)bl;
}

/* ---- trig ---- */
static void trig_init(void)
{
    const int64_t c = 65526, s = 1144;          /* cos and sin of one degree, 16.16 */
    int64_t x = 65536, y = 0;
    for (int i = 0; i <= 361; i++) {
        sin_t[i] = (int)(y >> 6);
        cos_t[i] = (int)(x >> 6);
        int64_t nx = (x * c - y * s) >> 16, ny = (x * s + y * c) >> 16;
        x = nx;
        y = ny;
    }
}

static int norm(int a)                          /* 0 .. 35999 */
{
    a %= 36000;
    return a < 0 ? a + 36000 : a;
}

static int tsin(int a100)
{
    a100 = norm(a100);
    int i = a100 / 100, f = a100 % 100;
    return sin_t[i] + (sin_t[i + 1] - sin_t[i]) * f / 100;
}

static int tcos(int a100)
{
    a100 = norm(a100);
    int i = a100 / 100, f = a100 % 100;
    return cos_t[i] + (cos_t[i + 1] - cos_t[i]) * f / 100;
}

static int angdiff(int a, int b)                /* the smaller way round, 0 .. 18000 */
{
    int d = iabs(norm(a) - norm(b));
    return d > 18000 ? 36000 - d : d;
}

/* ---- saving ---- */
static void load(void)
{
    struct save s;
    memset(&sv, 0, sizeof(sv));
    sv.magic = SAVE_MAGIC;
    vol = k86_home(home);
    if (vol >= 0 && k86_read(vol, home, SAVE_FILE, &s, sizeof(s)) == (int)sizeof(s) && s.magic == SAVE_MAGIC)
        sv = s;
}

static void save(void)
{
    if (vol >= 0)
        k86_write(vol, home, SAVE_FILE, &sv, sizeof(sv));
}

/* ---- filled shapes ---- */

/* A convex four-sided shape, filled by rows. */
static void quad(const int *x, const int *y, uint32_t c)
{
    int ymin = y[0], ymax = y[0];
    for (int i = 1; i < 4; i++) {
        ymin = imin(ymin, y[i]);
        ymax = imax(ymax, y[i]);
    }
    ymin = imax(ymin, 0);
    ymax = imin(ymax, canvas.h - 1);
    for (int yy = ymin; yy <= ymax; yy++) {
        int xl = 1 << 30, xr = -(1 << 30);
        for (int e = 0; e < 4; e++) {
            int x0 = x[e], y0 = y[e], x1 = x[(e + 1) & 3], y1 = y[(e + 1) & 3];
            if (y0 == y1 || yy < imin(y0, y1) || yy > imax(y0, y1))
                continue;
            int xi = x0 + (x1 - x0) * (yy - y0) / (y1 - y0);
            xl = imin(xl, xi);
            xr = imax(xr, xi);
        }
        xl = imax(xl, 0);
        xr = imin(xr, canvas.w - 1);
        uint32_t *row = canvas.px + (size_t)yy * canvas.stride;
        for (int xx = xl; xx <= xr; xx++)
            row[xx] = c;
    }
}

/* A strip from (px, py) to (qx, qy), w0 half wide at the start and w1 at the end. */
static void strip(int px, int py, int qx, int qy, int w0, int w1, uint32_t c)
{
    int dx = qx - px, dy = qy - py, len = 1;
    while ((len + 1) * (len + 1) <= dx * dx + dy * dy)           /* the length, by the square root's way up */
        len++;
    int nx = -dy * 1024 / len, ny = dx * 1024 / len;               /* the sideways direction, *1024 */
    int x[4] = { px + nx * w0 / 1024, qx + nx * w1 / 1024, qx - nx * w1 / 1024, px - nx * w0 / 1024 };
    int y[4] = { py + ny * w0 / 1024, qy + ny * w1 / 1024, qy - ny * w1 / 1024, py - ny * w0 / 1024 };
    quad(x, y, c);
}

/* A round disc, smooth at the edge (the library's circle isn't round at this size). alpha 255 is solid. */
static uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

static void put_a(struct gfx_surface *s, int x, int y, uint32_t c, int a)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h || a <= 0)
        return;
    uint32_t *p = s->px + (size_t)y * s->stride + x;
    *p = a >= 255 ? c : lerp(*p, c, a, 255);
}

static void disc_a(struct gfx_surface *s, int cx, int cy, int r, uint32_t c, int alpha)
{
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= s->h)
            continue;
        uint32_t xs = isqrt64((uint64_t)((int64_t)r * r - (int64_t)dy * dy) * 65536u);   /* half the row's width, 1/256 px */
        int full = (int)(xs >> 8), part = (int)(xs & 255);
        int x0 = imax(0, cx - full), x1 = imin(s->w - 1, cx + full);
        uint32_t *row = s->px + (size_t)y * s->stride;
        if (alpha >= 255) {
            for (int x = x0; x <= x1; x++)
                row[x] = c;
        } else {
            for (int x = x0; x <= x1; x++)
                row[x] = lerp(row[x], c, alpha, 255);
        }
        put_a(s, cx - full - 1, y, c, part * alpha / 255);
        put_a(s, cx + full + 1, y, c, part * alpha / 255);
    }
}

static void disc(struct gfx_surface *s, int cx, int cy, int r, uint32_t c)
{
    disc_a(s, cx, cy, r, c, 255);
}

/* ---- the pictures that are drawn once ---- */
static void make_pictures(void)
{
    free(bgpic.px);
    free(spr.px);
    bgpic = (struct gfx_surface){ (uint32_t *)calloc((size_t)canvas.w * (size_t)canvas.h, 4), canvas.w, canvas.h, canvas.w };
    spr = (struct gfx_surface){ (uint32_t *)calloc((size_t)SPR * (size_t)SPR, 4), SPR, SPR, SPR };
    if (!bgpic.px || !spr.px)
        return;
    for (int y = 0; y < canvas.h; y++)
        gfx_fill_rect(&bgpic, 0, y, canvas.w, 1, lerp(th->bg0, th->bg1, y, canvas.h));
    for (int i = 0; i < 24; i++)                                             /* a soft glow behind the melon (no hard edge) */
        disc_a(&bgpic, cx, cy, MELON_R + 300 - i * 12, th->glow1, 11);
    uint32_t q = 0x9e3779b9u;                                                /* the world's specks: snow, embers, stars, spores */
    for (int i = 0; i < 70; i++) {
        q = q * 1664525u + 1013904223u;
        int x = (int)(q >> 8) % canvas.w;
        q = q * 1664525u + 1013904223u;
        int y = (int)(q >> 8) % canvas.h, d = (int)(q >> 4 & 3);
        disc(&bgpic, x, y, 2 + d, lerp(lerp(th->bg0, th->glow1, 1, 2), th->dot, 1 + d, 4));
    }
    /* the melon, still parts: its picture starts as the background behind it */
    gfx_blit(&spr, 0, 0, &bgpic, cx - SPR / 2, cy - SPR / 2, SPR, SPR);
    int m = SPR / 2, r = MELON_R;
    disc_a(&spr, m + 4, m + 14, r, 0x050c0eu, 140);          /* its shadow */
    disc(&spr, m, m, r, th->rind0);                                                    /* rind */
    disc(&spr, m, m, r - 5, th->rind1);
    disc(&spr, m, m, r - 20, th->ring0);                                               /* the white ring */
    disc(&spr, m, m, r - 27, th->ring1);
    for (int i = 0; i < 18; i++)                                                                  /* the flesh: paler at the edge */
        disc(&spr, m, m, r - 30 - i * (r - 36) / 18, lerp(th->fl0, th->fl1, i, 17));
    gfx_blend_round_rect(&spr, m - r + 34, m - r + 28, 108, 46, 23, 0xffffffu, 46);              /* a shine */
}

static int theme_of(int lv)
{
    int t = (lv - 1) / PER_WORLD;
    return t < 0 ? 0 : t > 3 ? 3 : t;
}

static void set_theme(int t)
{
    if (t == cur_theme && bgpic.px)
        return;
    cur_theme = t;
    th = &themes[t];
    make_pictures();
}

/* ---- the rules ---- */
static void add_burst(int x, int y, int n, uint32_t c1, uint32_t c2, int big)
{
    for (int i = 0; i < n && i < PART_MAX; i++) {
        int a = (int)(rnd() % 360) * 100, sp = 160 + (int)(rnd() % 620);
        parts[i] = (struct particle){ x, y, tcos(a) * sp / 1024, tsin(a) * sp / 1024 - 160, 450 + (int)(rnd() % 450),
                                      big ? 7 + (int)(rnd() % 8) : 4 + (int)(rnd() % 4), i % 2 ? c1 : c2 };
    }
}

static void layout(void);

static void remember(void)                       /* the game to carry on: level and score */
{
    sv.run_valid = 1;
    sv.run_level = (uint32_t)level;
    sv.run_score = (uint32_t)score;
    if ((uint32_t)level > sv.best)
        sv.best = (uint32_t)level;
    save();
}

static void start_level(void)
{
    int idx = (level - 1) % PER_WORLD, w = theme_of(level);
    set_theme(w);
    nstuck = nobs = 0;
    flying = 0;
    fall_y = -1;
    in_level_ms = 0;
    score0 = score;
    mel_ang = (int)(rnd() % 36000);
    int pre = idx < 3 ? 0 : imin(idx / 6 + 1 + w / 2, 4);
    int ob = idx < 2 && w == 0 ? 0 : imin(w == 0 ? idx / 6 + 1 : 1 + idx / 8 + w / 2, MAX_OBS - (w > 1 ? 0 : 1));
    int n = pre + ob;
    for (int i = 0; i < n; i++) {                /* obstacles and knives already in it, spread so there is always a gap */
        int a = (36000 * i / n + (int)(rnd() % (uint32_t)(36000 / n / 3))) % 36000;
        if (i < ob)
            obs[nobs++] = a;
        else
            stuck[nstuck++] = a;
    }
    left = imin(4 + idx / 4 + w, 9);
    k86_sfx_music(w);
    screen = S_PLAY;
    screen_at = now_ms;
    hud_dirty = foot_dirty = 1;
    remember();
}

static void start_game(int lv, int sc)
{
    level = lv;
    score = sc;
    won = 0;
    memset(parts, 0, sizeof(parts));
    pressed = 0;
    start_level();
    k86_timer_set(FRAME_MS);
    last_frame = now_ms = k86_uptime_ms();
}

/* How fast the melon turns, 1/100 degree a second: each level has its own way of turning. */
static int spin_speed(void)
{
    int idx = (level - 1) % PER_WORLD;
    int base = imin(9000 + idx * 350 + theme_of(level) * 1800, 22000);
    int t = in_level_ms;
    switch (level % 4) {
    case 1:                                      /* steady */
        return base;
    case 2: {                                    /* quick and slow in turn */
        int w = iabs((t / 8) % 400 - 200);       /* 0..200 */
        return base * (40 + w) / 140;
    }
    case 3: {                                    /* turns about every 1.6 s */
        int ph = t % 3200;
        int k = ph < 1600 ? 1000 : -1000;
        int edge = imin(imin(ph % 1600, 1600 - ph % 1600), 250);
        return base * k / 1000 * edge / 250;
    }
    default:                                     /* the other way, faster */
        return -base * 13 / 10;
    }
}

static void finish_game(void)
{
    score = score0;                              /* the level is tried again from its start */
    screen = S_OVER;
    screen_at = now_ms;
}

static int ready_y(void)                         /* where the next knife's tip waits, at the foot of the screen */
{
    return canvas.h - 300;
}

static void throw_knife(void)
{
    if (screen != S_PLAY || flying || left <= 0)
        return;
    flying = 1;
    fly_y = ready_y();
    k86_sfx_play(SFX_WHOOSH, 0);
}

static void land(void)
{
    int rel = norm(9000 - mel_ang);                 /* the bottom of the melon, in its own turning */
    flying = 0;
    int bad = 0, from = rel;
    for (int i = 0; i < nstuck; i++)
        if (angdiff(stuck[i], rel) < HIT_DEG) {
            bad = 1;
            from = stuck[i];
        }
    for (int i = 0; i < nobs; i++)
        if (angdiff(obs[i], rel) < OBS_DEG) {
            bad = 2;
            from = obs[i];
        }
    if (bad) {                                       /* on top of a knife or an obstacle: the level is lost */
        fail_obs = bad == 2;
        fall_x = cx;
        fall_y = cy + MELON_R + 30;
        fall_vx = (from < rel || (from - rel > 18000)) ? 380 : -380;
        fall_rot = 0;
        add_burst(cx, cy + MELON_R, 14, C_STEEL, C_STEEL_DK, 0);
        k86_sfx_music(-1);
        k86_sfx_play(SFX_CLINK, fail_obs ? -5 : 0);
        k86_sfx_play(SFX_FAIL, 0);
        shake = 20;
        screen = S_FAIL;
        screen_at = now_ms;
        return;
    }
    if (nstuck < MAX_STUCK)
        stuck[nstuck++] = rel;
    score++;
    left--;
    pulse = 14;
    shake = imax(shake, 5);
    hud_dirty = foot_dirty = 1;
    add_burst(cx, cy + MELON_R - 10, 14, th->burst0, th->burst1, 0);
    if (left <= 0) {                                 /* the melon is cleared: it bursts */
        add_burst(cx, cy, PART_MAX, th->burst0, th->burst1, 1);
        k86_sfx_play(level > LEVELS ? SFX_WIN : SFX_LEVELUP, 0);
        k86_sfx_play(SFX_SPLAT, 0);
        shake = 14;
        level++;
        screen = S_CLEAR;
        screen_at = now_ms;
        return;
    }
    k86_sfx_play(SFX_THUD, 0);
    k86_sfx_play(SFX_COIN, imin(2 * (score - score0 - 1), 22));
}

/* ---- drawing ---- */

/* A knife with its tip at (tx, ty), its handle away along `a` (1/100 degree): a tapering blade with a bright
 * edge, a guard, and a handle with rivets. fade > 0 (percent): only a dim blade, for the trail. */
static void draw_knife(int tx, int ty, int a, int fade)
{
    int dx = tcos(a), dy = tsin(a);
    int bx = tx + dx * BLADE / 1024, by = ty + dy * BLADE / 1024;
    uint32_t steel = fade ? lerp(C_STEEL, th->bg0, fade, 100) : C_STEEL, dark = fade ? lerp(C_STEEL_DK, th->bg0, fade, 100) : C_STEEL_DK;
    strip(tx, ty, bx, by, 4, 12, dark);
    strip(tx, ty, bx, by, 2, 9, steel);
    if (fade)
        return;
    strip(tx + dx * 6 / 1024, ty + dy * 6 / 1024, bx, by, 1, 3, 0xffffffu);                      /* the bright edge */
    int hx = bx + dx * HANDLE / 1024, hy = by + dy * HANDLE / 1024;
    strip(bx - dy * 15 / 1024, by + dx * 15 / 1024, bx + dy * 15 / 1024, by - dx * 15 / 1024, 5, 5, 0x4a4f5bu);   /* guard */
    strip(bx, by, hx, hy, 14, 12, 0x4e2f1bu);
    strip(bx, by, hx, hy, 11, 9, C_HANDLE);
    strip(bx, by, hx, hy, 3, 2, 0xa86d3fu);
    for (int k = 1; k <= 2; k++)
        disc(&canvas, bx + dx * (HANDLE * k / 3) / 1024, by + dy * (HANDLE * k / 3) / 1024, 3, 0xe9c46au);
    disc(&canvas, hx, hy, 11, 0x4e2f1bu);
    disc(&canvas, hx, hy, 9, C_HANDLE);
}

static void text_c(int x, int y, const char *s, enum text_style st, uint32_t col)
{
    text_draw(&canvas, x - text_width(s, st) / 2 + 2, y + 3, s, st, 0x061014u);              /* a shadow */
    text_draw(&canvas, x - text_width(s, st) / 2, y, s, st, col);
}

static void draw_button(struct gfx_rect r, const char *label, int primary, int down)
{
    int lift = down ? 4 : 0;
    uint32_t col = primary ? C_BTN_PRIMARY : C_BTN;
    gfx_fill_round_rect(&canvas, r.x, r.y + 7, r.w, r.h, r.h / 2, 0x061014u);
    gfx_fill_round_rect(&canvas, r.x, r.y + lift, r.w, r.h, r.h / 2, lerp(col, 0x000000u, 25, 100));
    gfx_fill_round_rect(&canvas, r.x, r.y + lift, r.w, r.h - 6, (r.h - 6) / 2, col);
    gfx_blend_round_rect(&canvas, r.x + 16, r.y + lift + 6, r.w - 32, r.h / 3, r.h / 6, 0xffffffu, 36);
    text_c(r.x + r.w / 2, r.y + lift + (r.h - 6 - text_height(TEXT_BOLD)) / 2, label, TEXT_BOLD, C_TEXT);
}

static void copy_bg(int x, int y, int w, int h)
{
    x = imax(x, 0);
    y = imax(y, 0);
    w = imin(w, canvas.w - x);
    h = imin(h, canvas.h - y);
    if (w > 0 && h > 0)
        gfx_blit(&canvas, x, y, &bgpic, x, y, w, h);
}

/* An obstacle on the rim: a themed spike (thorn, ice, rock, crystal) that a knife must not touch. */
static void draw_obstacle(int mx, int my, int a)
{
    int dx = tcos(a), dy = tsin(a), px = -dy, py = dx;
    int rb = MELON_R - 6 + pulse / 2, rt = MELON_R + 40 + pulse / 2, ri = MELON_R - 36;
    int bx = mx + dx * rb / 1024, by = my + dy * rb / 1024;
    int x[4] = { bx + px * 28 / 1024, mx + dx * rt / 1024, bx - px * 28 / 1024, mx + dx * ri / 1024 };
    int y[4] = { by + py * 28 / 1024, my + dy * rt / 1024, by - py * 28 / 1024, my + dy * ri / 1024 };
    quad(x, y, 0x0b1a1fu);
    int rb2 = rb - 3, rt2 = rt - 8, ri2 = ri + 8;
    int bx2 = mx + dx * rb2 / 1024, by2 = my + dy * rb2 / 1024;
    int x2[4] = { bx2 + px * 22 / 1024, mx + dx * rt2 / 1024, bx2 - px * 22 / 1024, mx + dx * ri2 / 1024 };
    int y2[4] = { by2 + py * 22 / 1024, my + dy * rt2 / 1024, by2 - py * 22 / 1024, my + dy * ri2 / 1024 };
    quad(x2, y2, th->ob0);
    int rb3 = rb - 6, rt3 = rt - 16, ri3 = ri + 24;
    int bx3 = mx + dx * rb3 / 1024, by3 = my + dy * rb3 / 1024;
    int x3[4] = { bx3 + px * 9 / 1024, mx + dx * rt3 / 1024, bx3 - px * 5 / 1024, mx + dx * ri3 / 1024 };
    int y3[4] = { by3 + py * 9 / 1024, my + dy * rt3 / 1024, by3 - py * 5 / 1024, my + dy * ri3 / 1024 };
    quad(x3, y3, th->ob1);
}

/* the melon: its picture, then what turns on it (the stripes and seeds), then the knives in it */
static void draw_melon(int ox, int oy)
{
    int mx = cx + ox, my = cy + oy;
    gfx_blit(&canvas, mx - SPR / 2, my - SPR / 2, &spr, 0, 0, SPR, SPR);
    int r = MELON_R + pulse / 2;
    for (int i = 0; i < 14; i++) {                                                  /* the rind's stripes */
        int a0 = mel_ang + i * 2570, a1 = a0 + 1500, rr = r - 11;
        strip(mx + tcos(a0) * rr / 1024, my + tsin(a0) * rr / 1024, mx + tcos(a1) * rr / 1024, my + tsin(a1) * rr / 1024, 6, 6, th->stripe);
    }
    for (int i = 0; i < 10; i++) {                                                  /* seeds */
        int a = mel_ang + i * 3600, rr = 56 + (i % 3) * 22;
        int sx = mx + tcos(a) * rr / 1024, sy = my + tsin(a) * rr / 1024;
        disc(&canvas, sx, sy, 8, th->seed);
        disc(&canvas, sx - 2, sy - 3, 3, 0x6b4a4eu);
    }
    for (int i = 0; i < nobs; i++)
        draw_obstacle(mx, my, obs[i] + mel_ang);
    for (int i = 0; i < nstuck; i++) {
        int a = stuck[i] + mel_ang, rr = MELON_R - 30 + pulse / 2;
        draw_knife(mx + tcos(a) * rr / 1024, my + tsin(a) * rr / 1024, a, 0);
    }
}

static void draw_chip(int i)                    /* a world: a small melon in its colours, dim while locked */
{
    const struct world *t = &themes[i];
    int x = chip[i].x + chip[i].w / 2, y = chip[i].y + 48, locked = i && sv.best < (uint32_t)(i * PER_WORLD + 1);
    uint32_t a = t->rind0, b = t->rind1, c = t->ring1, d = t->fl1;
    if (locked) {
        a = lerp(a, 0x1a2a30u, 70, 100);
        b = lerp(b, 0x1a2a30u, 70, 100);
        c = lerp(c, 0x1a2a30u, 70, 100);
        d = lerp(d, 0x1a2a30u, 70, 100);
    }
    if (i == cur_theme)
        disc(&canvas, x, y, 40, C_GOLD);
    disc(&canvas, x, y, 35, a);
    disc(&canvas, x, y, 31, b);
    disc(&canvas, x, y, 25, c);
    disc(&canvas, x, y, 21, d);
    if (locked) {                                                        /* a padlock */
        gfx_fill_round_rect(&canvas, x - 12, y - 4, 24, 18, 4, 0xd8e4e8u);
        disc(&canvas, x, y - 6, 8, 0xd8e4e8u);
        disc(&canvas, x, y - 6, 4, c);
    }
    text_c(x, y + 50, t->name, TEXT_SMALL, locked ? 0x6f8890u : i == cur_theme ? C_GOLD : C_DIM);
}

static void draw_hud(void)
{
    char num[40];
    copy_bg(0, 0, canvas.w, box.y);
    if (screen == S_MENU) {
        text_c(canvas.w / 2, 14, "MELON KNIFE", TEXT_LARGE, C_TEXT);
        text_c(canvas.w / 2, 94, "100 levels  -  4 worlds", TEXT_SMALL, C_DIM);
        return;
    }
    snprintf(num, sizeof(num), "%d", score);
    text_c(canvas.w / 2, 30, num, TEXT_LARGE, C_TEXT);
    snprintf(num, sizeof(num), "Level %d  -  %s", imax(1, imin(LEVELS, level - (screen == S_CLEAR ? 1 : 0))), th->name);
    text_c(canvas.w / 2, 108, num, TEXT_SMALL, C_DIM);
    disc(&canvas, btn_pause.x + btn_pause.w / 2, btn_pause.y + btn_pause.h / 2, 38, 0x0b1a1fu);   /* the pause button */
    disc(&canvas, btn_pause.x + btn_pause.w / 2, btn_pause.y + btn_pause.h / 2 - 2, 36, C_BTN);
    gfx_fill_round_rect(&canvas, btn_pause.x + btn_pause.w / 2 - 13, btn_pause.y + btn_pause.h / 2 - 16, 9, 30, 3, C_TEXT);
    gfx_fill_round_rect(&canvas, btn_pause.x + btn_pause.w / 2 + 4, btn_pause.y + btn_pause.h / 2 - 16, 9, 30, 3, C_TEXT);
}

static void draw_menu_foot(void)
{
    char b[64];
    int lv = sv.run_valid ? imax(1, imin(LEVELS, (int)sv.run_level)) : 1;
    if (sv.run_valid)
        snprintf(b, sizeof(b), "Level %d of %d  -  %s", lv, LEVELS, themes[theme_of(lv)].name);
    else
        snprintf(b, sizeof(b), sv.best ? "Furthest: level %u" : "Ready when you are", (unsigned)sv.best);
    text_c(canvas.w / 2, box.y + box.h + 6, b, TEXT_BODY, C_GOLD);
    int bw = canvas.w - 160, bx = 80, py = btn_play.y - 8;                      /* a bar of how far through the 100 */
    gfx_fill_round_rect(&canvas, bx, py + 2, bw, 8, 4, 0x0b1a1fu);
    gfx_fill_round_rect(&canvas, bx, py, bw, 8, 4, 0x2a4651u);
    if (sv.best)
        gfx_fill_round_rect(&canvas, bx, py, imax(10, bw * imin(100, (int)sv.best) / 100), 8, 4, C_GOLD);
    draw_button(btn_play, sv.run_valid ? "Continue" : "Play", 1, pressed == 1);
    if (sv.run_valid)
        draw_button(btn_menu, "New game", 0, pressed == 2);
    for (int i = 0; i < 4; i++)
        draw_chip(i);
}

/* the foot of the screen: the next knife, ready to throw, and the ones left behind it */
static void draw_foot(void)
{
    int y0 = box.y + box.h;
    copy_bg(0, y0, canvas.w, canvas.h - y0);
    if (screen == S_MENU) {
        draw_menu_foot();
        return;
    }
    if (screen == S_OVER)
        return;
    for (int i = 0; i < left - (flying ? 1 : 0) - 1 && i < 11; i++) {               /* the knives still to come */
        int x = 40 + i * 30, y = canvas.h - 96;
        strip(x, y, x, y + 40, 4, 8, C_STEEL);
        strip(x, y + 40, x, y + 72, 8, 8, C_HANDLE);
    }
    if ((screen == S_PLAY || screen == S_PAUSE) && left > 0 && !flying) {
        draw_knife(cx, ready_y(), 9000, 0);
        if (screen == S_PLAY && now_ms - screen_at > 150)
            text_c(canvas.w / 2, canvas.h - 52, "Tap to throw", TEXT_SMALL, C_DIM);
    }
}

static void draw_world(void)                    /* the melon's box: it is all redrawn, every frame */
{
    int ox = shake ? (int)(rnd() % (uint32_t)(shake * 2 + 1)) - shake : 0, oy = shake ? (int)(rnd() % (uint32_t)(shake * 2 + 1)) - shake : 0;
    copy_bg(box.x, box.y, box.w, box.h);
    if (screen != S_CLEAR || now_ms - screen_at < 90)
        draw_melon(ox, oy);
    if (flying) {
        for (int g = 3; g >= 1; g--)                                               /* a trail behind the knife */
            if (fly_y + g * 60 < ready_y() + 20)
                draw_knife(cx, fly_y + g * 60, 9000, 52 + g * 14);
        draw_knife(cx, fly_y, 9000, 0);
    }
    if ((screen == S_FAIL || (screen == S_OVER && !won)) && fall_y >= 0 && screen == S_FAIL)
        draw_knife(fall_x, fall_y, 9000 + fall_rot, 0);
    for (int i = 0; i < PART_MAX; i++)
        if (parts[i].life > 0 && parts[i].x > box.x && parts[i].x < box.x + box.w && parts[i].y > box.y && parts[i].y < box.y + box.h)
            disc(&canvas, parts[i].x, parts[i].y, parts[i].r, parts[i].col);
    if (screen == S_CLEAR && now_ms - screen_at < 1100)
        text_c(canvas.w / 2, cy - 36, level > LEVELS ? "All clear!" : "Splat!", TEXT_LARGE, C_TEXT);
    if (screen == S_PLAY && now_ms - screen_at < 1300) {
        char lv[40];
        snprintf(lv, sizeof(lv), "LEVEL %d", level);
        text_c(canvas.w / 2, cy - 44, lv, TEXT_HEADING, C_GOLD);
        if ((level - 1) % PER_WORLD == 0) {
            snprintf(lv, sizeof(lv), "World %d  -  %s", theme_of(level) + 1, th->name);
            text_c(canvas.w / 2, cy + 14, lv, TEXT_BODY, C_TEXT);
        }
    }
}

/* the pause card, and the card after a lost level or a win */
static void draw_card(void)
{
    char b[48];
    int lv = imax(1, imin(LEVELS, level));
    gfx_darken(&canvas, 0, 0, canvas.w, canvas.h, 150);
    gfx_fill_round_rect(&canvas, 60, 340, canvas.w - 120, 580, 40, 0x1d4550u);
    gfx_fill_round_rect(&canvas, 66, 346, canvas.w - 132, 568, 36, 0x153841u);
    if (screen == S_PAUSE) {
        snprintf(b, sizeof(b), "Level %d", lv);
        text_c(canvas.w / 2, 384, "PAUSED", TEXT_HEADING, C_TEXT);
        text_c(canvas.w / 2, 450, b, TEXT_LARGE, C_GOLD);
        text_c(canvas.w / 2, 540, th->name, TEXT_BODY, C_DIM);
        text_c(canvas.w / 2, 590, "Your game is saved. Come back any time.", TEXT_SMALL, C_DIM);
        draw_button(btn_play, "Resume", 1, pressed == 1);
        draw_button(btn_menu, "Menu", 0, pressed == 2);
        return;
    }
    if (won) {
        text_c(canvas.w / 2, 384, "YOU WIN!", TEXT_HEADING, C_GOLD);
        text_c(canvas.w / 2, 450, "100", TEXT_LARGE, C_GOLD);
        text_c(canvas.w / 2, 540, "Every level cleared", TEXT_BODY, C_TEXT);
        text_c(canvas.w / 2, 590, "Nice one.", TEXT_SMALL, C_DIM);
        draw_button(btn_play, "Play again", 1, pressed == 1);
    } else {
        snprintf(b, sizeof(b), "Level %d", lv);
        text_c(canvas.w / 2, 384, "OOPS!", TEXT_HEADING, C_TEXT);
        text_c(canvas.w / 2, 450, b, TEXT_LARGE, C_TEXT);
        text_c(canvas.w / 2, 540, fail_obs ? "You hit an obstacle" : "You hit a knife", TEXT_BODY, C_DIM);
        snprintf(b, sizeof(b), "Furthest  level %u", (unsigned)sv.best);
        text_c(canvas.w / 2, 590, b, TEXT_SMALL, C_DIM);
        draw_button(btn_play, "Retry level", 1, pressed == 1);
    }
    draw_button(btn_menu, "Menu", 0, pressed == 2);
}

static void layout(void)
{
    cx = canvas.w / 2;
    cy = canvas.h * 40 / 100;
    int bx = 0, bw = canvas.w;                   /* the box runs the whole width: nothing at the sides is left undrawn */
    int by = imax(0, cy - BOX), bh = imin(canvas.h - 200, cy + BOX) - by;
    box = (struct gfx_rect){ bx, by, bw, bh };
    int bwid = imin(canvas.w - 160, 440), ov = screen == S_OVER || screen == S_PAUSE;
    btn_play = (struct gfx_rect){ (canvas.w - bwid) / 2, ov ? 690 : box.y + box.h + 53, bwid, ov ? 104 : 96 };
    btn_menu = (struct gfx_rect){ (canvas.w - bwid) / 2, ov ? 810 : canvas.h - 220, bwid, ov ? 84 : 72 };
    btn_pause = (struct gfx_rect){ canvas.w - 132, 14, 120, 110 };
    for (int i = 0; i < 4; i++)
        chip[i] = (struct gfx_rect){ i * canvas.w / 4, canvas.h - 134, canvas.w / 4, 130 };
}

/* Draws what changed and shows it. force: everything. */
static void redraw(int force)
{
    if (!bgpic.px || !spr.px)
        return;
    uint32_t t0 = k86_uptime_ms();
    int card = screen == S_OVER || screen == S_PAUSE;
    int all = force || card;
    draw_world();
    if (all || hud_dirty)
        draw_hud();
    if (all || foot_dirty || flying)
        draw_foot();
    if (card)
        draw_card();
    if (all) {
        k86_present(0, 0, canvas.w, canvas.h);
    } else {
        k86_present(box.x, box.y, box.w, box.h);
        if (hud_dirty)
            k86_present(0, 0, canvas.w, box.y);
        if (foot_dirty || flying)
            k86_present(0, box.y + box.h, canvas.w, canvas.h - box.y - box.h);
    }
    hud_dirty = foot_dirty = flying;                      /* (the knife's column is drawn while it flies, and once after) */
    perf_sum += k86_uptime_ms() - t0;
    perf_max = imax(perf_max, (int)(k86_uptime_ms() - t0));
    if (++perf_n >= 150) {                                /* the Log app shows how long a frame takes, to tune it */
        k86_logf("melonknife: draw avg %d ms, max %d", (int)(perf_sum / (uint32_t)perf_n), perf_max);
        perf_n = perf_max = 0;
        perf_sum = 0;
    }
}

static int in_front(void)
{
    struct k86_task tasks[K86_TASKS_MAX];
    struct k86_meminfo mem;
    int n = k86_tasks(tasks, K86_TASKS_MAX, &mem);
    for (int i = 0; i < n && i < K86_TASKS_MAX; i++)
        if (tasks[i].flags & K86_TASK_SELF)
            return (tasks[i].flags & K86_TASK_FRONT) != 0;
    return 1;
}

static void to_menu(void)
{
    screen = S_MENU;
    pressed = 0;
    nstuck = nobs = 0;
    flying = 0;
    set_theme(theme_of(sv.run_valid ? imax(1, (int)sv.run_level) : 1));
    k86_sfx_music(cur_theme);
    layout();
    redraw(1);
}

static void pause_game(void)
{
    if (screen != S_PLAY)
        return;
    screen = S_PAUSE;
    pressed = 0;
    layout();
    k86_sfx_close();
    k86_sfx_play(SFX_PAUSE, 0);
    k86_timer_set(FRAME_MS);
    redraw(1);
}

static void resume_game(void)
{
    k86_sfx_play(SFX_RESUME, 0);
    k86_sfx_music(cur_theme);
    screen = S_PLAY;
    pressed = 0;
    layout();
    last_frame = now_ms = k86_uptime_ms();
    screen_at = now_ms - 5000;
    hud_dirty = foot_dirty = 1;
    k86_timer_set(FRAME_MS);
    redraw(1);
}

static void frame(void)
{
    now_ms = k86_uptime_ms();
    int dt = imin(100, (int)(now_ms - last_frame));
    last_frame = now_ms;
    if (shake > 0)
        shake = imax(0, shake - 1);
    if (screen == S_MENU && !in_front()) {
        hidden = 1;
        k86_sfx_close();
        k86_timer_set(0);
        return;
    }
    k86_sfx_pump();
    if (screen == S_MENU) {
        mel_ang = norm(mel_ang + 4500 * dt / 1000);        /* the melon turns on the menu, too */
        redraw(0);
        return;
    }
    if (screen == S_OVER || screen == S_PAUSE) {            /* nothing moves: the timer only runs for the last of the sound */
        if (!k86_sfx_busy())
            k86_timer_set(0);
        return;
    }
    in_level_ms += dt;
    if (screen == S_PLAY)
        mel_ang = norm(mel_ang + spin_speed() * dt / 1000);
    else if (screen == S_CLEAR)
        mel_ang = norm(mel_ang + 6000 * dt / 1000);
    if (pulse > 0)
        pulse = imax(0, pulse - dt / 10);
    for (int i = 0; i < PART_MAX; i++)
        if (parts[i].life > 0) {
            parts[i].life -= dt;
            parts[i].x += parts[i].vx * dt / 1000;
            parts[i].y += parts[i].vy * dt / 1000;
            parts[i].vy += 1500 * dt / 1000;
        }
    if (flying) {
        fly_y -= THROW_SPEED * dt / 1000;
        if (fly_y <= cy + MELON_R - 40)
            land();
    }
    if (screen == S_FAIL) {
        fall_y += 700 * dt / 1000 + (int)(now_ms - screen_at) * dt / 1000 / 2;
        fall_x += fall_vx * dt / 1000;
        fall_rot += 700 * dt / 10;
        if (now_ms - screen_at > 950) {
            finish_game();
            layout();
            k86_timer_set(0);
            redraw(1);
            return;
        }
    }
    if (screen == S_CLEAR && now_ms - screen_at > 1150) {
        if (level > LEVELS) {                            /* all 100 done */
            won = 1;
            sv.run_valid = 0;
            sv.best = LEVELS;
            save();
            k86_sfx_music(-1);
            screen = S_OVER;
            layout();
            k86_timer_set(0);
            redraw(1);
            return;
        }
        start_level();
    }
    if (!in_front() && screen == S_PLAY) {
        pause_game();
        k86_sfx_close();
        k86_timer_set(0);
        return;
    }
    redraw(0);
}

static int hit(struct gfx_rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static int unlocked(int i)
{
    return i == 0 || sv.best >= (uint32_t)(i * PER_WORLD + 1);
}

static int press_id(int x, int y)
{
    if (hit(btn_play, x, y))
        return 1;
    if (hit(btn_menu, x, y) && (screen != S_MENU || sv.run_valid) && !(screen == S_OVER && won && 0))
        return 2;
    if (screen == S_MENU)
        for (int i = 0; i < 4; i++)
            if (hit(chip[i], x, y) && unlocked(i))
                return 3 + i;
    return 0;
}

static void act(int id)
{
    if (screen == S_MENU) {
        if (id == 1 && sv.run_valid)
            start_game(imax(1, imin(LEVELS, (int)sv.run_level)), (int)sv.run_score);
        else if (id == 1 || id == 2)
            start_game(1, 0);
        else if (id >= 3)
            start_game((id - 3) * PER_WORLD + 1, 0);
    } else if (screen == S_OVER) {
        if (id == 1)
            won ? start_game(1, 0) : start_game(level, score0);
        else if (id == 2)
            to_menu();
    } else if (screen == S_PAUSE) {
        if (id == 1)
            resume_game();
        else if (id == 2) {
            k86_sfx_play(SFX_BACK, 0);
            to_menu();
        }
    }
}

static void pointer(const struct k86_event *ev)
{
    int down = ev->buttons & 1, changed = ev->changed & 1;
    now_ms = k86_uptime_ms();
    if (screen == S_PLAY) {
        if (down && changed) {
            if (hit(btn_pause, ev->x, ev->y))
                pause_game();
            else
                throw_knife();
        }
        return;
    }
    if (screen == S_MENU || screen == S_OVER || screen == S_PAUSE) {
        if (down && changed) {
            pressed = press_id(ev->x, ev->y);
            if (pressed)
                k86_sfx_play(SFX_TAP, 0);
            redraw(1);
        } else if (!down && changed) {
            int id = pressed;
            pressed = 0;
            if (id && press_id(ev->x, ev->y) == id)
                act(id);
            else
                redraw(1);
        }
    }
}

int main(void)
{
    if (k86_window_open(&win))
        return 1;
    k86_window_bare(1);                         /* a game: no header bar, the whole window */
    if (k86_window_open(&win))                  /* (the canvas is bigger now) */
        return 1;
    canvas = (struct gfx_surface){ win.canvas, win.w, win.h, win.stride };
    trig_init();
    rng_state = k86_uptime_ms() ^ 0x5bd1e995u;
    now_ms = k86_uptime_ms();
    load();
    layout();
    set_theme(theme_of(sv.run_valid ? imax(1, (int)sv.run_level) : 1));
    mel_ang = 1200;
    k86_timer_set(FRAME_MS);
    redraw(1);
    k86_sfx_music(cur_theme);
    k86_log("melonknife: open");
    for (;;) {
        struct k86_event ev;
        k86_back_wanted(screen != S_MENU);
        if (k86_wait_event(&ev))
            return 1;
        switch (ev.type) {
        case K86_EVENT_POINTER:
            if (hidden) {
                hidden = 0;
                if (screen == S_MENU)
                    k86_sfx_music(cur_theme);
            }
            pointer(&ev);
            break;
        case K86_EVENT_KEY:
            if (ev.key.pressed && ev.key.key == KEY_ESC && screen != S_MENU) {
                if (screen == S_PLAY)
                    pause_game();
                else
                    to_menu();
            } else if (ev.key.pressed && (ev.key.key == KEY_SPACE || ev.key.key == KEY_ENTER)) {
                if (screen == S_PLAY)
                    throw_knife();
                else if (screen == S_MENU || screen == S_OVER || screen == S_PAUSE)
                    act(1);
            }
            break;
        case K86_EVENT_TIMER:
            frame();
            break;
        case K86_EVENT_THEME:
            redraw(1);
            break;
        case K86_EVENT_RESIZE:
            canvas.w = ev.x;
            canvas.h = ev.y;
            layout();
            make_pictures();
            redraw(1);
            break;
        case K86_EVENT_CLOSE:
            k86_sfx_close();
            k86_timer_set(0);
            return 0;
        default:
            break;
        }
        if (!hidden && (screen == S_MENU || screen == S_PLAY || screen == S_CLEAR || screen == S_FAIL || ((screen == S_OVER || screen == S_PAUSE) && k86_sfx_busy())))
            k86_timer_set(FRAME_MS);
    }
}
