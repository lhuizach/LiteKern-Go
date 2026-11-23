/* LiteKern X — the lock screen (see kernel/lockscreen.h). */
#include "kernel/lockscreen.h"
#include "kernel/kern86_abi.h"
#include "kernel/cursor.h"
#include "kernel/desktop.h"
#include "kernel/driver.h"
#include "kernel/errno.h"
#include "kernel/machine.h"
#include "kernel/pmm.h"
#include "kernel/power.h"
#include "kernel/printk.h"
#include "kernel/ramdisk.h"
#include "kernel/rtc.h"
#include "kernel/screen.h"
#include "kernel/screensaver.h"
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/timing.h"
#include "kernel/users.h"
#include "kernel/wallpaper.h"

#define WHITE       0xffffffu
#define POPOVER     0x36363au           /* the shell's popovers (kernel/desktop.c) */
#define WARN        0xf8e45cu           /* Caps Lock */
#define BAD         0xff7b63u           /* a wrong password */
#define DIM         96                  /* how much the wallpaper is darkened */
#define MENU_W      520                 /* the power menu (Power held): a sheet in the middle */
#define ITEM_H      100
#define MENU_PAD    16
#define POWER_HOLD_MS 500
#define SWIPE_UNLOCK 200                /* px up that unlock; less springs back */
#define PAD_R       72                  /* the PIN pad's keys */
#define PAD_DX      210
#define PAD_DY      176
#define PAD_BOTTOM  170                 /* its last row's centre, from the bottom */
#define PAD_KEYS    12                  /* 1-9, Del, 0, OK */
#define NOTES_SHOWN 4
#define NOTE_H      108
#define NOTE_ICON   68
#define QUIET_MS    400                 /* input just after locking is ignored */
#define SLIDE_MS    320                 /* the curtain sliding down (locking) or up (unlocking) */
#define SHADOW      28                  /* the soft shadow along its bottom edge */
#define MAX_FRAME_MS 60                 /* slower than this: go straight to the end */
#define AVATAR_R    36                  /* the prompt's avatar */
#define PICK_R      38                  /* the picker's */
#define PICK_GAP    150                 /* between the picker's avatars */
#define FIELD_W     280
#define FIELD_H     40
#define LOCK_OFF_MS 5000                /* the clock untouched this long: the screen goes off */
#define PROMPT_IDLE_MS 30000           /* the prompt untouched this long: back to the clock */
#ifndef ANIM_SLOW                       /* recordings: make EXTRA_CFLAGS=-DANIM_SLOW=N */
#define ANIM_SLOW 1
#endif

/* What it shows: the clock (locked, and at first sight), a password prompt
 * for one user, or (logging in) everyone to choose from. */
enum view { V_CLOCK, V_PROMPT, V_PICK };

static struct {
    int active;
    struct gfx_surface bg;              /* the dimmed wallpaper, made when it locks */
    struct gfx_surface pic;             /* its own picture (LOCK_PICTURE), if any */
    struct gfx_surface under, frame;    /* the animation: the desktop, and the lock screen */
    int menu_open, hover;               /* hover: 0 none, 1 the power button, 2+ menu items */
    uint8_t buttons;
    uint32_t locked_at, last_tick, last_input;
    struct rtc_time shown;              /* the time on screen */
    struct lock_settings set;
    enum view view;
    int login;                          /* nobody's logged in: unlocking logs `who` in */
    int ok, ok_dots;                    /* a right PIN: dots gone green so far; how many dots there were */
    int reveal;                         /* unlocked by PIN: the screen opens out from the dots */
    int who;                            /* the prompt's user */
    int pick;                           /* the picker's selection */
    char pw[PASSWORD_MAX];
    int pwlen, caps, shake;
    int fails;
    uint32_t wait_until;                /* after wrong passwords: no tries until then */
    const char *message;                /* under the field: what went wrong */
    int swiping, y0, lift;              /* a swipe up: where it started, how far (<= 0) */
    int pad_held;                       /* the PIN pad's key under the finger, or -1 */
    uint32_t power_down_ms;             /* Power is held since (0: it isn't) */
    int power_held;
} ls = { .set = { "desktop", 1, 0, 0, 0, 100, 100, 50, -1 }, .pad_held = -1 };

/* H_PAD + k: the PIN pad's k-th key; H_AVATAR + i: the picker's i-th. */
enum { H_NONE, H_RESTART, H_SHUTDOWN, H_PAD, H_AVATAR = H_PAD + PAD_KEYS };

static struct gfx_surface *S(void) { return screen_surface(); }

/* --- the clock's face (fonts/clock.lkxf, tools/font2c.py --bin) --------------------------- */

#define CLOCK_GLYPHS (95 + TEXT_EXTRA)
/* The lock screen's faces are big (320 px) and shrunk when drawn: a stretched clock
 * stays sharp. The last, fonts/clock.lkxf at 180 px, is the home screen's. */
#define HOME_FACE K86_CLOCK_FONTS
#define CLOCK_PX 180                    /* how big the clock is at 100% */
static struct text_glyph clock_glyphs[K86_CLOCK_FONTS + 2][CLOCK_GLYPHS];
static struct text_face clock_faces[K86_CLOCK_FONTS + 2];
static const char *const clock_files[K86_CLOCK_FONTS + 2] = {
    "fonts/clock-hd-bold.lkxf", "fonts/clock-hd-light.lkxf", "fonts/clock-hd-thin.lkxf",
    "fonts/clock-hd-condensed.lkxf", "fonts/clock-hd-mono.lkxf", "fonts/clock-hd-serif.lkxf",
    "fonts/clock-hd-slim.lkxf", "fonts/clock.lkxf", "fonts/clock-hello.lkxf",   /* the last: the setup's hello (letters) */
};

static uint32_t le(const uint8_t *p, int n)
{
    uint32_t v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

/* Read it from the ramdisk the first time; checked, so a damaged file only
 * means a smaller clock (the heading style). 0 or a negative error. */
static int load_clock_face(int n)
{
    if (clock_faces[n].glyphs)
        return 0;
    const uint8_t *d;
    uint32_t size;
    if (ramdisk_find(clock_files[n], (const void **)&d, &size))
        return -ENOENT;
    if (size < 12 || memcmp(d, "LKXF", 4) || le(d + 10, 2) != CLOCK_GLYPHS || size < 12 + 10u * CLOCK_GLYPHS)
        return -EINVAL;
    const uint8_t *t = d + 12, *cov = t + 10 * CLOCK_GLYPHS;
    uint32_t cov_len = size - (uint32_t)(cov - d);
    for (int i = 0; i < CLOCK_GLYPHS; i++, t += 10) {
        struct text_glyph *g = &clock_glyphs[n][i];
        *g = (struct text_glyph){ t[0], t[1], (int8_t)t[2], (int8_t)t[3], (uint16_t)le(t + 4, 2), le(t + 6, 4) };
        if (g->off > cov_len || (uint32_t)g->w * g->h > cov_len - g->off)
            return -EINVAL;
    }
    clock_faces[n] = (struct text_face){ "clock", (int)le(d + 4, 2), (int)le(d + 6, 2), (int)le(d + 8, 2),
                                         clock_glyphs[n], cov };
    return 0;
}

const struct text_face *lockscreen_face(int n)
{
    if (n < 0 || n > K86_CLOCK_FONTS + 1 || load_clock_face(n))
        return 0;
    return &clock_faces[n];
}

/* The clock's face, or the heading style's if it couldn't be read. */
static const struct text_face *clock_font(void)
{
    int n = ls.set.clock_font;
    if (n < 0 || n >= K86_CLOCK_FONTS)
        n = 0;
    if (load_clock_face(n) && n)
        n = 0;
    if (!clock_faces[n].glyphs && load_clock_face(0))
        return &text_faces[TEXT_HEADING];
    return clock_faces[n].glyphs ? &clock_faces[n] : &clock_faces[0];
}

const struct text_face *lockscreen_clock_face(void)
{
    if (load_clock_face(HOME_FACE))
        return &text_faces[TEXT_HEADING];
    return &clock_faces[HOME_FACE];
}

static int inside(struct gfx_rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* --- buffers ------------------------------------------------------------------------------ */

static int screen_buffer(struct gfx_surface *s)
{
    if (s->px)
        return 0;
    uint32_t bytes = (uint32_t)S()->w * (uint32_t)S()->h * 4;
    uintptr_t phys = pmm_alloc_contiguous((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!phys)
        return -ENOMEM;
    *s = (struct gfx_surface){ (uint32_t *)phys, S()->w, S()->h, S()->w };
    return 0;
}

static int clock_gen;                   /* up by one whenever the clock's mask or the background changes */

/* The wallpaper it shows, darkened so the clock reads. */
static void make_background(void)
{
    struct gfx_surface *b = &ls.bg;
    const char *w = ls.set.wallpaper;
    if (!strcmp(w, LOCK_PICTURE) && ls.pic.px)
        memcpy(b->px, ls.pic.px, (size_t)b->w * (size_t)b->h * 4);
    else if (!strcmp(w, "none"))
        wallpaper_render("", b);
    else if (strcmp(w, "desktop") && strcmp(w, LOCK_PICTURE) && wallpaper_render(w, b) == 0)
        ;
    else
        wallpaper_draw(b, 0, 0, 0);         /* the desktop's own */
    gfx_darken(b, 0, 0, b->w, b->h, DIM);
    clock_gen++;                        /* the glass clock shows what's behind it */
}

/* --- the clock ----------------------------------------------------------------------------- */

static int read_time(struct rtc_time *t)
{
    device_t *rtc = device_find("rtc0");
    return rtc && rtc->state == DEVICE_BOUND && dev_read(rtc, t, sizeof(*t)) == (int)sizeof(*t) ? 0 : -1;
}

static char *put(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = '\0';
    return p;
}

static char *put2(char *p, int v, int pad)
{
    if (v >= 10 || pad)
        *p++ = (char)('0' + v / 10);
    *p++ = (char)('0' + v % 10);
    *p = '\0';
    return p;
}

static void time_text(const struct rtc_time *t, char *hm, char *ampm)
{
    int h = t->hour;
    ampm[0] = '\0';
    if (!ls.set.clock24) {
        put(ampm, h < 12 ? "AM" : "PM");
        h = h % 12 ? h % 12 : 12;
    }
    char *p = put2(hm, h, ls.set.clock24);
    *p++ = ':';
    put2(p, t->minute, 1);
}

static void date_text(const struct rtc_time *t, char *out)
{
    static const char *const days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
                                        "Friday", "Saturday" };
    static const char *const months[] = { "January", "February", "March", "April", "May", "June", "July",
                                          "August", "September", "October", "November", "December" };
    char *p = put(out, days[t->weekday % 7]);
    *p++ = ' ';
    p = put2(p, t->day, 0);
    *p++ = ' ';
    put(p, months[(t->month + 11) % 12]);
}

/* --- layout -------------------------------------------------------------------------------- */

static struct gfx_rect menu_rect(void)
{
    int h = 2 * ITEM_H + 2 * MENU_PAD;
    return (struct gfx_rect){ (S()->w - MENU_W) / 2, (S()->h - h) / 2, MENU_W, h };
}

static struct gfx_rect item_rect(int k)
{
    struct gfx_rect m = menu_rect();
    return (struct gfx_rect){ m.x + MENU_PAD, m.y + MENU_PAD + k * ITEM_H, MENU_W - 2 * MENU_PAD, ITEM_H };
}

/* The big clock is always there: a little higher when the users or the
 * password prompt are under it. */

static int clock_y(void)
{
    return S()->h * (ls.view == V_CLOCK ? 11 : 12) / 100;
}

/* The clock is drawn once into a mask, changed only when the minute or a choice does:
 * its stretch and outline are too much to work out on every frame of a slide. */
#define BIG_W 1280                      /* where the big digits are drawn */
#define BIG_H 800
struct clkstate {
    char hm[8];
    int font, style, stretch, size, top_gap;    /* top_gap: above the digits, in the font's line; size: % */
    uint8_t *mask, *edge;               /* the digits; edge: the glass style's rim (screen-sized) */
    int w, h;
};
static struct clkstate clk = { .font = -1, .size = 100 };      /* the one in use: the lock screen's, or the home's */
static struct clkstate clk_lock_keep;   /* the lock screen's, while the home's is in use */
static struct {
    struct gfx_surface tmp;             /* BIG_W x BIG_H */
    uint8_t *raw, *work;                /* BIG_W x BIG_H */
} scratch;
static int cur_size = 100;              /* the clock's size, %: 100 on the lock screen, the home clock's own */

/* src (w x h) to dst (nw x h): each column the average of the ones it covers. */
static void shrink_cols(const uint8_t *src, int w, int h, uint8_t *dst, int nw)
{
    if (nw > w) {                               /* wider: each column between its two nearest, not a copy of one */
        for (int x = 0; x < nw; x++) {
            int at = (2 * x + 1) * w * 128 / nw - 128, ix = at >> 8, frac = at & 255;
            if (at < 0) {
                ix = 0;
                frac = 0;
            }
            int ix1 = ix + 1 < w ? ix + 1 : ix;
            for (int y = 0; y < h; y++) {
                const uint8_t *r = src + (size_t)y * (size_t)w;
                dst[(size_t)y * (size_t)nw + x] = (uint8_t)((r[ix] * (256 - frac) + r[ix1] * frac) >> 8);
            }
        }
        return;
    }
    for (int x = 0; x < nw; x++) {
        int a = x * w / nw, b = (x + 1) * w / nw;
        b = b > a ? b : a + 1;
        for (int y = 0; y < h; y++) {
            const uint8_t *r = src + (size_t)y * (size_t)w;
            int sum = 0;
            for (int i = a; i < b; i++)
                sum += r[i];
            dst[(size_t)y * (size_t)nw + x] = (uint8_t)(sum / (b - a));
        }
    }
}

/* src (w x h) to dst (w x nh): averaged where it shrinks, blended where it grows. */
static void scale_rows(const uint8_t *src, int w, int h, uint8_t *dst, int nh)
{
    for (int y = 0; y < nh; y++) {
        uint8_t *o = dst + (size_t)y * (size_t)w;
        if (nh <= h) {
            int a = y * h / nh, b = (y + 1) * h / nh;
            b = b > a ? b : a + 1;
            for (int x = 0; x < w; x++) {
                int sum = 0;
                for (int i = a; i < b; i++)
                    sum += src[(size_t)i * (size_t)w + x];
                o[x] = (uint8_t)(sum / (b - a));
            }
        } else {
            int at = y * (h - 1) * 256 / (nh > 1 ? nh - 1 : 1), row = at >> 8, frac = at & 255;
            const uint8_t *p = src + (size_t)row * (size_t)w, *q = row + 1 < h ? p + w : p;
            for (int x = 0; x < w; x++)
                o[x] = (uint8_t)((p[x] * (256 - frac) + q[x] * frac) >> 8);
        }
    }
}

/* Of mask (w x h) into out: only what's within r of an edge. */
static void edges(const uint8_t *mask, int w, int h, int r, uint8_t *out)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int inner = x >= r && y >= r && x < w - r && y < h - r;
            for (int k = 1; inner && k <= r; k++)
                inner = mask[y * w + x - k] > 127 && mask[y * w + x + k] > 127 &&
                        mask[(y - k) * w + x] > 127 && mask[(y + k) * w + x] > 127;
            out[y * w + x] = inner ? 0 : mask[y * w + x];
        }
}

static void *big_buffer(uint32_t bytes)
{
    return (void *)pmm_alloc_contiguous((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
}

/* Glass: the wallpaper behind the digits, blurred, kept until the clock, its place or
 * the background changes. So it's blurred once a minute at most, and each frame of a
 * slide is only a blend, as for the other styles. */
#define GLASS_R 7                       /* each of three box blurs: about 20 px in all */
struct glassstate {
    struct gfx_surface pic;
    int x0, y0, w, h, left, top, gen;
};
static struct glassstate glass = { .gen = -1 };
static struct gfx_surface *glass_src;   /* what the glass blurs: the lock screen's wallpaper, or the home's */

/* One box blur pass over count lines of n pixels: `along` apart within a line,
 * `across` from one line to the next. */
static void box_pass(uint32_t *p, int n, int count, int along, int across, uint32_t *line)
{
    const int r = GLASS_R, k = 2 * GLASS_R + 1;
    for (int c = 0; c < count; c++) {
        uint32_t *q = p + (size_t)c * (size_t)across;
        int sr = 0, sg = 0, sb = 0;
        for (int i = -r; i <= r; i++) {
            uint32_t v = q[(size_t)(i < 0 ? 0 : i >= n ? n - 1 : i) * (size_t)along];
            sr += (int)(v >> 16 & 255);
            sg += (int)(v >> 8 & 255);
            sb += (int)(v & 255);
        }
        for (int i = 0; i < n; i++) {
            line[i] = (uint32_t)(sr / k) << 16 | (uint32_t)(sg / k) << 8 | (uint32_t)(sb / k);
            uint32_t out = q[(size_t)(i - r < 0 ? 0 : i - r) * (size_t)along];
            uint32_t in = q[(size_t)(i + r + 1 >= n ? n - 1 : i + r + 1) * (size_t)along];
            sr += (int)(in >> 16 & 255) - (int)(out >> 16 & 255);
            sg += (int)(in >> 8 & 255) - (int)(out >> 8 & 255);
            sb += (int)(in & 255) - (int)(out & 255);
        }
        for (int i = 0; i < n; i++)
            q[(size_t)i * (size_t)along] = line[i];
    }
}

static int glass_make(int left, int top)
{
    static uint32_t line[2048];
    struct gfx_surface *b = glass_src ? glass_src : &ls.bg;
    if (!b->px || (!glass.pic.px && screen_buffer(&glass.pic)))
        return -1;
    if (glass.gen == clock_gen && glass.left == left && glass.top == top)
        return 0;
    int x0 = left - 2 * GLASS_R, y0 = top - 2 * GLASS_R, x1 = left + clk.w + 2 * GLASS_R, y1 = top + clk.h + 2 * GLASS_R;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > b->w ? b->w : x1;
    y1 = y1 > b->h ? b->h : y1;
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048)
        return -1;
    uint32_t *p = glass.pic.px;
    for (int y = 0; y < h; y++)
        memcpy(p + (size_t)y * (size_t)w, b->px + (size_t)(y0 + y) * (size_t)b->stride + x0, (size_t)w * 4);
    for (int pass = 0; pass < 3; pass++) {
        box_pass(p, w, h, 1, w, line);
        box_pass(p, h, w, w, 1, line);
    }
    glass.x0 = x0;
    glass.y0 = y0;
    glass.w = w;
    glass.h = h;
    glass.left = left;
    glass.top = top;
    glass.gen = clock_gen;
    return 0;
}

static void clock_mask(const char *hm)
{
    const struct lock_settings *c = &ls.set;
    if (!scratch.raw) {
        uint32_t *t = big_buffer(BIG_W * BIG_H * 4);
        scratch.raw = big_buffer(BIG_W * BIG_H);
        scratch.work = big_buffer(BIG_W * BIG_H);
        if (!t || !scratch.raw || !scratch.work) {
            scratch.raw = 0;
            return;
        }
        scratch.tmp = (struct gfx_surface){ t, BIG_W, BIG_H, BIG_W };
    }
    if (!clk.mask) {
        struct gfx_surface b = { 0 }, e = { 0 };
        if (screen_buffer(&b) || screen_buffer(&e))
            return;
        clk.mask = (uint8_t *)b.px;
        clk.edge = (uint8_t *)e.px;
    }
    if (clk.font == c->clock_font && clk.style == c->clock_style && clk.stretch == c->clock_stretch &&
        clk.size == cur_size && !strcmp(clk.hm, hm))
        return;
    memcpy(clk.hm, hm, sizeof(clk.hm));
    clock_gen++;
    clk.font = c->clock_font;
    clk.style = c->clock_style;
    clk.stretch = c->clock_stretch;
    clk.size = cur_size;
    struct gfx_surface *t = &scratch.tmp;
    const struct text_face *f = clock_font();
    memset(t->px, 0, (size_t)t->w * (size_t)t->h * 4);
    if (c->clock_style == 1) {                  /* stacked: the hours over the minutes */
        char top[8], low[8];
        int n = 0;
        for (; hm[n] && hm[n] != ':' && n < 7; n++)
            top[n] = hm[n];
        top[n] = '\0';
        memcpy(low, hm[n] ? hm + n + 1 : "", sizeof(low) - (size_t)n - 1);
        text_draw_face(t, (t->w - text_width_face(top, f)) / 2, 0, top, f, WHITE);
        const struct text_glyph *zero = &f->glyphs['0' - 32];  /* a line: a digit and a gap */
        text_draw_face(t, (t->w - text_width_face(low, f)) / 2, zero->yoff + zero->h + f->size / 12, low, f, WHITE);
    } else {
        text_draw_face(t, (t->w - text_width_face(hm, f)) / 2, 0, hm, f, WHITE);
    }
    /* Just the digits: what's lit, tight. */
    int x0 = t->w, x1 = -1, y0 = t->h, y1 = -1;
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++)
            if (t->px[y * t->stride + x] & 255) {
                x0 = x < x0 ? x : x0;
                x1 = x > x1 ? x : x1;
                y0 = y < y0 ? y : y0;
                y1 = y > y1 ? y : y1;
            }
    if (x1 < x0) {
        clk.w = clk.h = 0;
        return;
    }
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            scratch.raw[y * w + x] = (uint8_t)(t->px[(y + y0) * t->stride + x + x0] & 255);
    /* Down to the clock's size, then up by the stretch: from a big face, both stay sharp. */
    int size = f->size > 0 ? f->size : CLOCK_PX;
    int nw = w * CLOCK_PX / size * cur_size / 100, nh = h * CLOCK_PX / size * cur_size / 100;
    int stretch = c->clock_stretch < 100 ? 100 : c->clock_stretch;
    nw = nw < 1 ? 1 : nw > S()->w - 16 ? S()->w - 16 : nw;
    nh = nh * stretch / 100;
    nh = nh < 1 ? 1 : nh > S()->h * 3 / 4 ? S()->h * 3 / 4 : nh;
    clk.top_gap = y0 * CLOCK_PX / size * cur_size / 100;
    shrink_cols(scratch.raw, w, h, scratch.work, nw);
    scale_rows(scratch.work, nw, h, clk.mask, nh);
    if (c->clock_style == 2) {                  /* outline: only what's near an edge */
        edges(clk.mask, nw, nh, CLOCK_PX / 30 + 2, scratch.work);
        memcpy(clk.mask, scratch.work, (size_t)nw * (size_t)nh);
    } else if (c->clock_style == 3) {           /* glass: a hairline rim */
        edges(clk.mask, nw, nh, 2, clk.edge);
    }
    clk.w = nw;
    clk.h = nh;
}

static int home_mode;                   /* drawing the home screen's clock (home_enter) */
static struct home_clock home_set;       /* its choices (defined below with their first values) */
#define HOME_AUTO_Y 124                 /* the home clock's top by itself, under the status bar */

/* Where the digits go, on the screen. */
static void clock_place(int *x, int *y)
{
    const struct lock_settings *c = &ls.set;
    int sw = S()->w, sh = S()->h;
    int den = home_mode ? 1000 : 100;       /* the home clock's place is in tenths of a percent */
    int cx = sw * (c->clock_x < 0 || c->clock_x > den ? den / 2 : c->clock_x) / den;
    /* By itself, the digits sit where the home screen's face puts them: the big faces
     * have the space above their digits trimmed (tools/font-rasterize.py). */
    int lead = !load_clock_face(HOME_FACE) ? clock_faces[HOME_FACE].glyphs['0' - 32].yoff : 0;
    int top = c->clock_y < 0 ? (home_mode ? HOME_AUTO_Y : clock_y()) + lead + clk.top_gap
                              : sh * c->clock_y / den - clk.h / 2;
    int left = cx - clk.w / 2;
    left = left < 8 ? 8 : left + clk.w > sw - 8 ? sw - 8 - clk.w : left;
    int date = text_height(TEXT_BOLD) + 12, above = c->clock_style == 3 ? date : 0, under = above ? 0 : date;
    int least = (home_mode ? 84 : 60) + above;      /* clear of the status bar */
    top = top < least ? least : top + clk.h + under > sh - 80 ? sh - 80 - under - clk.h : top;
    *x = left;
    *y = top;
}

static struct gfx_rect clock_rect(void)
{
    char hm[8], ampm[4];
    int x = 0, y = clock_y();
    if (ls.shown.year) {
        time_text(&ls.shown, hm, ampm);
        clock_mask(hm);
        clock_place(&x, &y);
    }
    return (struct gfx_rect){ 0, y, S()->w, clk.h + 12 + text_height(TEXT_BOLD) };
}

/* The PIN's dots, the title over them and the message under them (in the
 * picker, which LiteKern Go hides: the users). */
static struct gfx_rect middle_rect(void)
{
    if (ls.view == V_PICK)
        return (struct gfx_rect){ 0, S()->h * 42 / 100, S()->w, S()->h * 48 / 100 };
    return (struct gfx_rect){ 0, S()->h * 17 / 100, S()->w, 240 };
}

static int dots_cy(void)
{
    return S()->h * 28 / 100;
}

static void pad_centre(int k, int *cx, int *cy)
{
    *cx = S()->w / 2 + (k % 3 - 1) * PAD_DX;
    *cy = S()->h - PAD_BOTTOM - (3 - k / 3) * PAD_DY;
}

static const char *pad_label(int k)
{
    static const char *const labels[PAD_KEYS] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "Del", "0", "OK" };
    return labels[k];
}

static int pick_x(int i)                    /* the picker's i-th avatar centre */
{
    int n = users_count();
    return S()->w / 2 + (2 * i - (n - 1)) * PICK_GAP / 2;
}

static int pick_cy(void)                    /* the users' avatars: under the clock */
{
    return S()->h * 58 / 100;
}

static int hit_at(int x, int y)
{
    if (ls.menu_open) {
        for (int k = 0; k < 2; k++)
            if (inside(item_rect(k), x, y))
                return H_RESTART + k;
    }
    if (ls.view == V_PROMPT)
        for (int k = 0; k < PAD_KEYS; k++) {
            int cx, cy;
            pad_centre(k, &cx, &cy);
            if (x >= cx - PAD_DX / 2 && x < cx + PAD_DX / 2 && y >= cy - PAD_DY / 2 && y < cy + PAD_DY / 2)
                return H_PAD + k;
        }
    if (ls.view == V_PICK)
        for (int i = 0; i < users_count(); i++) {
            int dx = x - pick_x(i), dy = y - pick_cy();
            if (dx > -PICK_GAP / 2 && dx < PICK_GAP / 2 && dy > -PICK_R - 6 && dy < PICK_R + 40)
                return H_AVATAR + i;
        }
    return H_NONE;
}

/* --- drawing ------------------------------------------------------------------------------- */

/* A user's avatar: their colour, their name's first letter. */
static void draw_avatar(struct gfx_surface *s, int cx, int cy, int r, const struct user *u, int ring)
{
    if (ring) {
        gfx_fill_circle(s, cx, cy, r + 5, WHITE);
        gfx_fill_circle(s, cx, cy, r + 2, gfx_mix(0, u->colour, 140));
    }
    gfx_fill_circle(s, cx, cy, r, u->colour);
    char letter[2] = { u->name[0] >= 'a' && u->name[0] <= 'z' ? (char)(u->name[0] - 32) : u->name[0], 0 };
    enum text_style st = r >= 40 ? TEXT_LARGE : TEXT_HEADING;
    text_draw(s, cx - text_width(letter, st) / 2, cy - text_height(st) / 2, letter, st, WHITE);
}

static void centred(struct gfx_surface *s, int ox, int oy, int y, const char *t, enum text_style st, uint32_t c)
{
    text_draw(s, (S()->w - text_width(t, st)) / 2 - ox, y - oy, t, st, c);
}

static void draw_clock(struct gfx_surface *s, int ox, int oy)
{
    char hm[8], ampm[4], date[40];
    if (!ls.shown.year)
        return;
    time_text(&ls.shown, hm, ampm);
    date_text(&ls.shown, date);
    /* The time, big, in the middle, and the date under it, as on Phosh. */
    clock_mask(hm);
    int left, top, alpha = ls.set.clock_opacity * 255 / 100;
    clock_place(&left, &top);
    int glassy = ls.set.clock_style == 3 && !glass_make(left, top);
    for (int y = 0; glassy && y < clk.h; y++) {
        int dy = top + y - oy, gy = top + y - glass.y0;
        if (dy < 0 || dy >= s->h || gy < 0 || gy >= glass.h)
            continue;
        uint32_t *row = s->px + (size_t)dy * (size_t)s->stride;
        const uint8_t *m = clk.mask + (size_t)y * (size_t)clk.w, *e = clk.edge + (size_t)y * (size_t)clk.w;
        const uint32_t *g = glass.pic.px + (size_t)gy * (size_t)glass.w;
        uint32_t shine = (uint32_t)(60 - 50 * y / (clk.h > 0 ? clk.h : 1));     /* lighter at the top */
        for (int x = 0; x < clk.w; x++) {
            int dx = left + x - ox, gx = left + x - glass.x0;
            if (!m[x] || dx < 0 || dx >= s->w || gx < 0 || gx >= glass.w)
                continue;
            uint32_t fill = gfx_mix(WHITE, g[gx], 70 + shine);         /* frosted: mostly what's behind */
            fill = gfx_mix(WHITE, fill, (uint32_t)(e[x] * 150 / 255));  /* the rim catches the light */
            row[dx] = gfx_mix(fill, row[dx], (uint32_t)(m[x] * alpha / 255));
        }
    }
    for (int y = 0; !glassy && y < clk.h; y++) {
        int dy = top + y - oy;
        if (dy < 0 || dy >= s->h)
            continue;
        uint32_t *row = s->px + (size_t)dy * (size_t)s->stride;
        const uint8_t *m = clk.mask + (size_t)y * (size_t)clk.w;
        for (int x = 0; x < clk.w; x++) {
            int dx = left + x - ox;
            if (m[x] && dx >= 0 && dx < s->w)
                row[dx] = gfx_mix(WHITE, row[dx], (uint32_t)(m[x] * alpha / 255));
        }
    }
    if (ampm[0])
        text_draw(s, left + clk.w + 12 - ox, top + clk.h - text_height(TEXT_HEADING) - oy, ampm, TEXT_HEADING, WHITE);
    if (home_mode && !home_set.date)
        return;
    int dw = text_width(date, TEXT_BOLD), dx = left + clk.w / 2 - dw / 2;
    dx = dx < 8 ? 8 : dx + dw > S()->w - 8 ? S()->w - 8 - dw : dx;
    int dy = ls.set.clock_style == 3 ? top - 12 - text_height(TEXT_BOLD) : top + clk.h + 12;
    text_draw(s, dx - ox, dy - oy, date, TEXT_BOLD, WHITE);
}

/* The PIN pad: the time small at the top, "Enter PIN", a dot for each digit
 * typed, then the keys (1-9, Del, 0, OK). */
static void draw_prompt(struct gfx_surface *s, int ox, int oy)
{
    char hm[8], ampm[4];
    if (ls.shown.year) {
        time_text(&ls.shown, hm, ampm);
        centred(s, ox, oy, S()->h * 6 / 100, hm, TEXT_LARGE, WHITE);
    }
    int waiting = (int32_t)(ls.wait_until - uptime_ms()) > 0;
    centred(s, ox, oy, S()->h * 17 / 100, waiting ? "Wait a moment" TEXT_ELLIPSIS : "Enter PIN", TEXT_HEADING, WHITE);
    int shown = ls.ok ? ls.ok_dots : ls.pwlen;
    int n = shown > 16 ? 16 : shown, w = n * 40 - 16, x = (S()->w - w) / 2 + ls.shake;
    for (int i = 0; i < n; i++) {
        int green = ls.ok && i < ls.ok;
        gfx_fill_circle(s, x + 12 + i * 40 - ox, dots_cy() - oy, green ? 15 : 12, green ? 0x57e389u : WHITE);
    }
    int y = dots_cy() + 40;
    if (ls.message)
        centred(s, ox, oy, y, ls.message, TEXT_BODY, BAD);
    else if (ls.caps)
        centred(s, ox, oy, y, "Caps Lock is on", TEXT_BODY, WARN);
    for (int k = 0; k < PAD_KEYS; k++) {
        int cx, cy;
        pad_centre(k, &cx, &cy);
        int digit = k != 9 && k != 11;
        if (digit || k == ls.pad_held)
            gfx_blend_round_rect(s, cx - PAD_R - ox, cy - PAD_R - oy, 2 * PAD_R, 2 * PAD_R, PAD_R, WHITE,
                                 k == ls.pad_held ? 110 : 36);
        enum text_style st = digit ? TEXT_LARGE : TEXT_BOLD;
        const char *l = pad_label(k);
        text_draw(s, cx - text_width(l, st) / 2 - ox, cy - text_height(st) / 2 - oy, l, st, WHITE);
    }
}

/* Notifications under the clock: which app, and the title, but not the text, as
 * that may be private. */
static void draw_notes(struct gfx_surface *s, int ox, int oy)
{
    struct gfx_rect c = clock_rect();
    int n = desktop_note_count(), y = c.y + c.h + 36, x = 48, w = S()->w - 96;
    int cards = (n < NOTES_SHOWN ? n : NOTES_SHOWN) * (NOTE_H + 12);
    if (y + cards > S()->h - 120 && c.y - cards - 24 > 60)  /* the clock's low: above it */
        y = c.y - cards - 24;
    for (int i = 0; i < n && i < NOTES_SHOWN; i++, y += NOTE_H + 12) {
        gfx_blend_round_rect(s, x - ox, y - oy, w, NOTE_H, 24, WHITE, 40);
        desktop_note_icon(i, s, x + 20 - ox, y + (NOTE_H - NOTE_ICON) / 2 - oy, NOTE_ICON);
        int tx = x + 20 + NOTE_ICON + 20, tw = w - (tx - x) - 24;
        int h = text_height(TEXT_SMALL) + text_height(TEXT_BOLD), ty = y + (NOTE_H - h) / 2;
        text_draw_fit(s, tx - ox, ty - oy, desktop_note_app(i), tw, TEXT_SMALL, gfx_mix(WHITE, 0, 200));
        text_draw_fit(s, tx - ox, ty + text_height(TEXT_SMALL) - oy, desktop_note_title(i), tw, TEXT_BOLD, WHITE);
    }
    if (n > NOTES_SHOWN) {
        char more[16] = "+";
        char *p = put2(more + 1, n - NOTES_SHOWN, 0);
        put(p, " more");
        centred(s, ox, oy, y, more, TEXT_SMALL, gfx_mix(WHITE, 0, 190));
    }
}

static void draw_pick(struct gfx_surface *s, int ox, int oy)
{
    for (int i = 0; i < users_count(); i++) {
        const struct user *u = users_get(i);
        int cx = pick_x(i), lit = i == ls.pick || ls.hover == H_AVATAR + i;
        draw_avatar(s, cx - ox, pick_cy() - oy, PICK_R, u, lit);
        text_draw_fit(s, cx - (PICK_GAP - 16) / 2 + ((PICK_GAP - 16) - (text_width(u->name, TEXT_BOLD) < PICK_GAP - 16
                                                                            ? text_width(u->name, TEXT_BOLD)
                                                                            : PICK_GAP - 16)) / 2 - ox,
                      pick_cy() + PICK_R + 12 - oy, u->name, PICK_GAP - 16, TEXT_BOLD, WHITE);
    }
}

/* Everything, into s, which shows the screen from (ox, oy). */
static void draw_into(struct gfx_surface *s, int ox, int oy)
{
    gfx_blit(s, 0, 0, &ls.bg, ox, oy, s->w, s->h);
    if (ls.view == V_PROMPT) {
        gfx_darken(s, 0, 0, s->w, s->h, 130);
        draw_prompt(s, ox, oy);
    } else {
        /* The clock, the notifications and the hint follow a swipe up. */
        int coy = oy - ls.lift;
        draw_clock(s, ox, coy);
        if (ls.view == V_PICK)
            draw_pick(s, ox, coy);
        else
            draw_notes(s, ox, coy);
        const char *hint = ls.view == V_PICK ? "Choose who you are" : "Swipe up to unlock";
        centred(s, ox, coy, S()->h - 64, hint, TEXT_BODY, gfx_mix(WHITE, 0, 190));
    }
    if (ls.menu_open) {                     /* Power held: Restart, Power off */
        gfx_darken(s, 0, 0, s->w, s->h, 120);
        struct gfx_rect m = menu_rect();
        gfx_fill_round_rect(s, m.x - ox, m.y - oy, m.w, m.h, 24, POPOVER);
        for (int k = 0; k < 2; k++) {
            struct gfx_rect r = item_rect(k);
            int disabled = k == 1 && !power_can_off();
            text_draw(s, r.x + 32 - ox, r.y + (r.h - text_height(TEXT_BODY)) / 2 - oy, k ? "Power off" : "Restart",
                      TEXT_BODY, disabled ? gfx_mix(WHITE, POPOVER, 110) : WHITE);
        }
    }
}

static void draw_rect(struct gfx_rect r)
{
    r = gfx_rect_intersect(r, (struct gfx_rect){ 0, 0, S()->w, S()->h });
    if (gfx_rect_empty(r))
        return;
    struct gfx_surface sub = gfx_sub(S(), r);
    draw_into(&sub, r.x, r.y);
    screen_damage(r.x, r.y, r.w, r.h);
    screen_present();
}

static void draw_all(void)
{
    draw_rect((struct gfx_rect){ 0, 0, S()->w, S()->h });
    cursor_refresh();
}

/* --- the curtain ----------------------------------------------------------------------------
 *
 * Locking, the lock screen slides down over the desktop like a curtain;
 * unlocking, it slides back up and the desktop is there underneath. A soft
 * shadow runs along its bottom edge. Only the rows that change are drawn
 * each frame, and a frame that takes too long skips to the end. */

static int ease_out(int t)                  /* t and the result 0..1024: fast, then settling */
{
    int64_t u = t - 1024;
    return (int)(1024 + u * u / 1024 * u / 1024);
}

static int ease_in(int t)                   /* slow, then fast: lifting off */
{
    return (int)((int64_t)t * t / 1024 * t / 1024);
}

static int ease_io(int t)                   /* smooth at both ends */
{
    return (int)((int64_t)t * t / 1024 * (3 * 1024 - 2 * t) / 1024);
}

/* One frame: the lock screen's bottom `vis` rows at the top, then the
 * desktop with the shadow on it. Shows rows 0..`dirty`. */
static void curtain(int vis, int dirty)
{
    struct gfx_surface *s = S();
    int w = s->w, h = s->h;
    if (vis > 0)
        gfx_blit(s, 0, 0, &ls.frame, 0, h - vis, w, vis);
    int sh_end = vis + SHADOW < h ? vis + SHADOW : h;
    if (sh_end < dirty)
        gfx_blit(s, 0, sh_end, &ls.under, 0, sh_end, w, dirty - sh_end);
    for (int y = vis; y < sh_end; y++) {    /* the shadow: darkest at the edge */
        gfx_blit(s, 0, y, &ls.under, 0, y, w, 1);
        gfx_darken(s, 0, y, w, 1, (uint32_t)(110 * (SHADOW - (y - vis)) / SHADOW));
    }
    screen_damage(0, 0, w, dirty);
    screen_present();
}

static void slide(int locking)
{
    int h = S()->h, last = locking ? 0 : h, frames = 0;
    uint32_t t0 = uptime_ms(), ms = SLIDE_MS * ANIM_SLOW, worst = 0;
    for (;;) {
        uint32_t f0 = uptime_ms(), el = f0 - t0;
        int t = el >= ms ? 1024 : (int)(el * 1024 / ms);
        int vis = locking ? h * ease_out(t) / 1024 : h - h * ease_in(t) / 1024;
        int dirty = (locking ? vis : last) + SHADOW;
        curtain(vis, dirty < h ? dirty : h);
        last = vis;
        frames++;
        uint32_t took = uptime_ms() - f0;
        if (took > worst)
            worst = took;
        if (t >= 1024)
            break;
        if (took > MAX_FRAME_MS)
            t0 -= ms;                       /* too slow here: the last frame next */
    }
    kprintf("anim: %s, %d frames, %u ms, slowest frame %u ms\n", locking ? "lock" : "unlock", frames,
            uptime_ms() - t0, worst);
}

/* A right PIN: the dots turn green one after another, a little bigger, quick enough
 * that it's feedback and not a wait. */
static void pin_success(void)
{
    int n = ls.pwlen > 16 ? 16 : ls.pwlen;
    if (ls.view != V_PROMPT || n < 1)
        return;
    ls.ok_dots = n;
    uint32_t gap = (uint32_t)(160 / n) * ANIM_SLOW;
    for (int i = 1; i <= n; i++) {
        ls.ok = i;
        draw_rect(middle_rect());
        uint32_t f0 = uptime_ms();
        while (uptime_ms() - f0 < gap)
            cpu_relax();
    }
    ls.reveal = 1;
}

static int isqrt_i(int64_t v)
{
    if (v <= 0)
        return 0;
    int64_t x = v, y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2;
    }
    return (int)x;
}

static void span_copy(uint32_t *row, const uint32_t *src, int a, int b, int w)
{
    a = a < 0 ? 0 : a;
    b = b > w ? w : b;
    if (a < b)
        memcpy(row + a, src + a, (size_t)(b - a) * 4);
}

static void span_rim(uint32_t *row, const uint32_t *src, int a, int b, int w)
{
    a = a < 0 ? 0 : a;
    b = b > w ? w : b;
    for (int x = a; x < b; x++)
        row[x] = gfx_mix(WHITE, src[x], 190);
}

#define RIM 5
#define REVEAL_MS 380

/* One frame of the reveal: the desktop inside a ring spreading from (cx, cy) with the
 * lock screen outside it. Only what the ring has crossed since the last frame (rp) is
 * drawn, so it costs about what the curtain does. */
static void reveal_frame(int cx, int cy, int r, int rp)
{
    struct gfx_surface *s = S();
    int w = s->w, h = s->h;
    int y0 = cy - r - RIM, y1 = cy + r + RIM + 1;
    y0 = y0 < 0 ? 0 : y0;
    y1 = y1 > h ? h : y1;
    for (int y = y0; y < y1; y++) {
        int64_t dy2 = (int64_t)(y - cy) * (y - cy);
        int hin = (int64_t)r * r > dy2 ? isqrt_i((int64_t)r * r - dy2) : 0;
        int hout = isqrt_i((int64_t)(r + RIM) * (r + RIM) - dy2);
        int pin = rp >= 0 && (int64_t)rp * rp > dy2 ? isqrt_i((int64_t)rp * rp - dy2) : 0;
        uint32_t *row = s->px + (size_t)y * (size_t)s->stride;
        const uint32_t *und = ls.under.px + (size_t)y * (size_t)ls.under.stride;
        const uint32_t *frm = ls.frame.px + (size_t)y * (size_t)ls.frame.stride;
        span_copy(row, und, cx - hin, cx - pin, w);
        span_copy(row, und, cx + pin, cx + hin, w);
        span_rim(row, frm, cx - hout, cx - hin, w);
        span_rim(row, frm, cx + hin, cx + hout, w);
    }
    screen_damage(0, y0, w, y1 - y0);
    screen_present();
}

/* Unlocked with a PIN: the desktop opens out from the dots like a ripple. */
static void reveal(void)
{
    struct gfx_surface *s = S();
    int cx = s->w / 2, cy = dots_cy(), dx = cx > s->w - cx ? cx : s->w - cx, dyy = cy > s->h - cy ? cy : s->h - cy;
    int rmax = isqrt_i((int64_t)dx * dx + (int64_t)dyy * dyy) + 2, last = -1, frames = 0;
    /* The back buffer holds the desktop, drawn quietly; the display still shows the lock
     * screen. A frame redraws only part of each row, so the rest must be the lock screen too. */
    gfx_blit(s, 0, 0, &ls.frame, 0, 0, s->w, s->h);
    uint32_t t0 = uptime_ms(), ms = REVEAL_MS * ANIM_SLOW, worst = 0;
    for (;;) {
        uint32_t f0 = uptime_ms(), el = f0 - t0;
        int t = el >= ms ? 1024 : (int)(el * 1024 / ms);
        int r = t >= 1024 ? rmax : rmax * ease_io(t) / 1024;
        if (r == last && t < 1024) {        /* nothing new to show yet */
            cpu_relax();
            continue;
        }
        reveal_frame(cx, cy, r, last);
        last = r;
        frames++;
        uint32_t took = uptime_ms() - f0;
        if (took > worst)
            worst = took;
        if (t >= 1024)
            break;
        if (took > MAX_FRAME_MS)
            t0 -= ms;                       /* too slow here: the last frame next */
    }
    kprintf("anim: unlock, %d frames, %u ms, slowest frame %u ms\n", frames, uptime_ms() - t0, worst);
}

/* A wrong password: the prompt shakes its head. */
static void shake(void)
{
    static const int8_t offs[] = { -14, 12, -10, 8, -6, 4, -2, 0 };
    for (unsigned i = 0; i < sizeof(offs); i++) {
        uint32_t f0 = uptime_ms();
        ls.shake = offs[i];
        draw_rect(middle_rect());
        while (uptime_ms() - f0 < 30 * ANIM_SLOW)
            cpu_relax();
    }
    ls.shake = 0;
}

/* --- locking, logging in, unlocking ------------------------------------------------------- */

static int start(int animate_from_desktop)
{
    if (ls.active || !screen_ready() || !desktop_started())
        return -1;
    if (screen_buffer(&ls.bg)) {
        kprintf("lock: out of memory; not locked\n");
        return -1;
    }
    int fe = load_clock_face(0);
    if (fe)
        kprintf("lock: the clock's font couldn't be read (%d); a smaller clock\n", fe);
    /* The curtain needs the desktop as it is, and the finished lock screen. */
    int animate = animate_from_desktop && !screensaver_active() && !screen_buffer(&ls.under) &&
                  !screen_buffer(&ls.frame);
    if (animate)
        gfx_blit(&ls.under, 0, 0, S(), 0, 0, S()->w, S()->h);
    desktop_cover(1);
    make_background();
    ls.active = 1;
    ls.menu_open = ls.hover = 0;
    ls.pwlen = 0;
    ls.message = 0;
    ls.swiping = ls.lift = 0;
    ls.pad_held = -1;
    ls.power_down_ms = 0;
    ls.power_held = 0;
    if (read_time(&ls.shown))
        ls.shown.year = 0;
    if (animate) {
        draw_into(&ls.frame, 0, 0);
        slide(1);
    }
    ls.locked_at = ls.last_tick = ls.last_input = uptime_ms();
    draw_all();
    return 0;
}

void lockscreen_lock(void)
{
    ls.login = 0;
    ls.who = users_current();
    ls.pick = ls.who > 0 ? ls.who : 0;
    ls.view = V_CLOCK;          /* LiteKern Go: one user, so no picker (UI-PLAN.md) */
    if (!start(1))
        kprintf("lock: locked (wallpaper %s)\n", ls.set.wallpaper);
}

void lockscreen_login(void)
{
    ls.login = 1;
    ls.who = 0;
    ls.pick = 0;
    ls.view = V_CLOCK;          /* the first user's: a swipe up, then their PIN */
    if (!start(0))
        kprintf("lock: login screen (%d user%s)\n", users_count(), users_count() == 1 ? "" : "s");
}

int lockscreen_active(void)
{
    return ls.active;
}

static void unlock(void)
{
    ls.active = 0;
    memset(ls.pw, 0, sizeof(ls.pw));        /* not left lying in memory */
    ls.pwlen = 0;
    if (ls.login) {
        users_set_current(ls.who);
        kprintf("lock: logged in as %s\n", users_get(ls.who)->name);
        ls.login = 0;
    } else if (ls.who != users_current()) {
        /* Someone else: the one logged in is logged out first (their apps
         * closed), as there's room for one at a time (kernel/users.h). */
        kprintf("lock: switching from %s to %s\n", users_get(users_current())->name, users_get(ls.who)->name);
        desktop_log_out();
        users_set_current(ls.who);
        kprintf("lock: logged in as %s\n", users_get(ls.who)->name);
    } else {
        kprintf("lock: unlocked\n");
    }
    if (screen_buffer(&ls.under) == 0 && screen_buffer(&ls.frame) == 0) {
        ls.menu_open = ls.hover = 0;
        draw_into(&ls.frame, 0, 0);         /* the lock screen as it was */
        desktop_render_quiet();             /* the desktop, drawn but not shown yet */
        gfx_blit(&ls.under, 0, 0, S(), 0, 0, S()->w, S()->h);
        if (ls.reveal)
            reveal();
        else
            slide(0);
    }
    ls.lift = ls.swiping = 0;
    ls.reveal = ls.ok = 0;
    desktop_cover(0);                       /* the desktop draws itself again */
}

void lockscreen_redraw(void)
{
    if (ls.active)
        draw_all();
}

static void show(enum view v)
{
    ls.view = v;
    ls.message = 0;
    ls.pwlen = 0;
    ls.lift = 0;
    ls.pad_held = -1;
    draw_all();
}

/* The clock's been looked at: on to the prompt, or straight in without a password. */
static void wake(void)
{
    const struct user *u = users_get(ls.who);
    if (!u || !u->has_password) {
        unlock();
        return;
    }
    kprintf("lock: PIN for %s\n", u->name);
    show(V_PROMPT);
}

/* The picker's choice. */
static void choose(int i)
{
    const struct user *u = users_get(i);
    if (!u)
        return;
    ls.who = i;
    ls.pick = i;
    if (!u->has_password) {
        unlock();
        return;
    }
    kprintf("lock: password for %s\n", u->name);
    show(V_PROMPT);
}

static void try_password(void)
{
    if ((int32_t)(ls.wait_until - uptime_ms()) > 0)
        return;
    ls.pw[ls.pwlen] = '\0';
    if (users_check(ls.who, ls.pw)) {
        ls.fails = 0;
        pin_success();
        unlock();
        return;
    }
    ls.fails++;
    kprintf("lock: wrong PIN for %s (%d)\n", users_get(ls.who)->name, ls.fails);
    memset(ls.pw, 0, sizeof(ls.pw));
    ls.pwlen = 0;
    /* After a few wrong tries, a wait that grows: 2, 4, 8 ... up to 30 s. */
    uint32_t wait = ls.fails < 3 ? 0 : 1000u << (ls.fails - 2 > 5 ? 5 : ls.fails - 2);
    if (wait > 30000)
        wait = 30000;
    ls.wait_until = uptime_ms() + wait;
    ls.message = wait ? "Wrong PIN. Wait a moment." : "Wrong PIN. Try again.";
    shake();
}

static void set_menu(int open)
{
    if (ls.menu_open == open)
        return;
    ls.menu_open = open;
    kprintf("lock: power menu %s\n", open ? "open" : "closed");
    draw_all();
}

void lockscreen_tick(void)
{
    if (!ls.active)
        return;
    if (ls.power_down_ms && !ls.power_held && (int32_t)(uptime_ms() - ls.power_down_ms) >= POWER_HOLD_MS) {
        ls.power_held = 1;
        if (machine_screen_on())
            set_menu(1);
        else
            machine_screen(1);              /* held with the screen off: wake it, still locked */
    }
    if (ls.view == V_PROMPT && uptime_ms() - ls.last_input > PROMPT_IDLE_MS)
        show(V_CLOCK);                      /* left alone: back to the clock */
    /* Left alone on the clock for LOCK_OFF_MS (counted from when the screen came on): the screen goes off, the lock stays. */
    static int was_on;
    int on = machine_screen_on();
    if (on && !was_on)
        ls.last_input = uptime_ms();
    was_on = on;
    if (on && ls.view == V_CLOCK && !ls.menu_open && !ls.power_down_ms && uptime_ms() - ls.last_input > LOCK_OFF_MS) {
        kprintf("lock: screen off (5 s alone)\n");
        machine_screen(0);
        was_on = 0;
    }
    if (ls.message && ls.wait_until && (int32_t)(ls.wait_until - uptime_ms()) <= 0) {
        ls.wait_until = 0;                  /* the wait's over: say so */
        ls.message = "Wrong PIN. Try again.";
        draw_rect(middle_rect());
    }
    if (uptime_ms() - ls.last_tick < 1000)
        return;
    ls.last_tick = uptime_ms();
    struct rtc_time t;
    if (read_time(&t) || (t.minute == ls.shown.minute && t.hour == ls.shown.hour && t.day == ls.shown.day))
        return;
    ls.shown = t;
    if (ls.view == V_PROMPT)
        draw_all();                         /* its small clock */
    else
        draw_rect(clock_rect());
}

static int is_modifier(uint16_t key)
{
    return key == KEY_LSHIFT || key == KEY_RSHIFT || key == KEY_LCTRL || key == KEY_RCTRL ||
           key == KEY_LALT || key == KEY_RALT || key == KEY_LSUPER || key == KEY_RSUPER ||
           key == KEY_CAPSLOCK;
}

static void pin_add(char c)
{
    if (ls.pwlen >= PASSWORD_MAX - 1)
        return;
    ls.pw[ls.pwlen++] = c;
    if (ls.message && !ls.wait_until)
        ls.message = 0;
}

/* A key of the PIN pad. */
static void pad_press(int k)
{
    if (k == 9) {
        if (ls.pwlen)
            ls.pw[--ls.pwlen] = '\0';
    } else if (k == 11) {
        kprintf("lock: PIN entered\n");
        try_password();
    } else {
        pin_add(pad_label(k)[0]);
    }
}

/* A keyboard (the VM's): digits, or any character of a password set elsewhere. */
static void prompt_key(const struct key_event *k)
{
    int caps = (k->mods & MOD_CAPSLOCK) != 0, redraw = caps != ls.caps;
    ls.caps = caps;
    if (k->key == KEY_ENTER) {
        try_password();
        if (ls.active)
            draw_rect(middle_rect());
        return;
    }
    if (k->key == KEY_BACKSPACE && ls.pwlen) {
        ls.pw[--ls.pwlen] = '\0';
        redraw = 1;
    } else if (k->ascii >= ' ' && k->ascii <= '~' && !(k->mods & MOD_CTRL)) {
        pin_add((char)k->ascii);
        redraw = 1;
    }
    if (redraw)
        draw_rect(middle_rect());
}

/* The phone's keys: Power held opens the power menu; Back closes it, or
 * leaves the PIN pad for the clock. Home and a short Power do nothing yet
 * (the screen can't go off in the VM). */
static void phone_key(const struct key_event *k)
{
    if (k->key == KEY_PHONE_POWER) {
        if (k->pressed && !ls.power_down_ms) {
            ls.power_down_ms = uptime_ms() | 1;
        } else if (!k->pressed) {
            if (ls.power_down_ms && !ls.power_held) {
                if (machine_screen_on()) {  /* a short press: the screen off, the lock stays */
                    kprintf("lock: screen off (Power)\n");
                    if (ls.menu_open)
                        set_menu(0);
                    show(V_CLOCK);
                    machine_screen(0);
                } else {                    /* ... and the next one: on, still locked */
                    kprintf("lock: screen on (Power), still locked\n");
                    machine_screen(1);
                }
            }
            ls.power_down_ms = 0;
            ls.power_held = 0;
        }
        return;
    }
    if (!k->pressed || k->key != KEY_PHONE_BACK)
        return;
    if (ls.menu_open)
        set_menu(0);
    else if (ls.view == V_PROMPT)
        show(V_CLOCK);
}

void lockscreen_key(const struct key_event *k)
{
    if (!ls.active)
        return;
    if (k->key & KEY_PHONE) {
        if (uptime_ms() - ls.locked_at >= QUIET_MS)
            phone_key(k);
        return;
    }
    if (!k->pressed)
        return;
    if (k->key == KEY_CAPSLOCK && ls.view == V_PROMPT) {
        ls.caps = !(k->mods & MOD_CAPSLOCK);    /* the state after this press */
        draw_rect(middle_rect());
        return;
    }
    if (is_modifier(k->key) || uptime_ms() - ls.locked_at < QUIET_MS)
        return;
    ls.last_input = uptime_ms();
    if (ls.menu_open) {
        set_menu(0);
        return;
    }
    switch (ls.view) {
    case V_CLOCK:
        if (k->key == KEY_ESC)
            return;
        wake();                             /* a keyboard: as a swipe up */
        if (ls.active && ls.view == V_PROMPT && k->ascii > ' ' && k->ascii <= '~')
            prompt_key(k);                  /* typing straight away: it's the PIN */
        return;
    case V_PROMPT:
        if (k->key == KEY_ESC)
            show(V_CLOCK);
        else
            prompt_key(k);
        return;
    case V_PICK:
        if (k->key == KEY_LEFT && ls.pick > 0)
            ls.pick--;
        else if (k->key == KEY_RIGHT && ls.pick + 1 < users_count())
            ls.pick++;
        else if (k->key == KEY_ENTER || k->key == KEY_SPACE) {
            choose(ls.pick);
            return;
        } else
            return;
        draw_rect(middle_rect());
        return;
    }
}

void lockscreen_pointer(int x, int y, uint8_t buttons)
{
    if (!ls.active)
        return;
    int down = (buttons & MOUSE_LEFT) && !(ls.buttons & MOUSE_LEFT);
    int up = !(buttons & MOUSE_LEFT) && (ls.buttons & MOUSE_LEFT);
    ls.buttons = buttons;
    if (down && uptime_ms() - ls.locked_at < QUIET_MS)
        return;
    if (down) {
        ls.last_input = uptime_ms();
        int h = hit_at(x, y);
        if (ls.menu_open) {
            if (h == H_RESTART) {
                kprintf("lock: restart\n");
                cursor_hide();
                power_restart();
            }
            if (h == H_SHUTDOWN && power_can_off()) {
                kprintf("lock: shut down\n");
                cursor_hide();
                power_off();
            }
            set_menu(0);                    /* a tap away from it closes it */
            return;
        }
        if (ls.view == V_CLOCK) {
            ls.swiping = 1;
            ls.y0 = y;
        } else if (ls.view == V_PROMPT && h >= H_PAD && h < H_PAD + PAD_KEYS) {
            ls.pad_held = h - H_PAD;
            pad_press(ls.pad_held);
            if (ls.active)
                draw_all();
        } else if (h >= H_AVATAR) {
            choose(h - H_AVATAR);
        }
        return;
    }
    if (ls.swiping && (buttons & MOUSE_LEFT)) {
        int lift = y < ls.y0 ? y - ls.y0 : 0;
        if (lift != ls.lift) {
            ls.lift = lift;
            draw_all();
        }
        return;
    }
    if (!up)
        return;
    if (ls.swiping) {
        ls.swiping = 0;
        ls.last_input = uptime_ms();
        if (-ls.lift >= SWIPE_UNLOCK) {
            kprintf("lock: swiped up\n");
            wake();
            return;
        }
        if (!ls.lift)
            kprintf("lock: swipe up to unlock\n");
        ls.lift = 0;                        /* not far enough: back down */
        draw_all();
    } else if (ls.pad_held >= 0) {
        ls.pad_held = -1;
        draw_all();
    }
}

/* --- its choices and its picture ---------------------------------------------------------- */

const struct lock_settings *lockscreen_settings(void)
{
    return &ls.set;
}

static void clock_choices(const struct lock_settings *s)
{
    ls.set.clock_font = s->clock_font >= 0 && s->clock_font < K86_CLOCK_FONTS ? s->clock_font : 0;
    ls.set.clock_style = s->clock_style >= 0 && s->clock_style <= 3 ? s->clock_style : 0;
    ls.set.clock_opacity = s->clock_opacity < 20 || s->clock_opacity > 100 ? 100 : s->clock_opacity;
    ls.set.clock_stretch = s->clock_stretch < 100 || s->clock_stretch > 250 ? 100 : s->clock_stretch;
    ls.set.clock_x = s->clock_x < 0 || s->clock_x > 100 ? 50 : s->clock_x;
    ls.set.clock_y = s->clock_y < 0 || s->clock_y > 100 ? -1 : s->clock_y;
}

int lockscreen_preview(const struct lock_settings *s, uint32_t *out, int w, int h)
{
    static struct gfx_surface shot;
    static char drawn[32];
    if (!shot.px && screen_buffer(&shot))
        return -ENOMEM;
    if (!ls.bg.px && screen_buffer(&ls.bg))
        return -ENOMEM;
    struct lock_settings keep = ls.set;
    if (!ls.active && strcmp(drawn, s->wallpaper)) {    /* only once: this takes a while */
        memcpy(ls.set.wallpaper, s->wallpaper, sizeof(ls.set.wallpaper));
        make_background();
        clock_gen++;
        memcpy(drawn, s->wallpaper, sizeof(drawn));
    }
    clock_choices(s);
    struct rtc_time was = ls.shown;
    if (!ls.shown.year && read_time(&ls.shown))
        ls.shown = (struct rtc_time){ .year = 2026, .month = 11, .day = 7, .hour = 12, .minute = 34 };
    memcpy(shot.px, ls.bg.px, (size_t)shot.w * (size_t)shot.h * 4);
    draw_clock(&shot, 0, 0);
    for (int y = 0; y < h; y++) {
        const uint32_t *row = shot.px + (size_t)(y * shot.h / h) * (size_t)shot.stride;
        for (int x = 0; x < w; x++)
            out[y * w + x] = row[x * shot.w / w];
    }
    ls.shown = was;
    ls.set = keep;
    return 0;
}

void lockscreen_configure(const struct lock_settings *s)
{
    /* The name isn't checked here (at boot the ramdisk isn't read yet):
     * make_background() shows the desktop's for one it can't draw. */
    ls.set.clock24 = !!s->clock24;
    ls.set.on_wake = !!s->on_wake;
    clock_choices(s);
    int n = 0;
    for (; s->wallpaper[n] && n < (int)sizeof(ls.set.wallpaper) - 1; n++)
        ls.set.wallpaper[n] = s->wallpaper[n];
    ls.set.wallpaper[n] = '\0';
    if (!n)
        memcpy(ls.set.wallpaper, "desktop", 8);
    if (ls.active) {
        make_background();
        draw_all();
    }
}

int lockscreen_set_picture(const uint32_t *px, int w, int h, int stride)
{
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || stride < w)
        return -EINVAL;
    if (screen_buffer(&ls.pic))
        return -ENOMEM;
    wallpaper_scale_cover(px, w, h, stride, &ls.pic);
    memcpy(ls.set.wallpaper, LOCK_PICTURE, sizeof(LOCK_PICTURE));
    kprintf("lock: its own picture, %dx%d\n", w, h);
    return 0;
}

int lockscreen_load_picture(const void *data, uint32_t size)
{
    if (screen_buffer(&ls.pic))
        return -ENOMEM;
    return wallpaper_render_packed(data, size, &ls.pic);
}

int lockscreen_has_picture(void)
{
    return ls.pic.px != 0;
}

int lockscreen_pack_picture(uintptr_t *phys, uint32_t *bytes, uint32_t *len)
{
    if (!ls.pic.px)
        return -ENOENT;
    return wallpaper_pack_surface(&ls.pic, phys, bytes, len);
}

/* --- the home screen's clock ----------------------------------------------------------------
 * The same clock as the lock screen's, with choices of its own (font, style, opacity, stretch,
 * size, place). It's drawn by the same code: home_enter() swaps the lock screen's clock state
 * for the home's, and home_leave() swaps it back. */

static struct home_clock home_set = { 1, 0, 0, 100, 100, 100, 500, -1, 1 };
static struct clkstate clk_home = { .font = -1, .size = 100 };
static struct glassstate glass_home = { .gen = -1 };
static struct glassstate glass_keep;
static struct gfx_surface home_bg;      /* the desktop's wallpaper, for the glass style */
static int home_bg_stale = 1;
static struct lock_settings home_keep;

static void home_enter(void)
{
    home_keep = ls.set;
    ls.set.clock_font = home_set.font;
    ls.set.clock_style = home_set.style;
    ls.set.clock_opacity = home_set.opacity;
    ls.set.clock_stretch = home_set.stretch;
    ls.set.clock_x = home_set.x;
    ls.set.clock_y = home_set.y;
    cur_size = home_set.size;
    home_mode = 1;
    clk_lock_keep = clk;
    clk = clk_home;
    glass_keep = glass;
    glass = glass_home;
    if (home_set.style == 3 && (home_bg.px || !screen_buffer(&home_bg))) {
        if (home_bg_stale) {
            wallpaper_draw(&home_bg, 0, 0, 0);
            home_bg_stale = 0;
            clock_gen++;
        }
        glass_src = &home_bg;
    } else {
        glass_src = 0;
    }
}

static void home_leave(void)
{
    clk_home = clk;
    clk = clk_lock_keep;
    glass_home = glass;
    glass = glass_keep;
    ls.set = home_keep;
    cur_size = 100;
    home_mode = 0;
    glass_src = 0;
}

const struct home_clock *lockscreen_home_clock(void)
{
    return &home_set;
}

static int clampi(int v, int lo, int hi, int dflt)
{
    return v < lo || v > hi ? dflt : v;
}

void lockscreen_set_home_clock(const struct home_clock *h)
{
    home_set.show = !!h->show;
    home_set.font = clampi(h->font, 0, K86_CLOCK_FONTS - 1, 0);
    home_set.style = clampi(h->style, 0, 3, 0);
    home_set.opacity = clampi(h->opacity, 20, 100, 100);
    home_set.stretch = clampi(h->stretch, 100, 250, 100);
    home_set.size = clampi(h->size, 30, 250, 100);
    home_set.x = clampi(h->x, 0, 1000, 500);
    home_set.y = clampi(h->y, 0, 1000, -1);
    home_set.date = h->date != 0;
}

void lockscreen_home_changed(void)
{
    home_bg_stale = 1;                  /* the wallpaper changed: the glass blurs it again */
    clock_gen++;
}

/* The digits and the date under them (over them, for glass), where they are now. */
static void home_boxes(const struct rtc_time *t, struct gfx_rect *mask, struct gfx_rect *box)
{
    char hm[8], ampm[4], date[40];
    time_text(t, hm, ampm);
    date_text(t, date);
    int left, top;
    clock_mask(hm);
    clock_place(&left, &top);
    int dw = text_width(date, TEXT_BOLD), dh = text_height(TEXT_BOLD);
    if (!home_set.date) {               /* no date: the box is the digits */
        if (mask)
            *mask = (struct gfx_rect){ left, top, clk.w, clk.h };
        if (box)
            *box = (struct gfx_rect){ left, top, clk.w, clk.h };
        return;
    }
    int dx = left + clk.w / 2 - dw / 2;
    dx = dx < 8 ? 8 : dx + dw > S()->w - 8 ? S()->w - 8 - dw : dx;
    int dy = ls.set.clock_style == 3 ? top - 12 - dh : top + clk.h + 12;
    struct gfx_rect m = { left, top, clk.w, clk.h }, d = { dx, dy, dw, dh };
    if (mask)
        *mask = m;
    if (box)
        *box = gfx_rect_union(m, d);
}

void lockscreen_home_clock_draw(struct gfx_surface *s, int ox, int oy, const struct rtc_time *t,
                                struct gfx_rect *mask, struct gfx_rect *box)
{
    struct rtc_time was = ls.shown;
    ls.shown = *t;
    home_enter();
    if (s)
        draw_clock(s, ox, oy);
    home_boxes(t, mask, box);
    home_leave();
    ls.shown = was;
}
