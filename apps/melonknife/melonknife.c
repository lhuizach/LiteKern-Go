/* Melon Knife: a spinning watermelon, a stack of knives. Tap to throw one: it sticks into the melon. Hit a knife
 * that's already in it and it's over. Clear the knives for a level and the melon bursts; the next one spins
 * harder, turns round and starts with knives already in it. Our own game with our own art, drawn in code.
 *
 * Whole numbers only (the apps have no floating point): angles are 1/100 of a degree, and sine and cosine come
 * from a table made at the start. To stay smooth on the phone the background (with its glow) and the melon's
 * still parts are drawn once into pictures of their own; a frame copies them, then draws what moves. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"
#include "sdk/audio.h"
#include "sdk/kern86.h"

#define SAVE_FILE "melonknife.dat"
#define SAVE_MAGIC 0x4d4b4e31u
#define FRAME_MS 33
#define MELON_R 190                     /* the melon's radius, px */
#define SPR (2 * MELON_R + 56)          /* the melon's still picture: a square this big */
#define MAX_STUCK 40
#define PART_MAX 48
#define HIT_DEG 800                     /* two knives closer than this (1/100 degree) hit each other */
#define THROW_SPEED 3200                /* px a second */
#define KNIFE_BLADE 124
#define KNIFE_HANDLE 100

#define C_TEXT 0xffffffu
#define C_DIM 0xaec4cau
#define C_GOLD 0xffd34fu
#define C_FLESH 0xf0455au
#define C_STEEL 0xe8eef6u
#define C_STEEL_DK 0x8d9bb0u
#define C_HANDLE 0x7a4a2au
#define C_BTN 0x2a4651u
#define C_BTN_PRIMARY 0x35a85eu

enum screen { S_MENU, S_PLAY, S_CLEAR, S_FAIL, S_OVER };

struct save {
    uint32_t magic, best;
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
static int cx, cy, split_y;             /* the melon's middle; the picture is redrawn in two parts, split here */
static int level, score, left, in_level_ms;
static int mel_ang;                     /* 1/100 degree */
static int stuck[MAX_STUCK], nstuck;    /* each knife's angle on the melon, 1/100 degree (0 = right, 90 = down) */
static int flying, fly_y;               /* a knife on its way: where its tip is */
static int fall_y, fall_x, fall_vx, fall_rot;   /* the knife that hit one: it drops */
static struct particle parts[PART_MAX];
static int pulse;                       /* the melon's bump when a knife lands, px */
static int shake;                       /* the whole picture's shake, px */
static int bottom_dirty = 1;
static struct gfx_rect btn_play, btn_menu;
static int pressed;                     /* 1 play, 2 menu */
static int new_best;

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

/* ---- the pictures that are drawn once ---- */
static void make_pictures(void)
{
    free(bgpic.px);
    free(spr.px);
    bgpic = (struct gfx_surface){ (uint32_t *)calloc((size_t)canvas.w * (size_t)canvas.h, 4), canvas.w, canvas.h, canvas.w };
    spr = (struct gfx_surface){ (uint32_t *)calloc((size_t)SPR * (size_t)SPR, 4), SPR, SPR, SPR };
    if (!bgpic.px || !spr.px)
        return;
    for (int y = 0; y < canvas.h; y++)                                       /* a deep teal, darker at the foot */
        gfx_fill_rect(&bgpic, 0, y, canvas.w, 1, lerp(0x1d4550u, 0x0a161au, y, canvas.h));
    for (int i = 0; i < 30; i++)                                             /* a soft glow behind the melon */
        gfx_fill_circle(&bgpic, cx, cy, MELON_R + 330 - i * 11, lerp(0x1f4a55u, 0x3a8a8cu, i, 30) & 0xfefefeu);
    for (int y = 0; y < canvas.h; y++)                                       /* (the glow's rings, blended into the tone) */
        for (int x = 0; x < canvas.w; x++) {
            uint32_t g = lerp(0x1d4550u, 0x0a161au, y, canvas.h), p = bgpic.px[y * bgpic.stride + x];
            bgpic.px[y * bgpic.stride + x] = lerp(g, p, 60, 100);
        }
    /* the melon, still parts: its picture starts as the background behind it */
    int ox = cx - SPR / 2, oy = cy - SPR / 2;
    gfx_blit(&spr, 0, 0, &bgpic, ox, oy, SPR, SPR);
    int m = SPR / 2, r = MELON_R;
    gfx_blend_round_rect(&spr, m - r + 4, m - r + 16, 2 * r, 2 * r, r, 0x050c0eu, 150);          /* its shadow */
    gfx_fill_circle(&spr, m, m, r, 0x1b6b30u);                                                    /* rind */
    gfx_fill_circle(&spr, m, m, r - 5, 0x2e9d46u);
    gfx_fill_circle(&spr, m, m, r - 22, 0xf1f6e8u);                                               /* the white ring */
    gfx_fill_circle(&spr, m, m, r - 30, 0xdfeacfu);
    for (int i = 0; i < 18; i++)                                                                  /* the flesh: paler at the edge */
        gfx_fill_circle(&spr, m, m, r - 34 - i * (r - 40) / 18, lerp(0xff93a0u, 0xe0304au, i, 17));
    gfx_blend_round_rect(&spr, m - r + 36, m - r + 30, 120, 52, 26, 0xffffffu, 46);              /* a shine */
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

static void start_level(void)
{
    nstuck = 0;
    flying = 0;
    fall_y = -1;
    in_level_ms = 0;
    mel_ang = (int)(rnd() % 36000);
    int pre = level < 2 ? 0 : imin(1 + level, 7);
    for (int i = 0; i < pre; i++)                /* knives already in it, spread so there is always a gap */
        stuck[nstuck++] = (36000 * i / pre + (int)(rnd() % (uint32_t)(36000 / pre / 3))) % 36000;
    left = imin(4 + level, 11);
    screen = S_PLAY;
    screen_at = now_ms;
    bottom_dirty = 1;
}

static void start_game(void)
{
    level = 1;
    score = 0;
    new_best = 0;
    memset(parts, 0, sizeof(parts));
    pressed = 0;
    start_level();
    k86_timer_set(FRAME_MS);
    last_frame = now_ms = k86_uptime_ms();
}

/* How fast the melon turns, 1/100 degree a second: each level has its own way of turning. */
static int spin_speed(void)
{
    int base = imin(9000 + level * 1100, 24000);
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
    if (score > (int)sv.best) {
        sv.best = (uint32_t)score;
        new_best = 1;
        save();
    }
    screen = S_OVER;
    screen_at = now_ms;
    bottom_dirty = 1;
}

static void throw_knife(void)
{
    if (screen != S_PLAY || flying || left <= 0)
        return;
    flying = 1;
    fly_y = canvas.h - 130;
    k86_audio_beep(520, 25, 0, 0);
}

static void land(void)
{
    int rel = norm(9000 - mel_ang);                 /* the bottom of the melon, in its own turning */
    flying = 0;
    for (int i = 0; i < nstuck; i++)
        if (angdiff(stuck[i], rel) < HIT_DEG) {      /* on top of one: it is over */
            fall_x = cx;
            fall_y = cy + MELON_R + 30;
            fall_vx = (stuck[i] < rel || (stuck[i] - rel > 18000)) ? 380 : -380;
            fall_rot = 0;
            add_burst(cx, cy + MELON_R, 14, C_STEEL, C_STEEL_DK, 0);
            k86_audio_beep(220, 90, 147, 220);
            shake = 22;
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
    bottom_dirty = 1;
    add_burst(cx, cy + MELON_R - 10, 14, C_FLESH, 0xffa3b0u, 0);
    if (left <= 0) {                                 /* the melon is cleared: it bursts */
        add_burst(cx, cy, PART_MAX, C_FLESH, 0x4fc060u, 1);
        k86_audio_beep(660, 70, 990, 140);
        shake = 16;
        level++;
        screen = S_CLEAR;
        screen_at = now_ms;
        return;
    }
    k86_audio_beep(330, 35, 0, 0);
}

/* ---- drawing ---- */
static void line(int x0, int y0, int x1, int y1, int w, uint32_t c)
{
    gfx_thick_line(&canvas, x0, y0, x1, y1, w, c);
}

/* A knife with its tip at (tx, ty), its handle away along `a` (1/100 degree): a tapering blade with a bright
 * edge, a guard, and a handle with two rivets. `fade` (0..100) dims it into the background for a trail. */
static void draw_knife(int tx, int ty, int a, int fade)
{
    int dx = tcos(a), dy = tsin(a);
    uint32_t steel = fade ? lerp(C_STEEL, 0x1d4550u, fade, 100) : C_STEEL, dark = fade ? lerp(C_STEEL_DK, 0x1d4550u, fade, 100) : C_STEEL_DK;
    static const int wd[5] = { 7, 13, 17, 19, 19 };
    for (int k = 0; k < 5; k++) {
        int p0 = KNIFE_BLADE * k / 5, p1 = KNIFE_BLADE * (k + 1) / 5 + 2;
        line(tx + dx * p0 / 1024, ty + dy * p0 / 1024, tx + dx * p1 / 1024, ty + dy * p1 / 1024, wd[k] + 4, dark);
        line(tx + dx * p0 / 1024, ty + dy * p0 / 1024, tx + dx * p1 / 1024, ty + dy * p1 / 1024, wd[k], steel);
    }
    if (fade)
        return;
    int bx = tx + dx * KNIFE_BLADE / 1024, by = ty + dy * KNIFE_BLADE / 1024;
    int hx = bx + dx * KNIFE_HANDLE / 1024, hy = by + dy * KNIFE_HANDLE / 1024;
    line(bx - dy * 16 / 1024, by + dx * 16 / 1024, bx + dy * 16 / 1024, by - dx * 16 / 1024, 9, 0x4a4f5bu);     /* guard */
    line(bx, by, hx, hy, 26, 0x4e2f1bu);
    line(bx, by, hx, hy, 20, C_HANDLE);
    line(bx, by, hx, hy, 6, 0xa86d3fu);
    for (int k = 0; k < 2; k++) {
        int p = KNIFE_HANDLE * (k + 1) / 3;
        gfx_fill_circle(&canvas, bx + dx * p / 1024, by + dy * p / 1024, 4, 0xe9c46au);
    }
    gfx_fill_circle(&canvas, hx, hy, 13, C_HANDLE);
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

/* the melon: its picture, then what turns on it (the stripes and seeds), then the knives in it */
static void draw_melon(int ox, int oy)
{
    int mx = cx + ox, my = cy + oy;
    gfx_blit(&canvas, mx - SPR / 2, my - SPR / 2, &spr, 0, 0, SPR, SPR);
    int r = MELON_R + pulse / 2;
    for (int i = 0; i < 14; i++) {                                                  /* the rind's stripes */
        int a0 = mel_ang + i * 2570, a1 = a0 + 1500, rr = r - 12;
        line(mx + tcos(a0) * rr / 1024, my + tsin(a0) * rr / 1024, mx + tcos(a1) * rr / 1024, my + tsin(a1) * rr / 1024, 11, 0x74d476u);
    }
    for (int i = 0; i < 10; i++) {                                                  /* seeds */
        int a = mel_ang + i * 3600, rr = 62 + (i % 3) * 24;
        int sx = mx + tcos(a) * rr / 1024, sy = my + tsin(a) * rr / 1024;
        gfx_fill_circle(&canvas, sx, sy, 9, 0x2a1418u);
        gfx_fill_circle(&canvas, sx - 2, sy - 3, 3, 0x6b4a4eu);
    }
    for (int i = 0; i < nstuck; i++) {
        int a = stuck[i] + mel_ang, rr = MELON_R - 36 + pulse / 2;
        draw_knife(mx + tcos(a) * rr / 1024, my + tsin(a) * rr / 1024, a, 0);
    }
}

static void draw_hud(void)
{
    char num[16], lv[24];
    snprintf(num, sizeof(num), "%d", score);
    text_c(canvas.w / 2, 36, num, TEXT_LARGE, C_TEXT);
    snprintf(lv, sizeof(lv), "Level %d", imax(1, level - (screen == S_CLEAR ? 1 : 0)));
    text_c(canvas.w / 2, 116, lv, TEXT_SMALL, C_DIM);
}

static void draw_bottom(void)
{
    gfx_blit(&canvas, 0, split_y, &bgpic, 0, split_y, canvas.w, canvas.h - split_y);
    if (screen == S_MENU || screen == S_OVER)
        return;
    for (int i = 0; i < left; i++) {                                                /* the knives still to throw */
        int x = 50 + i * 36, y = canvas.h - 150;
        line(x, y, x, y + 54, 12, C_STEEL);
        line(x, y + 54, x, y + 96, 15, C_HANDLE);
    }
    if (screen == S_PLAY && !flying && left > 0 && now_ms - screen_at > 150)
        text_c(canvas.w / 2, canvas.h - 120, "Tap to throw", TEXT_SMALL, C_DIM);
}

static void draw_top(void)
{
    int ox = shake ? (int)(rnd() % (uint32_t)(shake * 2 + 1)) - shake : 0, oy = shake ? (int)(rnd() % (uint32_t)(shake * 2 + 1)) - shake : 0;
    gfx_blit(&canvas, 0, 0, &bgpic, 0, 0, canvas.w, split_y);
    if (screen == S_MENU) {
        draw_melon(0, 0);
        text_c(canvas.w / 2, 60, "MELON", TEXT_LARGE, C_FLESH);
        text_c(canvas.w / 2, 138, "KNIFE", TEXT_LARGE, C_TEXT);
        return;
    }
    draw_hud();
    if (screen != S_CLEAR || now_ms - screen_at < 90)
        draw_melon(ox, oy);
    if (flying)
        for (int g = 3; g >= 1; g--)                                               /* a trail behind the knife */
            if (fly_y + g * 70 < canvas.h - 120)
                draw_knife(cx, fly_y + g * 70, 9000, 55 + g * 14);
    if (flying)
        draw_knife(cx, fly_y, 9000, 0);
    if (screen == S_FAIL && fall_y >= 0)
        draw_knife(fall_x, fall_y, 9000 + fall_rot, 0);
    for (int i = 0; i < PART_MAX; i++)
        if (parts[i].life > 0)
            gfx_fill_circle(&canvas, parts[i].x, parts[i].y, parts[i].r, parts[i].col);
    if (screen == S_CLEAR && now_ms - screen_at < 1100)
        text_c(canvas.w / 2, cy - 36, "Splat!", TEXT_LARGE, C_TEXT);
    if (screen == S_PLAY && now_ms - screen_at < 1300 && level > 1) {
        char lv[24];
        snprintf(lv, sizeof(lv), "LEVEL %d", level);
        text_c(canvas.w / 2, cy - 30, lv, TEXT_HEADING, C_GOLD);
    }
}

static void draw_menu_extras(void)
{
    char b[40];
    snprintf(b, sizeof(b), "Best  %u", (unsigned)sv.best);
    text_c(canvas.w / 2, cy + MELON_R + 64, b, TEXT_BODY, C_GOLD);
    draw_button(btn_play, "Play", 1, pressed == 1);
    text_c(canvas.w / 2, btn_play.y + btn_play.h + 40, "Tap to throw. Don't hit a knife.", TEXT_SMALL, C_DIM);
}

static void draw_over(void)
{
    gfx_darken(&canvas, 0, 0, canvas.w, canvas.h, 150);
    gfx_fill_round_rect(&canvas, 60, 340, canvas.w - 120, 580, 40, 0x102a32u);
    gfx_fill_round_rect(&canvas, 60, 340, canvas.w - 120, 580, 40, 0x1d4550u);
    gfx_fill_round_rect(&canvas, 66, 346, canvas.w - 132, 568, 36, 0x153841u);
    char b[40];
    text_c(canvas.w / 2, 384, "GAME OVER", TEXT_HEADING, C_TEXT);
    snprintf(b, sizeof(b), "%d", score);
    text_c(canvas.w / 2, 450, b, TEXT_LARGE, new_best ? C_GOLD : C_TEXT);
    text_c(canvas.w / 2, 540, new_best ? "NEW BEST!" : "knives stuck", TEXT_BODY, new_best ? C_GOLD : C_DIM);
    snprintf(b, sizeof(b), "Best  %u    Level %d", (unsigned)sv.best, level);
    text_c(canvas.w / 2, 590, b, TEXT_SMALL, C_DIM);
    draw_button(btn_play, "Play again", 1, pressed == 1);
    draw_button(btn_menu, "Menu", 0, pressed == 2);
}

static void layout(void)
{
    cx = canvas.w / 2;
    cy = canvas.h * 36 / 100;
    split_y = imin(canvas.h, cy + MELON_R + 220);
    int bw = imin(canvas.w - 160, 440);
    btn_play = (struct gfx_rect){ (canvas.w - bw) / 2, screen == S_OVER ? 690 : cy + MELON_R + 112, bw, 104 };
    btn_menu = (struct gfx_rect){ (canvas.w - bw) / 2, 810, bw, 84 };
}

/* Draws what changed and shows it: the top part (the melon, always), the bottom part (when it changed). */
static void redraw(int force)
{
    if (!bgpic.px || !spr.px)
        return;
    draw_top();
    int bottom = force || bottom_dirty || flying || screen != S_PLAY;
    if (bottom)
        draw_bottom();
    if (screen == S_MENU)
        draw_menu_extras();
    if (screen == S_OVER)
        draw_over();
    k86_present(0, 0, canvas.w, bottom ? canvas.h : split_y);
    bottom_dirty = flying;                               /* (the knife's column is drawn while it flies, and once after) */
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
    nstuck = 0;
    flying = 0;
    layout();
    redraw(1);
}

static void frame(void)
{
    now_ms = k86_uptime_ms();
    int dt = imin(100, (int)(now_ms - last_frame));
    last_frame = now_ms;
    if (shake > 0)
        shake = imax(0, shake - 1);
    if (screen == S_MENU) {
        mel_ang = norm(mel_ang + 4500 * dt / 1000);        /* the melon turns on the menu, too */
        redraw(0);
        return;
    }
    if (screen == S_OVER) {
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
        if (fly_y <= cy + MELON_R - 44)
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
        }
    }
    if (screen == S_CLEAR && now_ms - screen_at > 1150)
        start_level();
    if (!in_front() && screen == S_PLAY) {
        screen = S_MENU;
        layout();
    }
    redraw(0);
}

static int hit(struct gfx_rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static void pointer(const struct k86_event *ev)
{
    int down = ev->buttons & 1, changed = ev->changed & 1;
    now_ms = k86_uptime_ms();
    if (screen == S_PLAY) {
        if (down && changed)
            throw_knife();
        return;
    }
    if (screen == S_MENU || screen == S_OVER) {
        if (down && changed) {
            pressed = hit(btn_play, ev->x, ev->y) ? 1 : (screen == S_OVER && hit(btn_menu, ev->x, ev->y)) ? 2 : 0;
            redraw(1);
        } else if (!down && changed) {
            int id = pressed;
            pressed = 0;
            if (id == 1 && hit(btn_play, ev->x, ev->y))
                start_game();
            else if (id == 2 && hit(btn_menu, ev->x, ev->y))
                to_menu();
            else
                redraw(1);
        }
    }
}

int main(void)
{
    if (k86_window_open(&win))
        return 1;
    canvas = (struct gfx_surface){ win.canvas, win.w, win.h, win.stride };
    trig_init();
    rng_state = k86_uptime_ms() ^ 0x5bd1e995u;
    now_ms = k86_uptime_ms();
    load();
    layout();
    make_pictures();
    mel_ang = 1200;
    k86_timer_set(FRAME_MS);
    redraw(1);
    k86_log("melonknife: open");
    for (;;) {
        struct k86_event ev;
        k86_back_wanted(screen != S_MENU);
        if (k86_wait_event(&ev))
            return 1;
        switch (ev.type) {
        case K86_EVENT_POINTER:
            pointer(&ev);
            break;
        case K86_EVENT_KEY:
            if (ev.key.pressed && ev.key.key == KEY_ESC && screen != S_MENU)
                to_menu();
            else if (ev.key.pressed && (ev.key.key == KEY_SPACE || ev.key.key == KEY_ENTER)) {
                if (screen == S_PLAY)
                    throw_knife();
                else if (screen == S_MENU || screen == S_OVER)
                    start_game();
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
            k86_timer_set(0);
            return 0;
        default:
            break;
        }
        if (screen == S_MENU || screen == S_PLAY || screen == S_CLEAR || screen == S_FAIL)
            k86_timer_set(FRAME_MS);
    }
}
