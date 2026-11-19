/* Snake: our own take on the classic, for LiteKern Go. Swipe anywhere to steer. A splash screen, a home
 * screen (speed, mode, skin, sound), a 3-2-1 countdown, smooth movement (the snake slides between cells
 * at 30 frames a second, the game itself steps on a timer), a bonus apple, particles, floating scores,
 * a shake and a game over card. Best scores are kept per speed and mode in the user's home folder.
 *
 * Everything is drawn in code with whole numbers. The game's own colours are used for the art (like
 * Brickfit's blocks); dialogs and text follow the same flat look as the rest of the system. */
#include <stdint.h>
#include <string.h>

#include "kernel/input.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"
#include "sdk/kern86.h"
#include "sdk/audio.h"

#define COLS 15
#define ROWS_MAX 26
#define SNAKE_MAX (COLS * ROWS_MAX)
#define SAVE_FILE "snake.dat"
#define SAVE_MAGIC 0x534e4b32u

/* ---- the game's palette ---- */
#define C_BG_TOP     0x143b41u
#define C_BG_BOT     0x07141au
#define C_BOARD_A    0x173a40u
#define C_BOARD_B    0x13323au
#define C_FRAME      0x2a6a70u
#define C_CARD       0x0f272cu
#define C_CARD_EDGE  0x26565cu
#define C_TEXT       0xf2fbf8u
#define C_DIM        0x8fb4b2u
#define C_APPLE      0xf0475au
#define C_APPLE_HI   0xff9aa6u
#define C_LEAF       0x5fe08au
#define C_GOLD       0xffcf3fu
#define C_DARK       0x07211au

struct skin {
    const char *name;
    uint32_t body, alt, head;
};
static const struct skin skins[] = {
    { "Emerald", 0x3fd47au, 0x34bb69u, 0x63ea95u },
    { "Ocean",   0x3fa9f5u, 0x3492dbu, 0x70c6ffu },
    { "Sunset",  0xffa040u, 0xee8828u, 0xffbe70u },
    { "Berry",   0xc56bffu, 0xae55ebu, 0xdb97ffu },
};
#define SKINS ((int)(sizeof(skins) / sizeof(skins[0])))

static const char *speed_names[3] = { "Easy", "Normal", "Hard" };
static const int speed_base_ms[3] = { 200, 150, 112 };
static const char *mode_names[2] = { "Walls", "Wrap" };

enum screen { S_SPLASH, S_MENU, S_PLAY, S_PAUSE, S_DYING, S_OVER };
enum { B_NONE = -1, B_PLAY, B_SPEED0, B_SPEED1, B_SPEED2, B_MODE0, B_MODE1, B_SKIN0, B_SKIN1, B_SKIN2,
       B_SKIN3, B_SOUND, B_PAUSE, B_RESUME, B_RESTART, B_MENU, B_AGAIN, B_COUNT };

struct pt {
    int8_t x, y;
};

struct save {
    uint32_t magic;
    uint32_t best[6];                   /* [speed * 2 + mode] */
    uint8_t speed, mode, skin, sound;
};

struct particle {
    int x, y;                           /* 1/16 px, in the board */
    int vx, vy;
    int life, life0;                    /* ms */
    uint32_t colour;
    int size;
};
#define PART_MAX 64

struct floater {
    int x, y;
    int born;
    uint32_t colour;
    char text[10];
};
#define FLOAT_MAX 6

static struct k86_window win;
static struct gfx_surface canvas;
static struct save sv;
static char home[K86_PATH_MAX];
static int vol = -1;

static enum screen screen;
static uint32_t screen_since;           /* ms */
static uint32_t now_ms;
static uint32_t rng_state;

static struct gfx_rect btn[B_COUNT];    /* every button's place on the current screen; w == 0: not there */
static int pressed = B_NONE;

/* layout */
static int margin, cell, bw, bh, bx, by, rows_avail, rows;
static struct gfx_rect header_r, board_r, pause_r;

/* the game */
static struct pt snake[SNAKE_MAX];
static int len;
static struct pt old_tail;
static int dir_x, dir_y;
static struct pt queue[3];
static int queued;
static int grow;
static uint32_t tick_count;
static uint32_t step_start;
static int step_ms;
static int apples, score, best_at_start, new_best;
static struct pt food;
static uint32_t food_born;
static int bonus_on;
static struct pt bonus;
static uint32_t bonus_end;
static uint32_t count_start;            /* the 3-2-1 */
static int counting;
static uint32_t score_flash;
static uint32_t dying_since;
static struct particle parts[PART_MAX];
static struct floater floats[FLOAT_MAX];
static uint32_t last_frame;
static uint32_t front_checked;
static int swiping, swipe_x, swipe_y;
static int hdr_dirty;
static int fading;

/* ---- small helpers ---- */
static uint32_t rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iabs(int a) { return a < 0 ? -a : a; }

/* sin of degrees, -256..256 */
static int isin(int deg)
{
    deg %= 360;
    if (deg < 0)
        deg += 360;
    int neg = deg >= 180;
    if (neg)
        deg -= 180;
    int v = 4 * deg * (180 - deg) * 256 / (40500 - deg * (180 - deg));
    return neg ? -v : v;
}

/* 0..256 -> 0..256, smooth in and out */
static int smooth(int t)
{
    t = imax(0, imin(256, t));
    return t * t * (3 * 256 - 2 * t) >> 16;
}

/* 0..256 -> a little past 256 and back: things that pop in */
static int pop(int t)
{
    t = imax(0, imin(256, t));
    int s = smooth(t);
    return s + isin(t * 180 / 256) * 40 / 256;
}

static void num_text(int v, char *out)
{
    char tmp[12];
    int n = 0;
    if (v < 0)
        v = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 11);
    for (int i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

static void cat(char *dst, const char *src)
{
    while (*dst)
        dst++;
    while ((*dst++ = *src++))
        ;
}

static void sound(int hz1, int ms1, int hz2, int ms2)
{
    if (sv.sound)
        k86_audio_beep(hz1, ms1, hz2, ms2);
}

static int best_slot(void)
{
    return sv.speed * 2 + sv.mode;
}

static void load(void)
{
    struct save s;
    memset(&sv, 0, sizeof(sv));
    sv.magic = SAVE_MAGIC;
    sv.speed = 1;
    sv.sound = 1;
    vol = k86_home(home);
    if (vol >= 0 && k86_read(vol, home, SAVE_FILE, &s, sizeof(s)) == (int)sizeof(s) && s.magic == SAVE_MAGIC &&
        s.speed < 3 && s.mode < 2 && s.skin < SKINS)
        sv = s;
}

static void save(void)
{
    if (vol >= 0)
        k86_write(vol, home, SAVE_FILE, &sv, sizeof(sv));
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

/* ---- drawing helpers ---- */
static void bg_rows(int x, int y, int w, int h)
{
    int y1 = imin(y + h, canvas.h);
    for (int row = imax(y, 0); row < y1; row++)
        gfx_fill_rect(&canvas, x, row, w, 1, gfx_mix(C_BG_BOT, C_BG_TOP, row * 255 / imax(1, canvas.h - 1)));
}

static int text_w(const char *s, enum text_style st)
{
    return text_width(s, st);
}

static void text_c(int cx, int y, const char *s, enum text_style st, uint32_t col)
{
    text_draw(&canvas, cx - text_w(s, st) / 2, y, s, st, col);
}

static void text_shadow_c(int cx, int y, const char *s, enum text_style st, uint32_t col)
{
    text_c(cx + 2, y + 3, s, st, 0x04100fu);
    text_c(cx, y, s, st, col);
}

static void pill(struct gfx_rect r, uint32_t col, int lift)
{
    gfx_fill_round_rect(&canvas, r.x, r.y + 5, r.w, r.h, r.h / 2, gfx_mix(0x000000u, col, 110));
    gfx_fill_round_rect(&canvas, r.x, r.y + lift, r.w, r.h, r.h / 2, col);
}

/* a rounded box with a thin edge (two anti-aliased shapes: nothing square is left at the corners) */
static void box(struct gfx_rect r, int rad, uint32_t fill, uint32_t edge)
{
    gfx_fill_round_rect(&canvas, r.x, r.y, r.w, r.h, rad, edge);
    gfx_fill_round_rect(&canvas, r.x + 1, r.y + 1, r.w - 2, r.h - 2, rad - 1, fill);
}

static void button_text(struct gfx_rect r, const char *label, enum text_style st, uint32_t col, int lift)
{
    text_c(r.x + r.w / 2, r.y + lift + (r.h - text_height(st)) / 2, label, st, col);
}

static int hit(struct gfx_rect r, int x, int y)
{
    return r.w > 0 && x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static int button_at(int x, int y)
{
    for (int i = 0; i < B_COUNT; i++)
        if (hit(btn[i], x, y))
            return i;
    return B_NONE;
}

static uint32_t darker(uint32_t c, int a) { return gfx_mix(0x000000u, c, a); }
static uint32_t lighter(uint32_t c, int a) { return gfx_mix(0xffffffu, c, a); }

/* ---- the snake's picture ---- */
/* One capsule between two points of the board (pixel coordinates, any values): drawn once for every copy of
 * the board that touches it, so a snake going through the edge of a wrapping board shows on both sides. */
static void capsule(struct gfx_surface *s, int ax, int ay, int bxx, int byy, int hw, uint32_t col)
{
    int nx = ((ax % bw) + bw) % bw, ny = ((ay % bh) + bh) % bh;
    bxx += nx - ax;
    byy += ny - ay;
    ax = nx;
    ay = ny;
    for (int sy = -1; sy <= 1; sy++)
        for (int sx = -1; sx <= 1; sx++) {
            int x0 = imin(ax, bxx) - hw + sx * bw, y0 = imin(ay, byy) - hw + sy * bh;
            int w = iabs(ax - bxx) + 2 * hw, h = iabs(ay - byy) + 2 * hw;
            if (x0 >= bw || y0 >= bh || x0 + w <= 0 || y0 + h <= 0)
                continue;
            gfx_fill_round_rect(s, x0, y0, w, h, hw, col);
        }
}

static int wrap_delta(int d, int n)
{
    if (d > 1)
        d -= n;
    else if (d < -1)
        d += n;
    return d;
}

static void draw_eyes(struct gfx_surface *s, int hx, int hy, int dx, int dy, int dead)
{
    int e = cell * 2 / 9, off = cell / 4, fwd = cell / 6;
    int px = dx * fwd, py = dy * fwd;
    int sx = -dy, sy = dx;                   /* sideways */
    for (int k = -1; k <= 1; k += 2) {
        int ex = hx + px + sx * k * off, ey = hy + py + sy * k * off;
        if (dead) {
            gfx_thick_line(s, ex - e / 2, ey - e / 2, ex + e / 2, ey + e / 2, 3, C_DARK);
            gfx_thick_line(s, ex - e / 2, ey + e / 2, ex + e / 2, ey - e / 2, 3, C_DARK);
        } else {
            gfx_fill_circle(s, ex, ey, e, 0xffffffu);
            gfx_fill_circle(s, ex + dx * (e / 3), ey + dy * (e / 3), imax(2, e * 5 / 9), C_DARK);
        }
    }
}

static void draw_snake(struct gfx_surface *s, int f, int flash)
{
    const struct skin *sk = &skins[sv.skin];
    int hw = cell * 38 / 100;
    int n = len;
    static int px[SNAKE_MAX + 2], py[SNAKE_MAX + 2];
    int wrap = sv.mode == 1;
    /* the points of the path, from the head: unwrapped, starting at the cell behind the head */
    int cx1 = snake[1].x * cell + cell / 2, cy1 = snake[1].y * cell + cell / 2;
    int hdx = snake[0].x - snake[1].x, hdy = snake[0].y - snake[1].y;
    if (wrap) {
        hdx = wrap_delta(hdx, COLS);
        hdy = wrap_delta(hdy, rows);
    }
    px[0] = cx1 + hdx * cell * f / 256;
    py[0] = cy1 + hdy * cell * f / 256;
    px[1] = cx1;
    py[1] = cy1;
    for (int k = 2; k < n; k++) {
        int dx = snake[k].x - snake[k - 1].x, dy = snake[k].y - snake[k - 1].y;
        if (wrap) {
            dx = wrap_delta(dx, COLS);
            dy = wrap_delta(dy, rows);
        }
        px[k] = px[k - 1] + dx * cell;
        py[k] = py[k - 1] + dy * cell;
    }
    {
        int dx = old_tail.x - snake[n - 1].x, dy = old_tail.y - snake[n - 1].y;
        if (wrap) {
            dx = wrap_delta(dx, COLS);
            dy = wrap_delta(dy, rows);
        }
        px[n] = px[n - 1] + dx * cell * (256 - f) / 256;
        py[n] = py[n - 1] + dy * cell * (256 - f) / 256;
    }
    /* tail first, head last */
    for (int k = n - 1; k >= 0; k--) {
        uint32_t col = ((tick_count - (uint32_t)k) & 1) ? sk->alt : sk->body;
        if (k == 0)
            col = sk->head;
        if (flash)
            col = (flash & 1) ? 0xff6b6bu : lighter(col, 140);
        int w = hw;
        if (k == n - 1)                      /* the tail narrows */
            w = hw * 7 / 10;
        else if (k == n - 2)
            w = hw * 9 / 10;
        capsule(s, px[k], py[k], px[k + 1], py[k + 1], w, col);
    }
    /* the eyes (and now and then the tongue) */
    {
        int hxn = px[0] % bw, hyn = py[0] % bh;
        if (hxn < 0) hxn += bw;
        if (hyn < 0) hyn += bh;
        int dx = dir_x, dy = dir_y;
        if (f < 256) {                       /* the way it is going this very moment */
            dx = hdx ? (hdx > 0 ? 1 : -1) : 0;
            dy = hdy ? (hdy > 0 ? 1 : -1) : 0;
        }
        for (int sy = -1; sy <= 1; sy++)
            for (int sx = -1; sx <= 1; sx++) {
                int ex = hxn + sx * bw, ey = hyn + sy * bh;
                if (ex < -cell || ey < -cell || ex > bw + cell || ey > bh + cell)
                    continue;
                if (flash == 0 && ((now_ms / 1400) & 1) == 0 && now_ms % 1400 < 380 && screen == S_PLAY) {
                    int tx = ex + dx * (hw + cell / 3), ty = ey + dy * (hw + cell / 3);
                    gfx_thick_line(s, ex + dx * hw, ey + dy * hw, tx, ty, 3, 0xff4f6du);
                    gfx_thick_line(s, tx, ty, tx + dx * 4 + (-dy) * 5, ty + dy * 4 + dx * 5, 2, 0xff4f6du);
                    gfx_thick_line(s, tx, ty, tx + dx * 4 - (-dy) * 5, ty + dy * 4 - dx * 5, 2, 0xff4f6du);
                }
                draw_eyes(s, ex, ey, dx, dy, screen >= S_DYING);
            }
    }
}

static void draw_apple(struct gfx_surface *s, int cx, int cy, int r, int gold)
{
    uint32_t col = gold ? C_GOLD : C_APPLE;
    gfx_fill_circle(s, cx, cy + 2, r, darker(col, 150));
    gfx_fill_circle(s, cx, cy, r, col);
    gfx_fill_circle(s, cx - r / 3, cy - r / 3, imax(2, r / 4), gold ? 0xfff3b0u : C_APPLE_HI);
    if (gold) {                              /* a little star on it */
        gfx_thick_line(s, cx, cy - r / 2, cx, cy + r / 2, 2, 0xb8860bu);
        gfx_thick_line(s, cx - r / 2, cy, cx + r / 2, cy, 2, 0xb8860bu);
    } else {
        gfx_thick_line(s, cx, cy - r + 2, cx + r / 4, cy - r - r / 3, 3, 0x6b4a2au);
        gfx_fill_round_rect(s, cx + r / 6, cy - r - r / 3, r / 2 + 2, r / 3 + 2, r / 5, C_LEAF);
    }
}

static void draw_board_cells(struct gfx_surface *s)
{
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < COLS; x++)
            gfx_fill_rect(s, x * cell, y * cell, cell, cell, ((x + y) & 1) ? C_BOARD_B : C_BOARD_A);
}

static void draw_particles(struct gfx_surface *s)
{
    for (int i = 0; i < PART_MAX; i++) {
        struct particle *p = &parts[i];
        if (p->life <= 0)
            continue;
        int r = imax(1, p->size * p->life / p->life0);
        gfx_fill_circle(s, p->x >> 4, p->y >> 4, r, p->colour);
    }
}

static void draw_floaters(struct gfx_surface *s)
{
    for (int i = 0; i < FLOAT_MAX; i++) {
        struct floater *f = &floats[i];
        if (!f->text[0])
            continue;
        int age = (int)(now_ms - (uint32_t)f->born);
        if (age > 800) {
            f->text[0] = 0;
            continue;
        }
        int y = f->y - age * 60 / 800;
        uint32_t col = age > 450 ? gfx_mix(0x1a3f44u, f->colour, imax(0, (800 - age) * 255 / 350)) : f->colour;
        int w = text_width(f->text, TEXT_BOLD);
        int x = imax(4, imin(bw - w - 4, f->x - w / 2));
        text_draw(s, x + 2, y + 2, f->text, TEXT_BOLD, 0x061214u);
        text_draw(s, x, y, f->text, TEXT_BOLD, col);
    }
}

/* the board, at its place (plus a shake) */
static void draw_board(int ox, int oy, int f, int flash)
{
    struct gfx_rect around = { board_r.x - 14, board_r.y - 14, board_r.w + 28, board_r.h + 28 };
    gfx_fill_rect(&canvas, around.x, around.y, around.w, around.h, C_BG_BOT);
    gfx_fill_round_rect(&canvas, board_r.x + ox - 5, board_r.y + oy - 5, bw + 10, bh + 10, 22, C_FRAME);
    struct gfx_surface b = gfx_sub(&canvas, (struct gfx_rect){ board_r.x + ox, board_r.y + oy, bw, bh });
    draw_board_cells(&b);
    /* the apple (pops in, then breathes) */
    {
        int age = (int)(now_ms - food_born);
        int sc = age < 300 ? pop(age * 256 / 300) : 256 + isin((int)(now_ms / 3) % 360) / 28;
        int r = cell * 36 / 100 * sc / 256;
        if (r > 0)
            draw_apple(&b, food.x * cell + cell / 2, food.y * cell + cell / 2, r, 0);
    }
    if (bonus_on) {
        int left = (int)(bonus_end - now_ms);
        if (left > 2000 || (now_ms / 120) % 2) {
            int r = cell * 40 / 100 + isin((int)(now_ms / 2) % 360) / 40;
            draw_apple(&b, bonus.x * cell + cell / 2, bonus.y * cell + cell / 2, r, 1);
        }
    }
    draw_snake(&b, f, flash);
    draw_particles(&b);
    draw_floaters(&b);
    gfx_round_corners(&b, 0, 0, bw, bh, 18, C_FRAME);
}

/* ---- the header (score cards and the pause button) ---- */
static void draw_card(struct gfx_rect r, const char *label, const char *value, uint32_t value_col)
{
    box(r, 22, C_CARD, C_CARD_EDGE);
    text_draw(&canvas, r.x + 22, r.y + 12, label, TEXT_SMALL, C_DIM);
    text_draw(&canvas, r.x + 22, r.y + 12 + text_height(TEXT_SMALL), value, TEXT_HEADING, value_col);
}

static void draw_header(void)
{
    gfx_fill_rect(&canvas, header_r.x, header_r.y, header_r.w, header_r.h, C_BG_BOT);
    int gap = 14, pw = pause_r.w;
    int cw = (header_r.w - pw - 2 * gap) / 2;
    char num[16];
    struct gfx_rect a = { header_r.x, header_r.y, cw, header_r.h - 26 };
    struct gfx_rect b = { header_r.x + cw + gap, header_r.y, cw, header_r.h - 26 };
    num_text(score, num);
    draw_card(a, "SCORE", num, (int)(now_ms - score_flash) < 280 ? C_GOLD : C_TEXT);
    num_text((int)sv.best[best_slot()] > score ? (int)sv.best[best_slot()] : score, num);
    draw_card(b, "BEST", num, C_TEXT);
    /* pause button: a round key with two bars */
    int on = pressed == B_PAUSE;
    gfx_fill_round_rect(&canvas, pause_r.x, pause_r.y, pause_r.w, pause_r.h, pause_r.w / 2,
                        on ? lighter(C_CARD, 40) : C_CARD);
    int bwid = pause_r.w / 9, bhgt = pause_r.h * 3 / 8;
    int cx = pause_r.x + pause_r.w / 2, cy = pause_r.y + pause_r.h / 2;
    gfx_fill_round_rect(&canvas, cx - bwid * 17 / 10, cy - bhgt / 2, bwid, bhgt, bwid / 3, C_TEXT);
    gfx_fill_round_rect(&canvas, cx + bwid * 7 / 10, cy - bhgt / 2, bwid, bhgt, bwid / 3, C_TEXT);
    /* the bonus apple's time under the cards */
    int ty = header_r.y + header_r.h - 14;
    if (bonus_on) {
        gfx_fill_round_rect(&canvas, header_r.x, ty, header_r.w - pw - gap, 8, 4, C_CARD);
        int left = imax(0, (int)(bonus_end - now_ms));
        int wfull = header_r.w - pw - gap;
        gfx_fill_round_rect(&canvas, header_r.x, ty, imax(8, wfull * left / 7000), 8, 4, C_GOLD);
    }
}

/* ---- the screens ---- */
static void layout(void)
{
    margin = imax(12, canvas.w / 48);
    cell = (canvas.w - 2 * margin - 12) / COLS;
    bw = cell * COLS;
    header_r = (struct gfx_rect){ margin, 14, canvas.w - 2 * margin, 128 };
    pause_r = (struct gfx_rect){ canvas.w - margin - 104, header_r.y + 4, 104, 104 };
    int top = header_r.y + header_r.h + 18;
    rows_avail = imin(ROWS_MAX, (canvas.h - top - margin - 12) / cell);
    if (rows_avail < 8)
        rows_avail = 8;
    bx = (canvas.w - bw) / 2;
    by = top + (canvas.h - top - margin - rows_avail * cell) / 2;
    bh = rows_avail * cell;
    board_r = (struct gfx_rect){ bx, by, bw, bh };
}

static void set_buttons_none(void)
{
    memset(btn, 0, sizeof(btn));
}

/* the pause/over card */
static struct gfx_rect card_r;

static void layout_play_buttons(void)
{
    set_buttons_none();
    btn[B_PAUSE] = pause_r;
    btn[B_PAUSE].x -= 12;
    btn[B_PAUSE].y -= 6;
    btn[B_PAUSE].w += 24;
    btn[B_PAUSE].h += 24;
}

static void layout_pause(void)
{
    int cw = imin(canvas.w - 2 * margin - 40, 560), ch = 640;
    card_r = (struct gfx_rect){ (canvas.w - cw) / 2, (canvas.h - ch) / 2, cw, ch };
    set_buttons_none();
    int bwid = cw - 80, bhgt = 104, x = card_r.x + 40, y = card_r.y + 170;
    btn[B_RESUME] = (struct gfx_rect){ x, y, bwid, bhgt };
    btn[B_RESTART] = (struct gfx_rect){ x, y + bhgt + 24, bwid, bhgt };
    btn[B_MENU] = (struct gfx_rect){ x, y + 2 * (bhgt + 24), bwid, bhgt };
}

static void layout_over(void)
{
    int cw = imin(canvas.w - 2 * margin - 40, 560), ch = 700;
    card_r = (struct gfx_rect){ (canvas.w - cw) / 2, (canvas.h - ch) / 2, cw, ch };
    set_buttons_none();
    int bwid = cw - 80, bhgt = 104, x = card_r.x + 40, y = card_r.y + ch - 2 * bhgt - 64;
    btn[B_AGAIN] = (struct gfx_rect){ x, y, bwid, bhgt };
    btn[B_MENU] = (struct gfx_rect){ x, y + bhgt + 24, bwid, bhgt };
}

static struct gfx_rect menu_card;

static void layout_menu(void)
{
    set_buttons_none();
    int w = canvas.w, h = canvas.h;
    int pw = imin(w - 160, 440), ph = 112;
    int top = h * 30 / 100;
    btn[B_PLAY] = (struct gfx_rect){ (w - pw) / 2, top, pw, ph };
    int cw = imin(w - 2 * margin - 24, 620), x = (w - cw) / 2;
    int sh = 68, inner = x + 24, iw = cw - 48;
    int y = top + ph + 78;
    menu_card = (struct gfx_rect){ x, y, cw, 0 };
    y += 24 + 40;
    for (int i = 0; i < 3; i++)
        btn[B_SPEED0 + i] = (struct gfx_rect){ inner + i * (iw / 3), y, iw / 3, sh };
    y += sh + 26 + 40;
    for (int i = 0; i < 2; i++)
        btn[B_MODE0 + i] = (struct gfx_rect){ inner + i * (iw / 2), y, iw / 2, sh };
    y += sh + 26 + 40;
    int dot = 68, gapd = (iw - SKINS * dot) / (SKINS - 1);
    for (int i = 0; i < SKINS; i++)
        btn[B_SKIN0 + i] = (struct gfx_rect){ inner + i * (dot + gapd), y, dot, dot };
    y += dot + 24;
    btn[B_SOUND] = (struct gfx_rect){ inner, y, iw, 64 };
    menu_card.h = y + 64 + 24 - menu_card.y;
}

static void segmented(int first, int count, const char *const *names, int selected)
{
    struct gfx_rect all = btn[first];
    all.w = 0;
    for (int i = 0; i < count; i++)
        all.w += btn[first + i].w;
    box(all, all.h / 2, 0x0a1c20u, C_CARD_EDGE);
    for (int i = 0; i < count; i++) {
        struct gfx_rect r = btn[first + i];
        int on = i == selected;
        if (on)
            gfx_fill_round_rect(&canvas, r.x + 5, r.y + 5, r.w - 10, r.h - 10, (r.h - 10) / 2, skins[sv.skin].body);
        else if (pressed == first + i)
            gfx_fill_round_rect(&canvas, r.x + 5, r.y + 5, r.w - 10, r.h - 10, (r.h - 10) / 2, 0x1a3c42u);
        button_text(r, names[i], TEXT_BOLD, on ? C_DARK : C_DIM, 0);
    }
}

/* a few segments of a wiggling snake along a wave: the menu's and the splash's picture */
static void draw_wiggler(int cx, int cy, int span, int amp, int t, int count, uint32_t head_col)
{
    const struct skin *sk = &skins[sv.skin];
    int step = imax(1, span / count);
    for (int i = count - 1; i >= 0; i--) {
        int x = cx - span / 2 + i * step;
        int y = cy + isin((i * 11 + t / 6) % 360) * amp / 256;
        uint32_t col = i == 0 ? head_col : ((i / 4) & 1) ? sk->alt : sk->body;
        int rr = i == 0 ? 30 : 23 - (i * 13) / count;      /* it narrows toward the tail */
        gfx_fill_circle(&canvas, x, y, rr, col);
        if (i == 0) {
            gfx_fill_circle(&canvas, x - 4, y - 11, 9, 0xffffffu);
            gfx_fill_circle(&canvas, x + 13, y - 11, 9, 0xffffffu);
            gfx_fill_circle(&canvas, x - 7, y - 11, 4, C_DARK);
            gfx_fill_circle(&canvas, x + 10, y - 11, 4, C_DARK);
        }
    }
}

static void draw_menu(void)
{
    int w = canvas.w, h = canvas.h;
    bg_rows(0, 0, w, h);
    int span = w - 180, ty = h * 10 / 100;
    draw_wiggler(w / 2, ty, span, 34, (int)now_ms, 44, skins[sv.skin].head);
    text_shadow_c(w / 2, ty + 60, "SNAKE", TEXT_LARGE, C_TEXT);
    text_c(w / 2 + 1, ty + 60, "SNAKE", TEXT_LARGE, C_TEXT);            /* a touch bolder */
    gfx_fill_round_rect(&canvas, w / 2 - 36, ty + 140, 72, 6, 3, skins[sv.skin].body);
    text_c(w / 2, ty + 156, "Eat. Grow. Don't crash.", TEXT_SMALL, C_DIM);

    /* Play */
    struct gfx_rect p = btn[B_PLAY];
    int pulse = (isin((int)(now_ms / 5) % 360) + 256) * 6 / 512;
    int on = pressed == B_PLAY;
    struct gfx_rect pr = { p.x - pulse, p.y - pulse, p.w + 2 * pulse, p.h + 2 * pulse };
    gfx_blend_round_rect(&canvas, pr.x - 8, pr.y - 8, pr.w + 16, pr.h + 16, pr.h / 2 + 8, skins[sv.skin].body, 40);
    pill(pr, on ? skins[sv.skin].head : skins[sv.skin].body, on ? 5 : 0);
    button_text(pr, "PLAY", TEXT_HEADING, C_DARK, on ? 5 : 0);
    /* a triangle */
    {
        int tx = pr.x + 52, ty = pr.y + pr.h / 2;
        for (int i = 0; i < 26; i++)
            gfx_fill_rect(&canvas, tx + i, ty - (26 - i) * 6 / 10, 1, (26 - i) * 12 / 10 + 1, C_DARK);
    }
    char line[48] = "Best  ";
    char num[16];
    num_text((int)sv.best[best_slot()], num);
    cat(line, num);
    text_c(w / 2, p.y + p.h + 28, line, TEXT_BODY, C_GOLD);

    /* the settings */
    box(menu_card, 30, C_CARD, C_CARD_EDGE);
    text_draw(&canvas, btn[B_SPEED0].x + 8, btn[B_SPEED0].y - 38, "SPEED", TEXT_SMALL, C_DIM);
    segmented(B_SPEED0, 3, speed_names, sv.speed);
    text_draw(&canvas, btn[B_MODE0].x + 8, btn[B_MODE0].y - 38, "MODE", TEXT_SMALL, C_DIM);
    {
        const char *hint = sv.mode ? "Go through the edges" : "Edges end the game";
        text_draw(&canvas, btn[B_MODE1].x + btn[B_MODE1].w - 8 - text_w(hint, TEXT_SMALL), btn[B_MODE0].y - 38, hint,
                  TEXT_SMALL, C_DIM);
    }
    segmented(B_MODE0, 2, mode_names, sv.mode);
    text_draw(&canvas, btn[B_SKIN0].x + 8, btn[B_SKIN0].y - 38, "SKIN", TEXT_SMALL, C_DIM);
    for (int i = 0; i < SKINS; i++) {
        struct gfx_rect r = btn[B_SKIN0 + i];
        int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        if (i == sv.skin)
            gfx_fill_circle(&canvas, cx, cy, r.w / 2 + 6, C_TEXT);
        gfx_fill_circle(&canvas, cx, cy, r.w / 2 - (i == sv.skin ? 2 : 6), C_CARD);
        gfx_fill_circle(&canvas, cx, cy, r.w / 2 - (i == sv.skin ? 7 : 11), skins[i].body);
        gfx_fill_circle(&canvas, cx - 8, cy - 8, 8, skins[i].head);
    }
    {
        struct gfx_rect r = btn[B_SOUND];
        box(r, r.h / 2, pressed == B_SOUND ? 0x1a3c42u : 0x0a1c20u, C_CARD_EDGE);
        text_draw(&canvas, r.x + 34, r.y + (r.h - text_height(TEXT_BOLD)) / 2, "Sound", TEXT_BOLD, C_TEXT);
        int tw = 92, th = 44, tx = r.x + r.w - tw - 30, ty = r.y + (r.h - th) / 2;
        gfx_fill_round_rect(&canvas, tx, ty, tw, th, th / 2, sv.sound ? skins[sv.skin].body : 0x2a4a50u);
        int kx = sv.sound ? tx + tw - th + 4 : tx + 4;
        gfx_fill_circle(&canvas, kx + th / 2 - 4, ty + th / 2, th / 2 - 4, C_TEXT);
    }
    text_c(w / 2, h - 52, "Swipe anywhere to steer", TEXT_SMALL, C_DIM);
}

static void draw_splash(void)
{
    int w = canvas.w, h = canvas.h;
    int t = (int)(now_ms - screen_since);
    bg_rows(0, 0, w, h);
    int appear = smooth(t * 256 / 500);
    int cy = h * 40 / 100;
    draw_wiggler(w / 2, cy, (w - 160) * appear / 256 + 1, 40, t * 2, 44, skins[sv.skin].head);
    if (t > 250)
        text_shadow_c(w / 2, cy + 70, "SNAKE", TEXT_LARGE, C_TEXT);
    /* the loader: a bar that fills, the snake's head riding the front of it */
    int bwid = w * 6 / 10, bxx = (w - bwid) / 2, byy = h * 68 / 100;
    int fill = imax(0, imin(256, (t - 200) * 256 / 900));
    gfx_fill_round_rect(&canvas, bxx, byy, bwid, 14, 7, 0x0a1c20u);
    if (fill > 0) {
        gfx_fill_round_rect(&canvas, bxx, byy, imax(14, bwid * smooth(fill) / 256), 14, 7, skins[sv.skin].body);
        int hx = bxx + imax(14, bwid * smooth(fill) / 256) - 7;
        gfx_fill_circle(&canvas, hx, byy + 7, 16, skins[sv.skin].head);
        gfx_fill_circle(&canvas, hx + 6, byy + 3, 4, C_DARK);
    }
    text_c(w / 2, byy + 44, "Loading", TEXT_SMALL, C_DIM);
}

static void draw_countdown(void)
{
    int t = (int)(now_ms - count_start);
    int step = t / 600;
    if (step > 3)
        return;
    int local = t % 600;
    const char *label = step == 0 ? "3" : step == 1 ? "2" : step == 2 ? "1" : "GO!";
    int cx = board_r.x + board_r.w / 2, cy = board_r.y + board_r.h / 2;
    int rad = 100 + (600 - local) / 12;
    gfx_blend_round_rect(&canvas, cx - rad, cy - rad, 2 * rad, 2 * rad, rad, 0x000000u, 120);
    text_shadow_c(cx, cy - text_height(TEXT_LARGE) / 2, label, TEXT_LARGE, step == 3 ? skins[sv.skin].head : C_TEXT);
}

static void draw_card_box(struct gfx_rect r, int lift)
{
    gfx_blend_round_rect(&canvas, r.x - 6, r.y + lift + 8, r.w + 12, r.h + 8, 40, 0x000000u, 90);
    box((struct gfx_rect){ r.x, r.y + lift, r.w, r.h }, 36, C_CARD, C_CARD_EDGE);
}

static void draw_big_button(int id, const char *label, int primary, int lift)
{
    struct gfx_rect r = btn[id];
    r.y += lift;
    int on = pressed == id;
    uint32_t col = primary ? (on ? skins[sv.skin].head : skins[sv.skin].body) : (on ? 0x2a5159u : 0x1b3d43u);
    pill(r, col, on ? 4 : 0);
    button_text(r, label, TEXT_BOLD, primary ? C_DARK : C_TEXT, on ? 4 : 0);
}

static void draw_pause_card(void)
{
    draw_card_box(card_r, 0);
    text_c(card_r.x + card_r.w / 2, card_r.y + 56, "Paused", TEXT_HEADING, C_TEXT);
    char line[40] = "Score  ";
    char num[16];
    num_text(score, num);
    cat(line, num);
    text_c(card_r.x + card_r.w / 2, card_r.y + 112, line, TEXT_BODY, C_DIM);
    draw_big_button(B_RESUME, "Resume", 1, 0);
    draw_big_button(B_RESTART, "Restart", 0, 0);
    draw_big_button(B_MENU, "Menu", 0, 0);
}

static void draw_over_card(void)
{
    int t = (int)(now_ms - dying_since) - 750;
    if (t < 0)
        return;
    int e = pop(t * 256 / 420);
    int lift = (256 - e) * 90 / 256;
    gfx_darken(&canvas, 0, 0, canvas.w, canvas.h, imin(150, t * 150 / 300));
    draw_card_box(card_r, lift);
    int cx = card_r.x + card_r.w / 2, y = card_r.y + lift;
    text_c(cx, y + 48, "GAME OVER", TEXT_HEADING, C_TEXT);
    /* the score counts up */
    int shown = score * imin(1000, imax(0, t - 250)) / 1000;
    char num[16];
    num_text(shown, num);
    text_c(cx, y + 130, num, TEXT_LARGE, new_best && shown == score ? C_GOLD : C_TEXT);
    text_c(cx, y + 215, "points", TEXT_SMALL, C_DIM);
    if (new_best && t > 1200) {
        int pulse = 256 + isin((int)(now_ms / 3) % 360) / 6;
        struct gfx_rect badge = { cx - 130 * pulse / 256, y + 262, 260 * pulse / 256, 56 };
        gfx_fill_round_rect(&canvas, badge.x, badge.y, badge.w, badge.h, 28, C_GOLD);
        text_c(cx, badge.y + (badge.h - text_height(TEXT_BOLD)) / 2, "NEW BEST!", TEXT_BOLD, C_DARK);
    } else if (!new_best) {
        char line[40] = "Best  ";
        num_text((int)sv.best[best_slot()], num);
        cat(line, num);
        text_c(cx, y + 270, line, TEXT_BODY, C_GOLD);
    }
    draw_big_button(B_AGAIN, "Play Again", 1, lift);
    draw_big_button(B_MENU, "Menu", 0, lift);
}

/* the whole game picture (not the overlays) */
static void draw_game_full(int ox, int oy, int f, int flash)
{
    gfx_fill_rect(&canvas, 0, 0, canvas.w, canvas.h, C_BG_BOT);
    draw_header();
    draw_board(ox, oy, f, flash);
}

/* ---- the game's rules ---- */
static int cell_free(int x, int y)
{
    for (int i = 0; i < len; i++)
        if (snake[i].x == x && snake[i].y == y)
            return 0;
    if (food.x == x && food.y == y)
        return 0;
    if (bonus_on && bonus.x == x && bonus.y == y)
        return 0;
    return 1;
}

static int place_random(struct pt *out)
{
    static struct pt freec[SNAKE_MAX];
    int n = 0;
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < COLS; x++)
            if (cell_free(x, y)) {
                freec[n].x = (int8_t)x;
                freec[n].y = (int8_t)y;
                n++;
            }
    if (!n)
        return 0;
    *out = freec[rnd() % (uint32_t)n];
    return 1;
}

static void spawn_particles(int cx, int cy, uint32_t col, int count, int power)
{
    for (int i = 0; i < PART_MAX && count > 0; i++) {
        if (parts[i].life > 0)
            continue;
        int ang = (int)(rnd() % 360);
        int sp = power / 2 + (int)(rnd() % (uint32_t)power);
        parts[i].x = cx << 4;
        parts[i].y = cy << 4;
        parts[i].vx = isin(ang + 90) * sp / 256;
        parts[i].vy = isin(ang) * sp / 256;
        parts[i].life = parts[i].life0 = 380 + (int)(rnd() % 300);
        parts[i].colour = (rnd() & 3) ? col : 0xffffffu;
        parts[i].size = 3 + (int)(rnd() % 4);
        count--;
    }
}

static void add_floater(int cx, int cy, const char *text, uint32_t col)
{
    int slot = 0;
    for (int i = 0; i < FLOAT_MAX; i++)
        if (!floats[i].text[0]) {
            slot = i;
            break;
        }
    floats[slot].x = cx;
    floats[slot].y = cy;
    floats[slot].born = (int)now_ms;
    floats[slot].colour = col;
    int n = 0;
    while (text[n] && n < (int)sizeof(floats[slot].text) - 1) {
        floats[slot].text[n] = text[n];
        n++;
    }
    floats[slot].text[n] = 0;
}

static int idle;                        /* not in front: the timer is slow and nothing is drawn */

static uint32_t timer_ms(enum screen s)
{
    return s == S_PAUSE ? 0 : s == S_MENU || s == S_OVER ? 40 : 30;
}

static void go(enum screen s)
{
    screen = s;
    screen_since = now_ms;
    pressed = B_NONE;
    fading = 1;
    swiping = 0;
    if (s == S_MENU || s == S_SPLASH)
        layout_menu();
    else if (s == S_PLAY)
        layout_play_buttons();
    else if (s == S_PAUSE)
        layout_pause();
    else if (s == S_OVER) {
        layout_over();
        btn[B_PAUSE] = (struct gfx_rect){ 0, 0, 0, 0 };
    }
    idle = 0;
    k86_timer_set(timer_ms(s));
}

static void start_game(void)
{
    memset(parts, 0, sizeof(parts));
    memset(floats, 0, sizeof(floats));
    rows = rows_avail;
    len = 4;
    for (int i = 0; i < len; i++) {
        snake[i].x = (int8_t)(5 - i);
        snake[i].y = (int8_t)(rows / 2);
    }
    old_tail = snake[len - 1];
    dir_x = 1;
    dir_y = 0;
    queued = 0;
    grow = 0;
    tick_count = 0;
    apples = 0;
    score = 0;
    new_best = 0;
    bonus_on = 0;
    best_at_start = (int)sv.best[best_slot()];
    step_ms = speed_base_ms[sv.speed];
    food.x = -1;
    food.y = -1;
    place_random(&food);
    food_born = now_ms;
    counting = 1;
    count_start = now_ms;
    step_start = now_ms;
    score_flash = 0;
    front_checked = now_ms;
    go(S_PLAY);
}

static int steer(int dx, int dy)
{
    int ldx = queued ? queue[queued - 1].x : dir_x, ldy = queued ? queue[queued - 1].y : dir_y;
    if ((dx == ldx && dy == ldy) || (dx == -ldx && dy == -ldy))
        return 0;
    if (queued >= 3)
        return 0;
    queue[queued].x = (int8_t)dx;
    queue[queued].y = (int8_t)dy;
    queued++;
    return 1;
}

static void die(void)
{
    screen = S_DYING;
    dying_since = now_ms;
    pressed = B_NONE;
    sound(220, 130, 147, 260);
    int hx = snake[0].x * cell + cell / 2, hy = snake[0].y * cell + cell / 2;
    spawn_particles(hx, hy, C_APPLE, 28, 1500);
    spawn_particles(hx, hy, skins[sv.skin].body, 16, 1100);
    k86_timer_set(30);
    set_buttons_none();
    int final = score;
    if (final > (int)sv.best[best_slot()]) {
        sv.best[best_slot()] = (uint32_t)final;
        new_best = final > 0;
        save();
    }
}

static void tick(void)
{
    if (queued) {
        dir_x = queue[0].x;
        dir_y = queue[0].y;
        for (int i = 1; i < queued; i++)
            queue[i - 1] = queue[i];
        queued--;
    }
    int nx = snake[0].x + dir_x, ny = snake[0].y + dir_y;
    if (sv.mode == 1) {
        nx = (nx + COLS) % COLS;
        ny = (ny + rows) % rows;
    } else if (nx < 0 || ny < 0 || nx >= COLS || ny >= rows) {
        die();
        return;
    }
    int limit = grow > 0 ? len : len - 1;      /* the tail moves away this step, unless it's growing */
    for (int i = 0; i < limit; i++)
        if (snake[i].x == nx && snake[i].y == ny) {
            die();
            return;
        }
    old_tail = snake[len - 1];
    if (grow > 0) {
        grow--;
        if (len < SNAKE_MAX)
            len++;
    }
    for (int i = len - 1; i > 0; i--)
        snake[i] = snake[i - 1];
    snake[0].x = (int8_t)nx;
    snake[0].y = (int8_t)ny;
    tick_count++;
    int mult = sv.speed + 1;
    if (nx == food.x && ny == food.y) {
        apples++;
        grow++;
        int gain = 10 * mult;
        score += gain;
        score_flash = now_ms;
        char t[10] = "+";
        char n[10];
        num_text(gain, n);
        cat(t, n);
        int cx = nx * cell + cell / 2, cy = ny * cell + cell / 2;
        add_floater(cx, cy - cell / 2, t, C_TEXT);
        spawn_particles(cx, cy, C_APPLE, 14, 900);
        sound(660, 45, 990, 60);
        step_ms = imax(55, speed_base_ms[sv.speed] - 6 * imin(apples / 5, 9));
        if (!place_random(&food)) {              /* the whole board is snake: well done */
            new_best = score > best_at_start;
            sv.best[best_slot()] = (uint32_t)imax((int)sv.best[best_slot()], score);
            save();
            screen = S_DYING;
            dying_since = now_ms;
            return;
        }
        food_born = now_ms;
        if (!bonus_on && apples % 5 == 0) {
            if (place_random(&bonus)) {
                bonus_on = 1;
                bonus_end = now_ms + 7000;
            }
        }
        hdr_dirty = 1;
    } else if (bonus_on && nx == bonus.x && ny == bonus.y) {
        bonus_on = 0;
        grow += 2;
        int gain = 50 * mult;
        score += gain;
        score_flash = now_ms;
        char t[12] = "+";
        char n[10];
        num_text(gain, n);
        cat(t, n);
        int cx = nx * cell + cell / 2, cy = ny * cell + cell / 2;
        add_floater(cx, cy - cell / 2, t, C_GOLD);
        spawn_particles(cx, cy, C_GOLD, 24, 1200);
        sound(880, 55, 1320, 120);
        hdr_dirty = 1;
    }
}

static void update_particles(int dt)
{
    for (int i = 0; i < PART_MAX; i++) {
        struct particle *p = &parts[i];
        if (p->life <= 0)
            continue;
        p->life -= dt;
        p->x += p->vx * dt / 40;
        p->y += p->vy * dt / 40;
        p->vy += 14 * dt / 16;                   /* a little gravity */
    }
}

/* ---- events ---- */
static void frame(void)
{
    now_ms = k86_uptime_ms();
    int dt = imin(100, (int)(now_ms - last_frame));
    last_frame = now_ms;
    int f = 256;
    if (screen != S_PLAY && screen != S_DYING && screen != S_PAUSE && now_ms - front_checked >= 500) {
        /* Menu, splash and game over animate: not when another app is in front (the home screen, ...) */
        front_checked = now_ms;
        int front = in_front();
        if (!front && !idle) {
            idle = 1;
            k86_timer_set(500);
        } else if (front && idle) {
            idle = 0;
            k86_timer_set(timer_ms(screen));
        }
    }
    if (idle)
        return;
    switch (screen) {
    case S_SPLASH:
        draw_splash();
        if (now_ms - screen_since > 1500) {
            go(S_MENU);
            return;
        }
        break;
    case S_MENU:
        draw_menu();
        break;
    case S_PLAY: {
        update_particles(dt);
        if (counting) {
            step_start = now_ms;
            if (now_ms - count_start >= 2400) {
                counting = 0;
                sound(990, 60, 0, 0);
            } else if ((now_ms - count_start) % 600 < (uint32_t)dt && now_ms - count_start < 1800) {
                sound(440, 40, 0, 0);
            }
        } else {
            int guard = 0;
            while (screen == S_PLAY && (int)(now_ms - step_start) >= step_ms && guard++ < 3) {
                step_start += (uint32_t)step_ms;
                tick();
            }
            if (screen == S_PLAY && (int)(now_ms - step_start) >= step_ms)
                step_start = now_ms;
            if (bonus_on && now_ms >= bonus_end) {
                bonus_on = 0;
                hdr_dirty = 1;
            }
            if (now_ms - front_checked >= 500) {
                front_checked = now_ms;
                if (!in_front() && screen == S_PLAY) {
                    go(S_PAUSE);
                    draw_game_full(0, 0, 256, 0);
                    gfx_darken(&canvas, 0, 0, canvas.w, canvas.h, 140);
                    draw_pause_card();
                    k86_present(0, 0, canvas.w, canvas.h);
                    return;
                }
            }
        }
        if (screen != S_PLAY) {
            frame();                              /* it just ended: draw the dying frame */
            return;
        }
        f = counting || tick_count == 0 ? 256 : imin(256, (int)(now_ms - step_start) * 256 / step_ms);
        if (fading || hdr_dirty || (int)(now_ms - score_flash) < 400) {
            draw_game_full(0, 0, f, 0);
            hdr_dirty = 0;
        } else {
            draw_board(0, 0, f, 0);
            if (bonus_on)
                draw_header();
        }
        if (counting)
            draw_countdown();
        break;
    }
    case S_DYING: {
        update_particles(dt);
        int t = (int)(now_ms - dying_since);
        int shake = t < 600 ? (600 - t) / 40 : 0;
        int ox = shake ? ((int)(rnd() % (uint32_t)(shake * 2 + 1)) - shake) : 0;
        int oy = shake ? ((int)(rnd() % (uint32_t)(shake * 2 + 1)) - shake) : 0;
        int flash = t < 600 ? ((t / 110) % 2 ? 1 : 2) : 0;      /* red, light, red, light.. */
        draw_game_full(ox, oy, 256, flash);
        if (t >= 750) {
            go(S_OVER);
            dying_since = now_ms - 750;
            fading = 0;
        }
        draw_over_card();
        break;
    }
    case S_OVER:
        update_particles(dt);
        draw_game_full(0, 0, 256, 0);
        draw_over_card();
        break;
    case S_PAUSE:
        return;
    }
    if (fading) {
        int t = (int)(now_ms - screen_since);
        if (t >= 220)
            fading = 0;
        else
            gfx_darken(&canvas, 0, 0, canvas.w, canvas.h, 255 - t * 255 / 220);
    }
    if (screen == S_PLAY && !fading && !counting && !hdr_dirty && !bonus_on)
        k86_present(board_r.x - 14, board_r.y - 14, board_r.w + 28, board_r.h + 28);
    else
        k86_present(0, 0, canvas.w, canvas.h);
}

static void press_action(int id)
{
    switch (id) {
    case B_PLAY:
        sound(520, 40, 780, 70);
        start_game();
        break;
    case B_SPEED0: case B_SPEED1: case B_SPEED2:
        sv.speed = (uint8_t)(id - B_SPEED0);
        save();
        sound(330, 30, 0, 0);
        break;
    case B_MODE0: case B_MODE1:
        sv.mode = (uint8_t)(id - B_MODE0);
        save();
        sound(330, 30, 0, 0);
        break;
    case B_SKIN0: case B_SKIN1: case B_SKIN2: case B_SKIN3:
        sv.skin = (uint8_t)(id - B_SKIN0);
        save();
        sound(400, 30, 0, 0);
        break;
    case B_SOUND:
        sv.sound = !sv.sound;
        save();
        sound(500, 40, 0, 0);
        break;
    case B_PAUSE:
        go(S_PAUSE);
        draw_game_full(0, 0, 256, 0);
        gfx_darken(&canvas, 0, 0, canvas.w, canvas.h, 140);
        draw_pause_card();
        k86_present(0, 0, canvas.w, canvas.h);
        fading = 0;
        return;
    case B_RESUME:
        go(S_PLAY);
        counting = 1;
        count_start = now_ms - 1200;             /* a short "1, GO!" */
        step_start = now_ms;
        front_checked = now_ms;
        break;
    case B_RESTART:
    case B_AGAIN:
        sound(520, 40, 780, 70);
        start_game();
        break;
    case B_MENU:
        go(S_MENU);
        break;
    default:
        break;
    }
    frame();
}

static void pointer_event(const struct k86_event *ev)
{
    struct wg_pointer p = wg_pointer_make(ev->x, ev->y, ev->buttons, ev->changed, ev->time_ms);
    now_ms = k86_uptime_ms();
    if (screen == S_SPLASH) {
        if (p.up)
            go(S_MENU);
        return;
    }
    if (screen == S_DYING)
        return;
    if (p.down) {
        int id = button_at(p.x, p.y);
        pressed = id;
        if (screen == S_PLAY && id == B_NONE) {
            swiping = 1;
            swipe_x = p.x;
            swipe_y = p.y;
        } else if (screen != S_PLAY) {
            frame();
        }
        if (screen == S_PLAY && id != B_NONE)
            hdr_dirty = 1;
        return;
    }
    if (p.up) {
        int id = pressed;
        pressed = B_NONE;
        swiping = 0;
        if (screen == S_PLAY)
            hdr_dirty = 1;
        if (id != B_NONE && button_at(p.x, p.y) == id)
            press_action(id);
        else if (screen != S_PLAY && screen != S_PAUSE)
            frame();
        else if (screen == S_PAUSE) {
            draw_game_full(0, 0, 256, 0);
            gfx_darken(&canvas, 0, 0, canvas.w, canvas.h, 140);
            draw_pause_card();
            k86_present(0, 0, canvas.w, canvas.h);
        }
        return;
    }
    if (p.held && screen == S_PLAY && swiping) {
        int dx = p.x - swipe_x, dy = p.y - swipe_y;
        int need = imax(16, cell / 2);
        if (imax(iabs(dx), iabs(dy)) >= need) {
            if (iabs(dx) > iabs(dy))
                steer(dx > 0 ? 1 : -1, 0);
            else
                steer(0, dy > 0 ? 1 : -1);
            swipe_x = p.x;
            swipe_y = p.y;
        }
    }
}

static void key_event(const struct k86_event *ev)
{
    if (!ev->key.pressed)
        return;
    uint16_t k = ev->key.key;
    now_ms = k86_uptime_ms();
    if (k == KEY_ESC) {
        if (screen == S_PLAY && !counting)
            press_action(B_PAUSE);
        else if (screen == S_PAUSE)
            press_action(B_RESUME);
        else if (screen == S_OVER)
            press_action(B_MENU);
        return;
    }
    if (screen == S_PLAY) {
        if (k == KEY_UP)
            steer(0, -1);
        else if (k == KEY_DOWN)
            steer(0, 1);
        else if (k == KEY_LEFT)
            steer(-1, 0);
        else if (k == KEY_RIGHT)
            steer(1, 0);
    } else if (screen == S_MENU && (k == KEY_ENTER || k == KEY_SPACE)) {
        press_action(B_PLAY);
    }
}

int main(void)
{
    struct k86_header header;
    if (k86_window_open(&win))
        return 1;
    canvas = (struct gfx_surface){ win.canvas, win.w, win.h, win.stride };
    memset(&header, 0, sizeof(header));
    memcpy(header.title, "Snake", sizeof("Snake"));
    k86_header(&header);
    rng_state = k86_uptime_ms() ^ 0x5eed1234u;
    now_ms = k86_uptime_ms();
    last_frame = now_ms;
    load();
    layout();
    rows = rows_avail;
    go(S_SPLASH);
    frame();
    k86_log("snake: open");
    for (;;) {
        struct k86_event ev;
        k86_back_wanted(screen == S_PLAY || screen == S_PAUSE || screen == S_OVER);
        if (k86_wait_event(&ev)) {
            k86_timer_set(0);
            return 1;
        }
        switch (ev.type) {
        case K86_EVENT_POINTER:
            pointer_event(&ev);
            break;
        case K86_EVENT_KEY:
            key_event(&ev);
            break;
        case K86_EVENT_TIMER:
            frame();
            break;
        case K86_EVENT_THEME:
            now_ms = k86_uptime_ms();
            frame();
            break;
        case K86_EVENT_RESIZE:
            canvas.w = ev.x;
            canvas.h = ev.y;
            layout();
            now_ms = k86_uptime_ms();
            if (screen != S_SPLASH)
                go(S_MENU);
            else
                go(S_SPLASH);
            frame();
            break;
        case K86_EVENT_CLOSE:
            k86_timer_set(0);
            return 0;
        default:
            break;
        }
    }
}
