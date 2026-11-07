/* LiteKern Go: Settings' lock screen clock editor. A live picture of the lock screen
 * (the shell draws it: SYS_LOCK_PREVIEW), and under it the clock's font, style, opacity
 * and stretch. Drag the clock on the picture to move it. Changes are saved as they're
 * made: when a finger lifts, for the sliders and a drag. Logged for the tests. */
#include "apps/settings/clockedit.h"
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"

#define PW 225                          /* the picture: a third of the screen's 720x1280, near */
#define PH 400
#define SIDE 24
#define GAP 12
#define CHIP_H 64
#define SLIDER_H 12
#define KNOB_R 22
#define ROW_H 76
#define BACK_ID 100
#define PREVIEW_EVERY_MS 70

static const char *const fonts[K86_CLOCK_FONTS] = { "Bold", "Light", "Thin", "Condensed", "Mono", "Serif" };
static const char *const styles[3] = { "Standard", "Stacked", "Outline" };

static struct gfx_surface *cv;
static struct k86_lock *l;
static int active, held;                /* held: 0 none, 1 the picture, 2 opacity, 3 stretch */
static int moved;                       /* a finger changed something, not saved yet */
static uint32_t last_preview;
static uint32_t picture[PW * PH];

static const struct theme *T(void) { return theme_get(); }

static struct gfx_rect picture_rect(void)
{
    return (struct gfx_rect){ (cv->w - PW) / 2, 12, PW, PH };
}

static struct gfx_rect font_rect(int i)
{
    int w = (cv->w - 2 * SIDE - 2 * GAP) / 3, y = 12 + PH + 16;
    return (struct gfx_rect){ SIDE + i % 3 * (w + GAP), y + i / 3 * (CHIP_H + GAP), w, CHIP_H };
}

static struct gfx_rect style_rect(int i)
{
    int w = (cv->w - 2 * SIDE - 2 * GAP) / 3, y = 12 + PH + 16 + 2 * (CHIP_H + GAP) + 8;
    return (struct gfx_rect){ SIDE + i * (w + GAP), y, w, CHIP_H };
}

static int slider_y(int k)
{
    return 12 + PH + 16 + 3 * (CHIP_H + GAP) + 16 + k * ROW_H;
}

static struct gfx_rect slider_rect(int k)
{
    return (struct gfx_rect){ 200, slider_y(k) + (ROW_H - SLIDER_H) / 2, cv->w - 200 - SIDE - KNOB_R, SLIDER_H };
}

static struct gfx_rect reset_rect(void)
{
    return (struct gfx_rect){ SIDE, slider_y(2) + 8, cv->w - 2 * SIDE, CHIP_H };
}

static int inside(struct gfx_rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static void chip(struct gfx_rect r, const char *label, int on)
{
    const struct theme *t = T();
    gfx_fill_round_rect(cv, r.x, r.y, r.w, r.h, r.h / 2, on ? t->accent_bg : t->button_bg);
    text_draw(cv, r.x + (r.w - text_width(label, TEXT_BODY)) / 2, r.y + (r.h - text_height(TEXT_BODY)) / 2, label,
              TEXT_BODY, on ? t->accent_fg : t->fg);
}

static void slider(int k, const char *label, int value, int low, int high)
{
    const struct theme *t = T();
    struct gfx_rect r = slider_rect(k);
    int kx = r.x + r.w * (value - low) / (high - low);
    text_draw(cv, SIDE, slider_y(k) + (ROW_H - text_height(TEXT_BODY)) / 2, label, TEXT_BODY, t->fg);
    gfx_fill_round_rect(cv, r.x, r.y, r.w, r.h, r.h / 2, t->button_bg);
    gfx_fill_round_rect(cv, r.x, r.y, kx - r.x + r.h / 2, r.h, r.h / 2, t->accent_bg);
    gfx_fill_circle(cv, kx, r.y + r.h / 2, KNOB_R, held == k + 2 ? t->accent_bg : t->fg);
}

static void draw(void)
{
    const struct theme *t = T();
    struct gfx_rect p = picture_rect(), res = reset_rect();
    struct gfx_surface pic = { picture, PW, PH, PW };
    gfx_fill_rect(cv, 0, 0, cv->w, cv->h, t->window_bg);
    gfx_blit(cv, p.x, p.y, &pic, 0, 0, PW, PH);
    gfx_rect_outline(cv, p.x - 1, p.y - 1, PW + 2, PH + 2, t->border);
    for (int i = 0; i < K86_CLOCK_FONTS; i++)
        chip(font_rect(i), fonts[i], l->clock_font == i);
    for (int i = 0; i < 3; i++)
        chip(style_rect(i), styles[i], l->clock_style == i);
    slider(0, "Opacity", l->clock_opacity, 20, 100);
    slider(1, "Stretch", l->clock_stretch, 100, 250);
    chip(res, "Reset", 0);
    k86_present(0, 0, cv->w, cv->h);
}

static void refresh(void)
{
    last_preview = k86_uptime_ms();
    k86_lock_preview(l, picture, PW, PH);
    draw();
}

static void save(void)
{
    moved = 0;
    k86_lock_settings(l, 1);
    k86_logf("settings: clock font %d style %d opacity %d stretch %d at %d,%d", l->clock_font, l->clock_style,
             l->clock_opacity, l->clock_stretch, l->clock_x, l->clock_y);
}

void clockedit_open(struct gfx_surface *canvas, struct k86_lock *lock)
{
    struct k86_header h;
    cv = canvas;
    l = lock;
    active = 1;
    held = 0;
    memset(&h, 0, sizeof(h));
    memcpy(h.title, "Clock", 6);
    h.nbuttons = 1;
    h.buttons[0].side = 0;
    h.buttons[0].icon = 1;
    h.buttons[0].id = BACK_ID;
    k86_header(&h);
    k86_log("settings: clock editor");
    refresh();
}

int clockedit_active(void)
{
    return active;
}

static void close_it(void)
{
    if (moved)
        save();
    active = 0;
}

static int percent(int v, int low, int size)
{
    int p = (v - low) * 100 / size;
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

static void pointer(const struct k86_event *ev)
{
    struct wg_pointer p = wg_pointer_make(ev->x, ev->y, ev->buttons, ev->changed, ev->time_ms);
    struct gfx_rect pic = picture_rect();
    if (p.down) {
        held = inside(pic, p.x, p.y) ? 1 : 0;
        for (int k = 0; k < 2; k++) {
            struct gfx_rect r = slider_rect(k);
            if (p.y >= slider_y(k) && p.y < slider_y(k) + ROW_H && p.x >= r.x - KNOB_R)
                held = k + 2;
        }
    }
    if (held && p.held) {
        struct gfx_rect s0 = slider_rect(0);
        if (held == 1) {
            l->clock_x = percent(p.x, pic.x, pic.w);
            l->clock_y = percent(p.y, pic.y, pic.h);
        } else if (held == 2) {
            l->clock_opacity = 20 + percent(p.x, s0.x, s0.w) * 80 / 100;
        } else {
            l->clock_stretch = 100 + percent(p.x, s0.x, s0.w) * 150 / 100;
        }
        moved = 1;
        if (k86_uptime_ms() - last_preview >= PREVIEW_EVERY_MS)
            refresh();
        return;
    }
    if (held) {                         /* let go */
        held = 0;
        save();
        refresh();
        return;
    }
    if (!p.up)
        return;
    for (int i = 0; i < K86_CLOCK_FONTS; i++)
        if (inside(font_rect(i), p.x, p.y)) {
            l->clock_font = i;
            save();
            refresh();
            return;
        }
    for (int i = 0; i < 3; i++)
        if (inside(style_rect(i), p.x, p.y)) {
            l->clock_style = i;
            save();
            refresh();
            return;
        }
    if (inside(reset_rect(), p.x, p.y)) {
        l->clock_font = l->clock_style = 0;
        l->clock_opacity = l->clock_stretch = 100;
        l->clock_x = 50;
        l->clock_y = -1;
        save();
        refresh();
    }
}

int clockedit_event(const struct k86_event *ev)
{
    switch (ev->type) {
    case K86_EVENT_POINTER:
        pointer(ev);
        break;
    case K86_EVENT_HEADER:
        if (ev->id == BACK_ID) {
            close_it();
            return 1;
        }
        break;
    case K86_EVENT_KEY:
        if (ev->key.pressed && ev->key.key == KEY_ESC) {
            close_it();
            return 1;
        }
        break;
    case K86_EVENT_RESIZE:
    case K86_EVENT_THEME:
        draw();
        break;
    default:
        break;
    }
    return 0;
}
