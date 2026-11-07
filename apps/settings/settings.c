/* LiteKern Go — Settings, reworked for the phone's portrait screen.
 *
 * It opens on a list of pages, each with what it's set to now: Wi-Fi
 * (network.c), Display, Lock Screen, Battery and Date and Time (system.c)
 * and About Phone. A tap opens one; the header's back arrow, or the phone's
 * Back key, goes back to the list. Display and Lock Screen are one column
 * of sections that scroll with a drag.
 *
 * Display: the brightness, the style (dark or light, as two little window
 * previews), the accent colour, the background (the wallpapers in the
 * ramdisk, or none) and when the screen saver starts (or a preview).
 * Lock Screen: Lock Now, the PIN (pin.c), its clock, locking when the
 * screen saver ends, and its own background. A tap applies it at once: the
 * kernel redraws the shell, sends every window K86_EVENT_THEME, and saves
 * it (kernel/settings.h). The Users page (users.c) is kept but hidden: the
 * phone has one user (UI-PLAN.md). Actions are logged ("user: settings:
 * ...") for the tests. */
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/timezones.h"
#include "kernel/widget.h"
#include "sdk/kern86.h"
#include "apps/settings/network.h"
#include "apps/settings/about.h"
#include "apps/settings/clockedit.h"
#include "apps/settings/pin.h"
#include "apps/settings/system.h"

#define MARGIN 32
#define THUMB_W 160
#define THUMB_H 94
#define DOT_R 26
#define KNOB_R 22
#define SECTION_GAP 40
#define SLOP 24                         /* px a finger moves before it's a drag (a scroll) */
#define BACK_ID 100                     /* the header's back arrow */

enum page { P_HOME, P_WIFI, P_DISPLAY, P_LOCK, P_BATTERY, P_TIME, P_ABOUT, NPAGES };
static const char *const titles[NPAGES] = { "Settings", "Wi-Fi", "Display", "Lock Screen", "Battery",
                                            "Date and Time", "About Phone" };
static const char *const names[NPAGES] = { "home", "wifi", "display", "lock", "battery", "time", "about" };
#define NHOME (NPAGES - 1)              /* the list: every page but itself */

static struct k86_window win;
static struct gfx_surface canvas;
static struct k86_appearance app;
static struct k86_lock lock;
static struct k86_machine machine;
static struct k86_users users;
static enum page page;
static uint32_t thumbs[K86_MAX_WALLPAPERS][THUMB_W * THUMB_H];
static int have_thumb[K86_MAX_WALLPAPERS];

static int has_pin(void)
{
    return users.current >= 0 && users.user[users.current].has_password;
}

static void copy(char *dst, const char *src, int size)
{
    int n = 0;
    for (; src[n] && n < size - 1; n++)
        dst[n] = src[n];
    dst[n] = '\0';
}

/* --- the list of pages ------------------------------------------------------------------- */

static struct wg_list home;
static char details[NHOME][64];

static void home_row(void *ctx, int i, struct wg_row *out)
{
    (void)ctx;
    *out = (struct wg_row){ titles[i + 1], details[i], WG_ICON_NONE };
}

/* What each page is set to now, under its name. */
static void home_details(void)
{
    struct k86_net net;
    memset(&net, 0, sizeof(net));
    k86_net_status(&net);
    copy(details[P_WIFI - 1], net.wifi == K86_NET_UP && net.ssid[0] ? net.ssid
                              : net.wifi == K86_NET_NONE ? "None on this phone"
                              : net.wifi == K86_NET_OFF ? "Off" : "Not connected", 64);
    char *d = details[P_DISPLAY - 1];
    copy(d, app.style ? "Light, " : "Dark, ", 64);
    copy(d + strlen(d), app.accent >= 0 && app.accent < app.naccents ? app.accents[app.accent].name : "", 64 - (int)strlen(d));
    if (d[6 + app.style] >= 'a' && d[6 + app.style] <= 'z')
        d[6 + app.style] = (char)(d[6 + app.style] - 32);
    copy(details[P_LOCK - 1], has_pin() ? "PIN" : "Swipe", 64);
    char b[64];
    system_battery_lines(&machine, details[P_BATTERY - 1], b, 64);
    if (machine.battery < 0)
        copy(details[P_BATTERY - 1], "No battery", 64);
    copy(details[P_TIME - 1], machine.timezone >= 0 ? lkx_zones[machine.timezone].name : "The phone's own clock", 64);
    copy(details[P_ABOUT - 1], about_phone_name(), 64);
}

static void home_layout(void)
{
    home.r = (struct gfx_rect){ MARGIN, 24, canvas.w - 2 * MARGIN, NHOME * theme_get()->row_h };
}

static void home_draw(void)
{
    gfx_fill_rect(&canvas, 0, 0, canvas.w, canvas.h, theme_get()->window_bg);
    wg_list_draw(&canvas, &home);
    k86_present(0, 0, canvas.w, canvas.h);
}

/* --- the scrolling pages: Display and Lock Screen ----------------------------------------- */

static const int saver_s[] = { 0, 60, 120, 300, 600, 1800 };
static const char *const saver_label[] = { "Off", "1 min", "2 min", "5 min", "10 min", "30 min" };
#define NSAVER 6

/* Things to tap: kind + index, and where they are (in the page, before scrolling). */
enum kind { K_STYLE, K_ACCENT, K_WALLPAPER, K_NONE_WALLPAPER, K_SAVER, K_PREVIEW, K_LOCK_NOW, K_CLOCK_EDIT, K_CLOCK24,
            K_ON_WAKE, K_LOCK_DESKTOP, K_LOCK_PICTURE, K_PIN_SET, K_PIN_CHANGE, K_PIN_REMOVE };
struct spot {
    enum kind kind;
    int index;
    struct gfx_rect r;
};
static struct spot spots[2 + K86_MAX_ACCENTS + K86_MAX_WALLPAPERS + 3 + NSAVER + 1 + 8];
static int nspots, pressed = -1;

/* Section titles, and a line of text under one. */
struct label {
    int y;
    const char *text;
};
static struct label labels[8];
static int nlabels, y_pin_text;
static char lock_msg[64];
static int lock_msg_bad;

static int scroll, content_h, press_y, scroll0, dragging;
static int32_t glide;                   /* a flick's speed: px a ms x 256 (the lists' wg_list_tick) */
static uint32_t glide_ms;
static int glide_y;
static struct gfx_rect bright_r;        /* the brightness slider's track (w 0: none) */
static int bright_held, bright_value;

static void add_spot(enum kind k, int i, struct gfx_rect r)
{
    spots[nspots++] = (struct spot){ k, i, r };
}

static int add_label(int y, const char *text)
{
    labels[nlabels++] = (struct label){ y, text };
    return y + text_height(TEXT_BOLD) + 16;
}

/* The background tiles, three a row, from y: where they end. */
static int tiles(int y, int for_lock)
{
    int w = canvas.w - 2 * MARGIN, gap = 20, tw = (w - 2 * gap) / 3, th = tw * THUMB_H / THUMB_W;
    int n = 0;
    struct spot list[K86_MAX_WALLPAPERS + 3];
    if (for_lock)
        list[n++] = (struct spot){ K_LOCK_DESKTOP, 0, { 0 } };
    list[n++] = (struct spot){ K_NONE_WALLPAPER, 0, { 0 } };
    for (int i = 0; i < app.nwallpapers; i++)
        list[n++] = (struct spot){ K_WALLPAPER, i, { 0 } };
    if (for_lock && lock.has_picture)
        list[n++] = (struct spot){ K_LOCK_PICTURE, 0, { 0 } };
    int row_h = th + 10 + text_height(TEXT_BODY) + 24;
    for (int k = 0; k < n; k++)
        add_spot(list[k].kind, list[k].index,
                 (struct gfx_rect){ MARGIN + k % 3 * (tw + gap), y + k / 3 * row_h, tw, th });
    return y + (n + 2) / 3 * row_h;
}

static void layout(void)
{
    const struct theme *t = theme_get();
    int w = canvas.w - 2 * MARGIN, bs = t->button_size, bh = text_height(TEXT_BODY);
    int y = 24;
    nspots = nlabels = 0;
    bright_r.w = 0;
    if (page == P_DISPLAY) {
        if (machine.brightness >= 0) {
            y = add_label(y, "Brightness");
            bright_r = (struct gfx_rect){ MARGIN + KNOB_R, y + KNOB_R - 6, w - 2 * KNOB_R, 12 };
            y += 2 * KNOB_R + SECTION_GAP;
        }
        y = add_label(y, "Style");
        int cw = (w - 24) / 2, ch = cw * 3 / 5;
        for (int i = 0; i < 2; i++)
            add_spot(K_STYLE, i, (struct gfx_rect){ MARGIN + i * (cw + 24), y, cw, ch });
        y += ch + 12 + bh + SECTION_GAP;
        y = add_label(y, "Accent Color");
        int pitch = app.naccents ? w / app.naccents : w, size = 2 * DOT_R + 12;
        if (pitch > size + 24)
            pitch = size + 24;
        for (int i = 0; i < app.naccents; i++)
            add_spot(K_ACCENT, i, (struct gfx_rect){ MARGIN + i * pitch + (pitch - size) / 2, y, size, size });
        y += size + SECTION_GAP;
        y = add_label(y, "Background");
        y = tiles(y, 0) + SECTION_GAP - 24;
        y = add_label(y, "Screen Saver");
        int chip = (w - 2 * 16) / 3;
        for (int i = 0; i < NSAVER; i++)
            add_spot(K_SAVER, i, (struct gfx_rect){ MARGIN + i % 3 * (chip + 16), y + i / 3 * (bs + 16), chip, bs });
        add_spot(K_PREVIEW, 0, (struct gfx_rect){ MARGIN, y + 2 * (bs + 16), chip, bs });
        y += 3 * (bs + 16);
    } else if (page == P_LOCK) {
        add_spot(K_LOCK_NOW, 0, (struct gfx_rect){ MARGIN, y, w, bs });
        y += bs + SECTION_GAP;
        y = add_label(y, "PIN");
        y_pin_text = y;
        y += bh + 16;
        if (has_pin()) {
            add_spot(K_PIN_CHANGE, 0, (struct gfx_rect){ MARGIN, y, (w - 16) / 2, bs });
            add_spot(K_PIN_REMOVE, 0, (struct gfx_rect){ MARGIN + (w + 16) / 2, y, (w - 16) / 2, bs });
        } else {
            add_spot(K_PIN_SET, 0, (struct gfx_rect){ MARGIN, y, w, bs });
        }
        y += bs + SECTION_GAP;
        add_spot(K_CLOCK_EDIT, 0, (struct gfx_rect){ MARGIN, y, w, bs });
        y += bs + 16;
        add_spot(K_CLOCK24, 0, (struct gfx_rect){ MARGIN, y, w, t->row_h });
        y += t->row_h;
        add_spot(K_ON_WAKE, 0, (struct gfx_rect){ MARGIN, y, w, t->row_h });
        y += t->row_h + SECTION_GAP;
        y = add_label(y, "Background");
        y = tiles(y, 1);
    }
    content_h = y + 24;
    int max = content_h - canvas.h;
    scroll = scroll > max ? max : scroll;
    scroll = scroll < 0 ? 0 : scroll;
}

/* r on the canvas, as scrolled. */
static struct gfx_rect at(struct gfx_rect r)
{
    r.y -= scroll;
    return r;
}

static void ring(struct gfx_rect r, int radius, uint32_t colour)
{
    gfx_fill_round_rect(&canvas, r.x - 6, r.y - 6, r.w + 12, r.h + 12, radius + 6, colour);
    gfx_fill_round_rect(&canvas, r.x - 2, r.y - 2, r.w + 4, r.h + 4, radius + 2, theme_get()->window_bg);
}

/* A little window in one style: what it'll look like (sized from r). */
static void style_preview(struct gfx_rect r, int light, int selected, int lit)
{
    const struct theme *t = theme_get();
    uint32_t win_bg = light ? 0xfafafb : 0x222226, head = light ? 0xebebed : 0x2e2e32;
    uint32_t card = light ? 0xffffff : 0x343437, fg = light ? 0x2e2e33 : 0xffffff;
    int u = r.w / 20;                       /* 1/20 of its width */
    if (selected)
        ring(r, 16, t->accent_bg);
    else if (lit)
        ring(r, 16, gfx_mix(t->ink, t->window_bg, 60));
    gfx_fill_round_rect(&canvas, r.x, r.y, r.w, r.h, 16, win_bg);
    gfx_fill_round_rect(&canvas, r.x, r.y, r.w, 3 * u + 16, 16, head);     /* its header bar */
    gfx_fill_rect(&canvas, r.x, r.y + 3 * u, r.w, 16, win_bg);
    gfx_fill_rect(&canvas, r.x + r.w / 2 - 3 * u, r.y + 3 * u / 2, 6 * u, u / 2 + 1, gfx_mix(fg, head, 150));
    gfx_fill_round_rect(&canvas, r.x + 2 * u, r.y + 5 * u, r.w - 4 * u, 6 * u, 8, card);
    for (int i = 1; i < 3; i++)
        gfx_fill_rect(&canvas, r.x + 2 * u, r.y + 5 * u + i * 2 * u, r.w - 4 * u, 1, gfx_mix(fg, card, 30));
    for (int i = 0; i < 3; i++)
        gfx_fill_rect(&canvas, r.x + 3 * u, r.y + 5 * u + u * 2 / 3 + i * 2 * u, 6 * u - i * u, u / 2, gfx_mix(fg, card, 140));
    gfx_fill_round_rect(&canvas, r.x + r.w - 8 * u, r.y + r.h - 3 * u, 6 * u, 2 * u, 6, t->accent_bg);
}

static int desktop_preset(void)
{
    for (int i = 0; i < app.nwallpapers; i++)
        if (!strcmp(app.wallpaper, app.wallpapers[i]))
            return i;
    return -1;
}

static int tile_selected(const struct spot *s)
{
    if (page != P_LOCK)
        return s->kind == K_NONE_WALLPAPER ? !app.wallpaper[0] : !strcmp(app.wallpaper, app.wallpapers[s->index]);
    switch (s->kind) {
    case K_LOCK_DESKTOP: return !strcmp(lock.wallpaper, "desktop");
    case K_LOCK_PICTURE: return !strcmp(lock.wallpaper, "Custom");
    case K_NONE_WALLPAPER: return !strcmp(lock.wallpaper, "none");
    default: return !strcmp(lock.wallpaper, app.wallpapers[s->index]);
    }
}

static void draw_switch(struct gfx_rect row, const char *label, int on, int lit)
{
    const struct theme *t = theme_get();
    if (lit)
        gfx_fill_round_rect(&canvas, row.x - 12, row.y, row.w + 24, row.h, t->radius, t->button_hover);
    text_draw_fit(&canvas, row.x, row.y + (row.h - text_height(TEXT_BODY)) / 2, label, row.w - 140, TEXT_BODY, t->fg);
    int sw = 96, sh = 52, sx = row.x + row.w - sw, sy = row.y + (row.h - sh) / 2;
    gfx_fill_round_rect(&canvas, sx, sy, sw, sh, sh / 2, on ? t->accent_bg : t->button_bg);
    gfx_fill_circle(&canvas, on ? sx + sw - sh / 2 : sx + sh / 2, sy + sh / 2, sh / 2 - 6, 0xffffff);
}

static void pill(struct gfx_rect r, const char *label, int selected, int lit, int style)
{
    const struct theme *t = theme_get();
    uint32_t bg = selected || style == 1 ? t->accent_bg : style == 2 ? t->destructive
                : lit ? t->button_active : t->button_bg;
    if (lit && (selected || style))
        bg = gfx_mix(0, bg, 200);
    gfx_fill_round_rect(&canvas, r.x, r.y, r.w, r.h, r.h / 2, bg);
    text_draw(&canvas, r.x + (r.w - text_width(label, TEXT_BOLD)) / 2, r.y + (r.h - text_height(TEXT_BOLD)) / 2,
              label, TEXT_BOLD, selected || style ? t->accent_fg : t->fg);
}

static void spots_draw(void)
{
    const struct theme *t = theme_get();
    static const char *const styles[] = { "Dark", "Light" };
    gfx_fill_rect(&canvas, 0, 0, canvas.w, canvas.h, t->window_bg);
    for (int i = 0; i < nlabels; i++)
        text_draw(&canvas, MARGIN, labels[i].y - scroll, labels[i].text, TEXT_BOLD, t->fg);
    if (bright_r.w) {
        int v = bright_held ? bright_value : machine.brightness;
        char pct[8];
        int n = 0;
        if (v >= 100)
            pct[n++] = '1';
        if (v >= 10)
            pct[n++] = (char)('0' + v / 10 % 10);
        pct[n++] = (char)('0' + v % 10);
        pct[n++] = '%';
        pct[n] = '\0';
        int ly = labels[0].y - scroll;
        text_draw(&canvas, canvas.w - MARGIN - text_width(pct, TEXT_BODY), ly, pct, TEXT_BODY, t->fg_dim);
        struct gfx_rect r = at(bright_r);
        int kx = r.x + r.w * v / 100;
        gfx_fill_round_rect(&canvas, r.x, r.y, r.w, r.h, r.h / 2, t->button_bg);
        gfx_fill_round_rect(&canvas, r.x, r.y, kx - r.x + r.h / 2, r.h, r.h / 2, t->accent_bg);
        gfx_fill_circle(&canvas, kx, r.y + r.h / 2, KNOB_R, bright_held ? t->accent_bg : t->fg);
    }
    if (page == P_LOCK) {
        const char *line = lock_msg[0] ? lock_msg : has_pin() ? "A PIN unlocks the phone."
                                                              : "No PIN: a swipe up unlocks the phone.";
        text_draw_fit(&canvas, MARGIN, y_pin_text - scroll, line, canvas.w - 2 * MARGIN, TEXT_BODY,
                      lock_msg[0] && lock_msg_bad ? t->destructive : t->fg_dim);
    }
    for (int i = 0; i < nspots; i++) {
        struct spot *s = &spots[i];
        struct gfx_rect r = at(s->r);
        int lit = i == pressed;
        if (r.y > canvas.h || r.y + r.h + 60 < 0)
            continue;
        switch (s->kind) {
        case K_STYLE:
            style_preview(r, s->index, app.style == s->index, lit);
            text_draw(&canvas, r.x + (r.w - text_width(styles[s->index], TEXT_BODY)) / 2, r.y + r.h + 12,
                      styles[s->index], TEXT_BODY, app.style == s->index ? t->fg : t->fg_dim);
            break;
        case K_ACCENT: {
            int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
            uint32_t c = app.accents[s->index].colour;
            if (app.accent == s->index) {
                gfx_fill_circle(&canvas, cx, cy, DOT_R + 6, c);
                gfx_fill_circle(&canvas, cx, cy, DOT_R + 3, t->window_bg);
            } else if (lit) {
                gfx_fill_circle(&canvas, cx, cy, DOT_R + 5, gfx_mix(t->ink, t->window_bg, 60));
                gfx_fill_circle(&canvas, cx, cy, DOT_R + 2, t->window_bg);
            }
            gfx_fill_circle(&canvas, cx, cy, DOT_R, c);
            break;
        }
        case K_NONE_WALLPAPER:
        case K_WALLPAPER:
        case K_LOCK_DESKTOP:
        case K_LOCK_PICTURE: {
            int selected = tile_selected(s);
            if (selected)
                ring(r, 12, t->accent_bg);
            else if (lit)
                ring(r, 12, gfx_mix(t->ink, t->window_bg, 60));
            int thumb_of = s->kind == K_WALLPAPER ? s->index : s->kind == K_LOCK_DESKTOP ? desktop_preset() : -1;
            if (thumb_of >= 0 && have_thumb[thumb_of]) {
                struct gfx_surface thumb = { thumbs[thumb_of], THUMB_W, THUMB_H, THUMB_W };
                gfx_blit_scaled(&canvas, r, &thumb, (struct gfx_rect){ 0, 0, THUMB_W, THUMB_H }, 255, 0);
                gfx_round_corners(&canvas, r.x, r.y, r.w, r.h, 12, t->window_bg);
            } else {
                gfx_fill_round_rect(&canvas, r.x, r.y, r.w, r.h, 12,
                                    s->kind == K_LOCK_PICTURE ? gfx_mix(t->accent_bg, 0x202634, 90)
                                    : t->light ? 0xc8ccd6 : 0x202634);
            }
            const char *label = s->kind == K_NONE_WALLPAPER ? "None" : s->kind == K_LOCK_DESKTOP ? "Home screen's"
                              : s->kind == K_LOCK_PICTURE ? "Picture" : app.wallpapers[s->index];
            char name[48];
            copy(name, label, sizeof(name));
            if (name[0] >= 'a' && name[0] <= 'z')
                name[0] = (char)(name[0] - 32);     /* "Aurora" */
            text_draw_fit(&canvas, r.x, r.y + r.h + 10, name, r.w, TEXT_BODY, selected ? t->fg : t->fg_dim);
            break;
        }
        case K_SAVER:
            pill(r, saver_label[s->index], app.screensaver_s == saver_s[s->index], lit, 0);
            break;
        case K_PREVIEW:
            pill(r, "Preview", 0, lit, 0);
            break;
        case K_LOCK_NOW:
            pill(r, "Lock Now", 0, lit, 1);
            break;
        case K_PIN_SET:
            pill(r, "Set PIN", 0, lit, 1);
            break;
        case K_PIN_CHANGE:
            pill(r, "Change PIN", 0, lit, 0);
            break;
        case K_PIN_REMOVE:
            pill(r, "Remove PIN", 0, lit, 2);
            break;
        case K_CLOCK_EDIT:
            pill(r, "Customise the clock", 0, lit, 0);
            break;
        case K_CLOCK24:
            draw_switch(r, "24-hour clock", lock.clock24, lit);
            break;
        case K_ON_WAKE:
            draw_switch(r, "Lock when the screen saver ends", lock.on_wake, lit);
            break;
        }
    }
    pin_draw();
    k86_present(0, 0, canvas.w, canvas.h);
}

/* --- About Phone --------------------------------------------------------------------------- */

/* --- pages --------------------------------------------------------------------------------- */

static void draw(void)
{
    switch (page) {
    case P_HOME:    home_draw(); break;
    case P_WIFI:    network_page_draw(); break;
    case P_BATTERY:
    case P_TIME:    system_page_draw(); break;
    case P_ABOUT:   about_draw(); break;
    default:        spots_draw(); break;
    }
}

static void reload(void)
{
    k86_appearance(&app);
    k86_lock_settings(&lock, 0);
    k86_machine(&machine);
    k86_users(&users);
}

static void header(void)
{
    struct k86_header h;
    memset(&h, 0, sizeof(h));
    copy(h.title, titles[page], sizeof(h.title));
    if (page != P_HOME) {
        h.nbuttons = 1;
        h.buttons[0].side = 0;
        h.buttons[0].icon = 1;              /* the back arrow */
        h.buttons[0].id = BACK_ID;
    }
    k86_header(&h);
}

/* The timer: a frame at a time while a list glides after a flick, else
 * every 2 s on the pages that refresh themselves, else off. */
static uint32_t next_refresh;
static void set_scroll(int s);

static struct wg_list *page_list(void)
{
    return page == P_HOME ? &home : page == P_WIFI ? network_page_list() : page == P_TIME ? system_page_list() : 0;
}

static void timer_follow(void)
{
    static uint32_t was = 1;
    struct wg_list *l = page_list();
    uint32_t want = (l && wg_list_gliding(l)) || glide ? 16 : page == P_WIFI || page == P_BATTERY ? 2000 : 0;
    if (want != was)
        k86_timer_set(want);
    was = want;
}

static void timer(void)
{
    uint32_t now = k86_uptime_ms();
    struct wg_list *l = page_list();
    if (l && wg_list_gliding(l)) {
        wg_list_tick(l, now);
        draw();
    }
    if (glide && (page == P_DISPLAY || page == P_LOCK)) {  /* Display, Lock Screen: their own scrolling */
        uint32_t dt = now - glide_ms;
        dt = dt > 50 ? 50 : dt;
        glide_ms = now;
        int before = scroll;
        set_scroll(scroll - (int)(glide * (int32_t)dt / 256));
        for (uint32_t s = 0; s < (dt + 8) / 16; s++)
            glide = glide * 94 / 100;
        if ((glide > -8 && glide < 8) || scroll == before)
            glide = 0;
        spots_draw();
    } else {
        glide = 0;
    }
    if ((page == P_WIFI || page == P_BATTERY) && (int32_t)(now - next_refresh) >= 0) {
        next_refresh = now + 2000;
        if (page == P_WIFI)
            network_page_tick();
        else
            system_page_tick();
    }
}

static void show_page(enum page p)
{
    if (p == page)
        return;
    pin_cancel();
    page = p;
    k86_logf("settings: page %s", names[page]);
    if (page == P_ABOUT)
        about_open(&canvas, app.wallpaper);
    header();
    next_refresh = k86_uptime_ms() + 2000;  /* Wi-Fi and Battery refresh themselves */
    scroll = 0;
    pressed = -1;
    lock_msg[0] = '\0';
    reload();
    switch (page) {
    case P_HOME:
        home_details();
        home_layout();
        wg_list_select(&home, -1);
        home_draw();
        break;
    case P_WIFI:
        network_page_open(&canvas);
        break;
    case P_BATTERY:
        system_page_open(&canvas, SYSTEM_BATTERY);
        break;
    case P_TIME:
        system_page_open(&canvas, SYSTEM_TIME);
        break;
    default:
        layout();
        draw();
    }
}

static void pin_done(const char *message, int bad)
{
    copy(lock_msg, message, sizeof(lock_msg));
    lock_msg_bad = bad;
    k86_users(&users);
    layout();
    spots_draw();
}

static void set_lock_wallpaper(const char *w, const char *what)
{
    memset(lock.wallpaper, 0, sizeof(lock.wallpaper));
    memcpy(lock.wallpaper, w, strlen(w) < sizeof(lock.wallpaper) ? strlen(w) : sizeof(lock.wallpaper) - 1);
    k86_logf("settings: lock screen background %s", what);
    k86_lock_settings(&lock, 1);
    k86_lock_settings(&lock, 0);
    spots_draw();
}

static void choose(const struct spot *s)
{
    if (page == P_LOCK && (s->kind == K_WALLPAPER || s->kind == K_NONE_WALLPAPER)) {
        if (s->kind == K_WALLPAPER)
            set_lock_wallpaper(app.wallpapers[s->index], app.wallpapers[s->index]);
        else
            set_lock_wallpaper("none", "none");
        return;
    }
    switch (s->kind) {
    case K_LOCK_DESKTOP:
        set_lock_wallpaper("desktop", "the desktop's");
        return;
    case K_LOCK_PICTURE:
        set_lock_wallpaper("Custom", "its picture");
        return;
    case K_LOCK_NOW:
        k86_log("settings: lock now");
        k86_lock();
        return;
    case K_PIN_SET:
    case K_PIN_CHANGE:
    case K_PIN_REMOVE:
        lock_msg[0] = '\0';
        pin_start(&canvas, s->kind == K_PIN_SET ? PIN_SET : s->kind == K_PIN_CHANGE ? PIN_CHANGE : PIN_REMOVE,
                  has_pin(), users.current, pin_done, spots_draw);
        return;
    case K_CLOCK_EDIT:
        clockedit_open(&canvas, &lock);
        return;
    case K_CLOCK24:
    case K_ON_WAKE:
        if (s->kind == K_CLOCK24)
            lock.clock24 = !lock.clock24;
        else
            lock.on_wake = !lock.on_wake;
        k86_logf("settings: lock screen %s %s", s->kind == K_CLOCK24 ? "24-hour clock" : "lock on wake",
                 (s->kind == K_CLOCK24 ? lock.clock24 : lock.on_wake) ? "on" : "off");
        k86_lock_settings(&lock, 1);
        spots_draw();
        return;
    case K_STYLE:
        k86_logf("settings: style %s", s->index ? "light" : "dark");
        k86_appearance_set(s->index, -1, 0);
        break;
    case K_ACCENT:
        k86_logf("settings: accent %s", app.accents[s->index].name);
        k86_appearance_set(-1, s->index, 0);
        break;
    case K_WALLPAPER:
        k86_logf("settings: background %s", app.wallpapers[s->index]);
        k86_appearance_set(-1, -1, app.wallpapers[s->index]);
        break;
    case K_NONE_WALLPAPER:
        k86_log("settings: background none");
        k86_appearance_set(-1, -1, "");
        break;
    case K_SAVER:
        k86_logf("settings: screen saver %s", saver_label[s->index]);
        k86_screensaver_set(saver_s[s->index]);
        k86_appearance(&app);
        spots_draw();                       /* no theme change: redraw here */
        return;
    case K_PREVIEW:
        k86_log("settings: screen saver preview");
        k86_screensaver_preview();
        return;
    }
    k86_appearance(&app);                   /* the redraw comes with K86_EVENT_THEME */
}

static int spot_at(int x, int y)
{
    for (int i = 0; i < nspots; i++) {
        struct gfx_rect r = spots[i].r;
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
            return i;
    }
    return -1;
}

static void set_scroll(int s)
{
    int max = content_h - canvas.h;
    s = s > max ? max : s;
    s = s < 0 ? 0 : s;
    if (s != scroll) {
        scroll = s;
        spots_draw();
    }
}

static void spots_pointer(const struct k86_event *ev)
{
    struct wg_pointer p = wg_pointer_make(ev->x, ev->y, ev->buttons, ev->changed, ev->time_ms);
    int cy = p.y + scroll;
    if (bright_r.w && (bright_held || (p.down && p.x >= bright_r.x - KNOB_R && p.x <= bright_r.x + bright_r.w + KNOB_R &&
                                       cy >= bright_r.y - 2 * KNOB_R && cy <= bright_r.y + bright_r.h + 2 * KNOB_R))) {
        int v = (p.x - bright_r.x) * 100 / (bright_r.w > 0 ? bright_r.w : 1);
        bright_value = v < 5 ? 5 : v > 100 ? 100 : v;
        bright_held = p.held;
        if (!p.held) {                      /* let go: set */
            int err = k86_brightness_set(bright_value);
            k86_logf("settings: brightness %d: %s", bright_value, err ? "refused" : "set");
            k86_machine(&machine);
        }
        spots_draw();
        return;
    }
    if (p.down) {
        press_y = glide_y = p.y;
        glide_ms = p.time_ms;
        glide = 0;                          /* a finger stops a glide */
        scroll0 = scroll;
        dragging = 0;
        pressed = spot_at(p.x, cy);
        if (pressed >= 0)
            spots_draw();
        return;
    }
    if (p.held && !dragging && (p.y - press_y > SLOP || press_y - p.y > SLOP)) {
        dragging = 1;                       /* a drag: it scrolls, and taps nothing */
        pressed = -1;
    }
    if (p.held && dragging) {
        uint32_t dt = p.time_ms - glide_ms;
        if (dt > 0) {                       /* the finger's speed, for a glide */
            glide = (glide + (int32_t)((p.y - glide_y) * 256 / (int)dt)) / 2;
            glide_ms = p.time_ms;
            glide_y = p.y;
        }
        set_scroll(scroll0 - (p.y - press_y));
        spots_draw();
        return;
    }
    if (!p.up)
        return;
    int was = pressed;
    pressed = -1;
    if (dragging) {
        dragging = 0;
        if (p.time_ms - glide_ms > 80)
            glide = 0;                      /* it stopped before it let go: no glide */
        glide_ms = p.time_ms;
        return;
    }
    glide = 0;
    if (was >= 0 && was == spot_at(p.x, cy))
        choose(&spots[was]);
    else
        spots_draw();
}

static void home_pointer(const struct k86_event *ev)
{
    struct wg_pointer p = wg_pointer_make(ev->x, ev->y, ev->buttons, ev->changed, ev->time_ms);
    enum wg_list_result r = wg_list_pointer(&home, &p);
    if (r == WG_LIST_ACTIVATE && home.selected >= 0) {
        show_page((enum page)(home.selected + 1));
        return;
    }
    if (home.dirty)
        home_draw();
}

/* Back (Esc): a dialog first, then the list of pages. */
static int busy(void)
{
    return pin_active() || (page == P_WIFI && network_page_busy()) || (page == P_ABOUT && about_editing());
}

static void key(const struct key_event *k)
{
    if (pin_active()) {
        pin_key(k);
        return;
    }
    if (page == P_WIFI && network_page_busy()) {
        network_page_key(k);
        return;
    }
    if (page == P_ABOUT && about_key(k))
        return;
    if (k->pressed && k->key == KEY_ESC && page != P_HOME) {
        show_page(P_HOME);
        return;
    }
    switch (page) {
    case P_HOME:
        if (wg_list_key(&home, k) == WG_LIST_ACTIVATE && home.selected >= 0)
            show_page((enum page)(home.selected + 1));
        else if (home.dirty)
            home_draw();
        break;
    case P_WIFI:    network_page_key(k); break;
    case P_BATTERY:
    case P_TIME:    system_page_key(k); break;
    case P_DISPLAY:
        /* Left/Right walk the accents; L and D pick the style. */
        if (!k->pressed)
            break;
        if (k->key == KEY_LEFT && app.accent > 0)
            choose(&(struct spot){ K_ACCENT, app.accent - 1, { 0, 0, 0, 0 } });
        else if (k->key == KEY_RIGHT && app.accent + 1 < app.naccents)
            choose(&(struct spot){ K_ACCENT, app.accent + 1, { 0, 0, 0, 0 } });
        else if (k->ascii == 'l' || k->ascii == 'd')
            choose(&(struct spot){ K_STYLE, k->ascii == 'l', { 0, 0, 0, 0 } });
        break;
    default:
        break;
    }
}

static void relayout(void)
{
    home_layout();
    pin_layout();
    switch (page) {
    case P_WIFI:    network_page_layout(); break;
    case P_BATTERY:
    case P_TIME:    system_page_layout(); break;
    default:        layout(); break;
    }
}

int main(void)
{
    if (k86_window_open(&win))
        return 1;
    canvas = (struct gfx_surface){ win.canvas, win.w, win.h, win.stride };
    network_page_available();               /* logs what there is */
    reload();
    for (int i = 0; i < app.nwallpapers; i++)
        have_thumb[i] = k86_wallpaper_thumb(app.wallpapers[i], thumbs[i], THUMB_W, THUMB_H) == 0;
    home = (struct wg_list){ .selected = -1, .hover = -1, .row = home_row, .last_click_row = -1, .count = NHOME };
    header();
    home_details();
    home_layout();
    home_draw();
    k86_log("settings: open");

    for (;;) {
        struct k86_event ev;
        if (clockedit_active()) {
            k86_back_wanted(1);
            k86_keyboard_wanted(0);
            if (k86_wait_event(&ev))
                return 1;
            if (ev.type == K86_EVENT_CLOSE)
                return 0;
            if (ev.type == K86_EVENT_RESIZE) {
                canvas.w = ev.x;
                canvas.h = ev.y;
            }
            if (clockedit_event(&ev)) {             /* it's done */
                k86_lock_settings(&lock, 0);
                header();
                draw();
            }
            continue;
        }
        k86_back_wanted(page != P_HOME || busy());     /* the phone's Back: back to the list */
        k86_keyboard_wanted(pin_active() ? K86_KEYBOARD_NUMBERS : busy());
        timer_follow();
        if (k86_wait_event(&ev))
            return 1;
        switch (ev.type) {
        case K86_EVENT_POINTER:
            if (pin_active())
                pin_pointer(&ev);
            else if (page == P_HOME)
                home_pointer(&ev);
            else if (page == P_WIFI)
                network_page_pointer(&ev);
            else if (page == P_BATTERY || page == P_TIME)
                system_page_pointer(&ev);
            else if (page == P_DISPLAY || page == P_LOCK)
                spots_pointer(&ev);
            else if (page == P_ABOUT)
                about_pointer(&ev);
            break;
        case K86_EVENT_SCROLL:
            if (page == P_WIFI)
                network_page_scroll(&ev);
            else if (page == P_BATTERY || page == P_TIME)
                system_page_scroll(&ev);
            else if (page == P_DISPLAY || page == P_LOCK)
                set_scroll(scroll + ev.id * theme_get()->row_h);
            break;
        case K86_EVENT_KEY:
            key(&ev.key);
            break;
        case K86_EVENT_HEADER:
            if (ev.id == BACK_ID)
                show_page(P_HOME);
            break;
        case K86_EVENT_TIMER:
            timer();
            break;
        case K86_EVENT_THEME:
            reload();
            home_details();
            relayout();
            draw();
            break;
        case K86_EVENT_RESIZE:
            canvas.w = ev.x;
            canvas.h = ev.y;
            relayout();
            draw();
            break;
        case K86_EVENT_CLOSE:   return 0;
        default:                break;
        }
    }
}
