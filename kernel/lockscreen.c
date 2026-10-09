/* LiteKern X — the lock screen (see kernel/lockscreen.h). */
#include "kernel/lockscreen.h"
#include "kernel/cursor.h"
#include "kernel/desktop.h"
#include "kernel/driver.h"
#include "kernel/errno.h"
#include "kernel/pmm.h"
#include "kernel/power.h"
#include "kernel/printk.h"
#include "kernel/ramdisk.h"
#include "kernel/rtc.h"
#include "kernel/screen.h"
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/timing.h"
#include "kernel/wallpaper.h"

#define WHITE       0xffffffu
#define POPOVER     0x36363au           /* the shell's popovers (kernel/desktop.c) */
#define DIM         96                  /* how much the wallpaper is darkened */
#define BUTTON_W    40
#define BUTTON_H    24
#define MENU_W      180
#define ITEM_H      34
#define MENU_PAD    6
#define QUIET_MS    400                 /* input just after locking is ignored */

static struct {
    int active;
    struct gfx_surface bg;              /* the dimmed wallpaper, made when it locks */
    struct gfx_surface pic;             /* its own picture (LOCK_PICTURE), if any */
    int menu_open, hover;               /* hover: 0 none, 1 the power button, 2+ menu items */
    uint8_t buttons;
    uint32_t locked_at, last_tick;
    struct rtc_time shown;              /* the time on screen */
    struct lock_settings set;
} ls = { .set = { "desktop", 1, 0 } };

enum { H_NONE, H_POWER, H_RESTART, H_SHUTDOWN };

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
    uint32_t phys = pmm_alloc_contiguous((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
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

/* --- layout and drawing ------------------------------------------------------------------- */

static struct gfx_rect power_rect(void)
{
    return (struct gfx_rect){ S()->w - 8 - BUTTON_W, 3, BUTTON_W, BUTTON_H };
}

static struct gfx_rect menu_rect(void)
{
    return (struct gfx_rect){ S()->w - 8 - MENU_W, BUTTON_H + 8, MENU_W, 2 * ITEM_H + 2 * MENU_PAD };
}

static struct gfx_rect item_rect(int k)
{
    struct gfx_rect m = menu_rect();
    return (struct gfx_rect){ m.x + MENU_PAD, m.y + MENU_PAD + k * ITEM_H, MENU_W - 2 * MENU_PAD, ITEM_H };
}

static int clock_y(void)
{
    return S()->h * 22 / 100;
}

static struct gfx_rect clock_rect(void)
{
    const struct text_face *f = clock_font();
    int h = f->ascent + f->descent + 16 + text_height(TEXT_HEADING);
    return (struct gfx_rect){ 0, clock_y(), S()->w, h };
}

static int hit_at(int x, int y)
{
    if (ls.menu_open) {
        for (int k = 0; k < 2; k++)
            if (inside(item_rect(k), x, y))
                return H_RESTART + k;
    }
    return inside(power_rect(), x, y) ? H_POWER : H_NONE;
}

static void draw_power_icon(struct gfx_surface *s, int cx, int cy, uint32_t c, uint32_t bg)
{
    gfx_fill_circle(s, cx, cy + 1, 7, c);
    gfx_fill_circle(s, cx, cy + 1, 5, bg);
    gfx_fill_rect(s, cx - 3, cy - 7, 7, 5, bg);
    gfx_fill_rect(s, cx - 1, cy - 8, 2, 8, c);
}

/* Everything, into s, which shows the screen from (ox, oy). */
static void draw_into(struct gfx_surface *s, int ox, int oy)
{
    gfx_blit(s, 0, 0, &ls.bg, ox, oy, s->w, s->h);
    char hm[8], ampm[4], date[40];
    if (ls.shown.year) {
        time_text(&ls.shown, hm, ampm);
        date_text(&ls.shown, date);
        const struct text_face *f = clock_font();
        int tw = text_width_face(hm, f), aw = ampm[0] ? text_width(ampm, TEXT_HEADING) + 10 : 0;
        int x = (S()->w - tw - aw) / 2, y = clock_y();
        text_draw_face(s, x - ox, y - oy, hm, f, WHITE);
        if (ampm[0])
            text_draw(s, x + tw + 10 - ox, y + f->ascent - text_height(TEXT_HEADING) - oy, ampm, TEXT_HEADING,
                      WHITE);
        int dw = text_width(date, TEXT_HEADING);
        text_draw(s, (S()->w - dw) / 2 - ox, y + f->ascent + f->descent + 16 - oy, date, TEXT_HEADING, WHITE);
    }
    const char *hint = "Click or press a key to unlock";
    int hw = text_width(hint, TEXT_BODY);
    text_draw(s, (S()->w - hw) / 2 - ox, S()->h - 64 - oy, hint, TEXT_BODY, gfx_mix(WHITE, 0, 190));

    struct gfx_rect p = power_rect();
    int cx = p.x + p.w / 2 - ox, cy = p.y + p.h / 2 - oy;
    if (ls.hover == H_POWER || ls.menu_open)
        gfx_blend_round_rect(s, p.x - ox, p.y - oy, p.w, p.h, p.h / 2, WHITE, 40);
    if (cx >= 0 && cy >= 0 && cx < s->w && cy < s->h)     /* the icon's hole shows what's behind */
        draw_power_icon(s, cx, cy, WHITE, s->px[cy * s->stride + cx]);
    if (ls.menu_open) {
        struct gfx_rect m = menu_rect();
        gfx_fill_round_rect(s, m.x - ox, m.y - oy, m.w, m.h, 12, POPOVER);
        for (int k = 0; k < 2; k++) {
            struct gfx_rect r = item_rect(k);
            int disabled = k == 1 && !power_can_off();
            if (!disabled && ls.hover == H_RESTART + k)
                gfx_fill_round_rect(s, r.x - ox, r.y - oy, r.w, r.h, 6, gfx_mix(WHITE, POPOVER, 38));
            text_draw(s, r.x + 12 - ox, r.y + (r.h - text_height(TEXT_BODY)) / 2 - oy, k ? "Shut Down" : "Restart",
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

static struct gfx_rect top_right(void)
{
    struct gfx_rect m = menu_rect();
    return gfx_rect_union(power_rect(), (struct gfx_rect){ m.x - 4, m.y - 4, m.w + 8, m.h + 8 });
}

/* --- locking and unlocking --------------------------------------------------------------- */

void lockscreen_lock(void)
{
    if (ls.active || !screen_ready() || !desktop_started())
        return;
    if (screen_buffer(&ls.bg)) {
        kprintf("lock: out of memory; not locked\n");
        return;
    }
    int fe = load_clock_face();
    if (fe)
        kprintf("lock: the clock's font couldn't be read (%d); a smaller clock\n", fe);
    desktop_cover(1);
    make_background();
    ls.active = 1;
    ls.menu_open = ls.hover = 0;
    ls.locked_at = ls.last_tick = uptime_ms();
    if (read_time(&ls.shown))
        ls.shown.year = 0;
    draw_all();
    kprintf("lock: locked (wallpaper %s)\n", ls.set.wallpaper);
}

int lockscreen_active(void)
{
    return ls.active;
}

static void unlock(void)
{
    ls.active = 0;
    kprintf("lock: unlocked\n");
    desktop_cover(0);                       /* the desktop draws itself again */
}

void lockscreen_redraw(void)
{
    if (ls.active)
        draw_all();
}

void lockscreen_tick(void)
{
    if (!ls.active || uptime_ms() - ls.last_tick < 1000)
        return;
    ls.last_tick = uptime_ms();
    struct rtc_time t;
    if (read_time(&t) || (t.minute == ls.shown.minute && t.hour == ls.shown.hour && t.day == ls.shown.day))
        return;
    ls.shown = t;
    draw_rect(clock_rect());
}

static int is_modifier(uint16_t key)
{
    return key == KEY_LSHIFT || key == KEY_RSHIFT || key == KEY_LCTRL || key == KEY_RCTRL ||
           key == KEY_LALT || key == KEY_RALT || key == KEY_LSUPER || key == KEY_RSUPER ||
           key == KEY_CAPSLOCK;
}

void lockscreen_key(const struct key_event *k)
{
    if (!ls.active || !k->pressed || is_modifier(k->key) || uptime_ms() - ls.locked_at < QUIET_MS)
        return;
    if (ls.menu_open) {
        ls.menu_open = 0;
        draw_rect(top_right());
        if (k->key == KEY_ESC)
            return;
    }
    unlock();
}

void lockscreen_pointer(int x, int y, uint8_t buttons)
{
    if (!ls.active)
        return;
    int down = (buttons & MOUSE_LEFT) && !(ls.buttons & MOUSE_LEFT);
    ls.buttons = buttons;
    int h = hit_at(x, y);
    if (h != ls.hover) {
        ls.hover = h;
        draw_rect(top_right());
    }
    if (!down || uptime_ms() - ls.locked_at < QUIET_MS)
        return;
    switch (h) {
    case H_POWER:
        ls.menu_open = !ls.menu_open;
        draw_rect(top_right());
        return;
    case H_RESTART:
        kprintf("lock: restart\n");
        cursor_hide();
        power_restart();
    case H_SHUTDOWN:
        if (!power_can_off())
            return;
        kprintf("lock: shut down\n");
        cursor_hide();
        power_off();
    default:
        if (ls.menu_open) {                 /* a click away from the menu closes it */
            ls.menu_open = 0;
            draw_rect(top_right());
            return;
        }
        unlock();
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

int lockscreen_pack_picture(uint32_t *phys, uint32_t *bytes, uint32_t *len)
{
    if (!ls.pic.px)
        return -ENOENT;
    return wallpaper_pack_surface(&ls.pic, phys, bytes, len);
}
