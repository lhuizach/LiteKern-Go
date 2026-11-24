/* Stackfall: our own falling-block puzzle for LiteKern Go. Seven shapes drop into a well of 10 x 20; fill a
 * row and it clears. A bag shuffles the shapes (no long droughts), three are shown ahead, one can be held,
 * a ghost shows where the shape will land, and the fall gets faster every ten rows.
 *
 * Touch: drag sideways to slide the shape, tap to turn it, drag down to drop it faster, flick down to drop it
 * all the way, flick up to hold it (or tap the Hold box). Keys for the PC: arrows, Space (drop), Enter (hold).
 *
 * Whole numbers only, drawn in code. The game has its own palette (like Snake and Brickfit); the best score
 * is kept in the user's home folder. It only runs its timer while playing, so it costs nothing in the
 * background. */
#include <stdint.h>
#include <string.h>

#include "kernel/input.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"
#include "sdk/audio.h"
#include "sdk/sfx.h"
#include "sdk/kern86.h"

#define W 10
#define H 20
#define SAVE_FILE "stackfall.dat"
#define SAVE_MAGIC 0x53544b31u
#define LOCK_MS 450
#define CLEAR_MS 300
#define FRAME_MS 33
#define LOCK_RESETS 15

/* ---- the game's palette ---- */
#define C_BG_TOP    0x151a3fu
#define C_BG_BOT    0x080a1cu
#define C_BOARD     0x0b0e26u
#define C_GRID      0x141a44u
#define C_FRAME     0x3a3f93u
#define C_CARD_FILL 0x10143au
#define C_CARD_EDGE 0x3a3f93u
#define C_TEXT      0xf3f4ffu
#define C_DIM       0x8d93d0u
#define C_GOLD      0xffcf3fu
#define C_ACCENT    0x7a5cffu
#define C_BTN       0x262c6au

/* the colour of each shape: I O T S Z J L */
static const uint32_t piece_col[7] = { 0x35d6eeu, 0xffcf3fu, 0xb468ffu, 0x4fdc7au, 0xf0475au, 0x4a8cffu, 0xff9a3cu };

struct cellpos {
    int8_t x, y;
};

/* each shape in its spawn turn, inside a box of box_n x box_n; the other three turns are worked out at start */
static const struct cellpos base[7][4] = {
    { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 } },     /* I */
    { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } },     /* O */
    { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } },     /* T */
    { { 1, 0 }, { 2, 0 }, { 0, 1 }, { 1, 1 } },     /* S */
    { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 2, 1 } },     /* Z */
    { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } },     /* J */
    { { 2, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } },     /* L */
};
static const int8_t box_n[7] = { 4, 2, 3, 3, 3, 3, 3 };
static struct cellpos shape[7][4][4];               /* [piece][turn][cell] */

/* the fall's step in ms at levels 1 to 15 */
static const int drop_table[15] = { 800, 720, 630, 550, 470, 380, 300, 230, 170, 130, 100, 85, 70, 60, 50 };
static const int line_points[5] = { 0, 100, 300, 500, 800 };

/* the logo on the menu: 8 x 5 blocks */
static const char *const logo[5] = {
    "..ZZ..L.",
    "...ZZ.L.",
    "J..OO.LL",
    "JJJOOTTT",
    "SSIIIITT",
};

enum screen { S_MENU, S_PLAY, S_PAUSE, S_OVER };
enum { B_NONE = -1, B_PLAY, B_SOUND, B_PAUSE, B_HOLD, B_RESUME, B_RESTART, B_MENU, B_AGAIN, B_COUNT };

struct save {
    uint32_t magic;
    uint32_t best, best_lines;
    uint8_t sound, pad[3];
};

static struct k86_window win;
static struct gfx_surface canvas;
static struct save sv;
static char home[K86_PATH_MAX];
static int vol = -1;

static enum screen screen;
static uint32_t now_ms;
static uint32_t rng_state;
static struct gfx_rect btn[B_COUNT];            /* w == 0: not on this screen */
static int pressed = B_NONE;
static int dirty;
static uint32_t front_checked;

/* layout */
static int margin, cell, bw, bh, bx, by, col_x, col_w;
static struct gfx_rect card_r, hold_r, pause_r;

/* the game */
static uint8_t grid[H][W];                      /* 0 empty, else piece + 1 */
static int cur = -1, cur_rot, cur_x, cur_y;
static int nxt[3];
static int hold_p = -1, hold_used;
static int bag[7], bag_i = 7;
static int score, lines, level, new_best;
static uint32_t last_drop;
static int lock_on, lock_resets;
static uint32_t lock_start;
static int clearing, clear_mask;                /* bit r set: row r is being cleared */
static uint32_t clear_start;
static char msg[16];
static uint32_t msg_until;

/* the finger */
static int g_on, g_x0, g_y0, g_ax, g_sy, g_moved;
static uint32_t g_t0;

/* ---- small helpers ---- */
static uint32_t rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iabs(int a) { return a < 0 ? -a : a; }

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

static void sound(int id, int semis)
{
    if (sv.sound)
        k86_sfx_shot(id, semis);
}

static void load(void)
{
    struct save s;
    memset(&sv, 0, sizeof(sv));
    sv.magic = SAVE_MAGIC;
    sv.sound = 1;
    vol = k86_home(home);
    if (vol >= 0 && k86_read(vol, home, SAVE_FILE, &s, sizeof(s)) == (int)sizeof(s) && s.magic == SAVE_MAGIC)
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

static uint32_t darker(uint32_t c, int a) { return gfx_mix(0x000000u, c, (uint32_t)a); }
static uint32_t lighter(uint32_t c, int a) { return gfx_mix(0xffffffu, c, (uint32_t)a); }

static void shapes_init(void)
{
    for (int p = 0; p < 7; p++) {
        for (int i = 0; i < 4; i++)
            shape[p][0][i] = base[p][i];
        int n = box_n[p];
        for (int r = 1; r < 4; r++)
            for (int i = 0; i < 4; i++) {
                struct cellpos c = shape[p][r - 1][i];
                shape[p][r][i].x = (int8_t)(n - 1 - c.y);        /* a quarter turn clockwise */
                shape[p][r][i].y = c.x;
            }
    }
}

/* ---- drawing helpers ---- */
static int text_w(const char *s, enum text_style st) { return text_width(s, st); }

static void text_c(int cx, int y, const char *s, enum text_style st, uint32_t col)
{
    text_draw(&canvas, cx - text_w(s, st) / 2, y, s, st, col);
}

static void text_shadow_c(int cx, int y, const char *s, enum text_style st, uint32_t col)
{
    text_c(cx + 2, y + 3, s, st, 0x03040cu);
    text_c(cx, y, s, st, col);
}

static void box(struct gfx_rect r, int rad, uint32_t fill, uint32_t edge)
{
    gfx_fill_round_rect(&canvas, r.x, r.y, r.w, r.h, rad, edge);
    gfx_fill_round_rect(&canvas, r.x + 1, r.y + 1, r.w - 2, r.h - 2, rad - 1, fill);
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

/* one block of a shape: a darker body, the colour on top, a light glint */
static void block(int x, int y, int s, uint32_t col)
{
    int r = imax(2, s / 6);
    gfx_fill_round_rect(&canvas, x + 1, y + 1, s - 2, s - 2, r, darker(col, 110));
    gfx_fill_round_rect(&canvas, x + 1, y + 1, s - 2, s - 4, r, col);
    gfx_fill_round_rect(&canvas, x + s / 6 + 1, y + s / 7 + 1, imax(3, s / 2), imax(2, s / 7), imax(1, s / 14),
                        lighter(col, 100));
}

static void draw_button(int id, const char *label, int primary)
{
    struct gfx_rect r = btn[id];
    if (r.w <= 0)
        return;
    int lift = pressed == id ? 4 : 0;
    uint32_t col = primary ? C_ACCENT : C_BTN;
    gfx_fill_round_rect(&canvas, r.x, r.y + 5, r.w, r.h, r.h / 2, darker(col, 110));
    gfx_fill_round_rect(&canvas, r.x, r.y + lift, r.w, r.h, r.h / 2, col);
    text_c(r.x + r.w / 2, r.y + lift + (r.h - text_height(TEXT_BOLD)) / 2, label, TEXT_BOLD, C_TEXT);
}

static void bg_full(void)
{
    for (int row = 0; row < canvas.h; row++)
        gfx_fill_rect(&canvas, 0, row, canvas.w, 1, gfx_mix(C_BG_BOT, C_BG_TOP, (uint32_t)(row * 255 / imax(1, canvas.h - 1))));
}

/* ---- the layout ---- */
static void layout(void)
{
    margin = 12;
    int side = 168;
    cell = imin((canvas.w - 3 * margin - side) / W, (canvas.h - 2 * margin - 80) / H);
    if (cell < 8)
        cell = 8;
    bw = cell * W;
    bh = cell * H;
    bx = margin;
    by = imax(80, (canvas.h - bh) / 2 + 20);
    col_x = bx + bw + margin;
    col_w = canvas.w - col_x - margin;
    hold_r = (struct gfx_rect){ col_x, by, col_w, 150 };
    pause_r = (struct gfx_rect){ col_x, by + bh - 96, col_w, 96 };
}

static void layout_buttons(void)
{
    memset(btn, 0, sizeof(btn));
    int w = canvas.w, h = canvas.h;
    if (screen == S_MENU) {
        int pw = imin(w - 160, 440);
        btn[B_PLAY] = (struct gfx_rect){ (w - pw) / 2, h * 46 / 100, pw, 112 };
        btn[B_SOUND] = (struct gfx_rect){ (w - pw) / 2, h * 46 / 100 + 112 + 30, pw, 84 };
    } else if (screen == S_PLAY) {
        btn[B_PAUSE] = pause_r;
        btn[B_HOLD] = hold_r;
    } else if (screen == S_PAUSE) {
        int cw = imin(w - 2 * margin - 40, 560), ch = 560;
        card_r = (struct gfx_rect){ (w - cw) / 2, (h - ch) / 2, cw, ch };
        int bwid = cw - 80, bhgt = 100, x = card_r.x + 40, y = card_r.y + 120;
        btn[B_RESUME] = (struct gfx_rect){ x, y, bwid, bhgt };
        btn[B_RESTART] = (struct gfx_rect){ x, y + bhgt + 22, bwid, bhgt };
        btn[B_MENU] = (struct gfx_rect){ x, y + 2 * (bhgt + 22), bwid, bhgt };
    } else {
        int cw = imin(w - 2 * margin - 40, 560), ch = 660;
        card_r = (struct gfx_rect){ (w - cw) / 2, (h - ch) / 2, cw, ch };
        int bwid = cw - 80, bhgt = 100, x = card_r.x + 40, y = card_r.y + ch - 2 * bhgt - 22 - 40;
        btn[B_AGAIN] = (struct gfx_rect){ x, y, bwid, bhgt };
        btn[B_MENU] = (struct gfx_rect){ x, y + bhgt + 22, bwid, bhgt };
    }
}

/* ---- the rules ---- */
static int fits(int p, int rot, int x, int y)
{
    for (int i = 0; i < 4; i++) {
        int cx = x + shape[p][rot][i].x, cy = y + shape[p][rot][i].y;
        if (cx < 0 || cx >= W || cy >= H)
            return 0;
        if (cy >= 0 && grid[cy][cx])
            return 0;
    }
    return 1;
}

static int bag_next(void)
{
    if (bag_i >= 7) {
        for (int i = 0; i < 7; i++)
            bag[i] = i;
        for (int i = 6; i > 0; i--) {
            int j = (int)(rnd() % (uint32_t)(i + 1));
            int t = bag[i];
            bag[i] = bag[j];
            bag[j] = t;
        }
        bag_i = 0;
    }
    return bag[bag_i++];
}

static void go(enum screen s);

static void game_over(void)
{
    new_best = 0;
    if (score > (int)sv.best) {
        sv.best = (uint32_t)score;
        new_best = 1;
    }
    if (lines > (int)sv.best_lines)
        sv.best_lines = (uint32_t)lines;
    save();
    sound(SFX_FAIL, 0);
    cur = -1;
    go(S_OVER);
}

/* puts shape p at the top; false when there is no room (the game is over) */
static int spawn(int p)
{
    cur = p;
    cur_rot = 0;
    cur_x = (W - box_n[p]) / 2;
    cur_y = p == 0 ? -1 : 0;
    lock_on = 0;
    lock_resets = 0;
    last_drop = now_ms;
    return fits(cur, cur_rot, cur_x, cur_y);
}

static void spawn_next(void)
{
    int p = nxt[0];
    nxt[0] = nxt[1];
    nxt[1] = nxt[2];
    nxt[2] = bag_next();
    hold_used = 0;
    if (!spawn(p))
        game_over();
}

static void start_game(void)
{
    memset(grid, 0, sizeof(grid));
    score = 0;
    lines = 0;
    level = 1;
    new_best = 0;
    hold_p = -1;
    hold_used = 0;
    clearing = 0;
    clear_mask = 0;
    msg[0] = 0;
    bag_i = 7;
    for (int i = 0; i < 3; i++)
        nxt[i] = bag_next();
    go(S_PLAY);
    spawn_next();
    dirty = 1;
}

static int ghost_y(void)
{
    int gy = cur_y;
    while (fits(cur, cur_rot, cur_x, gy + 1))
        gy++;
    return gy;
}

/* after a move or a turn: on the floor, the lock timer starts over (a few times) */
static void moved(void)
{
    if (!fits(cur, cur_rot, cur_x, cur_y + 1)) {
        if (lock_resets < LOCK_RESETS) {
            lock_resets++;
            lock_start = now_ms;
            lock_on = 1;
        }
    } else {
        lock_on = 0;
    }
    dirty = 1;
}

static int try_move(int dx)
{
    if (cur < 0 || clearing || !fits(cur, cur_rot, cur_x + dx, cur_y))
        return 0;
    cur_x += dx;
    moved();
    return 1;
}

static void rotate(int dir)
{
    static const int8_t kicks[6][2] = { { 0, 0 }, { -1, 0 }, { 1, 0 }, { -2, 0 }, { 2, 0 }, { 0, -1 } };
    if (cur < 0 || clearing)
        return;
    int nr = (cur_rot + dir + 4) & 3;
    for (int k = 0; k < 6; k++) {
        int nx = cur_x + kicks[k][0], ny = cur_y + kicks[k][1];
        if (fits(cur, nr, nx, ny)) {
            cur_rot = nr;
            cur_x = nx;
            cur_y = ny;
            sound(SFX_TAP, 0);
            moved();
            return;
        }
    }
}

static void soft_step(void)
{
    if (cur < 0 || clearing || !fits(cur, cur_rot, cur_x, cur_y + 1))
        return;
    cur_y++;
    score += 1;
    last_drop = now_ms;
    lock_on = 0;
    dirty = 1;
}

static void lock_piece(void)
{
    int over = 0;
    for (int i = 0; i < 4; i++) {
        int cx = cur_x + shape[cur][cur_rot][i].x, cy = cur_y + shape[cur][cur_rot][i].y;
        if (cy < 0)
            over = 1;
        else
            grid[cy][cx] = (uint8_t)(cur + 1);
    }
    cur = -1;
    lock_on = 0;
    dirty = 1;
    if (over) {
        game_over();
        return;
    }
    int mask = 0, n = 0;
    for (int r = 0; r < H; r++) {
        int full = 1;
        for (int c = 0; c < W; c++)
            if (!grid[r][c])
                full = 0;
        if (full) {
            mask |= 1 << r;
            n++;
        }
    }
    if (n) {
        clearing = 1;
        clear_mask = mask;
        clear_start = now_ms;
        if (n >= 4)
            sound(SFX_LEVELUP, 0);
        else
            sound(SFX_COIN, 0);
    } else {
        sound(SFX_THUD, 0);
        spawn_next();
    }
}

static void hard_drop(void)
{
    if (cur < 0 || clearing)
        return;
    int d = 0;
    while (fits(cur, cur_rot, cur_x, cur_y + 1)) {
        cur_y++;
        d++;
    }
    score += 2 * d;
    lock_piece();
}

static void finish_clear(void)
{
    int n = 0, dst = H - 1;
    for (int r = H - 1; r >= 0; r--) {
        if (clear_mask & (1 << r)) {
            n++;
            continue;
        }
        if (dst != r)
            memcpy(grid[dst], grid[r], W);
        dst--;
    }
    for (; dst >= 0; dst--)
        memset(grid[dst], 0, W);
    score += line_points[imin(n, 4)] * level;
    lines += n;
    level = imin(15, lines / 10 + 1);
    clearing = 0;
    clear_mask = 0;
    if (n >= 4)
        memcpy(msg, "4 LINES!", sizeof("4 LINES!"));
    else if (n == 3)
        memcpy(msg, "3 LINES", sizeof("3 LINES"));
    else
        msg[0] = 0;
    msg_until = now_ms + 900;
    spawn_next();
    dirty = 1;
}

static void hold_swap(void)
{
    if (cur < 0 || clearing || hold_used)
        return;
    int old = hold_p;
    hold_p = cur;
    sound(SFX_TAP, 0);
    if (old < 0) {
        spawn_next();                           /* clears hold_used, set it again below */
    } else if (!spawn(old)) {
        game_over();
        return;
    }
    hold_used = 1;
    dirty = 1;
}

static void update(void)
{
    if (msg[0] && now_ms >= msg_until) {
        msg[0] = 0;
        dirty = 1;
    }
    if (clearing) {
        dirty = 1;                              /* the flash is animating */
        if (now_ms - clear_start >= CLEAR_MS)
            finish_clear();
        return;
    }
    if (cur < 0)
        return;
    int drop_ms = drop_table[level - 1];
    if (!fits(cur, cur_rot, cur_x, cur_y + 1)) {
        if (!lock_on) {
            lock_on = 1;
            lock_start = now_ms;
        } else if (now_ms - lock_start >= LOCK_MS) {
            lock_piece();
        }
    } else {
        lock_on = 0;
        int guard = 0;
        while ((int)(now_ms - last_drop) >= drop_ms && guard++ < 4 && fits(cur, cur_rot, cur_x, cur_y + 1)) {
            cur_y++;
            last_drop += (uint32_t)drop_ms;
            dirty = 1;
        }
        if ((int)(now_ms - last_drop) >= drop_ms)
            last_drop = now_ms;
    }
}

/* ---- drawing the game ---- */
static void draw_mini(int p, struct gfx_rect r, int pc, int dim)
{
    int x0 = 99, x1 = -1, y0 = 99, y1 = -1;
    for (int i = 0; i < 4; i++) {
        x0 = imin(x0, shape[p][0][i].x);
        x1 = imax(x1, shape[p][0][i].x);
        y0 = imin(y0, shape[p][0][i].y);
        y1 = imax(y1, shape[p][0][i].y);
    }
    int ox = r.x + (r.w - (x1 - x0 + 1) * pc) / 2, oy = r.y + (r.h - (y1 - y0 + 1) * pc) / 2;
    uint32_t col = dim ? darker(piece_col[p], 150) : piece_col[p];
    for (int i = 0; i < 4; i++)
        block(ox + (shape[p][0][i].x - x0) * pc, oy + (shape[p][0][i].y - y0) * pc, pc, col);
}

static void stat_box(int y, const char *label, int value)
{
    struct gfx_rect r = { col_x, y, col_w, 92 };
    char num[16];
    box(r, 16, C_CARD_FILL, C_CARD_EDGE);
    text_c(r.x + r.w / 2, r.y + 10, label, TEXT_SMALL, C_DIM);
    num_text(value, num);
    text_c(r.x + r.w / 2, r.y + 44, num, TEXT_BOLD, C_TEXT);
}

static void draw_side(void)
{
    struct gfx_rect hold = hold_r;
    box(hold, 16, C_CARD_FILL, C_CARD_EDGE);
    text_c(hold.x + hold.w / 2, hold.y + 10, "HOLD", TEXT_SMALL, C_DIM);
    int pc = imax(8, col_w / 6);
    if (hold_p >= 0)
        draw_mini(hold_p, (struct gfx_rect){ hold.x, hold.y + 40, hold.w, hold.h - 46 }, pc, hold_used);

    struct gfx_rect nx = { col_x, by + 150 + 14, col_w, 34 + 3 * (2 * pc + 22) };
    box(nx, 16, C_CARD_FILL, C_CARD_EDGE);
    text_c(nx.x + nx.w / 2, nx.y + 10, "NEXT", TEXT_SMALL, C_DIM);
    for (int i = 0; i < 3; i++)
        draw_mini(nxt[i], (struct gfx_rect){ nx.x, nx.y + 40 + i * (2 * pc + 22), nx.w, 2 * pc + 22 }, pc, 0);

    int y = nx.y + nx.h + 14;
    stat_box(y, "LEVEL", level);
    stat_box(y + 106, "LINES", lines);
    stat_box(y + 212, "BEST", (int)sv.best);

    struct gfx_rect p = pause_r;
    int lift = pressed == B_PAUSE ? 3 : 0;
    box((struct gfx_rect){ p.x, p.y + lift, p.w, p.h }, 20, C_BTN, C_CARD_EDGE);
    int bwid = 14, bhgt = 40;
    gfx_fill_round_rect(&canvas, p.x + p.w / 2 - 18, p.y + lift + (p.h - bhgt) / 2, bwid, bhgt, 4, C_TEXT);
    gfx_fill_round_rect(&canvas, p.x + p.w / 2 + 4, p.y + lift + (p.h - bhgt) / 2, bwid, bhgt, 4, C_TEXT);
}

static void draw_board(void)
{
    gfx_fill_round_rect(&canvas, bx - 5, by - 5, bw + 10, bh + 10, 12, C_FRAME);
    gfx_fill_rect(&canvas, bx, by, bw, bh, C_BOARD);
    for (int i = 1; i < W; i++)
        gfx_fill_rect(&canvas, bx + i * cell, by, 1, bh, C_GRID);
    for (int i = 1; i < H; i++)
        gfx_fill_rect(&canvas, bx, by + i * cell, bw, 1, C_GRID);

    int wipe = clearing ? imin(256, (int)(now_ms - clear_start) * 256 / CLEAR_MS) : 0;
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++) {
            if (!grid[r][c])
                continue;
            uint32_t col = piece_col[grid[r][c] - 1];
            if (clearing && (clear_mask & (1 << r))) {
                if (iabs(2 * c - (W - 1)) * 28 < wipe)
                    continue;                   /* wiped away from the middle out */
                col = lighter(col, 170);
            }
            block(bx + c * cell, by + r * cell, cell, col);
        }
    if (cur >= 0 && !clearing) {
        int gy = ghost_y();
        for (int i = 0; i < 4; i++) {
            int cy = gy + shape[cur][cur_rot][i].y;
            if (cy >= 0 && gy != cur_y)
                gfx_blend_round_rect(&canvas, bx + (cur_x + shape[cur][cur_rot][i].x) * cell + 2, by + cy * cell + 2,
                                     cell - 4, cell - 4, imax(2, cell / 6), piece_col[cur], 80);
        }
        for (int i = 0; i < 4; i++) {
            int cy = cur_y + shape[cur][cur_rot][i].y;
            if (cy >= 0)
                block(bx + (cur_x + shape[cur][cur_rot][i].x) * cell, by + cy * cell, cell, piece_col[cur]);
        }
    }
    if (msg[0])
        text_shadow_c(bx + bw / 2, by + bh / 3, msg, TEXT_HEADING, C_GOLD);
}

static void draw_play(void)
{
    char num[16];
    gfx_fill_rect(&canvas, 0, 0, canvas.w, canvas.h, C_BG_BOT);
    num_text(score, num);
    text_c(canvas.w / 2, (by - 5 - text_height(TEXT_LARGE)) / 2, num, TEXT_LARGE, C_TEXT);
    draw_board();
    draw_side();
}

static void draw_card(const char *title)
{
    gfx_darken(&canvas, 0, 0, canvas.w, canvas.h, 150);
    box(card_r, 28, C_CARD_FILL, C_CARD_EDGE);
    text_c(card_r.x + card_r.w / 2, card_r.y + 36, title, TEXT_HEADING, C_TEXT);
}

static void draw_menu(void)
{
    char num[16], line[40] = "Best  ";
    bg_full();
    int s = 44, lw = 8 * s, x0 = (canvas.w - lw) / 2, y0 = canvas.h * 9 / 100;
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 8; c++) {
            const char *names = "IOTSZJL";
            const char *f = strchr(names, logo[r][c]);
            if (logo[r][c] != '.' && f)
                block(x0 + c * s, y0 + r * s, s, piece_col[f - names]);
        }
    int ty = y0 + 5 * s + 30;
    text_shadow_c(canvas.w / 2, ty, "STACKFALL", TEXT_LARGE, C_TEXT);
    num_text((int)sv.best, num);
    int n = 0;
    while (line[n])
        n++;
    for (int i = 0; num[i]; i++)
        line[n++] = num[i];
    line[n] = 0;
    text_c(canvas.w / 2, ty + 90, line, TEXT_BODY, C_GOLD);
    draw_button(B_PLAY, "Play", 1);
    draw_button(B_SOUND, sv.sound ? "Sound: on" : "Sound: off", 0);
    int hy = btn[B_SOUND].y + btn[B_SOUND].h + 50;
    text_c(canvas.w / 2, hy, "Drag to move  -  Tap to turn", TEXT_SMALL, C_DIM);
    text_c(canvas.w / 2, hy + 38, "Drag down to drop faster", TEXT_SMALL, C_DIM);
    text_c(canvas.w / 2, hy + 76, "Flick down to drop, flick up to hold", TEXT_SMALL, C_DIM);
}

static void draw_over(void)
{
    char num[16], line[40] = "Best  ";
    draw_play();
    draw_card("GAME OVER");
    int cx = card_r.x + card_r.w / 2, y = card_r.y;
    num_text(score, num);
    text_c(cx, y + 110, num, TEXT_LARGE, new_best ? C_GOLD : C_TEXT);
    text_c(cx, y + 190, "points", TEXT_SMALL, C_DIM);
    if (new_best) {
        struct gfx_rect badge = { cx - 130, y + 238, 260, 52 };
        gfx_fill_round_rect(&canvas, badge.x, badge.y, badge.w, badge.h, 26, C_GOLD);
        text_c(cx, badge.y + (badge.h - text_height(TEXT_BOLD)) / 2, "NEW BEST!", TEXT_BOLD, 0x1a1400u);
    } else {
        int n = 0;
        while (line[n])
            n++;
        num_text((int)sv.best, num);
        for (int i = 0; num[i]; i++)
            line[n++] = num[i];
        line[n] = 0;
        text_c(cx, y + 244, line, TEXT_BODY, C_GOLD);
    }
    draw_button(B_AGAIN, "Play Again", 1);
    draw_button(B_MENU, "Menu", 0);
}

static void redraw(void)
{
    switch (screen) {
    case S_MENU:
        draw_menu();
        break;
    case S_PLAY:
        draw_play();
        break;
    case S_PAUSE:
        draw_play();
        draw_card("PAUSED");
        draw_button(B_RESUME, "Resume", 1);
        draw_button(B_RESTART, "Restart", 0);
        draw_button(B_MENU, "Menu", 0);
        break;
    case S_OVER:
        draw_over();
        break;
    }
    k86_present(0, 0, canvas.w, canvas.h);
    dirty = 0;
}

static void go(enum screen s)
{
    screen = s;
    pressed = B_NONE;
    g_on = 0;
    layout_buttons();
    k86_timer_set(s == S_PLAY ? FRAME_MS : 0);
    front_checked = now_ms;
    dirty = 1;
}

static void pause_game(void)
{
    go(S_PAUSE);
    redraw();
}

/* ---- events ---- */
static void frame(void)
{
    now_ms = k86_uptime_ms();
    if (screen != S_PLAY)
        return;
    if (now_ms - front_checked >= 500) {
        front_checked = now_ms;
        if (!in_front()) {
            pause_game();
            return;
        }
    }
    update();
    if (screen == S_PLAY && dirty)
        redraw();
}

static void press_action(int id)
{
    switch (id) {
    case B_PLAY:
    case B_AGAIN:
    case B_RESTART:
        sound(SFX_TAP, 0);
        start_game();
        break;
    case B_SOUND:
        sv.sound = !sv.sound;
        save();
        sound(SFX_TAP, 0);
        break;
    case B_PAUSE:
        pause_game();
        return;
    case B_HOLD:
        hold_swap();
        break;
    case B_RESUME:
        go(S_PLAY);
        last_drop = now_ms;
        lock_start = now_ms;
        break;
    case B_MENU:
        go(S_MENU);
        break;
    default:
        break;
    }
    redraw();
}

static void pointer_event(const struct k86_event *ev)
{
    struct wg_pointer p = wg_pointer_make(ev->x, ev->y, ev->buttons, ev->changed, ev->time_ms);
    now_ms = k86_uptime_ms();
    int step = imax(10, cell * 7 / 10);
    if (p.down) {
        int id = button_at(p.x, p.y);
        pressed = id;
        if (screen == S_PLAY && id == B_NONE) {
            g_on = 1;
            g_x0 = g_ax = p.x;
            g_y0 = g_sy = p.y;
            g_t0 = now_ms;
            g_moved = 0;
        } else if (screen != S_PLAY) {
            redraw();
        } else {
            dirty = 1;
        }
        return;
    }
    if (p.up) {
        int id = pressed;
        pressed = B_NONE;
        if (g_on && screen == S_PLAY) {
            int dx = p.x - g_x0, dy = p.y - g_y0, dt = (int)(now_ms - g_t0);
            g_on = 0;
            if (!g_moved && iabs(dx) < 24 && iabs(dy) < 24 && dt < 350)
                rotate(1);
            else if (dy > cell * 3 && dt < 280 && iabs(dx) < dy / 2)
                hard_drop();
            else if (dy < -cell * 3 && dt < 320 && iabs(dx) < -dy / 2)
                hold_swap();
            dirty = 1;
            return;
        }
        if (id != B_NONE && button_at(p.x, p.y) == id)
            press_action(id);
        else if (screen != S_PLAY)
            redraw();
        else
            dirty = 1;
        return;
    }
    if (p.held && screen == S_PLAY && g_on) {
        while (p.x - g_ax >= step && try_move(1)) {
            g_ax += step;
            g_moved = 1;
        }
        while (g_ax - p.x >= step && try_move(-1)) {
            g_ax -= step;
            g_moved = 1;
        }
        if (iabs(p.y - g_y0) > iabs(p.x - g_x0))
            while (p.y - g_sy >= step) {
                g_sy += step;
                g_moved = 1;
                soft_step();
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
        if (screen == S_PLAY)
            press_action(B_PAUSE);
        else if (screen == S_PAUSE)
            press_action(B_RESUME);
        else if (screen == S_OVER)
            press_action(B_MENU);
        return;
    }
    if (screen == S_PLAY) {
        if (k == KEY_LEFT)
            try_move(-1);
        else if (k == KEY_RIGHT)
            try_move(1);
        else if (k == KEY_UP)
            rotate(1);
        else if (k == KEY_DOWN)
            soft_step();
        else if (k == KEY_SPACE)
            hard_drop();
        else if (k == KEY_ENTER)
            hold_swap();
        if (dirty)
            redraw();
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
    memcpy(header.title, "Stackfall", sizeof("Stackfall"));
    k86_header(&header);
    rng_state = k86_uptime_ms() ^ 0x57ac4f11u;
    now_ms = k86_uptime_ms();
    shapes_init();
    load();
    layout();
    go(S_MENU);
    redraw();
    k86_log("stackfall: open");
    for (;;) {
        struct k86_event ev;
        k86_back_wanted(screen != S_MENU);
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
            redraw();
            break;
        case K86_EVENT_RESIZE:
            canvas.w = ev.x;
            canvas.h = ev.y;
            layout();
            layout_buttons();
            now_ms = k86_uptime_ms();
            if (screen == S_PLAY)
                pause_game();
            else
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
