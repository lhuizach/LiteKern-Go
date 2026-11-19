/* Traffic Racer: our own lane-dodging driving game for LiteKern Go. A high chase camera looks down the road:
 * the road is drawn a line at a time in perspective (it narrows only a little, because the camera looks steeply
 * down), it bends gently, guardrails and desert scenery slide past, and the cars are drawn from above and
 * behind, shaded so they look solid. Nothing is a 3D model, so it runs on the phone's CPU.
 *
 * Four lanes. Swipe left or right (or tap the left or right of the screen) to change lane. Traffic comes in
 * waves that always leave one lane free; the speed climbs the longer you last. Squeezing past a car scores
 * extra (a near miss). Keys for the PC: left and right arrows, Esc.
 *
 * Whole numbers only, drawn in code. Its own palette and art; the best score is kept in the user's home
 * folder. The timer only runs while driving, and the game pauses itself when another app is in front. */
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

/* The world: units are 1 cm. The road's 4 lanes are 500 wide; z is the distance from the camera. The camera
 * looks steeply down, so the horizon is far above the top of the screen (a row of -1.1 screens). */
#define LANES 4
#define LANE_W 500
#define ROAD_HW 1000                    /* half the road */
#define FOC 3500                        /* the camera's focal length (scales sizes against distance) */
#define CAM_H 7560                      /* its height: puts the road across the screen at the bottom */
#define HIT_DZ 850                      /* cars this close along the road can touch */
#define SLOT 1100                       /* roadside things: one every this far along, on each side */
#define V_START 5500                    /* units per second */
#define V_MAX 11000
#define LANE_SPEED 3200                 /* sideways, units per second */
#define CARS_MAX 24
#define PART_MAX 28

/* ---- palette ---- */
#define C_SAND_A    0xdcbf80u
#define C_SAND_B    0xd3b574u
#define C_SAND_SH   0xb89a5cu
#define C_ASPH_A    0x4d5059u
#define C_ASPH_B    0x474a53u
#define C_LINE      0xf1f1eau
#define C_RAIL_HI   0xe2e8efu
#define C_RAIL      0x9099a8u
#define C_POST      0x4d5565u
#define C_TEXT      0xf6f8ffu
#define C_DIM       0xc9d3f0u
#define C_GOLD      0xffcf3fu
#define C_ACCENT    0xff7a2eu
#define C_BTN       0x2a3566u
#define C_CARD      0x111a3cu
#define C_CARD_EDGE 0x3d4c99u

static const uint32_t skin_col[4] = { 0xe8384fu, 0x3b82f6u, 0xffa21fu, 0x35d07fu };
static const uint32_t traffic_col[8] = { 0xf2f2f4u, 0x2b2f3au, 0x8a94a6u, 0x2e7dd1u, 0xe3b21fu, 0x7e57c2u,
                                         0xe05a2au, 0x2fa36bu };
/* body shapes: hatchback, saloon, van, lorry. Width and length along the road, in units */
static const int car_w[4] = { 410, 420, 430, 460 };
static const int car_h[4] = { 570, 640, 700, 960 };

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
    int x, y, vx, vy, life;             /* x, y in 1/16 pixel; life in ms */
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

/* the picture */
static int horizon, cxm;                /* the horizon's row (above the screen), the screen's middle */
static int zp, zfar, zcut;              /* the player's distance (a row near the bottom), where traffic appears
                                         * (above the top), and where it's gone (below the bottom) */

/* the game */
static struct car cars[CARS_MAX];
static struct particle parts[PART_MAX];
static int play_ms, v, tv, scroll;
static int dist, bonus, score, new_best;
static int xp, lane_target;             /* the player's sideways place (units), and the lane they're heading for */
static int curve, curve_tgt, curve_timer;    /* the road's bend: -256..256 */
static int wave_acc, next_gap, free_lane;
static uint32_t crash_since;
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

static uint32_t hash(uint32_t a)
{
    a ^= a >> 16;
    a *= 0x7feb352du;
    a ^= a >> 15;
    a *= 0x846ca68bu;
    a ^= a >> 16;
    return a;
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
    text_c(cx + 2, y + 3, s, st, 0x1a1208u);
    text_c(cx, y, s, st, col);
}

static void text_shadow_l(int x, int y, const char *s, enum text_style st, uint32_t col)
{
    text_draw(&canvas, x + 2, y + 3, s, st, 0x1a1208u);
    text_draw(&canvas, x, y, s, st, col);
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
/* How far the bend pushes the road sideways at distance z: nothing at the player's car, more towards the
 * top of the screen. */
static int bend(int z)
{
    int u = imin(z - zp, 45000);                /* (the square stays inside a 32-bit number) */
    if (u <= 0)
        return 0;
    return curve * (u * u >> 10) / (256 * 1000);
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

static int lane_x(int lane) { return (lane * 2 - (LANES - 1)) * LANE_W / 2; }

static void draw_road(void)
{
    int w = canvas.w, base = CAM_H * FOC;
    for (int y = 0; y < canvas.h; y += 2) {
        int dy = y - horizon;                   /* always more than 1000: the horizon is far above */
        int z = base / dy;
        int g = z + scroll;                     /* where on the ground this row is: the stripes move with it */
        int c = cxm + bend(z);
        int hw = dy * ROAD_HW / CAM_H;
        int odd = (g >> 9) & 1;
        int rail = imax(3, dy * 60 / CAM_H), gap = imax(1, dy * 15 / CAM_H);
        gfx_fill_rect(&canvas, 0, y, w, 2, odd ? C_SAND_B : C_SAND_A);
        /* the rail's shadow on the sand, the rail, its posts, then the road */
        gfx_fill_rect(&canvas, c - hw - gap - rail * 22 / 10, y, rail * 12 / 10, 2, C_SAND_SH);
        gfx_fill_rect(&canvas, c + hw + gap + rail, y, rail * 12 / 10, 2, C_SAND_SH);
        gfx_fill_rect(&canvas, c - hw - gap - rail, y, rail, 2, C_RAIL);
        gfx_fill_rect(&canvas, c + hw + gap, y, rail, 2, C_RAIL);
        gfx_fill_rect(&canvas, c - hw - gap - rail * 70 / 100, y, imax(1, rail * 40 / 100), 2, C_RAIL_HI);
        gfx_fill_rect(&canvas, c + hw + gap + rail * 30 / 100, y, imax(1, rail * 40 / 100), 2, C_RAIL_HI);
        if (((g >> 7) & 7) == 0) {
            gfx_fill_rect(&canvas, c - hw - gap - rail * 2, y, rail * 2, 2, C_POST);
            gfx_fill_rect(&canvas, c + hw + gap, y, rail * 2, 2, C_POST);
        }
        gfx_fill_rect(&canvas, c - hw, y, hw * 2, 2, odd ? C_ASPH_B : C_ASPH_A);
        /* the white edge lines, the solid middle line, and the dashes between lanes */
        int lw = imax(2, dy * 40 / CAM_H), dw = imax(2, dy * 28 / CAM_H);
        int e = (ROAD_HW - 90) * dy / CAM_H;
        gfx_fill_rect(&canvas, c - e - lw / 2, y, lw, 2, C_LINE);
        gfx_fill_rect(&canvas, c + e - lw / 2, y, lw, 2, C_LINE);
        gfx_fill_rect(&canvas, c - dw, y, dw * 2, 2, C_LINE);
        if (((g >> 8) & 3) == 0) {
            int o = LANE_W * dy / CAM_H;
            gfx_fill_rect(&canvas, c - o - dw / 2, y, dw, 2, C_LINE);
            gfx_fill_rect(&canvas, c + o - dw / 2, y, dw, 2, C_LINE);
        }
    }
}

static void rr(int x, int y, int w, int h, int r, uint32_t col)
{
    if (w > 0 && h > 0)
        gfx_fill_round_rect(&canvas, x, y, w, h, imin(r, imin(w, h) / 2), col);
}

/* ---- scenery: bushes, rocks and cacti on the sand, from our own drawings ---- */
static void draw_thing(int type, int x, int y, int s)
{
    if (s < 4)
        return;
    gfx_blend_round_rect(&canvas, x - s * 60 / 100 - s / 6, y - s * 10 / 100, s * 130 / 100, s * 40 / 100, s / 5, 0x6b4a1au, 70);
    if (type == 0) {                                    /* a bush: three leafy lumps */
        gfx_fill_circle(&canvas, x - s * 22 / 100, y - s * 18 / 100, s * 30 / 100, 0x4f8433u);
        gfx_fill_circle(&canvas, x + s * 24 / 100, y - s * 16 / 100, s * 28 / 100, 0x4f8433u);
        gfx_fill_circle(&canvas, x, y - s * 34 / 100, s * 34 / 100, 0x6a9e3fu);
        gfx_fill_circle(&canvas, x - s * 8 / 100, y - s * 42 / 100, s * 16 / 100, 0x86b852u);
    } else if (type == 1) {                             /* a rock */
        rr(x - s * 56 / 100, y - s * 62 / 100, s * 112 / 100, s * 66 / 100, s * 30 / 100, 0x86603bu);
        rr(x - s * 54 / 100, y - s * 66 / 100, s * 100 / 100, s * 56 / 100, s * 28 / 100, 0xb28350u);
        rr(x - s * 36 / 100, y - s * 60 / 100, s * 44 / 100, s * 22 / 100, s * 10 / 100, 0xd0a672u);
    } else {                                            /* a cactus */
        rr(x - s * 12 / 100, y - s * 100 / 100, s * 24 / 100, s * 104 / 100, s * 12 / 100, 0x3f8a4au);
        rr(x - s * 34 / 100, y - s * 66 / 100, s * 12 / 100, s * 36 / 100, s * 6 / 100, 0x3f8a4au);
        rr(x - s * 34 / 100, y - s * 66 / 100, s * 26 / 100, s * 12 / 100, s * 6 / 100, 0x3f8a4au);
        rr(x + s * 22 / 100, y - s * 80 / 100, s * 12 / 100, s * 34 / 100, s * 6 / 100, 0x3f8a4au);
        rr(x + s * 8 / 100, y - s * 56 / 100, s * 26 / 100, s * 12 / 100, s * 6 / 100, 0x3f8a4au);
        rr(x - s * 5 / 100, y - s * 94 / 100, s * 8 / 100, s * 80 / 100, s * 4 / 100, 0x66b06eu);
    }
}

static void draw_scenery(void)
{
    static const int size_units[3] = { 330, 420, 600 };
    int k_hi = (scroll + zfar - 3000) / SLOT, k_lo = (scroll + zcut) / SLOT;
    for (int k = k_hi; k >= k_lo; k--) {                /* far to near */
        int z = k * SLOT - scroll;
        if (z < zcut || z > zfar - 3000)
            continue;
        for (int side = 0; side < 2; side++) {
            uint32_t h = hash((uint32_t)k * 2u + (uint32_t)side + 77u);
            if (h % 100 >= 72)
                continue;
            int x = ROAD_HW + 420 + (int)((h >> 8) % 1000);
            int type = (int)((h >> 20) % 3);
            draw_thing(type, proj_x(side ? x : -x, z), proj_y(z), size_units[type] * FOC / z);
        }
    }
}

/* ---- the cars, seen from above and behind ---- */
/* A car with its middle at (cx, cy), width w and length h in pixels. Its front is at the top of the picture.
 * lean slides the roof a little sideways in a lane change. */
static void draw_car_px(int cx, int cy, int w, int h, int kind, uint32_t col, int lean)
{
    int top = cy - h / 2, bot = cy + h / 2, r = imax(2, w * 26 / 100), in = imax(1, w / 40);
    uint32_t glass = 0x1c2a40u, dk = darker(col, 90), hi = lighter(col, 55);
    /* the shadow, thrown a little down and to the left */
    gfx_blend_round_rect(&canvas, cx - w / 2 - w * 16 / 100, top + h * 6 / 100, w, h, r, 0x2a1a05u, 105);
    /* the wheels show as slivers at the sides */
    int ww = imax(2, w * 12 / 100), wh = h * 17 / 100;
    rr(cx - w / 2 - w * 3 / 100, top + h * 13 / 100, ww, wh, 2, 0x111218u);
    rr(cx + w / 2 - ww + w * 3 / 100, top + h * 13 / 100, ww, wh, 2, 0x111218u);
    rr(cx - w / 2 - w * 3 / 100, bot - h * 30 / 100, ww, wh, 2, 0x111218u);
    rr(cx + w / 2 - ww + w * 3 / 100, bot - h * 30 / 100, ww, wh, 2, 0x111218u);
    if (kind == 3) {                                    /* a lorry: a cab, and a long box behind it */
        rr(cx - w / 2, top, w, h * 28 / 100, r, dk);
        rr(cx - w * 40 / 100, top + h * 12 / 100, w * 80 / 100, h * 12 / 100, w / 10, glass);
        rr(cx - w / 2, top + h * 22 / 100, w, h * 78 / 100, w / 14, dk);
        rr(cx - w / 2 + in, top + h * 22 / 100 + in, w - 2 * in, h * 78 / 100 - 2 * in, w / 14, hi);
        for (int i = 1; i < 5; i++)
            rr(cx - w * 44 / 100, top + h * 22 / 100 + i * h * 15 / 100, w * 88 / 100, imax(1, h / 100), 1, dk);
        rr(cx - w * 44 / 100 + lean / 2, top + h * 26 / 100, w * 88 / 100, imax(2, h * 3 / 100), 1, lighter(col, 90));
    } else {
        rr(cx - w / 2, top, w, h, r, dk);
        rr(cx - w / 2 + in, top + in, w - 2 * in, h - 2 * in, r, col);
        rr(cx - w * 30 / 100, top + h * 6 / 100, w * 60 / 100, h * 16 / 100, r / 2, lighter(col, 35));   /* the bonnet's sheen */
        int ws = kind == 2 ? 22 : kind == 1 ? 30 : 27;          /* where the windscreen starts, % of the length */
        int rf0 = kind == 2 ? 35 : kind == 1 ? 43 : 40;         /* the roof, % */
        int rf1 = kind == 2 ? 96 : kind == 1 ? 63 : 66;
        rr(cx - w * 36 / 100 + lean / 2, top + h * ws / 100, w * 72 / 100, h * 14 / 100, w / 10, glass);
        rr(cx - w * 34 / 100 + lean, top + h * rf0 / 100, w * 68 / 100, h * (rf1 - rf0) / 100, w / 8, hi);
        rr(cx - w * 26 / 100 + lean, top + h * (rf0 + 2) / 100, w * 18 / 100, h * (rf1 - rf0 - 4) / 100, w / 14,
           lighter(col, 100));                                  /* a shine along the roof */
        if (kind != 2)
            rr(cx - w * 33 / 100 + lean / 2, top + h * (rf1 + 1) / 100, w * 66 / 100, h * 12 / 100, w / 10, glass);
        else
            rr(cx - 1, top + h * 40 / 100, imax(2, w / 40), h * 56 / 100, 1, dk);        /* the van's back doors */
    }
    /* the tail lights */
    int tl = imax(2, h * 5 / 100);
    rr(cx - w / 2 + w * 5 / 100, bot - tl - h * 2 / 100, w * 24 / 100, tl, 2, 0xff3347u);
    rr(cx + w / 2 - w * 29 / 100, bot - tl - h * 2 / 100, w * 24 / 100, tl, 2, 0xff3347u);
}

static void draw_world_car(int x, int z, int kind, uint32_t col, int lean)
{
    int w = car_w[kind] * FOC / z, h = car_h[kind] * FOC / z, cy = proj_y(z);
    if (w < 3 || cy + h / 2 < 0 || cy - h / 2 > canvas.h)
        return;
    draw_car_px(proj_x(x, z), cy, w, h, kind, col, lean * w / 410);
}

static int player_lean(void)
{
    return iclamp((lane_x(lane_target) - xp) / 12, -40, 40);
}

static void draw_cars_and_player(void)
{
    /* far to near: the cars ahead of the player, the player, then any that are alongside */
    int done[CARS_MAX];
    memset(done, 0, sizeof(done));
    for (int pass = 0; pass < CARS_MAX; pass++) {
        int best = -1;
        for (int i = 0; i < CARS_MAX; i++)
            if (cars[i].used && !done[i] && cars[i].z >= zp && (best < 0 || cars[i].z > cars[best].z))
                best = i;
        if (best < 0)
            break;
        done[best] = 1;
        draw_world_car(lane_x(cars[best].lane), cars[best].z, cars[best].kind, cars[best].col, 0);
    }
    if (screen != S_DYING || (now_ms - crash_since) / 90 % 2 == 0)
        draw_world_car(xp, zp, 0, skin_col[sv.skin], player_lean());
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
            gfx_fill_rect(&canvas, parts[i].x >> 4, parts[i].y >> 4, 8, 8, parts[i].col);
}

static void draw_scene(int ox, int oy)
{
    int sh = horizon, sc = cxm;
    horizon += oy;
    cxm += ox;
    draw_road();
    draw_scenery();
    draw_cars_and_player();
    draw_particles();
    horizon = sh;
    cxm = sc;
}

static void draw_hud(void)
{
    char num[16], line[24];
    text_shadow_l(24, 14, "SCORE", TEXT_SMALL, C_TEXT);
    num_text(score, num);
    text_shadow_l(24, 40, num, TEXT_LARGE, C_TEXT);
    struct gfx_rect p = btn[B_PAUSE];
    if (p.w > 0) {
        int lift = pressed == B_PAUSE ? 3 : 0;
        box((struct gfx_rect){ p.x, p.y + lift, p.w, p.h }, 22, C_BTN, C_CARD_EDGE);
        gfx_fill_round_rect(&canvas, p.x + p.w / 2 - 16, p.y + lift + (p.h - 36) / 2, 12, 36, 4, C_TEXT);
        gfx_fill_round_rect(&canvas, p.x + p.w / 2 + 4, p.y + lift + (p.h - 36) / 2, 12, 36, 4, C_TEXT);
    }
    int rx = canvas.w - 112;                    /* the middle of the speed and distance lines (they're about 190 wide) */
    num_text(v / 50, num);
    line[0] = 0;
    cat(line, num);
    cat(line, " km/h");
    text_shadow_c(rx, 122, line, TEXT_BOLD, C_TEXT);
    num_text(dist / 10000 / 10, num);
    line[0] = 0;
    cat(line, num);
    cat(line, ".");
    num_text(dist / 10000 % 10, num);
    cat(line, num);
    cat(line, " km");
    text_shadow_c(rx, 160, line, TEXT_BODY, C_GOLD);
    if (msg[0] && now_ms < msg_until)
        text_shadow_c(proj_x(xp, zp), proj_y(zp) - car_h[0] * FOC / zp / 2 - 70, msg, TEXT_HEADING, C_GOLD);
}

/* ---- layout ---- */
static void layout(void)
{
    cxm = canvas.w / 2;
    horizon = -canvas.h * 110 / 100;
    zp = CAM_H * FOC / (canvas.h * 84 / 100 - horizon);
    zfar = CAM_H * FOC / (-260 - horizon);
    zcut = CAM_H * FOC / (canvas.h + 230 - horizon);
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
    return imax(3200, s * t / 1000);
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
        spawn_wave(zp + 4500 + k * s0 * 16 / 10);
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
}

static void crash(void)
{
    crash_since = now_ms;
    add_burst(proj_x(xp, zp), proj_y(zp));
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
        curve_tgt = (int)(rnd() % 100) < 40 ? 0 : (int)(rnd() % 441) - 220;
        curve_timer = 3000 + (int)(rnd() % 4000);
    }
    int dc = curve_tgt - curve, stp = imax(1, dt / 10);
    curve += iclamp(dc, -stp, stp);

    /* the player slides to their lane */
    int tx = lane_x(lane_target), mv = LANE_SPEED * dt / 1000;
    xp += iclamp(tx - xp, -mv, mv);

    /* traffic */
    wave_acc += s * dt / 1000;
    if (wave_acc >= next_gap) {
        wave_acc -= next_gap;
        spawn_wave(zfar);
        next_gap = gap_for(s);
    }
    for (int i = 0; i < CARS_MAX; i++) {
        struct car *c = &cars[i];
        if (!c->used)
            continue;
        c->prev_z = c->z;
        c->z -= s * dt / 1000;
        int dx = iabs(lane_x(c->lane) - xp);
        if (c->prev_z > zp - HIT_DZ && c->z < zp + HIT_DZ && dx < 390) {
            crash();
            return;
        }
        if (!c->near_done && c->z < zp - HIT_DZ) {
            c->near_done = 1;
            if (dx < 470) {                     /* squeezed past: the player was partly in the next lane */
                bonus += 50;
                memcpy(msg, "CLOSE! +50", sizeof("CLOSE! +50"));
                msg_until = now_ms + 700;
                sound(880, 40, 1180, 60);
            }
        }
        if (c->z < zcut)
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
    add_car(0, zp + 3600);
    add_car(3, zp + 2100);
    add_car(3, zp + 6800);
    add_car(0, zp + 8800);
    cars[0].kind = 2;
    cars[2].kind = 3;
    xp = lane_x(1);
    lane_target = 1;
    curve = 0;
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
    int ty = canvas.h * 8 / 100;
    gfx_blend_round_rect(&canvas, 70, ty - 20, canvas.w - 140, 270, 32, 0x14100au, 150);
    text_shadow_c(canvas.w / 2, ty, "TRAFFIC", TEXT_LARGE, C_TEXT);
    text_shadow_c(canvas.w / 2, ty + 70, "RACER", TEXT_LARGE, C_GOLD);
    num_text((int)sv.best, num);
    cat(line, num);
    text_shadow_c(canvas.w / 2, ty + 165, line, TEXT_BODY, C_TEXT);
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
