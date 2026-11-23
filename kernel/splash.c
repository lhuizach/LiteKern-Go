#include "kernel/splash.h"
#include "kernel/console.h"
#include "kernel/gfx.h"
#include "kernel/pmm.h"
#include "kernel/printk.h"
#include "kernel/screen.h"
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/timing.h"
#include "defaults.h"           /* DEFAULT_SPLASH */

/* The boot animation (LiteKern Go): minimal, in the iOS / One UI way: black, one mark, one word.
 *
 *   0.3 s   a thin ring draws itself round, clockwise from the top
 *   1.7 s   a blue disc grows in the middle (with a little overshoot)
 *   2.6 s   "LiteKern" fades in and rises a few pixels, then a small "GO" under it
 *   3.4 s   a dot with a short tail runs round the ring, and the disc breathes, until the end
 *   all the while: a hairline progress bar along the bottom, full at HOLD_MS
 *   10 s    it cross-fades into the first screen (lock or home)
 *
 * Everything is drawn from the clock (not frame counts), in whole numbers (no floating point: this file is
 * shared with the kernel build), anti-aliased from 1/16 pixel positions. The mark is built in a small
 * coverage buffer (0..255 per pixel, shapes added with max), then laid over black in its colour, so edges
 * stay smooth and a frame costs about a millisecond. The steps of the boot call splash_progress(), which draws
 * a frame at the time it is; the rest of the time, splash_fade_begin() draws frames until HOLD_MS. */

#define BG          0x000000
#define RING_FG     0xe9e9ef    /* the ring and the word */
#define SUB_FG      0x8a8a93    /* "GO" */
#define TRACK_FG    0x26262b    /* the bar's track */
#define DISC_FG     0x3584e4    /* the disc: Adwaita blue, the default accent */
#define HOLD_MS     10000       /* shown at least this long, however quick the start */
#define CROSS_MS    600         /* into the lock screen or the home screen */

#define RING_R      112         /* px, to the middle of the stroke */
#define RING_W      7           /* stroke */
#define DISC_R      42
#define DOT_R       8
#define HALF        140         /* the mark's area: 2 * HALF square, centred on the ring */
#define MARK_LIFT   40          /* the mark sits this far above the screen's middle */
#define WORD_GAP    104         /* from the ring's bottom edge to the word's top */
#define TRACK_W     240
#define TRACK_H     4
#define TRACK_BOTTOM 200        /* the bar's middle, from the bottom of the screen */

static int active, brief;
static uint32_t started_ms;
static struct gfx_surface from, to;     /* the cross-fade: the splash, and what comes next */

/* ---- whole-number maths ------------------------------------------------------------------------ */

/* Angles are in 1/4096 turns; sines are in 1/4096 (Bhaskara's approximation, within 0.2%). */
static int sin_q12(int a)
{
    a &= 4095;
    int neg = a >= 2048;
    if (neg)
        a -= 2048;
    uint32_t g = ((uint32_t)a * (uint32_t)(2048 - a)) >> 4;             /* 0 .. 65536 */
    int s = (int)(16384u * g / (327680u - g));
    return neg ? -s : s;
}

static int cos_q12(int a)
{
    return sin_q12(a + 1024);
}

static uint32_t isqrt32(uint32_t n)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > n)
        bit >>= 2;
    while (bit) {
        if (n >= r + bit) {
            n -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* Progress through [t0, t1] at time t, 0..1024. */
static int span(uint32_t t, uint32_t t0, uint32_t t1)
{
    if (t <= t0)
        return 0;
    if (t >= t1)
        return 1024;
    return (int)((t - t0) * 1024 / (t1 - t0));
}

static int ease_in_out(int x)                   /* smoothstep */
{
    return ((x * x >> 10) * (3072 - 2 * x)) >> 10;
}

static int ease_out(int x)                      /* a cubic, quick and then soft */
{
    int u = 1024 - x;
    return 1024 - ((u * u >> 10) * u >> 10);
}

static int ease_out_back(int x)                 /* past the end a little, then back */
{
    int u = x - 1024;
    int u2 = u * u >> 10, u3 = u2 * u >> 10;
    return 1024 + ((2766 * u3 + 1743 * u2) >> 10);
}

/* ---- the mark, in a coverage buffer -------------------------------------------------------------- */

#define LW (2 * HALF)
static uint8_t cov[LW * LW];

static void cov_clear(void)
{
    memset(cov, 0, sizeof(cov));
}

/* A disc at (cx, cy) with radius r, in 1/16 pixels in the buffer, its edge smooth; `peak` (0..255) is how
 * solid it is. Overlaps keep the stronger value, so a line made of many of them has no beads. */
static void cov_disc(int cx, int cy, int r, int peak)
{
    if (r <= 0 || peak <= 0)
        return;
    int x0 = (cx - r - 16) >> 4, x1 = (cx + r + 16) >> 4, y0 = (cy - r - 16) >> 4, y1 = (cy + r + 16) >> 4;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > LW - 1) x1 = LW - 1;
    if (y1 > LW - 1) y1 = LW - 1;
    int inner = r - 16 > 0 ? r - 16 : 0;
    uint32_t in2 = (uint32_t)(inner * inner);
    for (int y = y0; y <= y1; y++) {
        int dy = (y << 4) + 8 - cy;
        for (int x = x0; x <= x1; x++) {
            int dx = (x << 4) + 8 - cx;
            uint32_t d2 = (uint32_t)(dx * dx + dy * dy);
            int c;
            if (d2 <= in2) {
                c = peak;
            } else {
                int e = r + 8 - (int)isqrt32(d2);       /* 0 outside .. 16 inside, in 1/16 px */
                if (e <= 0)
                    continue;
                c = (e >= 16 ? 255 : e * 16) * peak / 255;
            }
            if (c > cov[y * LW + x])
                cov[y * LW + x] = (uint8_t)c;
        }
    }
}

/* The coverage laid over what is on the screen, in `colour`, with the mark's middle at (mx, my). */
static void cov_lay(struct gfx_surface *s, int mx, int my, uint32_t colour)
{
    for (int y = 0; y < LW; y++) {
        int sy = my - HALF + y;
        if (sy < 0 || sy >= s->h)
            continue;
        uint32_t *row = s->px + (size_t)sy * s->stride;
        for (int x = 0; x < LW; x++) {
            uint8_t c = cov[y * LW + x];
            int sx = mx - HALF + x;
            if (!c || sx < 0 || sx >= s->w)
                continue;
            row[sx] = c == 255 ? colour : gfx_mix(colour, row[sx], c);
        }
    }
}

/* A point on the ring, in 1/16 pixels in the buffer. */
static void ring_point(int ang, int radius16, int *x, int *y)
{
    *x = HALF * 16 + ((radius16 * sin_q12(ang)) >> 12);
    *y = HALF * 16 - ((radius16 * cos_q12(ang)) >> 12);     /* 0 is the top, then clockwise */
}

/* ---- one frame ------------------------------------------------------------------------------------- */

static const char WORD[] = "LiteKern";
#define WORD_TRACK 6            /* px between the letters */

static int word_width(enum text_style st, const char *w, int track)
{
    int total = 0;
    for (int i = 0; w[i]; i++)
        total += text_width_face((char[2]){ w[i], 0 }, &text_faces[st]) + (w[i + 1] ? track : 0);
    return total;
}

static void word_draw(struct gfx_surface *s, int cx, int y, const char *w, enum text_style st, int track, uint32_t col)
{
    int x = cx - word_width(st, w, track) / 2;
    for (int i = 0; w[i]; i++)
        x += text_draw_n(s, x, y, w + i, 1, st, col) + track;
}

static uint32_t fade_col(uint32_t colour, int a)            /* a: 0..255, from black */
{
    return a >= 255 ? colour : gfx_mix(colour, BG, (uint32_t)(a < 0 ? 0 : a));
}

static void frame(uint32_t t)
{
    struct gfx_surface *s = screen_surface();
    int mx = s->w / 2, my = s->h / 2 - MARK_LIFT;

    /* the mark: clear its area, then the ring (an arc as it draws), the disc, the dot and its tail */
    gfx_fill_rect(s, mx - HALF, my - HALF, LW, LW, BG);

    int sweep = ease_in_out(span(t, 300, 2600)) * 4096 / 1024;      /* the ring's arc, in 1/4096 turns */
    cov_clear();
    for (int a = 0; a <= sweep; a += 5) {                           /* a stamp about every pixel along it */
        int x, y;
        ring_point(a, RING_R * 16, &x, &y);
        cov_disc(x, y, RING_W * 8, 255);
    }
    if (sweep > 0) {                                                /* the end of the arc, exactly */
        int x, y;
        ring_point(sweep, RING_R * 16, &x, &y);
        cov_disc(x, y, RING_W * 8, 255);
    }
    cov_lay(s, mx, my, RING_FG);

    int grow = span(t, 1700, 3000);
    if (grow > 0) {
        int r16 = DISC_R * 16 * ease_out_back(grow) / 1024;
        if (t > 3000)                                               /* breathing, about 5% */
            r16 += DISC_R * 16 * sin_q12((int)((t - 3000) * 4096 / 2800)) / 4096 * 5 / 100;
        cov_clear();
        cov_disc(HALF * 16, HALF * 16, r16 < 0 ? 0 : r16, 255);
        cov_lay(s, mx, my, DISC_FG);
    }

    int orb_in = span(t, 3400, 4200);
    if (orb_in > 0) {
        int head = -1024 + (int)((t - 3400) * 4096 / 2400);         /* from the top, a lap in 2.4 s */
        cov_clear();
        for (int k = 0; k < 60; k++) {                              /* the tail: thinner and fainter behind, no beads */
            int x, y, ang = head - k * 9;
            int left = 60 - k;                                      /* 60 at the head .. 1 at the end */
            int peak = (255 * left / 60) * (left * 1024 / 60) / 1024 * ease_out(orb_in) / 1024;
            ring_point(ang, RING_R * 16, &x, &y);
            cov_disc(x, y, 20 + (DOT_R * 16 - 20) * left / 60, peak);
        }
        cov_lay(s, mx, my, 0xffffff);
    }
    screen_damage(mx - HALF, my - HALF, LW, LW);

    /* the word, and the small one under it */
    int wy = my + RING_R + WORD_GAP;
    int wa = ease_out(span(t, 2600, 3700)), rise = (1024 - wa) * 14 / 1024;
    int sa = ease_out(span(t, 3100, 4000));
    gfx_fill_rect(s, mx - 220, wy - 20, 440, 130, BG);
    if (wa > 0)
        word_draw(s, mx, wy + rise, WORD, TEXT_HEADING, WORD_TRACK, fade_col(RING_FG, wa * 255 / 1024));
    if (sa > 0)
        word_draw(s, mx, wy + 62 + rise, "GO", TEXT_SMALL, 14, fade_col(SUB_FG, sa * 255 / 1024));
    screen_damage(mx - 220, wy - 20, 440, 130);

    /* the hairline bar: fills over the whole hold */
    int bx = (s->w - TRACK_W) / 2, by = s->h - TRACK_BOTTOM - TRACK_H / 2;
    int bar_in = ease_out(span(t, 400, 1200));
    int fill = ease_in_out(span(t, 400, HOLD_MS - 300)) * TRACK_W * 256 / 1024;   /* in 1/256 px */
    gfx_fill_rect(s, bx - 4, by - 4, TRACK_W + 8, TRACK_H + 8, BG);
    if (bar_in > 0) {
        gfx_fill_round_rect(s, bx, by, TRACK_W, TRACK_H, TRACK_H / 2, fade_col(TRACK_FG, bar_in * 255 / 1024));
        int fw = fill >> 8;
        if (fw >= TRACK_H)
            gfx_fill_round_rect(s, bx, by, fw, TRACK_H, TRACK_H / 2, fade_col(RING_FG, bar_in * 255 / 1024));
        if (fw >= TRACK_H && fw < TRACK_W)                          /* the last, partly covered column */
            for (int y = 0; y < TRACK_H; y++)
                s->px[(size_t)(by + y) * s->stride + bx + fw] =
                    gfx_mix(fade_col(RING_FG, bar_in * 255 / 1024), fade_col(TRACK_FG, bar_in * 255 / 1024), (uint32_t)(fill & 255));
    }
    screen_damage(bx - 4, by - 4, TRACK_W + 8, TRACK_H + 8);
    screen_present();
}

void splash_preview(uint32_t t_ms)
{
    if (screen_ready())
        frame(t_ms);
}

void splash_start(void)
{
    if (strcmp(DEFAULT_SPLASH, "logo") || !screen_ready()) {
        kprintf("splash: none (the boot log)\n");
        return;
    }
    console_set_visible(0);         /* still recorded: the Log app shows it */
    struct gfx_surface *s = screen_surface();
    gfx_fill_rect(s, 0, 0, s->w, s->h, BG);
    screen_damage_all();
    started_ms = uptime_ms();
    if (brief)
        frame(HOLD_MS);             /* the tests: the finished picture at once */
    else
        frame(0);
    active = 1;
    kprintf("splash: logo\n");
}

/* The steps of the boot: a frame at the time it is. */
void splash_progress(int percent)
{
    (void)percent;
    if (active && !brief)
        frame(uptime_ms() - started_ms);
}

int splash_active(void)
{
    return active;
}

static int copy_screen(struct gfx_surface *dst)
{
    struct gfx_surface *s = screen_surface();
    if (!dst->px) {
        uint32_t bytes = (uint32_t)s->w * (uint32_t)s->h * 4;
        uintptr_t phys = pmm_alloc_contiguous((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
        if (!phys)
            return -1;
        *dst = (struct gfx_surface){ (uint32_t *)phys, s->w, s->h, s->w };
    }
    gfx_blit(dst, 0, 0, s, 0, 0, s->w, s->h);
    return 0;
}

static void free_screen(struct gfx_surface *b)
{
    if (!b->px)
        return;
    uint32_t bytes = (uint32_t)b->w * (uint32_t)b->h * 4;
    for (uint32_t off = 0; off < bytes; off += PAGE_SIZE)
        pmm_free((uintptr_t)b->px + off);
    b->px = 0;
}

void splash_set_brief(int on)
{
    brief = on;
}

int splash_fade_begin(void)
{
    if (!active || brief)
        return 0;
    for (;;) {                      /* the animation goes on until the hold is over */
        uint32_t t = uptime_ms() - started_ms;
        if (t >= HOLD_MS)
            break;
        frame(t);
    }
    frame(HOLD_MS);                 /* the last picture: the bar full */
    if (copy_screen(&from))
        return 0;                   /* no memory: it just cuts across */
    screen_hold(1);
    return 1;
}

/* a * n + b * (256 - n) for every colour in a pixel at once (two at a time in a 32-bit word), n 0..256 */
static inline uint32_t lerp_px(uint32_t a, uint32_t b, uint32_t n)
{
    uint32_t rb = ((a & 0x00ff00ffu) * n + (b & 0x00ff00ffu) * (256 - n)) >> 8;
    uint32_t g = ((a & 0x0000ff00u) * n + (b & 0x0000ff00u) * (256 - n)) >> 8;
    return (rb & 0x00ff00ffu) | (g & 0x0000ff00u);
}

void splash_fade_end(void)
{
    screen_hold(0);
    struct gfx_surface *s = screen_surface();
    if (from.px && !copy_screen(&to)) {
        uint32_t t0 = uptime_ms();
        for (;;) {
            uint32_t t = uptime_ms() - t0;
            if (t >= CROSS_MS)
                break;
            uint32_t n = (uint32_t)ease_in_out((int)(t * 1024 / CROSS_MS)) >> 2;   /* 0..256, soft at both ends */
            for (int y = 0; y < s->h; y++) {
                uint32_t *d = s->px + (size_t)y * s->stride, *f = from.px + (size_t)y * from.stride,
                         *nx = to.px + (size_t)y * to.stride;
                for (int x = 0; x < s->w; x++)
                    d[x] = lerp_px(nx[x], f[x], n);
            }
            screen_damage_all();
            screen_present();
        }
        gfx_blit(s, 0, 0, &to, 0, 0, s->w, s->h);
    }
    screen_damage_all();
    screen_present();
    free_screen(&from);
    free_screen(&to);
    kprintf("splash: faded out\n");
}

void splash_end(void)
{
    active = 0;
}
