/* LiteKern Go — Settings: the Battery and Date and time pages
 * (kernel/machine.h), one column for the phone's portrait screen.
 *
 * Battery: how full, charging, how long left, and a gauge; refreshed every
 * 2 s while it shows. Date and time: the time zones, a tap picks one and
 * the clock follows at once. What the machine can't do (no battery driver,
 * the computer's own clock) is said instead. The brightness is on the
 * Display page (settings.c). Actions are logged ("user: settings: system
 * ...") for the tests. */
#include "apps/settings/system.h"
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/timezones.h"
#include "kernel/widget.h"

#define MARGIN 32

static struct gfx_surface *c;
static struct k86_machine m;
static struct wg_list zones;
static int which;                       /* SYSTEM_BATTERY or SYSTEM_TIME */
static char status[96];

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
    char d[12], r[12];
    int n = 0;
    if (v < 0) {
        append(dst, "-", size);
        v = -v;
    }
    do
        d[n++] = (char)('0' + v % 10);
    while ((v /= 10) && n < 11);
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
    int y = 24 + text_height(TEXT_BODY) + 24;
    zones.r = (struct gfx_rect){ MARGIN, y, c->w - 2 * MARGIN, c->h - 24 - y };
}

/* "72%, on battery" and "Empty in 3 h 10 min" (empty if unknown). */
void system_battery_lines(const struct k86_machine *mm, char *a, char *b, int size)
{
    a[0] = b[0] = '\0';
    if (mm->battery < 0) {
        copy(a, "No battery found.", size);
        return;
    }
    append_int(a, mm->battery, size);
    append(a, mm->charging == K86_BATTERY_CHARGING ? "%, charging"
              : mm->charging == K86_BATTERY_FULL ? "%, on the charger" : "%, on battery", size);
    if (mm->minutes >= 0) {
        copy(b, mm->charging == K86_BATTERY_CHARGING ? "Full in " : "Empty in ", size);
        if (mm->minutes >= 60) {
            append_int(b, mm->minutes / 60, size);
            append(b, " h ", size);
        }
        append_int(b, mm->minutes % 60, size);
        append(b, " min", size);
    }
}

void system_page_draw(void)
{
    const struct theme *t = theme_get();
    char a[96], b[96];
    int w = c->w - 2 * MARGIN, bh = text_height(TEXT_BODY);
    gfx_fill_rect(c, 0, 0, c->w, c->h, t->window_bg);
    if (which == SYSTEM_BATTERY) {
        system_battery_lines(&m, a, b, sizeof(a));
        int low = m.battery >= 0 && m.battery <= 10 && m.charging == K86_BATTERY_DISCHARGING;
        if (m.battery >= 0)
            text_draw(c, MARGIN, 24, a, TEXT_LARGE, low ? t->destructive : t->fg);
        else
            text_draw_fit(c, MARGIN, 24, a, w, TEXT_BODY, t->fg_dim);
        int y = 24 + text_height(m.battery >= 0 ? TEXT_LARGE : TEXT_BODY) + 16;
        if (b[0]) {
            text_draw_fit(c, MARGIN, y, b, w, TEXT_BODY, t->fg_dim);
            y += bh + 16;
        }
        if (m.battery >= 0) {               /* a gauge */
            gfx_fill_round_rect(c, MARGIN, y, w, 24, 12, t->button_bg);
            int fw = w * m.battery / 100;
            if (fw >= 24)
                gfx_fill_round_rect(c, MARGIN, y, fw, 24, 12, low ? t->destructive : t->accent_bg);
        }
    } else if (m.timezone < 0) {
        text_draw_fit(c, MARGIN, 24, "The clock shows the computer's own time.", w, TEXT_BODY, t->fg_dim);
    } else {
        copy(a, "Now: ", sizeof(a));
        append(a, lkx_zones[m.timezone].name, sizeof(a));
        text_draw_fit(c, MARGIN, 24, a, w, TEXT_BODY, t->fg_dim);
        wg_list_draw(c, &zones);
    }
    if (status[0])
        text_draw_fit(c, MARGIN, c->h - text_height(TEXT_SMALL) - 8, status, w, TEXT_SMALL, t->fg_dim);
    k86_present(0, 0, c->w, c->h);
}

static void reload(void)
{
    k86_machine(&m);
    zones.count = m.timezone >= 0 ? LKX_ZONES : 0;
}

void system_page_open(struct gfx_surface *canvas, int page)
{
    c = canvas;
    which = page;
    status[0] = '\0';
    if (!zones.row)
        zones = (struct wg_list){ .selected = -1, .hover = -1, .row = row, .last_click_row = -1 };
    reload();
    system_page_layout();
    if (m.timezone >= 0) {
        wg_list_select(&zones, m.timezone);
        zones.top = m.timezone;             /* the chosen one at the top */
    }
    system_page_draw();
}

void system_page_tick(void)
{
    struct k86_machine before = m;
    reload();
    if (memcmp(&before, &m, sizeof(m)))
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

void system_page_pointer(const struct k86_event *ev)
{
    struct wg_pointer p = wg_pointer_make(ev->x, ev->y, ev->buttons, ev->changed, ev->time_ms);
    if (which != SYSTEM_TIME || m.timezone < 0)
        return;
    enum wg_list_result r = wg_list_pointer(&zones, &p);
    if (r == WG_LIST_ACTIVATE && zones.selected >= 0 && zones.selected != m.timezone) {
        set_zone(zones.selected);
        return;
    }
    if (zones.dirty)
        system_page_draw();
}

void system_page_scroll(const struct k86_event *ev)
{
    if (which == SYSTEM_TIME && m.timezone >= 0 && wg_list_scroll(&zones, ev->x, ev->y, ev->id))
        system_page_draw();
}

void system_page_key(const struct key_event *k)
{
    if (!k->pressed || which != SYSTEM_TIME || m.timezone < 0)
        return;
    enum wg_list_result r = wg_list_key(&zones, k);
    if (r == WG_LIST_ACTIVATE && zones.selected >= 0)
        set_zone(zones.selected);
    else if (r != WG_LIST_NONE)
        system_page_draw();
}
