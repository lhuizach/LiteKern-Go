/* Melon Knife: a spinning watermelon, a stack of knives. Tap to throw one: it sticks into the melon. Hit a knife
 * that's already in it and it's over. Clear the knives for a level and the melon bursts; the next one spins
 * harder, turns round and starts with knives already in it. Our own game with our own art, drawn in code.
 *
 * Whole numbers only (the apps have no floating point): angles are 1/100 of a degree, and sine and cosine come
 * from a table made at the start. Only the area the melon and the knives move in is redrawn each frame. */
#include <stdint.h>
#include <stdio.h>
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
#define MAX_STUCK 40
#define PART_MAX 40
#define HIT_DEG 800                     /* two knives closer than this (1/100 degree) hit each other */
#define THROW_SPEED 3200                /* px a second */

#define C_BG 0x16282eu
#define C_RIND 0x2e9d46u
#define C_RIND_LT 0x6fd070u
#define C_WHITE 0xf1f6e8u
#define C_FLESH 0xf0455au
#define C_FLESH_DK 0xd22f49u
#define C_SEED 0x2a1418u
#define C_STEEL 0xdfe7f0u
#define C_STEEL_DK 0x9aa7b8u
#define C_HANDLE 0x7a4a2au
#define C_TEXT 0xffffffu
#define C_GOLD 0xffd34fu
#define C_BTN 0x2a4651u
#define C_BTN_PRIMARY 0x35a85eu

enum screen { S_MENU, S_PLAY, S_CLEAR, S_FAIL, S_OVER };

struct save {
    uint32_t magic, best;
};

struct particle {
    int x, y, vx, vy, life;
    uint32_t col;
};

static struct k86_window win;
static struct gfx_surface canvas;
static char home[K86_PATH_MAX];
static int vol = -1;
static struct save sv;

static int sin_t[361], cos_t[361];      /* *1024, per degree */

static enum screen screen = S_MENU;
static uint32_t now_ms, last_frame, screen_at, rng_state;
static int cx, cy;                      /* the melon's middle */
static int level, score, left, in_level_ms;
static int mel_ang;                     /* 1/100 degree */
static int stuck[MAX_STUCK], nstuck;    /* each knife's angle on the melon, 1/100 degree (0 = right, 90 = down) */
static int obstacle[MAX_STUCK];         /* 1: was there at the start of the level */
static int flying, fly_y;               /* a knife on its way: where its tip is */
static int fall_y, fall_x, fall_vx, fall_rot;   /* the knife that hit one: it drops */
static struct particle parts[PART_MAX];
static int pulse;                       /* the melon's bump when a knife lands, px */
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

/* ---- trig ---- */
static void trig_init(void)
{
    const int64_t c = 65526, s = 1144;          /* cos and sin of one degree, 16.16 */
    int64_t x = 65536, y = 0;
    for (int i = 0; i <= 360; i++) {
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

static void beep(int a, int b, int c, int d)
{
    k86_audio_beep(a, b, c, d);
}

/* ---- the rules ---- */
static void add_burst(int x, int y, int n, uint32_t c1, uint32_t c2)
{
    for (int i = 0; i < n && i < PART_MAX; i++) {
        int a = (int)(rnd() % 360) * 100, sp = 120 + (int)(rnd() % 520);
        parts[i] = (struct particle){ x, y, tcos(a) * sp / 1024, tsin(a) * sp / 1024 - 120, 400 + (int)(rnd() % 400),
                                      i % 2 ? c1 : c2 };
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
    for (int i = 0; i < pre; i++) {              /* knives already in it, spread so there is always a gap */
        int a = (36000 * i / pre + (int)(rnd() % (uint32_t)(36000 / pre / 3))) % 36000;
        stuck[nstuck] = a;
        obstacle[nstuck++] = 1;
    }
    left = imin(4 + level, 11);
    screen = S_PLAY;
    screen_at = now_ms;
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
}

static void throw_knife(void)
{
    if (screen != S_PLAY || flying || left <= 0)
        return;
    flying = 1;
    fly_y = canvas.h - 120;
    beep(520, 25, 0, 0);
}

static void land(void)
{
    int rel = norm(9000 - mel_ang);                 /* the bottom of the melon, in its own turning */
    for (int i = 0; i < nstuck; i++)
        if (angdiff(stuck[i], rel) < HIT_DEG) {      /* on top of one: it is over */
            flying = 0;
            fall_x = cx;
            fall_y = cy + MELON_R + 30;
            fall_vx = (stuck[i] < rel || (stuck[i] - rel > 18000)) ? 380 : -380;
            fall_rot = 0;
            add_burst(cx, cy + MELON_R, 8, C_STEEL, C_STEEL_DK);
            beep(220, 90, 147, 220);
            screen = S_FAIL;
            screen_at = now_ms;
            return;
        }
    flying = 0;
    if (nstuck < MAX_STUCK) {
        stuck[nstuck] = rel;
        obstacle[nstuck++] = 0;
    }
    score++;
    left--;
    pulse = 14;
    add_burst(cx, cy + MELON_R - 10, 10, C_FLESH, 0xff9aa8u);
    if (left <= 0) {                                 /* the melon is cleared: it bursts */
        add_burst(cx, cy, PART_MAX, C_FLESH, C_RIND_LT);
        beep(660, 70, 990, 140);
        level++;
        screen = S_CLEAR;
        screen_at = now_ms;
        return;
    }
    beep(330, 35, 0, 0);
}

/* ---- drawing ---- */
static void line(int x0, int y0, int x1, int y1, int w, uint32_t c)
{
    gfx_thick_line(&canvas, x0, y0, x1, y1, w, c);
}

/* a knife with its tip at (tx, ty) pointing along `a` (1/100 degree) from the handle: blade len px, handle len px */
static void draw_knife(int tx, int ty, int a, int blade, int handle)
{
    int dx = tcos(a), dy = tsin(a);                  /* from the tip towards the handle */
    int bx = tx + dx * blade / 1024, by = ty + dy * blade / 1024;
    int hx = bx + dx * handle / 1024, hy = by + dy * handle / 1024;
    line(tx, ty, bx, by, 16, C_STEEL_DK);
    line(tx, ty, bx, by, 9, C_STEEL);
    line(bx - dy * 14 / 1024, by + dx * 14 / 1024, bx + dy * 14 / 1024, by - dx * 14 / 1024, 6, C_STEEL_DK);   /* the guard */
    line(bx, by, hx, hy, 22, C_HANDLE);
    line(bx, by, hx, hy, 8, 0xa5693cu);
}

static void draw_melon(void)
{
    int r = MELON_R + pulse / 2;
    gfx_fill_circle(&canvas, cx + 10, cy + 14, r, 0x0e1b20u);                       /* shadow */
    gfx_fill_circle(&canvas, cx, cy, r, C_RIND);
    for (int i = 0; i < 12; i++) {                                                  /* the stripes, turning */
        int a0 = mel_ang + i * 3000, a1 = a0 + 1500, rr = r - 8;
        line(cx + tcos(a0) * rr / 1024, cy + tsin(a0) * rr / 1024, cx + tcos(a1) * rr / 1024, cy + tsin(a1) * rr / 1024, 14, C_RIND_LT);
    }
    gfx_fill_circle(&canvas, cx, cy, r - 22, C_WHITE);
    gfx_fill_circle(&canvas, cx, cy, r - 36, C_FLESH);
    gfx_fill_circle(&canvas, cx, cy, r - 120, C_FLESH_DK);
    gfx_fill_circle(&canvas, cx, cy, r - 130, C_FLESH);
    for (int i = 0; i < 9; i++) {                                                   /* seeds */
        int a = mel_ang + i * 4000, rr = 78 + (i % 2) * 26;
        int sx = cx + tcos(a) * rr / 1024, sy = cy + tsin(a) * rr / 1024;
        gfx_fill_circle(&canvas, sx, sy, 7, C_SEED);
    }
}

static void draw_stuck(void)
{
    for (int i = 0; i < nstuck; i++) {
        int a = stuck[i] + mel_ang, r = MELON_R - 40 + pulse / 2;
        int tx = cx + tcos(a) * r / 1024, ty = cy + tsin(a) * r / 1024;
        draw_knife(tx, ty, a, 120, 96);

    }
}

static void draw_particles(void)
{
    for (int i = 0; i < PART_MAX; i++)
        if (parts[i].life > 0)
            gfx_fill_circle(&canvas, parts[i].x, parts[i].y, 6, parts[i].col);
}

static void text_c(int x, int y, const char *s, enum text_style st, uint32_t col)
{
    text_draw(&canvas, x - text_width(s, st) / 2, y, s, st, col);
}

static void draw_button(struct gfx_rect r, const char *label, int primary, int down)
{
    int lift = down ? 4 : 0;
    gfx_fill_round_rect(&canvas, r.x, r.y + 6, r.w, r.h, r.h / 2, 0x0b161au);
    gfx_fill_round_rect(&canvas, r.x, r.y + lift, r.w, r.h, r.h / 2, primary ? C_BTN_PRIMARY : C_BTN);
    text_c(r.x + r.w / 2, r.y + lift + (r.h - text_height(TEXT_BOLD)) / 2, label, TEXT_BOLD, C_TEXT);
}

static void draw_hud(void)
{
    char num[16];
    snprintf(num, sizeof(num), "%d", score);
    text_c(canvas.w / 2, 40, num, TEXT_LARGE, C_TEXT);
    char lv[24];
    snprintf(lv, sizeof(lv), "Level %d", imin(level, 99) - (screen == S_CLEAR ? 1 : 0));
    text_c(canvas.w / 2, 120, lv, TEXT_SMALL, 0xb8c9ceu);
    for (int i = 0; i < left; i++) {                                                /* the knives still to throw */
        int x = 56 + i * 34, y = canvas.h - 150;
        line(x, y, x, y + 56, 10, C_STEEL);
        line(x, y + 56, x, y + 100, 14, C_HANDLE);
    }
}

static void draw_all(void)
{
    gfx_fill_rect(&canvas, 0, 0, canvas.w, canvas.h, C_BG);
    if (screen == S_MENU) {
        draw_melon();
        text_c(canvas.w / 2, 70, "MELON", TEXT_LARGE, C_FLESH);
        text_c(canvas.w / 2, 150, "KNIFE", TEXT_LARGE, C_TEXT);
        char b[40];
        snprintf(b, sizeof(b), "Best  %u", (unsigned)sv.best);
        text_c(canvas.w / 2, cy + MELON_R + 60, b, TEXT_BODY, C_GOLD);
        draw_button(btn_play, "Play", 1, pressed == 1);
        text_c(canvas.w / 2, btn_play.y + btn_play.h + 40, "Tap to throw. Don't hit a knife.", TEXT_SMALL, 0xb8c9ceu);
        return;
    }
    draw_hud();
    draw_stuck();
    draw_melon();
    if (flying)
        draw_knife(cx, fly_y, 9000, 120, 96);
    if (screen == S_FAIL && fall_y >= 0)
        draw_knife(fall_x, fall_y, 9000 + fall_rot, 120, 96);
    draw_particles();
    if (screen == S_CLEAR && now_ms - screen_at < 1400)
        text_c(canvas.w / 2, cy - 30, "Splat!", TEXT_LARGE, C_TEXT);
    if (screen == S_OVER) {
        gfx_darken(&canvas, 0, 0, canvas.w, canvas.h, 150);
        gfx_fill_round_rect(&canvas, 60, 360, canvas.w - 120, 520, 36, 0x1d3a43u);
        text_c(canvas.w / 2, 400, "GAME OVER", TEXT_HEADING, C_TEXT);
        char b[40];
        snprintf(b, sizeof(b), "%d", score);
        text_c(canvas.w / 2, 470, b, TEXT_LARGE, new_best ? C_GOLD : C_TEXT);
        text_c(canvas.w / 2, 560, new_best ? "NEW BEST!" : "knives stuck", TEXT_BODY, new_best ? C_GOLD : 0xb8c9ceu);
        snprintf(b, sizeof(b), "Best  %u   Level %d", (unsigned)sv.best, level);
        text_c(canvas.w / 2, 610, b, TEXT_SMALL, 0xb8c9ceu);
        draw_button(btn_play, "Play again", 1, pressed == 1);
        draw_button(btn_menu, "Menu", 0, pressed == 2);
    }
}

static void present_area(void)
{
    k86_present(0, 0, canvas.w, canvas.h);
}

static void layout(void)
{
    cx = canvas.w / 2;
    cy = canvas.h * 38 / 100;
    int bw = imin(canvas.w - 160, 440);
    btn_play = (struct gfx_rect){ (canvas.w - bw) / 2, screen == S_OVER ? 690 : cy + MELON_R + 120, bw, 104 };
    btn_menu = (struct gfx_rect){ (canvas.w - bw) / 2, 810, bw, 84 };
}

static void redraw(void)
{
    layout();
    draw_all();
    present_area();
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

static void frame(void)
{
    now_ms = k86_uptime_ms();
    int dt = imin(100, (int)(now_ms - last_frame));
    last_frame = now_ms;
    if (screen == S_MENU) {
        mel_ang = norm(mel_ang + 4500 * dt / 1000);        /* the melon turns on the menu, too */
        redraw();
        return;
    }
    if (screen == S_PLAY || screen == S_CLEAR || screen == S_FAIL) {
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
                parts[i].vy += 1400 * dt / 1000;
            }
        if (flying) {
            fly_y -= THROW_SPEED * dt / 1000;
            if (fly_y <= cy + MELON_R - 40)
                land();
        }
        if (screen == S_FAIL) {
            fall_y += 900 * dt / 1000 + (now_ms - screen_at) / 20;
            fall_x += fall_vx * dt / 1000;
            fall_rot += 700 * dt / 10;
            if (now_ms - screen_at > 900)
                finish_game();
        }
        if (screen == S_CLEAR && now_ms - screen_at > 1100) {
            start_level();
        }
        if (!in_front() && screen == S_PLAY) {
            screen = S_MENU;
            k86_timer_set(FRAME_MS);
        }
        redraw();
    }
}

static int hit(struct gfx_rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static void to_menu(void)
{
    screen = S_MENU;
    pressed = 0;
    nstuck = 0;
    flying = 0;
    redraw();
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
            redraw();
        } else if (!down && changed) {
            int id = pressed;
            pressed = 0;
            if (id == 1 && hit(btn_play, ev->x, ev->y))
                start_game();
            else if (id == 2 && hit(btn_menu, ev->x, ev->y))
                to_menu();
            else
                redraw();
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
    mel_ang = 1200;
    k86_timer_set(FRAME_MS);
    redraw();
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
            redraw();
            break;
        case K86_EVENT_RESIZE:
            canvas.w = ev.x;
            canvas.h = ev.y;
            redraw();
            break;
        case K86_EVENT_CLOSE:
            k86_timer_set(0);
            return 0;
        default:
            break;
        }
    }
}
