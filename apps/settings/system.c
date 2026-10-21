/* LiteKern X — Settings: the System page (kernel/machine.h, the Linux port's
 * phase L4), laid out like the Network page.
 *
 * On the left the battery (how full, charging, how long left) and the
 * screen's brightness, a slider: dragged, or Left/Right in steps of 10, set
 * when let go (each change is written to the stick). On the right the time
 * zones: a click picks one, and the clock follows at once. The battery
 * refreshes every 2 s while the page shows. What the machine can't do here
 * (the kernel version: no battery or brightness driver, the EeePC's own
 * clock) is said instead. Actions are logged ("user: settings: system ...")
 * for the tests. */
#include "apps/settings/system.h"
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/timezones.h"
#include "kernel/widget.h"

static struct gfx_surface *c;
static struct k86_machine m;
static struct wg_list zones;
static struct gfx_rect slider;          /* the brightness slider's track (its knob's travel) */
static int col_x, col_w, right_x, right_w, y_battery, y_bright, y_zone;
static int dragging, drag_value = -1;   /* the slider held: the value shown until let go */
static char status[96];

#define KNOB_R 10

static void copy(char *dst, const char *src, int size)
{
    int n = 0;
    for (; src[n] && n < size - 1; n++)
        dst[n] = src[n];
    dst[n] = '\0';
}

static void append(char *dst, const char *src, int size)
{
    int n = (int)strlen(dst);
    copy(dst + n, src, size - n);
}

static void append_int(char *dst, int v, int size)
{
    char d[12];
    int n = 0;
    if (v < 0) {
        append(dst, "-", size);
        v = -v;
    }
    do
        d[n++] = (char)('0' + v % 10);
    while ((v /= 10) && n < 11);
    char r[12];
    for (int i = 0; i < n; i++)
        r[i] = d[n - 1 - i];
    r[n] = '\0';
    append(dst, r, size);
}

static void row(void *ctx, int i, struct wg_row *out)
{
    (void)ctx;
    *out = (struct wg_row){ lkx_zones[i].name, i == m.timezone ? "Chosen" : 0, WG_ICON_NONE };
}

void system_page_layout(void)
{
    int w = c->w - 2 * 48, x = 48;
    if (w > 820)
        w = 820, x = (c->w - w) / 2;
    col_x = x;
    col_w = (w - 40) / 2;
    right_x = x + col_w + 40;
    right_w = w - col_w - 40;
    int lh = text_height(TEXT_BOLD), bh = text_height(TEXT_BODY);
    y_battery = 20 + text_height(TEXT_HEADING) + 16;
    y_bright = y_battery + lh + 8 + 2 * (bh + 4) + 40;
    slider = (struct gfx_rect){ col_x + KNOB_R, y_bright + lh + 8 + bh + 16, col_w - 2 * KNOB_R, 6 };
    y_zone = y_battery;
    int list_y = y_zone + lh + 8 + bh + 10;
    zones.r = (struct gfx_rect){ right_x, list_y, right_w, c->h - 40 - list_y };
}

static int shown_brightness(void)
{
    return drag_value >= 0 ? drag_value : m.brightness;
}

static void battery_lines(char *a, char *b, int size)
{
    a[0] = b[0] = '\0';
    if (m.battery < 0) {
        copy(a, "No battery found.", size);
        return;
    }
    append_int(a, m.battery, size);
    append(a, m.charging == K86_BATTERY_CHARGING ? "%, charging" : m.charging == K86_BATTERY_FULL ? "%, on the charger"
                                                                                                   : "%, on battery", size);
    if (m.minutes >= 0) {
        copy(b, m.charging == K86_BATTERY_CHARGING ? "Full in " : "Empty in ", size);
        if (m.minutes >= 60) {
            append_int(b, m.minutes / 60, size);
            append(b, " h ", size);
        }
        append_int(b, m.minutes % 60, size);
        append(b, " min", size);
    }
}

void system_page_draw(void)
{
    const struct theme *t = theme_get();
    char a[96], b[96];
    int lh = text_height(TEXT_BOLD), bh = text_height(TEXT_BODY);
    gfx_fill_rect(c, 0, 0, c->w, c->h, t->window_bg);
    text_draw(c, col_x, 20, "System", TEXT_HEADING, t->fg);

    text_draw(c, col_x, y_battery, "Battery", TEXT_BOLD, t->fg);
    battery_lines(a, b, sizeof(a));
    text_draw_fit(c, col_x, y_battery + lh + 8, a, col_w, TEXT_BODY,
                  m.battery >= 0 && m.battery <= 10 && m.charging == K86_BATTERY_DISCHARGING ? t->destructive : t->fg_dim);
    if (b[0])
        text_draw_fit(c, col_x, y_battery + lh + 8 + bh + 4, b, col_w, TEXT_BODY, t->fg_dim);
    if (m.battery >= 0) {               /* a gauge */
        int gy = y_battery + lh + 8 + 2 * (bh + 4) + 2, gw = col_w;
        gfx_fill_round_rect(c, col_x, gy, gw, 8, 4, t->button_bg);
        int fw = gw * m.battery / 100;
        if (fw >= 8)
            gfx_fill_round_rect(c, col_x, gy, fw, 8, 4, m.battery <= 10 ? t->destructive : t->accent_bg);
    }

    text_draw(c, col_x, y_bright, "Brightness", TEXT_BOLD, t->fg);
    int v = shown_brightness();
    if (v < 0) {
        text_draw_fit(c, col_x, y_bright + lh + 8, "The screen's brightness can't be changed here.", col_w, TEXT_BODY,
                      t->fg_dim);
    } else {
        copy(a, "", sizeof(a));
        append_int(a, v, sizeof(a));
        append(a, "%  (the Fn+F5 and Fn+F6 keys work too)", sizeof(a));
        text_draw_fit(c, col_x, y_bright + lh + 8, a, col_w, TEXT_BODY, t->fg_dim);
        int kx = slider.x + slider.w * v / 100, ky = slider.y + slider.h / 2;
        gfx_fill_round_rect(c, slider.x, slider.y, slider.w, slider.h, 3, t->button_bg);
        gfx_fill_round_rect(c, slider.x, slider.y, kx - slider.x + 3, slider.h, 3, t->accent_bg);
        gfx_fill_circle(c, kx, ky, KNOB_R, dragging ? t->accent_bg : t->fg);
    }

    text_draw(c, right_x, y_zone, "Time Zone", TEXT_BOLD, t->fg);
    if (m.timezone < 0) {
        text_draw_fit(c, right_x, y_zone + lh + 8, "The clock shows the computer's own time.", right_w, TEXT_BODY,
                      t->fg_dim);
    } else {
        copy(a, "Now: ", sizeof(a));
        append(a, lkx_zones[m.timezone].name, sizeof(a));
        text_draw_fit(c, right_x, y_zone + lh + 8, a, right_w, TEXT_BODY, t->fg_dim);
        wg_list_draw(c, &zones);
    }
    if (status[0])
        text_draw_fit(c, col_x, c->h - 22, status, col_w, TEXT_SMALL, t->fg_dim);
    k86_present(0, 0, c->w, c->h);
}

static void reload(void)
{
    k86_machine(&m);
    zones.count = m.timezone >= 0 ? LKX_ZONES : 0;
}

void system_page_open(struct gfx_surface *canvas)
{
    c = canvas;
    if (!zones.row)
        zones = (struct wg_list){ .selected = -1, .hover = -1, .row = row, .last_click_row = -1 };
    reload();
    system_page_layout();
    if (m.timezone >= 0)
        wg_list_select(&zones, m.timezone);
    system_page_draw();
}

void system_page_tick(void)
{
    struct k86_machine before = m;
    reload();
    if (memcmp(&before, &m, sizeof(m)) && !dragging)
        system_page_draw();
}

static void set_brightness(int v)
{
    v = v < 5 ? 5 : v > 100 ? 100 : v;
    int err = k86_brightness_set(v);
    k86_logf("settings: system brightness %d: %s", v, err ? "refused" : "set");
    copy(status, err ? "That didn't work." : "", sizeof(status));
    reload();
    system_page_draw();
}

static void set_zone(int z)
{
    int err = k86_timezone_set(z);
    k86_logf("settings: system time zone %s: %s", lkx_zones[z].name, err ? "refused" : "set");
    copy(status, err ? "That didn't work." : "", sizeof(status));
    reload();
    system_page_draw();
}

static int slider_value(int x)
{
    int v = (x - slider.x) * 100 / (slider.w > 0 ? slider.w : 1);
    return v < 5 ? 5 : v > 100 ? 100 : v;
}

void system_page_pointer(const struct k86_event *ev)
{
    struct wg_pointer p = wg_pointer_make(ev->x, ev->y, ev->buttons, ev->changed, ev->time_ms);
    if (m.brightness >= 0) {
        int near = p.x >= slider.x - KNOB_R && p.x <= slider.x + slider.w + KNOB_R &&
                   p.y >= slider.y - 2 * KNOB_R && p.y <= slider.y + slider.h + 2 * KNOB_R;
        if (p.down && near)
            dragging = 1;
        if (dragging) {
            drag_value = slider_value(p.x);
            if (p.up || !p.held) {
                dragging = 0;
                int v = drag_value;
                drag_value = -1;
                set_brightness(v);
                return;
            }
            system_page_draw();
            return;
        }
    }
    if (m.timezone >= 0) {
        enum wg_list_result r = wg_list_pointer(&zones, &p);
        if ((r == WG_LIST_CHANGED || r == WG_LIST_ACTIVATE) && zones.selected >= 0 && zones.selected != m.timezone) {
            set_zone(zones.selected);
            return;
        }
        if (zones.dirty)
            system_page_draw();
    }
}

void system_page_scroll(const struct k86_event *ev)
{
    if (m.timezone >= 0 && wg_list_scroll(&zones, ev->x, ev->y, ev->id))
        system_page_draw();
}

void system_page_key(const struct key_event *k)
{
    if (!k->pressed)
        return;
    if ((k->key == KEY_LEFT || k->key == KEY_RIGHT) && m.brightness >= 0) {
        set_brightness((m.brightness + 5) / 10 * 10 + (k->key == KEY_LEFT ? -10 : 10));
        return;
    }
    if (m.timezone < 0)
        return;
    enum wg_list_result r = wg_list_key(&zones, k);
    if (r == WG_LIST_ACTIVATE && zones.selected >= 0)
        set_zone(zones.selected);
    else if (r != WG_LIST_NONE)
        system_page_draw();
}
