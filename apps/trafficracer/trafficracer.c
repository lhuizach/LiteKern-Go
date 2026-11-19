/* Traffic Racer: our own lane-dodging driving game for LiteKern Go, in the old pseudo-3D way: the road is
 * drawn a line at a time with perspective (near lines wide, far lines narrow), it bends left and right, and
 * the cars are drawn from behind, scaled by how far away they are. Nothing is a 3D model, so it runs on the
 * phone's CPU.
 *
 * Four lanes. Swipe left or right (or tap the left or right of the screen) to change lane. Traffic comes in
 * waves that always leave one lane free; the speed climbs the longer you last. Squeezing past a car scores
 * extra (a near miss). Keys for the PC: left and right arrows, Esc.
 *
 * Whole numbers only, drawn in code. Its own palette; the best score is kept in the user's home folder. The
 * timer only runs while driving, and the game pauses itself when another app is in front. */
#include <stdint.h>
#include <string.h>

#include "kernel/input.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"
#include "sdk/audio.h"
#include "sdk/kern86.h"

#define SAVE_FILE "trafficracer.dat"
#define SAVE_MAGIC 0x54524331u
#define FRAME_MS 33

/* the world: units are 1 cm. The road's 4 lanes are 500 wide; z is how far ahead of the camera */
#define LANES 4
#define LANE_W 500
#define ROAD_HW 1000                    /* half the road */
#define FOC 700                         /* the camera's focal length, in pixels */
#define CAM_H 2376                      /* its height: puts the road across the screen at the bottom */
#define ZP 2300                         /* where the player's car is */
#define ZFAR 40000                      /* where traffic appears */
#define ZCUT 1700                       /* behind this, it's off the bottom of the screen */
#define V_START 5500                    /* units per second */
#define V_MAX 11000
#define LANE_SPEED 3200                 /* sideways, units per second */
#define CARS_MAX 24
#define PART_MAX 28

/* ---- palette ---- */
#define C_SKY_TOP   0x2f6fc4u
#define C_HAZE      0xa9c9ebu
#define C_GRASS_A   0x2f9150u
#define C_GRASS_B   0x2a8648u
#define C_ROAD_A    0x5a5e6au
#define C_ROAD_B    0x535763u
#define C_RUMBLE_A  0xd9484fu
#define C_RUMBLE_B  0xf3f3f3u
#define C_DASH      0xf6f2d2u
#define C_TEXT      0xf6f8ffu
#define C_DIM       0xc9d3f0u
#define C_GOLD      0xffcf3fu
#define C_ACCENT    0xff7a2eu
#define C_BTN       0x2a3566u
#define C_CARD      0x111a3cu
#define C_CARD_EDGE 0x3d4c99u

static const uint32_t skin_col[4] = { 0xe8384fu, 0x3b82f6u, 0xffc93cu, 0x35d07fu };
static const uint32_t traffic_col[8] = { 0xf5f5f7u, 0x2b2f3au, 0x8a94a6u, 0x2e7dd1u, 0xd9a21fu, 0x7e57c2u,
                                         0xe07a2au, 0x2fa36bu };
/* body shapes: hatchback, sedan, van, lorry. Width and height in units */
static const int car_w[4] = { 410, 420, 430, 460 };
static const int car_h[4] = { 330, 295, 430, 520 };

enum screen { S_MENU, S_PLAY, S_DYING, S_PAUSE, S_OVER };
enum { B_NONE = -1, B_PLAY, B_SKIN0, B_SKIN1, B_SKIN2, B_SKIN3, B_SOUND, B_PAUSE, B_RESUME, B_RESTART, B_MENU,
       B_AGAIN, B_COUNT };

struct save {
    uint32_t magic;
    uint32_t best;
    uint8_t sound, skin, pad[2];
};

struct car {
    int used, lane, z, prev_z, kind, near_done;
    uint32_t col;
};

struct particle {
    int x, y, vx, vy, life;             /* pixels, 1/16 px per ms.., ms */
    uint32_t col;
};

static struct k86_window win;
static struct gfx_surface canvas;
static struct save sv;
static char home[K86_PATH_MAX];
static int vol = -1;

static enum screen screen;
static uint32_t now_ms, last_frame, front_checked, rng_state;
static struct gfx_rect btn[B_COUNT];
static int pressed = B_NONE;
static struct gfx_rect card_r;
static int dirty;

/* the picture */
static int horizon, cxm;                /* the horizon's row, the screen's middle */

/* the game */
static struct car cars[CARS_MAX];
static struct particle parts[PART_MAX];
static int play_ms, v, tv, scroll;
static int dist, bonus, score, new_best;
static int xp, lane_target;             /* the player's sideways place (units), and the lane they're heading for */
static int curve, curve_tgt, curve_timer;    /* the road's bend: -256..256 */
static int bgx;                         /* the hills move with the bends */
static int wave_acc, next_gap, free_lane;
static uint32_t crash_since;
static int crash_x, crash_y;
static char msg[16];
static uint32_t msg_until;
static int perf_n, perf_max;
static uint32_t perf_sum;
static int g_on, g_x0, g_y0, g_moved;
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
static int iclamp(int a, int lo, int hi) { return a < lo ? lo : a > hi ? hi : a; }

static void num_text(int val, char *out)
{
    char tmp[12];
    int n = 0;
    if (val < 0)
        val = 0;
    do {
        tmp[n++] = (char)('0' + val % 10);
        val /= 10;
    } while (val && n < 11);
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

static void load(void)
{
    struct save s;
    memset(&sv, 0, sizeof(sv));
    sv.magic = SAVE_MAGIC;
    sv.sound = 1;
    vol = k86_home(home);
    if (vol >= 0 && k86_read(vol, home, SAVE_FILE, &s, sizeof(s)) == (int)sizeof(s) && s.magic == SAVE_MAGIC &&
        s.skin < 4)
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

static int text_w(const char *s, enum text_style st) { return text_width(s, st); }

static void text_c(int cx, int y, const char *s, enum text_style st, uint32_t col)
{
    text_draw(&canvas, cx - text_w(s, st) / 2, y, s, st, col);
}

static void text_shadow_c(int cx, int y, const char *s, enum text_style st, uint32_t col)
{
    text_c(cx + 2, y + 3, s, st, 0x06101cu);
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

/* ---- the road, in perspective ---- */
/* How far the bend pushes the road sideways at distance z: nothing at the player's car, more and more
 * towards the horizon. */
static int bend(int z)
{
    int u = imin(z - ZP, 45000);                /* (the square stays inside a 32-bit number) */
    if (u <= 0)
        return 0;
    return curve * (u * u >> 10) / (256 * 6836);
}

/* the screen column of a point of the world: sideways x units, distance z */
static int proj_x(int x, int z)
{
    return cxm + bend(z) + x * FOC / z;
}

static int proj_y(int z)
{
    return horizon + CAM_H * FOC / z;
}

static uint32_t fog(uint32_t c, int f)
{
    return f > 0 ? gfx_mix(C_HAZE, c, (uint32_t)f) : c;
}

static void draw_sky(void)
{
    for (int y = 0; y < horizon; y += 2)
        gfx_fill_rect(&canvas, 0, y, canvas.w, 2, gfx_mix(C_HAZE, C_SKY_TOP, (uint32_t)(y * 255 / imax(1, horizon))));
    /* the sun */
    gfx_fill_circle(&canvas, canvas.w * 84 / 100 - (bgx / 6) % 120, horizon - 150, 46, 0xfff1b8u);
    /* two ranges of hills, the far one moves less */
    for (int i = -1; i < 6; i++) {
        int x = ((i * 190 - bgx / 3) % (6 * 190) + 6 * 190) % (6 * 190) - 190;
        gfx_fill_circle(&canvas, x, horizon + 20, 90 + (i & 1) * 24, 0x6f93c2u);
    }
    for (int i = -1; i < 5; i++) {
        int x = ((i * 260 - bgx) % (5 * 260) + 5 * 260) % (5 * 260) - 260;
        gfx_fill_circle(&canvas, x, horizon + 40, 100 + ((i * 7) & 3) * 14, 0x4f7d9bu);
    }
    gfx_fill_rect(&canvas, 0, horizon - 2, canvas.w, 6, C_HAZE);
}

static void draw_road(void)
{
    int w = canvas.w, base = CAM_H * FOC;
    gfx_fill_rect(&canvas, 0, horizon, w, 8, fog(C_GRASS_A, 255));
    for (int dy = 6; horizon + dy < canvas.h; dy += 2) {
        int y = horizon + dy;
        int z = base / dy;
        int ph = (z + scroll) >> 9;
        int c = cxm + bend(z);
        int hw = dy * ROAD_HW / CAM_H;
        int rw = imax(2, dy * 140 / CAM_H);
        int f = dy < 220 ? (220 - dy) * 255 / 220 : 0;
        uint32_t grass = fog((ph & 1) ? C_GRASS_B : C_GRASS_A, f);
        uint32_t road = fog((ph & 1) ? C_ROAD_B : C_ROAD_A, f);
        uint32_t rumble = fog(((z + scroll) >> 8) & 1 ? C_RUMBLE_A : C_RUMBLE_B, f);
        gfx_fill_rect(&canvas, 0, y, w, 2, grass);
        gfx_fill_rect(&canvas, c - hw - rw, y, hw * 2 + rw * 2, 2, rumble);
        gfx_fill_rect(&canvas, c - hw, y, hw * 2, 2, road);
        if ((((z + scroll) >> 8) & 3) == 0) {
            uint32_t dash = fog(C_DASH, f);
            int lw = imax(1, dy * 26 / CAM_H);
            for (int k = -1; k <= 1; k++)
                gfx_fill_rect(&canvas, c + k * LANE_W * dy / CAM_H - lw / 2, y, lw, 2, dash);
        }
    }
}

/* ---- the cars, seen from behind ---- */
static void rr(int x, int y, int w, int h, int r, uint32_t col)
{
    if (w > 0 && h > 0)
        gfx_fill_round_rect(&canvas, x, y, w, h, imin(r, imin(w, h) / 2), col);
}

/* A car with its bottom middle at (cx, by), width w and height h in pixels, seen from behind. lean slides the
 * roof a little sideways in a lane change. */
static void draw_car_px(int cx, int by, int w, int h, int kind, uint32_t col, int lean)
{
    int r = imax(1, w / 12);
    uint32_t dark = darker(col, 90), glass = 0x1b2a40u;
    gfx_blend_round_rect(&canvas, cx - w * 58 / 100, by - h * 8 / 100, w * 116 / 100, h * 16 / 100, h * 8 / 100, 0x000000u,
                         110);
    rr(cx - w / 2 - w * 3 / 100, by - h * 22 / 100, w * 17 / 100, h * 22 / 100, 2, 0x0c0d12u);
    rr(cx + w / 2 - w * 14 / 100, by - h * 22 / 100, w * 17 / 100, h * 22 / 100, 2, 0x0c0d12u);
    if (kind == 3) {                                    /* a lorry: a big box */
        rr(cx - w * 48 / 100 + lean / 2, by - h, w * 96 / 100, h * 74 / 100, r, col);
        rr(cx - w * 48 / 100 + lean / 2, by - h, w * 96 / 100, h * 8 / 100, r, lighter(col, 50));
        rr(cx - 1 + lean / 2, by - h * 90 / 100, imax(1, w / 60), h * 58 / 100, 1, dark);
        rr(cx - w * 45 / 100, by - h * 26 / 100, w * 90 / 100, h * 14 / 100, r, 0x20222cu);
    } else {
        int top, ch, cwid;
        if (kind == 2) {                                /* a van: tall cabin */
            top = by - h;
            ch = h * 74 / 100;
            cwid = w * 92 / 100;
        } else if (kind == 1) {                         /* a saloon: low cabin */
            top = by - h * 92 / 100;
            ch = h * 46 / 100;
            cwid = w * 70 / 100;
        } else {                                        /* a hatchback */
            top = by - h;
            ch = h * 54 / 100;
            cwid = w * 78 / 100;
        }
        rr(cx - w / 2, by - h * 58 / 100, w, h * 46 / 100, r, col);
        rr(cx - cwid / 2 + lean, top, cwid, ch, r, kind == 2 ? col : dark);
        rr(cx - cwid * 44 / 100 + lean, top + ch * 10 / 100, cwid * 88 / 100, ch * 62 / 100, r, glass);
        rr(cx - cwid * 40 / 100 + lean, top + ch * 14 / 100, cwid * 80 / 100, imax(1, ch * 9 / 100), 1, 0x4a6286u);
        rr(cx - w / 2, by - h * 22 / 100, w, h * 12 / 100, r, dark);
    }
    rr(cx - w / 2 + w * 4 / 100, by - h * 46 / 100, w * 23 / 100, h * 12 / 100, 2, 0xff3347u);
    rr(cx + w / 2 - w * 27 / 100, by - h * 46 / 100, w * 23 / 100, h * 12 / 100, 2, 0xff3347u);
    rr(cx - w * 10 / 100, by - h * 34 / 100, w * 20 / 100, imax(2, h * 7 / 100), 1, 0xf2f2f2u);
}

static void draw_world_car(int x, int z, int kind, uint32_t col, int lean)
{
    if (z < ZCUT)
        return;
    int w = car_w[kind] * FOC / z, h = car_h[kind] * FOC / z;
    if (w < 3)
        return;
    int f = z > 22000 ? imin(200, (z - 22000) * 200 / 18000) : 0;     /* far cars fade into the haze */
    uint32_t c = fog(col, f);
    draw_car_px(proj_x(x, z), proj_y(z), w, h, kind, c, lean * w / 380);
}

static int lane_x(int lane) { return (lane * 2 - (LANES - 1)) * LANE_W / 2; }

static int player_lean(void)
{
    return iclamp((lane_x(lane_target) - xp) / 14, -34, 34);
}

static void draw_cars_and_player(void)
{
    /* far to near: the cars ahead of the player, the player, then any that are alongside */
    int done[CARS_MAX];
    memset(done, 0, sizeof(done));
    for (int pass = 0; pass < CARS_MAX; pass++) {
        int best = -1;
        for (int i = 0; i < CARS_MAX; i++)
            if (cars[i].used && !done[i] && cars[i].z >= ZP && (best < 0 || cars[i].z > cars[best].z))
                best = i;
        if (best < 0)
            break;
        done[best] = 1;
        draw_world_car(lane_x(cars[best].lane), cars[best].z, cars[best].kind, cars[best].col, 0);
    }
    if (screen != S_DYING || (now_ms - crash_since) / 90 % 2 == 0)
        draw_world_car(xp, ZP, 0, skin_col[sv.skin], player_lean());
    for (int pass = 0; pass < CARS_MAX; pass++) {
        int best = -1;
        for (int i = 0; i < CARS_MAX; i++)
            if (cars[i].used && !done[i] && (best < 0 || cars[i].z > cars[best].z))
                best = i;
        if (best < 0)
            break;
        done[best] = 1;
        draw_world_car(lane_x(cars[best].lane), cars[best].z, cars[best].kind, cars[best].col, 0);
    }
}

static void draw_particles(void)
{
    for (int i = 0; i < PART_MAX; i++)
        if (parts[i].life > 0)
            gfx_fill_rect(&canvas, parts[i].x >> 4, parts[i].y >> 4, 7, 7, parts[i].col);
}

static void draw_scene(int ox, int oy)
{
    int sh = horizon, sc = cxm;
    horizon += oy;
    cxm += ox;
    draw_sky();
    draw_road();
    draw_cars_and_player();
    draw_particles();
    horizon = sh;
    cxm = sc;
}

static void draw_hud(void)
{
    char num[16], line[24];
    num_text(score, num);
    text_shadow_c(canvas.w / 2, 18, num, TEXT_LARGE, C_TEXT);
    line[0] = 0;
    num_text(v / 50, num);
    cat(line, num);
    cat(line, " km/h");
    text_shadow_c(112, 40, line, TEXT_BOLD, C_TEXT);
    struct gfx_rect p = btn[B_PAUSE];
    if (p.w > 0) {
        int lift = pressed == B_PAUSE ? 3 : 0;
        box((struct gfx_rect){ p.x, p.y + lift, p.w, p.h }, 22, C_BTN, C_CARD_EDGE);
        gfx_fill_round_rect(&canvas, p.x + p.w / 2 - 16, p.y + lift + (p.h - 36) / 2, 12, 36, 4, C_TEXT);
        gfx_fill_round_rect(&canvas, p.x + p.w / 2 + 4, p.y + lift + (p.h - 36) / 2, 12, 36, 4, C_TEXT);
    }
    if (msg[0] && now_ms < msg_until)
        text_shadow_c(canvas.w / 2, canvas.h * 56 / 100, msg, TEXT_HEADING, C_GOLD);
}

/* ---- layout ---- */
static void layout(void)
{
    cxm = canvas.w / 2;
    horizon = canvas.h * 33 / 100;
}

static void layout_buttons(void)
{
    memset(btn, 0, sizeof(btn));
    int w = canvas.w, h = canvas.h;
    if (screen == S_MENU) {
        int pw = imin(w - 160, 440), x = (w - pw) / 2, y = h * 47 / 100;
        btn[B_PLAY] = (struct gfx_rect){ x, y, pw, 112 };
        y += 112 + 36;
        int dot = 76, gap = (pw - 4 * dot) / 3;
        for (int i = 0; i < 4; i++)
            btn[B_SKIN0 + i] = (struct gfx_rect){ x + i * (dot + gap), y, dot, dot };
        y += dot + 34;
        btn[B_SOUND] = (struct gfx_rect){ x, y, pw, 84 };
    } else if (screen == S_PLAY) {
        btn[B_PAUSE] = (struct gfx_rect){ w - 12 - 96, 12, 96, 96 };
    } else if (screen == S_PAUSE) {
        int cw = imin(w - 80, 560), ch = 560;
        card_r = (struct gfx_rect){ (w - cw) / 2, (h - ch) / 2, cw, ch };
        int bwid = cw - 80, bhgt = 100, x = card_r.x + 40, y = card_r.y + 120;
        btn[B_RESUME] = (struct gfx_rect){ x, y, bwid, bhgt };
        btn[B_RESTART] = (struct gfx_rect){ x, y + bhgt + 22, bwid, bhgt };
        btn[B_MENU] = (struct gfx_rect){ x, y + 2 * (bhgt + 22), bwid, bhgt };
    } else if (screen == S_OVER) {
        int cw = imin(w - 80, 560), ch = 660;
        card_r = (struct gfx_rect){ (w - cw) / 2, (h - ch) / 2, cw, ch };
        int bwid = cw - 80, bhgt = 100, x = card_r.x + 40, y = card_r.y + ch - 2 * bhgt - 22 - 40;
        btn[B_AGAIN] = (struct gfx_rect){ x, y, bwid, bhgt };
        btn[B_MENU] = (struct gfx_rect){ x, y + bhgt + 22, bwid, bhgt };
    }
}

/* ---- the rules ---- */
static void add_car(int lane, int z)
{
    for (int i = 0; i < CARS_MAX; i++)
        if (!cars[i].used) {
            int roll = (int)(rnd() % 100);
            cars[i] = (struct car){ 1, lane, z, z, roll < 40 ? 0 : roll < 70 ? 1 : roll < 90 ? 2 : 3, 0,
                                    traffic_col[rnd() % 8] };
            return;
        }
}

/* A wave of traffic across the road, always with one lane free, next to the last wave's free lane. */
static void spawn_wave(int z)
{
    free_lane = iclamp(free_lane + (int)(rnd() % 3) - 1, 0, LANES - 1);
    int density = imin(92, 45 + play_ms / 2000);
    for (int lane = 0; lane < LANES; lane++)
        if (lane != free_lane && (int)(rnd() % 100) < density)
            add_car(lane, z + (int)(rnd() % 900));
}

static int gap_for(int s)
{
    int t = 1300 - imin(400, play_ms / 250) + (int)(rnd() % 600);
    return imax(2500, s * t / 1000);
}

static void start_game(void)
{
    memset(cars, 0, sizeof(cars));
    memset(parts, 0, sizeof(parts));
    play_ms = 0;
    v = V_START;
    tv = 1200 + v / 10;
    scroll = 0;
    dist = 0;
    bonus = 0;
    score = 0;
    new_best = 0;
    lane_target = 1 + (int)(rnd() % 2);
    xp = lane_x(lane_target);
    curve = 0;
    curve_tgt = 0;
    curve_timer = 4000;
    free_lane = lane_target;
    msg[0] = 0;
    int s0 = v - tv;
    for (int k = 0; k < 3; k++)
        spawn_wave(11000 + k * s0 * 16 / 10);
    wave_acc = 0;
    next_gap = gap_for(s0);
    screen = S_PLAY;
    layout_buttons();
    last_frame = now_ms = k86_uptime_ms();
    front_checked = now_ms;
    g_on = 0;
    pressed = B_NONE;
    k86_timer_set(FRAME_MS);
}

static void add_burst(int x, int y)
{
    for (int i = 0; i < PART_MAX; i++) {
        int a = (int)(rnd() % 360);
        int sp = 18 + (int)(rnd() % 60);
        /* a rough circle: cheap, no trig */
        int vx = ((a % 180) < 90 ? (a % 90) - 45 : 45 - (a % 90)) * sp / 24;
        int vy = -(int)(rnd() % 70) * sp / 40 - 10;
        parts[i] = (struct particle){ x << 4, y << 4, vx, vy, 500 + (int)(rnd() % 400),
                                      i % 3 == 0 ? 0xffd34fu : i % 3 == 1 ? 0xff7a2eu : 0xdfe3eeu };
    }
}

static void to_screen(enum screen s)
{
    screen = s;
    pressed = B_NONE;
    g_on = 0;
    layout_buttons();
    k86_timer_set(s == S_PLAY || s == S_DYING ? FRAME_MS : 0);
    dirty = 1;
}

static void crash(int car_z)
{
    crash_since = now_ms;
    crash_x = proj_x(xp, ZP);
    crash_y = proj_y(ZP) - 60;
    (void)car_z;
    add_burst(crash_x, crash_y);
    sound(220, 90, 90, 260);
    if (score > (int)sv.best) {
        sv.best = (uint32_t)score;
        new_best = 1;
        save();
    }
    to_screen(S_DYING);
}

static void change_lane(int dir)
{
    int t = iclamp(lane_target + dir, 0, LANES - 1);
    if (t != lane_target) {
        lane_target = t;
        sound(420, 14, 0, 0);
    }
}

static void update(int dt)
{
    play_ms += dt;
    v = imin(V_MAX, V_START + play_ms * 60 / 1000);
    tv = 1200 + v / 10;
    int s = v - tv;
    scroll += v * dt / 1000;
    dist += v * dt / 1000;
    score = dist / 200 + bonus;

    /* the bends */
    curve_timer -= dt;
    if (curve_timer <= 0) {
        curve_tgt = (int)(rnd() % 100) < 30 ? 0 : (int)(rnd() % 441) - 220;
        curve_timer = 3000 + (int)(rnd() % 4000);
    }
    int dc = curve_tgt - curve, stp = imax(1, dt / 10);
    curve += iclamp(dc, -stp, stp);
    bgx += curve * dt / 3500;

    /* the player slides to their lane */
    int tx = lane_x(lane_target), mv = LANE_SPEED * dt / 1000;
    xp += iclamp(tx - xp, -mv, mv);

    /* traffic */
    wave_acc += s * dt / 1000;
    if (wave_acc >= next_gap) {
        wave_acc -= next_gap;
        spawn_wave(ZFAR);
        next_gap = gap_for(s);
    }
    for (int i = 0; i < CARS_MAX; i++) {
        struct car *c = &cars[i];
        if (!c->used)
            continue;
        c->prev_z = c->z;
        c->z -= s * dt / 1000;
        int dx = iabs(lane_x(c->lane) - xp);
        if (c->prev_z > ZP - 260 && c->z < ZP + 260 && dx < 390) {
            crash(c->z);
            return;
        }
        if (!c->near_done && c->z < ZP - 260) {
            c->near_done = 1;
            if (dx < 470) {                     /* squeezed past: the player was partly in the next lane */
                bonus += 25;
                memcpy(msg, "CLOSE! +25", sizeof("CLOSE! +25"));
                msg_until = now_ms + 700;
                sound(880, 40, 1180, 60);
            }
        }
        if (c->z < ZCUT)
            c->used = 0;
    }
    if (msg[0] && now_ms >= msg_until)
        msg[0] = 0;
}

static void update_particles(int dt)
{
    for (int i = 0; i < PART_MAX; i++) {
        if (parts[i].life <= 0)
            continue;
        parts[i].life -= dt;
        parts[i].x += parts[i].vx * dt / 16;
        parts[i].y += parts[i].vy * dt / 16;
        parts[i].vy += dt / 3;
    }
}

/* ---- screens ---- */
static void demo_scene(void)
{
    memset(cars, 0, sizeof(cars));
    add_car(0, 9000);
    add_car(2, 6200);
    add_car(3, 14000);
    add_car(1, 20000);
    add_car(0, 28000);
    add_car(2, 33000);
    cars[0].kind = 2;
    cars[2].kind = 3;
    xp = lane_x(1);
    lane_target = 1;
    curve = 90;
    scroll = 0;
    v = V_START;
}

static void draw_card(const char *title)
{
    gfx_darken(&canvas, 0, 0, canvas.w, canvas.h, 150);
    box(card_r, 28, C_CARD, C_CARD_EDGE);
    text_c(card_r.x + card_r.w / 2, card_r.y + 36, title, TEXT_HEADING, C_TEXT);
}

static void draw_menu(void)
{
    char num[16], line[32] = "Best  ";
    demo_scene();
    draw_scene(0, 0);
    text_shadow_c(canvas.w / 2, canvas.h * 9 / 100, "TRAFFIC", TEXT_LARGE, C_TEXT);
    text_shadow_c(canvas.w / 2, canvas.h * 9 / 100 + 74, "RACER", TEXT_LARGE, C_GOLD);
    num_text((int)sv.best, num);
    cat(line, num);
    text_shadow_c(canvas.w / 2, canvas.h * 9 / 100 + 168, line, TEXT_BODY, C_TEXT);
    draw_button(B_PLAY, "Drive", 1);
    for (int i = 0; i < 4; i++) {
        struct gfx_rect r = btn[B_SKIN0 + i];
        int sel = i == sv.skin;
        gfx_fill_circle(&canvas, r.x + r.w / 2, r.y + r.h / 2, r.w / 2, sel ? C_TEXT : C_CARD_EDGE);
        gfx_fill_circle(&canvas, r.x + r.w / 2, r.y + r.h / 2, r.w / 2 - 5, skin_col[i]);
    }
    draw_button(B_SOUND, sv.sound ? "Sound: on" : "Sound: off", 0);
    text_shadow_c(canvas.w / 2, btn[B_SOUND].y + btn[B_SOUND].h + 40, "Swipe or tap left and right", TEXT_SMALL, C_TEXT);
    text_shadow_c(canvas.w / 2, btn[B_SOUND].y + btn[B_SOUND].h + 76, "to change lane", TEXT_SMALL, C_TEXT);
}

static void draw_over(void)
{
    char num[16], line[32] = "Best  ";
    draw_scene(0, 0);
    draw_card("CRASHED");
    int cx = card_r.x + card_r.w / 2, y = card_r.y;
    num_text(score, num);
    text_c(cx, y + 110, num, TEXT_LARGE, new_best ? C_GOLD : C_TEXT);
    text_c(cx, y + 190, "points", TEXT_SMALL, C_DIM);
    if (new_best) {
        struct gfx_rect badge = { cx - 130, y + 238, 260, 52 };
        gfx_fill_round_rect(&canvas, badge.x, badge.y, badge.w, badge.h, 26, C_GOLD);
        text_c(cx, badge.y + (badge.h - text_height(TEXT_BOLD)) / 2, "NEW BEST!", TEXT_BOLD, 0x1a1400u);
    } else {
        num_text((int)sv.best, num);
        cat(line, num);
        text_c(cx, y + 244, line, TEXT_BODY, C_GOLD);
    }
    draw_button(B_AGAIN, "Drive Again", 1);
    draw_button(B_MENU, "Menu", 0);
}

static void redraw(void)
{
    switch (screen) {
    case S_MENU:
        draw_menu();
        break;
    case S_PLAY:
        draw_scene(0, 0);
        draw_hud();
        break;
    case S_DYING: {
        int t = (int)(now_ms - crash_since);
        int sh = t < 600 ? (600 - t) / 30 : 0;
        int ox = sh ? (int)(rnd() % (uint32_t)(sh * 2 + 1)) - sh : 0, oy = sh ? (int)(rnd() % (uint32_t)(sh * 2 + 1)) - sh : 0;
        draw_scene(ox, oy);
        draw_hud();
        break;
    }
    case S_PAUSE:
        draw_scene(0, 0);
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

static void pause_game(void)
{
    to_screen(S_PAUSE);
    redraw();
}

static void frame(void)
{
    now_ms = k86_uptime_ms();
    int dt = imin(100, (int)(now_ms - last_frame));
    last_frame = now_ms;
    if (screen == S_PLAY) {
        if (now_ms - front_checked >= 500) {
            front_checked = now_ms;
            if (!in_front()) {
                pause_game();
                return;
            }
        }
        update(dt);
        if (screen == S_PLAY || screen == S_DYING) {
            uint32_t t0 = k86_uptime_ms();
            redraw();
            /* how long the picture takes: the Log app shows an average every 150 frames (to tune it on the phone) */
            uint32_t took = k86_uptime_ms() - t0;
            perf_sum += took;
            perf_max = imax(perf_max, (int)took);
            if (++perf_n >= 150) {
                char line[64] = "trafficracer: draw avg ", num[16];
                num_text((int)(perf_sum / (uint32_t)perf_n), num);
                cat(line, num);
                cat(line, " ms, max ");
                num_text(perf_max, num);
                cat(line, num);
                k86_log(line);
                perf_n = perf_max = 0;
                perf_sum = 0;
            }
        }
    } else if (screen == S_DYING) {
        update_particles(dt);
        if (now_ms - crash_since >= 900) {
            to_screen(S_OVER);
            redraw();
            return;
        }
        redraw();
    }
}

static void press_action(int id)
{
    switch (id) {
    case B_PLAY:
    case B_AGAIN:
    case B_RESTART:
        sound(520, 40, 780, 70);
        start_game();
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
        pause_game();
        return;
    case B_RESUME:
        to_screen(S_PLAY);
        last_frame = now_ms = k86_uptime_ms();
        front_checked = now_ms;
        break;
    case B_MENU:
        to_screen(S_MENU);
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
    if (screen == S_DYING)
        return;
    if (p.down) {
        int id = button_at(p.x, p.y);
        pressed = id;
        if (screen == S_PLAY && id == B_NONE) {
            g_on = 1;
            g_x0 = p.x;
            g_y0 = p.y;
            g_t0 = now_ms;
            g_moved = 0;
        } else if (screen != S_PLAY) {
            redraw();
        }
        return;
    }
    if (p.up) {
        int id = pressed;
        pressed = B_NONE;
        if (g_on && screen == S_PLAY) {
            g_on = 0;
            if (!g_moved && iabs(p.x - g_x0) < 30 && iabs(p.y - g_y0) < 30 && (int)(now_ms - g_t0) < 350)
                change_lane(p.x < canvas.w / 2 ? -1 : 1);
            return;
        }
        if (id != B_NONE && button_at(p.x, p.y) == id)
            press_action(id);
        else if (screen != S_PLAY)
            redraw();
        return;
    }
    if (p.held && screen == S_PLAY && g_on && iabs(p.x - g_x0) >= 64 && iabs(p.x - g_x0) > iabs(p.y - g_y0)) {
        change_lane(p.x > g_x0 ? 1 : -1);
        g_x0 = p.x;                             /* a long drag can take more than one lane */
        g_moved = 1;
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
            change_lane(-1);
        else if (k == KEY_RIGHT)
            change_lane(1);
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
    memcpy(header.title, "Traffic Racer", sizeof("Traffic Racer"));
    k86_header(&header);
    rng_state = k86_uptime_ms() ^ 0x7ac3e517u;
    now_ms = k86_uptime_ms();
    load();
    layout();
    to_screen(S_MENU);
    redraw();
    k86_log("trafficracer: open");
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
