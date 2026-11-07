/* LiteKern X — the lock screen (see kernel/lockscreen.h). */
#include "kernel/lockscreen.h"
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
#define NOTE_H      88
#define QUIET_MS    400                 /* input just after locking is ignored */
#define SLIDE_MS    320                 /* the curtain sliding down (locking) or up (unlocking) */
#define SHADOW      28                  /* the soft shadow along its bottom edge */
#define MAX_FRAME_MS 60                 /* slower than this: go straight to the end */
#define AVATAR_R    36                  /* the prompt's avatar */
#define PICK_R      38                  /* the picker's */
#define PICK_GAP    150                 /* between the picker's avatars */
#define FIELD_W     280
#define FIELD_H     40
#define PROMPT_IDLE_MS 30000            /* the prompt untouched this long: back to the clock */
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
} ls = { .set = { "desktop", 1, 0 }, .pad_held = -1 };

/* H_PAD + k: the PIN pad's k-th key; H_AVATAR + i: the picker's i-th. */
enum { H_NONE, H_RESTART, H_SHUTDOWN, H_PAD, H_AVATAR = H_PAD + PAD_KEYS };

static struct gfx_surface *S(void) { return screen_surface(); }

/* --- the clock's face (fonts/clock.lkxf, tools/font2c.py --bin) --------------------------- */

#define CLOCK_GLYPHS (95 + TEXT_EXTRA)
static struct text_glyph clock_glyphs[CLOCK_GLYPHS];
static struct text_face clock_face;

static uint32_t le(const uint8_t *p, int n)
{
    uint32_t v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

/* Read it from the ramdisk the first time; checked, so a damaged file only
 * means a smaller clock (the heading style). 0 or a negative error. */
static int load_clock_face(void)
{
    if (clock_face.glyphs)
        return 0;
    const uint8_t *d;
    uint32_t size;
    if (ramdisk_find("fonts/clock.lkxf", (const void **)&d, &size))
        return -ENOENT;
    if (size < 12 || memcmp(d, "LKXF", 4) || le(d + 10, 2) != CLOCK_GLYPHS || size < 12 + 10u * CLOCK_GLYPHS)
        return -EINVAL;
    const uint8_t *t = d + 12, *cov = t + 10 * CLOCK_GLYPHS;
    uint32_t cov_len = size - (uint32_t)(cov - d);
    for (int i = 0; i < CLOCK_GLYPHS; i++, t += 10) {
        struct text_glyph *g = &clock_glyphs[i];
        *g = (struct text_glyph){ t[0], t[1], (int8_t)t[2], (int8_t)t[3], (uint16_t)le(t + 4, 2), le(t + 6, 4) };
        if (g->off > cov_len || (uint32_t)g->w * g->h > cov_len - g->off)
            return -EINVAL;
    }
    clock_face = (struct text_face){ "clock", (int)le(d + 4, 2), (int)le(d + 6, 2), (int)le(d + 8, 2),
                                     clock_glyphs, cov };
    return 0;
}

/* The clock's face, or the heading style's if it couldn't be read. */
static const struct text_face *clock_font(void)
{
    return clock_face.glyphs ? &clock_face : &text_faces[TEXT_HEADING];
}

const struct text_face *lockscreen_clock_face(void)
{
    load_clock_face();
    return clock_font();
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

static struct gfx_rect clock_rect(void)
{
    const struct text_face *f = clock_font();
    int h = text_height(TEXT_BOLD) + 12 + f->ascent + f->descent;
    return (struct gfx_rect){ 0, clock_y(), S()->w, h };
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
    const struct text_face *f = clock_font();
    int y = clock_y(), tw = text_width_face(hm, f);
    text_draw_face(s, (S()->w - tw) / 2 - ox, y - oy, hm, f, WHITE);
    if (ampm[0])
        text_draw(s, (S()->w + tw) / 2 + 12 - ox, y + f->ascent - text_height(TEXT_HEADING) - oy, ampm, TEXT_HEADING, WHITE);
    centred(s, ox, oy, y + f->ascent + f->descent + 12, date, TEXT_BOLD, WHITE);
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
    int n = ls.pwlen > 16 ? 16 : ls.pwlen, w = n * 40 - 16, x = (S()->w - w) / 2 + ls.shake;
    for (int i = 0; i < n; i++)
        gfx_fill_circle(s, x + 12 + i * 40 - ox, dots_cy() - oy, 12, WHITE);
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

/* Notifications under the clock: their titles only, as the content may be private. */
static void draw_notes(struct gfx_surface *s, int ox, int oy)
{
    struct gfx_rect c = clock_rect();
    int n = desktop_note_count(), y = c.y + c.h + 36, x = 48, w = S()->w - 96;
    for (int i = 0; i < n && i < NOTES_SHOWN; i++, y += NOTE_H + 12) {
        gfx_blend_round_rect(s, x - ox, y - oy, w, NOTE_H, 24, WHITE, 40);
        text_draw_fit(s, x + 28 - ox, y + (NOTE_H - text_height(TEXT_BOLD)) / 2 - oy, desktop_note_title(i), w - 56,
                      TEXT_BOLD, WHITE);
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
    int fe = load_clock_face();
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
        slide(0);
    }
    ls.lift = ls.swiping = 0;
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

void lockscreen_configure(const struct lock_settings *s)
{
    /* The name isn't checked here (at boot the ramdisk isn't read yet):
     * make_background() shows the desktop's for one it can't draw. */
    ls.set.clock24 = !!s->clock24;
    ls.set.on_wake = !!s->on_wake;
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
