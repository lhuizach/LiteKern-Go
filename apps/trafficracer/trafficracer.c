/* Traffic Racer: our own driving game for LiteKern Go, in the style of the Android game of that name. Landscape:
 * hold the phone on its side like a steering wheel and tilt it to steer (the accelerometer); the car moves
 * smoothly across the whole road, there are no lanes to switch. The car accelerates by itself, like a normal car
 * (30 km/h at the start, then steadily faster), and never stops completely. The whole bottom half of the screen
 * is the brake: hold it to brake hard, let go and it accelerates again. Squeezing past traffic scores extra.
 * PC: left and right arrows steer, Down or Space brakes, Esc pauses. Without the accelerometer, dragging a finger
 * left and right in the top half steers.
 *
 * With the extras area installed the road, scenery and cars are drawn in real 3D on the phone's GPU (see the GPU
 * section below); without it, a 2.5D picture is drawn on the CPU. Whole numbers only. The app draws on a
 * landscape picture and turns it onto the portrait window. Its own palette and art; the best score is kept in the
 * user's home folder. The timer only runs while driving, and the game pauses itself when another app is in front. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kernel/input.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"
#include "sdk/audio.h"
#include "sdk/kern86.h"
#include "sdk/touch2.h"

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
/* Speeds are in units per second; 220 units are a metre, so 1 km/h is about 61 units/s. */
#define V_MIN 733                       /* 12 km/h: the car never stops */
#define V_START 1833                    /* 30 km/h */
#define V_MAX 14667                     /* 240 km/h */
#define KPH(v) ((v) * 9 / 550)
#define BRAKE_DECEL 2300                /* units/s per second: about 38 km/h a second, hard */
#define RAIL_X 765                      /* the most the car's middle can be from the road's middle */
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
       B_AGAIN, B_FLIP, B_COUNT };

struct save {
    uint32_t magic;
    uint32_t best;
    uint8_t sound, skin, flip, pad;           /* flip: steering the other way round */
};

struct car {
    int used, lane, z, prev_z, kind, near_done, sp;    /* sp: its speed along the road, units/s */
    uint32_t col;
};

struct particle {
    int x, y, vx, vy, life;             /* x, y in 1/16 pixel; life in ms */
    uint32_t col;
};

static struct k86_window win;
static struct gfx_surface wcan;          /* the window's canvas, portrait */
static struct gfx_surface canvas;        /* what everything draws on: the same picture on its side (landscape) */
static int land_dir;                     /* 0: the phone's top is on the left, 1: on the right */
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
static int play_ms, v, v_rem, scroll;
static int dist, bonus, score, new_best;
static int xp;                          /* the player's sideways place, units from the road's middle */
static int steer, steer_s, braking;     /* wanted steering and smoothed (-1000..1000), the brake held */
static int key_l, key_r, key_b, touch_steer, touch_brake, finger_down, brake_hint_until;
static int off_x, off_y;                /* screen pixels to window pixels, from the last pointer event */
static const int lane_sp[LANES] = { 3667, 3178, 2750, 2322 };   /* each lane's traffic speed (60, 52, 45, 38 km/h) */
static int curve, curve_tgt, curve_timer;    /* the road's bend: -256..256 */
static int wave_acc, next_gap, free_lane;
static uint32_t crash_since;
static char msg[16];
static uint32_t msg_until;
static int perf_n, perf_max;
static uint32_t perf_sum;
static int g_on, g_x0;

/* ---- small helpers ---- */
/* ---- the tilt sensor: the phone's accelerometer (Bosch BMC150 on i2c), read from sysfs ---- */
static int acc_fd[3] = { -1, -1, -1 }, acc_ok, acc_g = 512, acc_tried_ms;
static int acc_m[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
static int acc_rx, acc_ry;

static int read_text(const char *path, char *out, int max)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int n = (int)read(fd, out, (size_t)max - 1);
    close(fd);
    if (n < 0)
        return -1;
    out[n] = 0;
    return n;
}

static void acc_init(void)
{
    char path[96], txt[96];
    acc_tried_ms = (int)k86_uptime_ms();
    for (int d = 0; d < 8; d++) {
        strcpy(path, "/sys/bus/iio/devices/iio:device0/name");
        path[strlen("/sys/bus/iio/devices/iio:device")] = (char)('0' + d);
        if (read_text(path, txt, sizeof(txt)) < 0 || strncmp(txt, "bmc150_accel", 12))
            continue;
        const char *axes = "xyz";
        char base[64];
        strcpy(base, "/sys/bus/iio/devices/iio:device0/");
        base[strlen("/sys/bus/iio/devices/iio:device")] = (char)('0' + d);
        for (int a = 0; a < 3; a++) {
            strcpy(path, base);
            strcat(path, "in_accel_");
            path[strlen(path)] = 0;
            int l = (int)strlen(path);
            path[l] = axes[a];
            path[l + 1] = 0;
            strcat(path, "_raw");
            acc_fd[a] = open(path, O_RDONLY | O_CLOEXEC);
        }
        strcpy(path, base);
        strcat(path, "in_accel_scale");
        if (read_text(path, txt, sizeof(txt)) > 0) {          /* "0.019154": metres/s2 a step; 1 g is 9.807 */
            int whole = 0, frac = 0, digits = 0;
            const char *c = txt;
            while (*c >= '0' && *c <= '9') whole = whole * 10 + (*c++ - '0');
            if (*c == '.')
                for (c++; *c >= '0' && *c <= '9' && digits < 6; digits++)
                    frac = frac * 10 + (*c++ - '0');
            for (; digits < 6; digits++)
                frac *= 10;
            int micro = whole * 1000000 + frac;
            if (micro > 0)
                acc_g = 9807000 / micro;
        }
        strcpy(path, base);
        strcat(path, "in_mount_matrix");
        if (read_text(path, txt, sizeof(txt)) > 0) {          /* "-1, 0, 0; 0, 1, 0; 0, 0, 1" */
            int k = 0, sign = 1, val = 0, have = 0;
            for (const char *c = txt; ; c++) {
                if (*c == '-') sign = -1;
                else if (*c >= '0' && *c <= '9') { val = val * 10 + (*c - '0'); have = 1; }
                else {
                    if (have && k < 9)
                        acc_m[k++] = sign * val;
                    sign = 1; val = 0; have = 0;
                    if (!*c)
                        break;
                }
            }
        }
        acc_ok = acc_fd[0] >= 0 && acc_fd[1] >= 0 && acc_fd[2] >= 0;
        if (acc_ok)
            k86_log("trafficracer: tilt sensor found");
        return;
    }
}

/* The reading, in the phone's own axes (x to the right of the screen, y up it, z out of it): 1 g is acc_g. On
 * this phone it is the pull of gravity, pointing DOWN (measured: flat on its back it reads z = -1 g): held upright
 * it is (0, -g, 0), on its side with the top to the left (-g, 0, 0), top to the right (+g, 0, 0). Turning the
 * phone like a wheel then moves the y part, and the sign of x says which way up it is. */
static int acc_read(void)
{
    int raw[3];
    if (!acc_ok)
        return -1;
    for (int a = 0; a < 3; a++) {
        char t[24];
        int n = (int)pread(acc_fd[a], t, sizeof(t) - 1, 0);
        if (n <= 0)
            return -1;
        t[n] = 0;
        raw[a] = atoi(t);
    }
    acc_rx = acc_m[0] * raw[0] + acc_m[1] * raw[1] + acc_m[2] * raw[2];
    acc_ry = acc_m[3] * raw[0] + acc_m[4] * raw[1] + acc_m[5] * raw[2];
    return 0;
}

/* window (portrait) pixels <-> landscape pixels */
static void to_land(int px, int py, int *lx, int *ly)
{
    if (land_dir == 0) { *lx = py; *ly = wcan.w - 1 - px; }
    else { *lx = wcan.h - 1 - py; *ly = px; }
}

static void land_alloc(void)
{
    free(canvas.px);
    canvas = (struct gfx_surface){ (uint32_t *)calloc((size_t)wcan.w * (size_t)wcan.h, 4), wcan.h, wcan.w, wcan.h };
}

/* The landscape picture onto the window, turned (in small squares, for the cache). */
static void present(void)
{
    enum { T = 32 };
    for (int ty = 0; ty < wcan.h; ty += T) {
        int ye = ty + T < wcan.h ? ty + T : wcan.h;
        for (int tx = 0; tx < wcan.w; tx += T) {
            int xe = tx + T < wcan.w ? tx + T : wcan.w;
            for (int py = ty; py < ye; py++) {
                uint32_t *d = wcan.px + (size_t)py * wcan.stride;
                if (land_dir == 0) {
                    const uint32_t *sp = canvas.px + (size_t)(wcan.w - 1 - tx) * canvas.stride + py;
                    for (int px = tx; px < xe; px++, sp -= canvas.stride)
                        d[px] = *sp;
                } else {
                    const uint32_t *sp = canvas.px + (size_t)tx * canvas.stride + (wcan.h - 1 - py);
                    for (int px = tx; px < xe; px++, sp += canvas.stride)
                        d[px] = *sp;
                }
            }
        }
    }
    k86_present(0, 0, wcan.w, wcan.h);
}

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
    return iclamp(steer_s / 25, -40, 40);
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

/* ---- the GPU 3D renderer ---------------------------------------------------------------------------
 * With the extras area installed (phone/sandbox/gld.c, built into the Alpine sandbox) the road, the scenery and the
 * cars are drawn in real 3D on the phone's GPU by a helper program, lkx-gld, run by this app. Each frame we send it
 * the scene in whole numbers (a few hundred bytes) and it writes a picture into a shared file, which we enlarge into
 * the window (it draws at 3/4 of the size: the GPU's readback is what costs) and put the HUD on. Without the helper,
 * or if it stops, the 2.5D drawing above is used as before. */
#define GL_ENTER "/extras/enter.sh"
#define GL_BIN "/extras/alpine/usr/local/bin/lkx-gld"
#define GL_FRAME "/dev/shm/lkxgl.frame"
#define GL_SOCK "/dev/shm/lkxgl.sock"
#define GL_FRAME_MAGIC 0x4c47474cu
#define GL_SCENE_MAGIC 0x4e435347u
#define GL_QUIT_MAGIC 0x5154494bu
#define GL_HDR 64
#define GL_PCT 75
#define GL_CARS 32

struct gl_frame_header { uint32_t magic, seq, w, h, stride, scene_seq; int32_t player_x, player_y; uint32_t slot; };
struct gl_car { int32_t x, z, kind; uint32_t col; int32_t yaw, roll, pad[2]; };
struct gl_scene {
    uint32_t magic, seq;
    int32_t xp, zp, scroll, curve, speed, shake_x, shake_y, time_ms, ncars, player_vis;
    uint32_t player_col;
    int32_t player_lean;
    struct gl_car cars[GL_CARS];
};

static pid_t gl_pid;
static int gl_sock = -1, gl_state;                      /* 0 not tried, 1 starting, 2 running, 3 unavailable */
static const unsigned char *gl_map;
static size_t gl_len;
static uint32_t gl_w, gl_h, gl_sent_seq, gl_started_ms;

static int gl_map_frame(void)
{
    struct stat st;
    int fd = open(GL_FRAME, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) || st.st_size < GL_HDR) { close(fd); return -1; }
    const struct gl_frame_header *h = mmap(NULL, GL_HDR, PROT_READ, MAP_SHARED, fd, 0);
    if (h == MAP_FAILED) { close(fd); return -1; }
    uint32_t magic = h->magic, w = h->w, ht = h->h;
    munmap((void *)h, GL_HDR);
    size_t len = GL_HDR + 2 * (size_t)w * ht * 4;
    if (magic != GL_FRAME_MAGIC || w == 0 || ht == 0 || (size_t)st.st_size < len) { close(fd); return -1; }
    void *m = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
        return -1;
    gl_map = m;
    gl_len = len;
    gl_w = w;
    gl_h = ht;
    return 0;
}

static int gl_connect(void)
{
    int s = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, GL_SOCK);
    if (s < 0)
        return -1;
    if (connect(s, (struct sockaddr *)&a, sizeof(a))) { close(s); return -1; }
    if (gl_map_frame()) { close(s); return -1; }
    gl_sock = s;
    return 0;
}

static void gl_num(int val, char *out)                  /* val >= 0 */
{
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + val % 10); val /= 10; } while (val);
    while (n)
        *out++ = tmp[--n];
    *out = 0;
}

static void gl_start(void)
{
    if (gl_state)
        return;
    if (access(GL_ENTER, X_OK) || access(GL_BIN, X_OK)) {
        gl_state = 3;
        return;
    }
    gl_started_ms = k86_uptime_ms();
    if (gl_connect() == 0) {                            /* one is running already */
        gl_state = 2;
        return;
    }
    char size[32], num[12];
    gl_num((canvas.w * GL_PCT / 100) & ~1, size);
    strcat(size, "x");
    gl_num((canvas.h * GL_PCT / 100) & ~1, num);
    strcat(size, num);
    mkdir("/extras/alpine/run", 0755);
    mkdir("/extras/alpine/run/u", 0700);
    unlink(GL_SOCK);
    unlink(GL_FRAME);
    pid_t p = fork();
    if (p < 0) {
        gl_state = 3;
        return;
    }
    if (p == 0) {
        setsid();
        int in = open("/dev/null", O_RDONLY), out = open("/tmp/lkx-gld.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (in >= 0) dup2(in, 0);
        if (out >= 0) { dup2(out, 1); dup2(out, 2); }
        for (int fd = 3; fd < 64; fd++)
            close(fd);
        execl(GL_ENTER, GL_ENTER, "env", "XDG_RUNTIME_DIR=/run/u", "HOME=/root", "/usr/local/bin/lkx-gld", "--serve",
              "--size", size, "--msaa", "0", (char *)NULL);
        _exit(127);
    }
    gl_pid = p;
    gl_state = 1;
    k86_log("trafficracer: starting the GPU renderer");
}

/* While it starts: is it up yet? Returns 1 the moment it is. */
static int gl_check(void)
{
    if (gl_state != 1)
        return 0;
    if (gl_connect() == 0) {
        gl_state = 2;
        k86_log("trafficracer: GPU renderer ready");
        return 1;
    }
    if (k86_uptime_ms() - gl_started_ms > 30000) {
        gl_state = 3;
        k86_log("trafficracer: no GPU renderer, drawing on the CPU");
    }
    return 0;
}

static void gl_stop(void)
{
    if (gl_sock >= 0) {
        struct gl_scene q;
        memset(&q, 0, sizeof(q));
        q.magic = GL_QUIT_MAGIC;
        (void)!send(gl_sock, &q, 32, 0);
        close(gl_sock);
        gl_sock = -1;
    }
    if (gl_pid > 0) {
        for (int i = 0; i < 20; i++) {
            if (waitpid(gl_pid, NULL, WNOHANG) == gl_pid) { gl_pid = 0; break; }
            usleep(50000);
        }
        if (gl_pid > 0) { kill(-gl_pid, SIGKILL); waitpid(gl_pid, NULL, 0); }
    }
}

static int gl_active(void)
{
    return gl_state == 2 && gl_map && ((const volatile struct gl_frame_header *)gl_map)->seq > 0;
}

static void gl_send(int ox, int oy)
{
    struct gl_scene sc;
    memset(&sc, 0, sizeof(sc));
    sc.magic = GL_SCENE_MAGIC;
    sc.seq = ++gl_sent_seq;
    sc.xp = xp;
    sc.zp = zp;
    sc.scroll = scroll;
    sc.curve = curve;
    sc.speed = v;
    sc.shake_x = ox * 5;
    sc.shake_y = oy * 5;
    sc.time_ms = (int32_t)now_ms;
    sc.player_vis = screen != S_DYING || (now_ms - crash_since) / 90 % 2 == 0;
    sc.player_col = skin_col[sv.skin];
    sc.player_lean = player_lean();
    for (int i = 0; i < CARS_MAX && sc.ncars < GL_CARS; i++)
        if (cars[i].used && cars[i].z > zp - 3500) {
            struct gl_car *c = &sc.cars[sc.ncars++];
            c->x = lane_x(cars[i].lane);
            c->z = cars[i].z;
            c->kind = cars[i].kind;
            c->col = cars[i].col;
        }
    if (send(gl_sock, &sc, sizeof(sc), 0) < 0 && errno != EAGAIN && errno != EINTR) {
        gl_state = 3;                                   /* it has stopped: back to the 2.5D drawing */
        k86_log("trafficracer: GPU renderer stopped, drawing on the CPU");
    }
}

/* two pixels' channels at once: a * n + b * (256 - n) per channel, n 0..256 */
static inline uint32_t gl_lerp(uint32_t a, uint32_t b, uint32_t n)
{
    uint32_t rb = ((a & 0x00ff00ffu) * n + (b & 0x00ff00ffu) * (256 - n)) >> 8;
    uint32_t g = ((a & 0x0000ff00u) * n + (b & 0x0000ff00u) * (256 - n)) >> 8;
    return (rb & 0x00ff00ffu) | (g & 0x0000ff00u);
}

/* The picture, enlarged into the window with a smooth (bilinear) filter: each wanted source row is first made
 * the width of the window (kept, as the next window row nearly always needs it again), then two of them blended. */
static uint32_t gl_rows[2][2048];
static int gl_row_y[2];

static const uint32_t *gl_row(const uint32_t *src, int sy, const uint16_t *xi, const uint8_t *xf)
{
    int slot = sy & 1;
    if (gl_row_y[slot] != sy) {
        const uint32_t *r = src + (size_t)sy * gl_w;
        for (int x = 0; x < canvas.w; x++) {
            uint32_t a = r[xi[x]], b = r[xi[x] + 1 < (int)gl_w ? xi[x] + 1 : xi[x]];
            gl_rows[slot][x] = gl_lerp(b, a, xf[x]);
        }
        gl_row_y[slot] = sy;
    }
    return gl_rows[slot];
}

static void gl_blit(void)
{
    static uint16_t xi[2048];
    static uint8_t xf[2048];
    static int map_dw, map_sw;
    const struct gl_frame_header *h = (const struct gl_frame_header *)gl_map;
    const uint32_t *src = (const uint32_t *)(gl_map + GL_HDR + (size_t)(h->slot & 1) * gl_w * gl_h * 4);
    int dw = canvas.w, dh = canvas.h, sw = (int)gl_w, sh = (int)gl_h;
    if (dw > 2048 || dw < 2 || dh < 2)
        return;
    if (map_dw != dw || map_sw != sw) {
        for (int x = 0; x < dw; x++) {
            int p = x * (sw - 1) * 256 / (dw - 1);
            xi[x] = (uint16_t)(p >> 8);
            xf[x] = (uint8_t)(p & 255);
        }
        map_dw = dw;
        map_sw = sw;
    }
    gl_row_y[0] = gl_row_y[1] = -1;
    for (int y = 0; y < dh; y++) {
        int p = y * (sh - 1) * 256 / (dh - 1), y0 = p >> 8, fy = p & 255, y1 = y0 + 1 < sh ? y0 + 1 : y0;
        const uint32_t *r0 = gl_row(src, y0, xi, xf), *r1 = gl_row(src, y1, xi, xf);
        uint32_t *d = canvas.px + (size_t)y * canvas.stride;
        if (!fy || y0 == y1) {
            memcpy(d, r0, (size_t)dw * 4);
        } else {
            for (int x = 0; x < dw; x++)
                d[x] = gl_lerp(r1[x], r0[x], (uint32_t)fy);
        }
    }
}

static void gl_draw(int ox, int oy)
{
    gl_send(ox, oy);
    if (gl_state != 2)
        return;
    if (screen != S_PLAY && screen != S_DYING) {        /* a still screen: wait for its picture */
        const volatile struct gl_frame_header *h = (const volatile struct gl_frame_header *)gl_map;
        for (int i = 0; i < 80 && (int32_t)(h->scene_seq - gl_sent_seq) < 0; i++)
            usleep(4000);
    }
    gl_blit();
}

/* where the player's car is, in the window (from the picture) */
static int gl_player_x(void)
{
    return ((const struct gl_frame_header *)gl_map)->player_x * canvas.w / (int)gl_w;
}
static int gl_player_y(void)
{
    return ((const struct gl_frame_header *)gl_map)->player_y * canvas.h / (int)gl_h;
}

static void draw_scene(int ox, int oy)
{
    if (gl_active()) {
        gl_draw(ox, oy);
        if (gl_state == 2) {
            draw_particles();
            return;
        }
    }
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
    num_text(KPH(v), num);
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
    if (braking) {                              /* the brake zone lights while it is held */
        gfx_blend_round_rect(&canvas, 0, canvas.h / 2, canvas.w, canvas.h / 2, 0, 0xc01020u, 70);
        text_shadow_c(canvas.w / 2, canvas.h - 80, "BRAKE", TEXT_HEADING, C_TEXT);
    } else if ((int)now_ms < brake_hint_until) {
        text_shadow_c(canvas.w / 2, canvas.h - 70, "Bottom half of the screen: brake", TEXT_BODY, C_DIM);
    }
    if (msg[0] && now_ms < msg_until)
        text_shadow_c(gl_active() ? gl_player_x() : proj_x(xp, zp),
                      gl_active() ? gl_player_y() - 150 : proj_y(zp) - car_h[0] * FOC / zp / 2 - 70, msg, TEXT_HEADING, C_GOLD);
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
        int hw2 = (pw - 20) / 2;
        btn[B_SOUND] = (struct gfx_rect){ x, y, hw2, 84 };
        btn[B_FLIP] = (struct gfx_rect){ x + hw2 + 20, y, hw2, 84 };
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
            cars[i] = (struct car){ 1, lane, z, z, roll < 40 ? 0 : roll < 70 ? 1 : roll < 90 ? 2 : 3, 0, lane_sp[lane],
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

static int gap_for(void)                 /* how far the player drives between one wave of traffic and the next */
{
    return 5200 + (int)(rnd() % 3000) - imin(1400, play_ms / 30);
}

static int spawn_z(void)                 /* far off, in the haze */
{
    return gl_active() ? zp + 28000 : zfar;
}

static void start_game(void)
{
    memset(cars, 0, sizeof(cars));
    memset(parts, 0, sizeof(parts));
    play_ms = 0;
    v = V_START;
    v_rem = 0;
    scroll = 0;
    dist = 0;
    bonus = 0;
    score = 0;
    new_best = 0;
    xp = lane_x(1 + (int)(rnd() % 2));
    steer = steer_s = braking = touch_steer = touch_brake = finger_down = 0;
    curve = 0;
    curve_tgt = 0;
    curve_timer = 4000;
    free_lane = 1;
    msg[0] = 0;
    for (int k = 0; k < 3; k++)
        spawn_wave(zp + 9000 + k * 7000);
    wave_acc = 0;
    next_gap = gap_for();
    screen = S_PLAY;
    brake_hint_until = (int)k86_uptime_ms() + 5000;
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
    k86_timer_set(s == S_PLAY || s == S_DYING ? FRAME_MS : s == S_MENU ? 300 : 0);
}

static void crash(void)
{
    crash_since = now_ms;
    if (gl_active())
        add_burst(gl_player_x(), gl_player_y());
    else
        add_burst(proj_x(xp, zp), proj_y(zp));
    sound(220, 90, 90, 260);
    if (score > (int)sv.best) {
        sv.best = (uint32_t)score;
        new_best = 1;
        save();
    }
    to_screen(S_DYING);
}

/* The input as one wanted steering, -1000 (full left) .. 1000: the tilt, else a finger in the top half, else keys. */
static int wanted_steer(void)
{
    int w = 0;
    if (acc_ok && acc_read() == 0) {
        int sgn = acc_rx >= 0 ? 1 : -1;                 /* which way up the phone is: the wheel turns the other way */
        int lock = acc_g * 42 / 100;                    /* full lock at about 25 degrees of turn */
        int t = sgn * acc_ry * 1000 / lock;
        if (t > -40 && t < 40)
            t = 0;                                      /* a dead zone */
        w = iclamp(t, -1000, 1000);
        if (sv.flip)
            w = -w;
    }
    if (touch_steer)
        w = touch_steer;
    if (key_l || key_r)
        w = (key_r ? 800 : 0) - (key_l ? 800 : 0);
    return w;
}

static void update(int dt)
{
    play_ms += dt;
    braking = touch_brake || key_b;

    /* the speed: it picks up by itself, a normal car's way (quick at first, less so the faster it goes); the
     * brake is hard; never below the crawl */
    if (braking)
        v -= BRAKE_DECEL * dt / 1000;
    else {                                      /* about 134 units/s per second at 30 km/h (2.2 km/h a second), less as it goes faster */
        v_rem += (1100 - 700 * v / V_MAX) * 611 / 5 * dt;      /* in millionths of a unit, so that nothing is lost to rounding */
        v += v_rem / 1000000;
        v_rem %= 1000000;
    }
    v = iclamp(v, V_MIN, V_MAX);
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

    /* steering: smooth, the car slides across the road wherever it is told, quicker the faster it goes */
    steer = wanted_steer();
    steer_s += (steer - steer_s) * imin(100, dt * 100 / 90) / 100;
    int lat = 900 + v / 6;
    xp += steer_s * lat / 1000 * dt / 1000;
    if (xp > RAIL_X || xp < -RAIL_X) {                  /* the guard rail: it scrapes, and slows the car */
        xp = iclamp(xp, -RAIL_X, RAIL_X);
        v = imax(V_MIN, v - 1400 * dt / 1000);
    }

    /* traffic: waves of cars, each lane at its own speed (a lane never overtakes itself) */
    wave_acc += v * dt / 1000;
    if (wave_acc >= next_gap) {
        wave_acc -= next_gap;
        spawn_wave(spawn_z());
        next_gap = gap_for();
    }
    for (int i = 0; i < CARS_MAX; i++) {
        struct car *c = &cars[i];
        if (!c->used)
            continue;
        c->prev_z = c->z;
        c->z -= (v - c->sp) * dt / 1000;
        int dx = iabs(lane_x(c->lane) - xp);
        int half_l = (car_h[c->kind] + car_h[0]) / 2 * 9 / 10, half_w = (car_w[c->kind] + car_w[0]) / 2 - 25;
        int z_lo = imin(c->prev_z, c->z), z_hi = imax(c->prev_z, c->z);
        if (z_lo <= zp + half_l && z_hi >= zp - half_l && dx < half_w) {
            crash();
            return;
        }
        if (!c->near_done && c->z < zp - half_l) {
            c->near_done = 1;
            if (dx < half_w + 130) {            /* squeezed past */
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
    steer_s = 0;
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
    draw_button(B_FLIP, sv.flip ? "Steer: flip" : "Steer: normal", 0);
    text_shadow_c(canvas.w / 2, btn[B_SOUND].y + btn[B_SOUND].h + 16, acc_ok ? "Tilt to steer, bottom half brakes" : "No tilt sensor: drag in the top half", TEXT_SMALL, C_TEXT);
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
    present();
}

static void pause_game(void)
{
    to_screen(S_PAUSE);
    redraw();
}

static void frame(void)
{
    now_ms = k86_uptime_ms();
    if (screen == S_MENU) {                 /* waiting for the GPU renderer, and for the phone to be turned */
        int changed = gl_check();
        if (!acc_ok && (int)now_ms - acc_tried_ms > 2000)
            acc_init();
        if (acc_ok && acc_read() == 0 && (acc_rx > 60 || acc_rx < -60)) {
            int d = acc_rx > 0 ? 1 : 0;            /* the reading is gravity: down is +x when the top is on the right */
            if (d != land_dir) {
                land_dir = d;
                changed = 1;
            }
        }
        if (changed)
            redraw();
        return;
    }
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
    case B_FLIP:
        sv.flip = !sv.flip;
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

/* The fingers: the brake is any finger in the bottom half of the (landscape) screen, held; a finger in the top
 * half steers when there is no tilt sensor. Positions of a second finger come from /dev/shm/lkx-touch2. */
static void read_fingers(int primary_down, int px, int py)
{
    int lx, ly, brake = 0;
    if (primary_down) {
        to_land(px, py, &lx, &ly);
        if (button_at(lx, ly) == B_NONE && ly >= canvas.h / 2)
            brake = 1;
    }
    struct k86_touch2 t;
    if (k86_touch2(&t) && t.n >= 2) {
        to_land(t.x2 + off_x, t.y2 + off_y, &lx, &ly);
        if (ly >= canvas.h / 2)
            brake = 1;
    }
    touch_brake = brake;
}

static void pointer_event(const struct k86_event *ev0)
{
    int lx, ly;
    struct k86_touch2 t2;
    to_land(ev0->x, ev0->y, &lx, &ly);
    if (k86_touch2(&t2) && t2.n >= 1) {
        off_x = ev0->x - t2.x1;
        off_y = ev0->y - t2.y1;
    }
    struct wg_pointer p = wg_pointer_make(lx, ly, ev0->buttons, ev0->changed, ev0->time_ms);
    now_ms = k86_uptime_ms();
    if (screen == S_DYING)
        return;
    if (screen == S_PLAY) {
        if (p.down) {
            int id = button_at(p.x, p.y);
            pressed = id;
            finger_down = 1;
            g_x0 = p.x;
        } else if (p.up) {
            int id = pressed;
            pressed = B_NONE;
            finger_down = 0;
            touch_steer = 0;
            if (id != B_NONE && button_at(p.x, p.y) == id)
                press_action(id);
        } else if (p.held && finger_down && !acc_ok && pressed == B_NONE && p.y < canvas.h / 2) {
            touch_steer = iclamp((p.x - g_x0) * 1000 / 220, -1000, 1000);     /* a finger slid left or right */
        }
        read_fingers(finger_down, ev0->x, ev0->y);
        return;
    }
    if (p.down) {
        pressed = button_at(p.x, p.y);
        redraw();
        return;
    }
    if (p.up) {
        int id = pressed;
        pressed = B_NONE;
        if (id != B_NONE && button_at(p.x, p.y) == id)
            press_action(id);
        else
            redraw();
    }
}

static void key_event(const struct k86_event *ev)
{
    uint16_t k = ev->key.key;
    now_ms = k86_uptime_ms();
    if (k == KEY_LEFT) { key_l = ev->key.pressed; return; }
    if (k == KEY_RIGHT) { key_r = ev->key.pressed; return; }
    if (k == KEY_DOWN || k == KEY_SPACE) { key_b = ev->key.pressed; if (screen != S_MENU) return; }
    if (!ev->key.pressed)
        return;
    if (k == KEY_ESC) {
        if (screen == S_PLAY)
            press_action(B_PAUSE);
        else if (screen == S_PAUSE)
            press_action(B_RESUME);
        else if (screen == S_OVER)
            press_action(B_MENU);
        return;
    }
    if (screen == S_MENU && (k == KEY_ENTER || k == KEY_SPACE))
        press_action(B_PLAY);
}

int main(void)
{
    struct k86_header header;
    if (k86_window_open(&win))
        return 1;
    wcan = (struct gfx_surface){ win.canvas, win.w, win.h, win.stride };
    land_alloc();
    acc_init();
    memset(&header, 0, sizeof(header));
    memcpy(header.title, "Traffic Racer", sizeof("Traffic Racer"));
    k86_header(&header);
    rng_state = k86_uptime_ms() ^ 0x7ac3e517u;
    now_ms = k86_uptime_ms();
    load();
    layout();
    gl_start();
    to_screen(S_MENU);
    redraw();
    k86_log("trafficracer: open");
    for (;;) {
        struct k86_event ev;
        k86_back_wanted(screen != S_MENU);
        if (k86_wait_event(&ev)) {
            k86_timer_set(0);
            gl_stop();
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
            wcan.w = ev.x;
            wcan.h = ev.y;
            land_alloc();
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
            gl_stop();
            return 0;
        default:
            break;
        }
    }
}
