/* LiteKern X — Settings: the Appearance page (Phase 3 §5), like GNOME's.
 *
 * Style (dark or light, shown as two little window previews), accent colour
 * (GNOME's presets), background (the wallpapers in the ramdisk, or none),
 * when the screen saver starts (or a preview of it), and the lock screen:
 * Lock Now, its clock, locking when the screen saver ends, and (the
 * Background row's Lock Screen switch) its own background. A click applies
 * it at once: the kernel redraws the shell, sends every window
 * K86_EVENT_THEME, and saves it on the stick (kernel/settings.h); the
 * defaults come from `make STYLE= ACCENT= WALLPAPER= SCREENSAVER=`. */
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"
#include "sdk/kern86.h"
#include "apps/settings/network.h"
#include "apps/settings/users.h"

#define CARD_W 200
#define CARD_H 120
#define DOT_R 14
#define THUMB_W 160
#define THUMB_H 94
#define GAP 24

static struct k86_window win;
static struct gfx_surface canvas;
static struct k86_appearance app;
static struct k86_lock lock;
static int bg_lock;                     /* the Background row is for the lock screen */
static int page;                        /* 0: Appearance, 1: Users (users.c), 2: Network (network.c) */
#define PAGE_APPEARANCE 1               /* the header's buttons */
#define PAGE_USERS 2
#define PAGE_NETWORK 3
static int has_network;                 /* the Network page: only with Ethernet or Wi-Fi */
static uint32_t thumbs[K86_MAX_WALLPAPERS][THUMB_W * THUMB_H];
static int have_thumb[K86_MAX_WALLPAPERS];

/* The screen saver's choices, in seconds (0: never). */
static const int saver_s[] = { 0, 60, 120, 300, 600, 1800 };
static const char *const saver_label[] = { "Off", "1 min", "2 min", "5 min", "10 min", "30 min" };
#define NSAVER 6
#define CHIP_W 120
#define CHIP_H 34
#define CHIP_GAP 12

/* Clickable things: kind + index, and where they are. */
enum kind { K_STYLE, K_ACCENT, K_WALLPAPER, K_NONE_WALLPAPER, K_SAVER, K_PREVIEW,
            K_LOCK_NOW, K_CLOCK24, K_ON_WAKE, K_BG_TARGET, K_LOCK_DESKTOP, K_LOCK_PICTURE };
struct spot {
    enum kind kind;
    int index;
    struct gfx_rect r;
};
static struct spot spots[2 + K86_MAX_ACCENTS + K86_MAX_WALLPAPERS + 3 + NSAVER + 1 + 3 + 2];
static int nspots, hover = -1, pressed = -1;

static int x0, x_saver, y_style, y_accent, y_background, y_lock, tile_w, tile_h;

static void add_spot(enum kind k, int i, struct gfx_rect r)
{
    spots[nspots++] = (struct spot){ k, i, r };
}

static void layout(void)
{
    const struct theme *t = theme_get();
    int content_w = 2 * CARD_W + GAP;
    int items = 1 + app.nwallpapers;
    int bg_w = items * THUMB_W + (items - 1) * GAP;
    if (bg_w > content_w)
        content_w = bg_w;
    x0 = (canvas.w - content_w) / 2;
    int lh = text_height(TEXT_BOLD);
    y_style = 20;
    y_accent = y_style + lh + 10 + CARD_H + 10 + text_height(TEXT_BODY) + 24;
    y_background = y_accent + lh + 10 + 2 * DOT_R + 8 + 24;

    nspots = 0;
    for (int i = 0; i < 2; i++)
        add_spot(K_STYLE, i, (struct gfx_rect){ x0 + i * (CARD_W + GAP), y_style + lh + 10, CARD_W, CARD_H });
    for (int i = 0; i < app.naccents; i++)
        add_spot(K_ACCENT, i, (struct gfx_rect){ x0 + i * (2 * DOT_R + 20), y_accent + lh + 10,
                                                 2 * DOT_R + 8, 2 * DOT_R + 8 });
    /* The background row: None and the wallpapers; for the lock screen also
     * "Desktop's" first and its own picture last, the tiles smaller to fit. */
    int n = 1 + app.nwallpapers + (bg_lock ? 1 + lock.has_picture : 0), gap = GAP;
    tile_w = THUMB_W;
    if (n * THUMB_W + (n - 1) * GAP > content_w) {
        gap = 12;
        tile_w = (content_w - (n - 1) * gap) / n;
    }
    tile_h = THUMB_H * tile_w / THUMB_W;
    int ty = y_background + lh + 10, k = 0;
    if (bg_lock)
        add_spot(K_LOCK_DESKTOP, 0, (struct gfx_rect){ x0 + k++ * (tile_w + gap), ty, tile_w, tile_h });
    add_spot(K_NONE_WALLPAPER, 0, (struct gfx_rect){ x0 + k++ * (tile_w + gap), ty, tile_w, tile_h });
    for (int i = 0; i < app.nwallpapers; i++)
        add_spot(K_WALLPAPER, i, (struct gfx_rect){ x0 + k++ * (tile_w + gap), ty, tile_w, tile_h });
    if (bg_lock && lock.has_picture)
        add_spot(K_LOCK_PICTURE, 0, (struct gfx_rect){ x0 + k++ * (tile_w + gap), ty, tile_w, tile_h });
    /* Desktop | Lock Screen, after the row's title. */
    int bx = x0 + text_width("Background", TEXT_BOLD) + 20;
    for (int i = 0; i < 2; i++)
        add_spot(K_BG_TARGET, i, (struct gfx_rect){ bx + i * 116, y_background - 6, 112, 28 });
    /* The screen saver: to the right of the style cards, three choices a
     * row, then Preview. */
    x_saver = x0 + 2 * (CARD_W + GAP) + GAP;
    int sy = y_style + lh + 10;
    for (int i = 0; i < NSAVER; i++)
        add_spot(K_SAVER, i, (struct gfx_rect){ x_saver + (i % 3) * (CHIP_W + CHIP_GAP),
                                                sy + (i / 3) * (CHIP_H + 10), CHIP_W, CHIP_H });
    add_spot(K_PREVIEW, 0, (struct gfx_rect){ x_saver, sy + 2 * (CHIP_H + 10), CHIP_W, CHIP_H });
    /* The lock screen, under them. */
    y_lock = sy + 3 * (CHIP_H + 10) + 14;
    int ly = y_lock + lh + 10;
    add_spot(K_LOCK_NOW, 0, (struct gfx_rect){ x_saver, ly, CHIP_W, CHIP_H });
    add_spot(K_CLOCK24, 0, (struct gfx_rect){ x_saver + CHIP_W + CHIP_GAP, ly, 2 * CHIP_W + CHIP_GAP, CHIP_H });
    add_spot(K_ON_WAKE, 0, (struct gfx_rect){ x_saver, ly + CHIP_H + 10, 3 * CHIP_W + 2 * CHIP_GAP, CHIP_H });
    (void)t;
}

/* A selection ring: the accent, 3 px, around r. */
static void ring(struct gfx_rect r, int radius, uint32_t colour)
{
    gfx_fill_round_rect(&canvas, r.x - 4, r.y - 4, r.w + 8, r.h + 8, radius + 4, colour);
    gfx_fill_round_rect(&canvas, r.x - 1, r.y - 1, r.w + 2, r.h + 2, radius + 1, theme_get()->window_bg);
}

/* A little window in one style: what it'll look like. */
static void style_preview(struct gfx_rect r, int light, int selected, int lit)
{
    const struct theme *t = theme_get();
    uint32_t win_bg = light ? 0xfafafb : 0x222226, head = light ? 0xebebed : 0x2e2e32;
    uint32_t card = light ? 0xffffff : 0x343437, fg = light ? 0x2e2e33 : 0xffffff;
    if (selected)
        ring(r, 10, t->accent_bg);
    else if (lit)
        ring(r, 10, gfx_mix(t->ink, t->window_bg, 60));
    gfx_fill_round_rect(&canvas, r.x, r.y, r.w, r.h, 10, win_bg);
    gfx_fill_rect(&canvas, r.x, r.y + 10, r.w, 14, head);
    gfx_fill_round_rect(&canvas, r.x, r.y, r.w, 24, 10, head);
    gfx_fill_rect(&canvas, r.x + r.w / 2 - 24, r.y + 10, 48, 4, gfx_mix(fg, head, 150));   /* title */
    gfx_fill_round_rect(&canvas, r.x + 16, r.y + 38, r.w - 32, 46, 6, card);
    for (int i = 1; i < 3; i++)
        gfx_fill_rect(&canvas, r.x + 16, r.y + 38 + i * 15, r.w - 32, 1, gfx_mix(fg, card, 30));
    for (int i = 0; i < 3; i++)
        gfx_fill_rect(&canvas, r.x + 26, r.y + 44 + i * 15, 50 - i * 10, 3, gfx_mix(fg, card, 140));
    gfx_fill_round_rect(&canvas, r.x + r.w - 66, r.y + r.h - 26, 50, 16, 4, t->accent_bg);
}

/* The desktop's wallpaper, if it's a preset: its index (for its thumbnail). */
static int desktop_preset(void)
{
    for (int i = 0; i < app.nwallpapers; i++)
        if (!strcmp(app.wallpaper, app.wallpapers[i]))
            return i;
    return -1;
}

static int tile_selected(const struct spot *s)
{
    if (!bg_lock)
        return s->kind == K_NONE_WALLPAPER ? !app.wallpaper[0] : !strcmp(app.wallpaper, app.wallpapers[s->index]);
    switch (s->kind) {
    case K_LOCK_DESKTOP: return !strcmp(lock.wallpaper, "desktop");
    case K_LOCK_PICTURE: return !strcmp(lock.wallpaper, "Custom");
    case K_NONE_WALLPAPER: return !strcmp(lock.wallpaper, "none");
    default: return !strcmp(lock.wallpaper, app.wallpapers[s->index]);
    }
}

static void draw(void)
{
    const struct theme *t = theme_get();
    gfx_fill_rect(&canvas, 0, 0, canvas.w, canvas.h, t->window_bg);
    int lh = text_height(TEXT_BOLD);
    static const char *const styles[] = { "Dark", "Light" };

    text_draw(&canvas, x0, y_style, "Style", TEXT_BOLD, t->fg);
    for (int i = 0; i < nspots; i++) {
        struct spot *s = &spots[i];
        int lit = i == hover || i == pressed;
        switch (s->kind) {
        case K_STYLE:
            style_preview(s->r, s->index, app.style == s->index, lit);
            text_draw(&canvas, s->r.x + (s->r.w - text_width(styles[s->index], TEXT_BODY)) / 2,
                      s->r.y + s->r.h + 10, styles[s->index], TEXT_BODY,
                      app.style == s->index ? t->fg : t->fg_dim);
            break;
        case K_ACCENT: {
            int cx = s->r.x + s->r.w / 2, cy = s->r.y + s->r.h / 2;
            uint32_t c = app.accents[s->index].colour;
            if (app.accent == s->index) {
                gfx_fill_circle(&canvas, cx, cy, DOT_R + 4, c);
                gfx_fill_circle(&canvas, cx, cy, DOT_R + 2, t->window_bg);
            } else if (lit) {
                gfx_fill_circle(&canvas, cx, cy, DOT_R + 3, gfx_mix(t->ink, t->window_bg, 60));
                gfx_fill_circle(&canvas, cx, cy, DOT_R + 1, t->window_bg);
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
                ring(s->r, 8, t->accent_bg);
            else if (lit)
                ring(s->r, 8, gfx_mix(t->ink, t->window_bg, 60));
            int thumb_of = s->kind == K_WALLPAPER ? s->index : s->kind == K_LOCK_DESKTOP ? desktop_preset() : -1;
            if (thumb_of >= 0 && have_thumb[thumb_of]) {
                struct gfx_surface thumb = { thumbs[thumb_of], THUMB_W, THUMB_H, THUMB_W };
                if (s->r.w == THUMB_W)
                    gfx_blit(&canvas, s->r.x, s->r.y, &thumb, 0, 0, THUMB_W, THUMB_H);
                else
                    gfx_blit_scaled(&canvas, s->r, &thumb, (struct gfx_rect){ 0, 0, THUMB_W, THUMB_H }, 255, 0);
                gfx_round_corners(&canvas, s->r.x, s->r.y, s->r.w, s->r.h, 8, t->window_bg);
            } else {
                gfx_fill_round_rect(&canvas, s->r.x, s->r.y, s->r.w, s->r.h, 8,
                                    s->kind == K_LOCK_PICTURE ? gfx_mix(t->accent_bg, 0x202634, 90)
                                    : t->light ? 0xc8ccd6 : 0x202634);
            }
            const char *label = s->kind == K_NONE_WALLPAPER ? "None" : s->kind == K_LOCK_DESKTOP ? "Desktop's"
                              : s->kind == K_LOCK_PICTURE ? "Picture" : app.wallpapers[s->index];
            text_draw(&canvas, s->r.x + (s->r.w - text_width(label, TEXT_BODY)) / 2,
                      s->r.y + s->r.h + 8, label, TEXT_BODY, selected ? t->fg : t->fg_dim);
            break;
        }
        case K_BG_TARGET:
        case K_LOCK_NOW:
        case K_CLOCK24:
        case K_ON_WAKE: {
            int selected = s->kind == K_BG_TARGET ? bg_lock == s->index
                         : s->kind == K_CLOCK24 ? lock.clock24 : s->kind == K_ON_WAKE ? lock.on_wake : 0;
            uint32_t bg = selected ? t->accent_bg
                        : i == pressed && i == hover ? t->button_active
                        : lit ? t->button_hover : t->button_bg;
            const char *label = s->kind == K_BG_TARGET ? (s->index ? "Lock Screen" : "Desktop")
                              : s->kind == K_LOCK_NOW ? "Lock Now"
                              : s->kind == K_CLOCK24 ? "24-Hour Clock" : "Lock When the Screen Saver Ends";
            enum text_style st = s->kind == K_BG_TARGET ? TEXT_BODY : TEXT_BOLD;
            gfx_fill_round_rect(&canvas, s->r.x, s->r.y, s->r.w, s->r.h, s->r.h / 2, bg);
            text_draw(&canvas, s->r.x + (s->r.w - text_width(label, st)) / 2,
                      s->r.y + (s->r.h - text_height(st)) / 2, label, st, selected ? t->accent_fg : t->fg);
            break;
        }
        case K_SAVER:
        case K_PREVIEW: {
            /* Pill buttons: the chosen one in the accent colour. */
            int selected = s->kind == K_SAVER && app.screensaver_s == saver_s[s->index];
            uint32_t bg = selected ? t->accent_bg
                        : i == pressed && i == hover ? t->button_active
                        : lit ? t->button_hover : t->button_bg;
            const char *label = s->kind == K_SAVER ? saver_label[s->index] : "Preview";
            gfx_fill_round_rect(&canvas, s->r.x, s->r.y, s->r.w, s->r.h, s->r.h / 2, bg);
            text_draw(&canvas, s->r.x + (s->r.w - text_width(label, TEXT_BOLD)) / 2,
                      s->r.y + (s->r.h - text_height(TEXT_BOLD)) / 2, label, TEXT_BOLD,
                      selected ? t->accent_fg : t->fg);
            break;
        }
        }
    }
    text_draw(&canvas, x_saver, y_style, "Screen Saver", TEXT_BOLD, t->fg);
    text_draw(&canvas, x_saver, y_lock, "Lock Screen", TEXT_BOLD, t->fg);
    text_draw(&canvas, x0, y_accent, "Accent Color", TEXT_BOLD, t->fg);
    text_draw(&canvas, x0, y_background, "Background", TEXT_BOLD, t->fg);
    /* At the bottom, but never over the wallpapers' names above it. */
    const char *note = "Changes are saved on the USB stick.";
    int note_y = canvas.h - text_height(TEXT_SMALL) - 14;
    int below = y_background + lh + 10 + tile_h + 8 + text_height(TEXT_BODY) + 12;
    text_draw(&canvas, (canvas.w - text_width(note, TEXT_SMALL)) / 2, note_y > below ? note_y : below,
              note, TEXT_SMALL, t->fg_dim);
    (void)lh;
    k86_present(0, 0, canvas.w, canvas.h);
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

static void set_lock_wallpaper(const char *w, const char *what)
{
    memset(lock.wallpaper, 0, sizeof(lock.wallpaper));
    memcpy(lock.wallpaper, w, strlen(w) < sizeof(lock.wallpaper) ? strlen(w) : sizeof(lock.wallpaper) - 1);
    k86_logf("settings: lock screen background %s", what);
    k86_lock_settings(&lock, 1);
    k86_lock_settings(&lock, 0);
    draw();
}

static void choose(const struct spot *s)
{
    if (bg_lock && (s->kind == K_WALLPAPER || s->kind == K_NONE_WALLPAPER)) {
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
    case K_BG_TARGET:
        bg_lock = s->index;
        k86_lock_settings(&lock, 0);
        layout();
        draw();
        return;
    case K_LOCK_NOW:
        k86_log("settings: lock now");
        k86_lock();
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
        draw();
        return;
    default:
        break;
    }
    switch (s->kind) {
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
        draw();                     /* no theme change: redraw here */
        return;
    case K_PREVIEW:
        k86_log("settings: screen saver preview");
        k86_screensaver_preview();
        return;
    default:
        return;
    }
    k86_appearance(&app);           /* the redraw comes with K86_EVENT_THEME */
}

static void pointer(const struct k86_event *ev)
{
    struct wg_pointer p = wg_pointer_make(ev->x, ev->y, ev->buttons, ev->changed, ev->time_ms);
    int over = spot_at(p.x, p.y), old_hover = hover, old_pressed = pressed;
    hover = over;
    if (p.down)
        pressed = over;
    if (p.up) {
        int was = pressed;
        pressed = -1;
        if (was >= 0 && was == over) {
            choose(&spots[was]);
            return;
        }
    }
    if (hover != old_hover || pressed != old_pressed)
        draw();
}

static void key(const struct key_event *k)
{
    if (!k->pressed)
        return;
    /* Left/Right walk the accents; L and D pick the style. */
    if (k->key == KEY_LEFT && app.accent > 0)
        choose(&(struct spot){ K_ACCENT, app.accent - 1, { 0, 0, 0, 0 } });
    else if (k->key == KEY_RIGHT && app.accent + 1 < app.naccents)
        choose(&(struct spot){ K_ACCENT, app.accent + 1, { 0, 0, 0, 0 } });
    else if (k->ascii == 'l' || k->ascii == 'd')
        choose(&(struct spot){ K_STYLE, k->ascii == 'l', { 0, 0, 0, 0 } });
}

static void header(void)
{
    struct k86_header h;
    memset(&h, 0, sizeof(h));
    static const char *const titles[] = { "Appearance", "Users", "Network" };
    memcpy(h.title, titles[page], strlen(titles[page]) + 1);
    static const struct { int id; const char *label; } b[] = { { PAGE_APPEARANCE, "Appearance" },
                                                               { PAGE_USERS, "Users" },
                                                               { PAGE_NETWORK, "Network" } };
    h.nbuttons = has_network ? 3 : 2;
    for (int i = 0; i < h.nbuttons; i++) {
        h.buttons[i].side = 0;
        h.buttons[i].id = b[i].id;
        h.buttons[i].selected = i == page;      /* the page showing: its tab stands out */
        memcpy(h.buttons[i].label, b[i].label, strlen(b[i].label) + 1);
    }
    k86_header(&h);
}

static void show_page(int p)
{
    if (p == page)
        return;
    page = p;
    static const char *const names[] = { "appearance", "users", "network" };
    k86_logf("settings: page %s", names[page]);
    header();
    k86_timer_set(page == 2 ? 2000 : 0);    /* the Network page refreshes itself */
    if (page == 1)
        users_page_open(&canvas);
    else if (page == 2)
        network_page_open(&canvas);
    else {
        layout();
        draw();
    }
}

int main(void)
{
    if (k86_window_open(&win))
        return 1;
    canvas = (struct gfx_surface){ win.canvas, win.w, win.h, win.stride };
    has_network = network_page_available();
    header();
    k86_appearance(&app);
    k86_lock_settings(&lock, 0);
    for (int i = 0; i < app.nwallpapers; i++)
        have_thumb[i] = k86_wallpaper_thumb(app.wallpapers[i], thumbs[i], THUMB_W, THUMB_H) == 0;
    layout();
    draw();
    k86_log("settings: open");

    for (;;) {
        struct k86_event ev;
        if (k86_wait_event(&ev))
            return 1;
        switch (ev.type) {
        case K86_EVENT_POINTER:
            if (page == 1)
                users_page_pointer(&ev);
            else if (page == 2)
                network_page_pointer(&ev);
            else
                pointer(&ev);
            break;
        case K86_EVENT_KEY:
            if (page == 1)
                users_page_key(&ev.key);
            else if (page == 2)
                network_page_key(&ev.key);
            else
                key(&ev.key);
            break;
        case K86_EVENT_HEADER:
            show_page(ev.id == PAGE_USERS ? 1 : ev.id == PAGE_NETWORK ? 2 : 0);
            break;
        case K86_EVENT_TIMER:
            if (page == 2)
                network_page_tick();
            break;
        case K86_EVENT_THEME:
            k86_lock_settings(&lock, 0);
            layout();
            if (page == 1)
                users_page_draw();
            else if (page == 2)
                network_page_draw();
            else
                draw();
            break;
        case K86_EVENT_RESIZE:
            canvas.w = ev.x;
            canvas.h = ev.y;
            layout();
            if (page == 1) {
                users_page_layout();
                users_page_draw();
            } else if (page == 2) {
                network_page_layout();
                network_page_draw();
            } else {
                draw();
            }
            break;
        case K86_EVENT_CLOSE:   return 0;
        default:                break;
        }
    }
}
