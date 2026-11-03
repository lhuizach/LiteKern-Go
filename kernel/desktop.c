/* LiteKern X — the shell: top bar, dock, app menu, windows' animations, and
 * the compositor that puts it all on screen (see kernel/desktop.h). */
#include "kernel/desktop.h"
#include "kernel/text.h"
#include "kernel/apps.h"
#include "kernel/console.h"
#include "kernel/driver.h"
#include "kernel/icon.h"
#include "kernel/input.h"
#include "kernel/lkx.h"
#include "kernel/pmm.h"
#include "kernel/power.h"
#include "kernel/timing.h"
#include "kernel/printk.h"
#include "kernel/rtc.h"
#include "kernel/screen.h"
#include "kernel/lockscreen.h"
#include "kernel/machine.h"
#include "kernel/net.h"
#include "kernel/settings.h"
#include "kernel/users.h"
#include "kernel/string.h"
#include "kernel/theme.h"
#include "kernel/wallpaper.h"
#include "kernel/ramdisk.h"
#include "kernel/splash.h"
#include "kernel/cursor.h"
#include "defaults.h"
#include "kernel/wm.h"

#define GRID_TOP 48         /* gap between the top bar and the app menu */
#define TILE_RADIUS 12
#define BAR_BUTTON_W 40     /* top bar buttons */
#define BAR_BUTTON_H 24
#define DOCK_ICON 48
#define DOCK_CELL 60
#define SEP_CELL 17         /* the line between favourites and the rest */
#define DOCK_PAD 8
#define DOCK_GAP 8          /* from the bottom of the screen */
#define DOCK_H (DOCK_ICON + 2 * DOCK_PAD)
#define DOCK_HIDDEN (DOCK_H + DOCK_GAP + 10)    /* how far down the dock goes to hide */
#define REVEAL_EDGE 2       /* px from the bottom that bring a hidden dock up */
#define HIDE_DELAY_MS 600   /* the dock stays this long after the pointer leaves it */
#define MENU_W 180
#define MENU_ITEM_H 34
#define MENU_PAD 6
#define MENU_ITEMS 4        /* Restart, Shut Down, Lock, Log Out: new ones last, so the others stay put */
#define menu_items() (users_need_login() ? 4 : 3)   /* Log Out: only with accounts */
#define WHITE 0xffffff
/* The shell (top bar, its menus, the dock) stays dark in both styles, like GNOME Shell. */
#define SHELL_FG      0xffffff
#define SHELL_POPOVER 0x36363a
#define DOT_GREY      0x8e8e93
#define DOT_W 6             /* a dot under an open app ... */
#define PILL_W 20           /* ... and the focused one's pill */

/* Translucent white over the wallpaper, as alpha out of 255. */
#define A_DOCK      20      /* the dock: 8% */
#define A_HOVER     38      /* 15% */
#define A_PRESSED   64      /* 25% */
#define A_DIM       90      /* black over the wallpaper while the app menu is open */

static int started;         /* desktop_start() ran: until then, the windows get the pointer */
static int covered;         /* the screen saver has the screen (desktop_cover) */
static int menu_open;
static int grid_open;       /* the app menu */
static struct rtc_time clock_shown;
static uint32_t last_tick = ~0u;
static device_t *rtc;

/* The app each window belongs to: app_win[i] is builtin_apps[i]'s. */
static struct wm_window *app_win[APPS_MAX];
static uint32_t app_seq[APPS_MAX], seq;     /* the order they were opened */

static const struct theme *T(void)
{
    return theme_get();
}

static struct gfx_surface *S(void)
{
    return screen_surface();
}

static int inside(struct gfx_rect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static int app_index(const struct app *a)
{
    return (int)(a - builtin_apps);
}

static struct wm_window *window_of(int i)
{
    return i >= 0 && i < builtin_app_count ? app_win[i] : 0;
}

/* --- the compositor ---------------------------------------------------------------- */

#define MAX_DAMAGE 16
static struct gfx_rect dmg[MAX_DAMAGE];
static int ndmg, flushing;

/* The view being composed: something at screen (x, y) goes to VS at
 * (VX(x), VY(y)). */
static struct gfx_view *V;
#define VS (&V->s)
#define VX(x) ((x) - V->ox)
#define VY(y) ((y) - V->oy)

static int touches(struct gfx_rect a, struct gfx_rect b)
{
    return a.x <= b.x + b.w && b.x <= a.x + a.w && a.y <= b.y + b.h && b.y <= a.y + a.h;
}

void shell_damage(struct gfx_rect r)
{
    if (!screen_ready())
        return;
    r = gfx_rect_intersect(r, (struct gfx_rect){ 0, 0, S()->w, S()->h });
    if (gfx_rect_empty(r))
        return;
    for (int i = 0; i < ndmg;) {
        if (touches(r, dmg[i])) {
            r = gfx_rect_union(r, dmg[i]);
            dmg[i] = dmg[--ndmg];
            i = 0;
        } else {
            i++;
        }
    }
    if (ndmg == MAX_DAMAGE) {
        for (int i = 0; i < ndmg; i++)
            r = gfx_rect_union(r, dmg[i]);
        ndmg = 0;
    }
    dmg[ndmg++] = r;
}

static void damage_all(void)
{
    if (screen_ready())
        shell_damage((struct gfx_rect){ 0, 0, S()->w, S()->h });
}

static void draw_home(void);
static int home_shown(void);
static void draw_topbar(void);
static void draw_menu(void);

/* Everything in r, bottom to top. */
static void compose(struct gfx_rect r)
{
    struct gfx_view v = { gfx_sub(S(), r), r.x, r.y };
    struct gfx_view *outer = V;
    V = &v;
    wallpaper_draw(VS, v.ox, v.oy, 0);
    if (home_shown())
        draw_home();
    else
        wm_draw(VS, v.ox, v.oy);
    if (started) {
        draw_topbar();
        if (menu_open)
            draw_menu();
    }
    V = outer;
}

void shell_flush(void)
{
    if (flushing || !screen_ready() || covered)
        return;                 /* a log line while composing: the loop below gets it;
                                 * covered: the damage waits for desktop_cover(0) */
    flushing = 1;
    uint32_t t0 = uptime_ms();
    int drew = ndmg > 0;
    while (ndmg) {
        struct gfx_rect list[MAX_DAMAGE];
        int n = ndmg;
        memcpy(list, dmg, sizeof(list[0]) * (uint32_t)n);
        ndmg = 0;
        for (int i = 0; i < n; i++) {
            compose(list[i]);
            screen_damage(list[i].x, list[i].y, list[i].w, list[i].h);
        }
    }
    screen_present();
    flushing = 0;
    /* How a drag (or resize) went, for measuring on the EeePC: logged when
     * it ends. */
    static uint32_t drag_redraws, drag_slowest;
    if (wm_grabbed() && drew) {
        uint32_t dt = uptime_ms() - t0;
        drag_redraws++;
        if (dt > drag_slowest)
            drag_slowest = dt;
    } else if (!wm_grabbed() && drag_redraws) {
        /* Reset first: with the Log app open, kprintf draws into its window,
         * which comes back here, and would log again and again until the
         * kernel stack overflowed. */
        uint32_t n = drag_redraws, slowest = drag_slowest;
        drag_redraws = drag_slowest = 0;
        kprintf("desktop: drag: %u redraws, slowest %u ms\n", n, slowest);
    }
}

/* --- springs: everything in the dock moves on one ------------------------------------
 *
 * A value chases its target with a little momentum, so it overshoots a bit
 * and settles: the "blobby" feel. Fixed point, 1/256 units, one step per
 * TICK_MS. */

/* Recordings only (make EXTRA_CFLAGS=-DANIM_SLOW=6): everything N times
 * slower, so screenshots can catch the frames in between. */
#ifndef ANIM_SLOW
#define ANIM_SLOW 1
#endif

#define TICK_MS (16 * ANIM_SLOW)
#define FX(v) ((v) * 256)

struct spring {
    int x, v, t;
};

static int spring_step(struct spring *s, int k, int d)
{
    if (s->x == s->t && !s->v)
        return 0;
    s->v += (s->t - s->x) * k / 256 - s->v * d / 256;
    s->x += s->v;
    int dx = s->t - s->x;
    if (dx > -24 && dx < 24 && s->v > -24 && s->v < 24) {
        s->x = s->t;
        s->v = 0;
    }
    return 1;
}

static void spring_snap(struct spring *s)
{
    s->x = s->t;
    s->v = 0;
}

/* --- the dock ----------------------------------------------------------------------- */

#define I_SEP  APPS_MAX         /* the line */
#define I_APPS (APPS_MAX + 1)   /* Show Apps */
#define NITEMS (APPS_MAX + 2)

struct item {
    int in;                     /* in the dock now */
    int drawn;                  /* on screen (in, or still shrinking away) */
    struct spring pos;          /* centre x */
    struct spring scale;        /* FX(1): full size */
    struct spring dot_w, dot_white, dot_alpha;
};

static struct item items[NITEMS];
static struct spring dock_w;    /* the dock's width */
static struct spring reveal;    /* how far down it is: 0 up, FX(DOCK_HIDDEN) hidden */
static int peek;                /* brought up by the bottom edge over a maximised window */
static uint32_t hide_at, last_dock_tick;
static int dock_moving;
static char dock_logged[96];

static int dock_y(void)         /* its top, where it is now */
{
    return S()->h - DOCK_GAP - DOCK_H + reveal.x / 256;
}

static int dock_base_y(void)    /* its top when it's up */
{
    return S()->h - DOCK_GAP - DOCK_H;
}

/* The band the dock and its labels can draw in. */
static struct gfx_rect dock_band(void)
{
    int top = dock_base_y() - text_height(TEXT_BODY) - 24;
    return (struct gfx_rect){ 0, top, S()->w, S()->h - top };
}

/* The dock hides while a maximised window is in front (unless the pointer
 * brought it up), and never while the app menu is open. */
static int hide_mode(void)
{
    struct wm_window *top = wm_top_visible();
    return started && !grid_open && top && wm_is_maximised(top);
}

static int dock_up(void)        /* up enough to use */
{
    return started && reveal.x < FX(DOCK_H / 2);
}

static int dot_state(int i)     /* 0 closed, 1 minimised, 2 on screen, 3 focused */
{
    struct wm_window *w = window_of(i);
    if (!w)
        return 0;
    if (wm_is_minimised(w))
        return 1;
    return w == wm_focused() ? 3 : 2;
}

/* Work out where everything in the dock should be, and let it move there. */
static void dock_layout(int instant)
{
    int order[NITEMS], n = 0, cells[NITEMS];
    for (int i = 0; i < builtin_app_count; i++)
        if (builtin_apps[i].pinned)
            order[n++] = i;
    int first_other = n;
    for (;;) {                  /* the others that are open, oldest first */
        int best = -1;
        for (int i = 0; i < builtin_app_count; i++) {
            int listed = 0;
            for (int k = first_other; k < n; k++)
                listed |= order[k] == i;
            if (!builtin_apps[i].pinned && app_win[i] && !listed &&
                (best < 0 || app_seq[i] < app_seq[best]))
                best = i;
        }
        if (best < 0)
            break;
        order[n++] = best;
    }
    if (n > first_other) {      /* the line goes before them */
        for (int k = n; k > first_other; k--)
            order[k] = order[k - 1];
        order[first_other] = I_SEP;
        n++;
    }
    order[n++] = I_APPS;

    int total = 2 * DOCK_PAD;
    for (int k = 0; k < n; k++) {
        cells[k] = order[k] == I_SEP ? SEP_CELL : DOCK_CELL;
        total += cells[k];
    }
    for (int i = 0; i < NITEMS; i++)
        items[i].in = 0;
    int x = (S()->w - total) / 2 + DOCK_PAD;
    for (int k = 0; k < n; k++) {
        struct item *it = &items[order[k]];
        it->in = 1;
        it->pos.t = FX(x + cells[k] / 2);
        if (!it->drawn) {       /* new: it pops up where it belongs */
            it->pos.x = it->pos.t;
            it->pos.v = 0;
            it->scale.x = 0;
        }
        it->drawn = 1;
        it->scale.t = FX(1);
        x += cells[k];
    }
    for (int i = 0; i < NITEMS; i++)
        if (!items[i].in)
            items[i].scale.t = 0;               /* leaving: it shrinks away */
    dock_w.t = FX(total);

    for (int i = 0; i < builtin_app_count; i++) {
        struct item *it = &items[i];
        int st = dot_state(i);
        it->dot_alpha.t = st ? FX(1) : 0;
        if (st) {
            it->dot_w.t = FX(st == 3 ? PILL_W : DOT_W);
            it->dot_white.t = st >= 2 ? FX(1) : 0;
            if (!it->dot_alpha.x)               /* appearing: start as a dot */
                it->dot_w.x = FX(DOT_W);
        }
    }
    reveal.t = hide_mode() && !peek ? FX(DOCK_HIDDEN) : 0;

    if (instant) {
        for (int i = 0; i < NITEMS; i++) {
            struct item *it = &items[i];
            spring_snap(&it->pos);
            spring_snap(&it->scale);
            spring_snap(&it->dot_w);
            spring_snap(&it->dot_white);
            spring_snap(&it->dot_alpha);
            it->drawn = it->in;
        }
        spring_snap(&dock_w);
        spring_snap(&reveal);
    }
    dock_moving = 1;
    shell_damage(dock_band());

    /* Log what's in it when that changes (the tests read it). */
    char line[96];
    int len = 0;
    for (int k = 0; k < n && len < 80; k++) {
        const char *name = order[k] == I_SEP ? "|" : order[k] == I_APPS ? "Show Apps"
                                                                      : builtin_apps[order[k]].name;
        if (k)
            line[len++] = ' ';
        for (int c = 0; name[c] && len < 90; c++)
            line[len++] = name[c];
    }
    line[len] = '\0';
    if (strcmp(line, dock_logged)) {
        memcpy(dock_logged, line, (uint32_t)len + 1);
        kprintf("dock: %s\n", line);
    }
}

/* One step of every spring; 1 while anything still moves. */
static int dock_step(void)
{
    int moving = 0;
    for (int i = 0; i < NITEMS; i++) {
        struct item *it = &items[i];
        if (!it->drawn)
            continue;
        moving |= spring_step(&it->pos, 50, 100);
        moving |= spring_step(&it->scale, 46, 80);
        moving |= spring_step(&it->dot_w, 50, 90);
        moving |= spring_step(&it->dot_white, 40, 100);
        moving |= spring_step(&it->dot_alpha, 40, 100);
        if (!it->in && it->scale.x <= 0 && it->scale.t == 0) {
            it->drawn = 0;
            moving = 1;
        }
    }
    moving |= spring_step(&dock_w, 50, 100);
    moving |= spring_step(&reveal, 44, 90);
    return moving;
}

static void dock_tick(void)
{
    uint32_t now = uptime_ms();
    if (hide_at && (int32_t)(now - hide_at) >= 0) {
        hide_at = 0;
        peek = 0;
        reveal.t = hide_mode() ? FX(DOCK_HIDDEN) : 0;
        dock_moving = 1;
    }
    if (!dock_moving) {
        last_dock_tick = now;
        return;
    }
    uint32_t steps = (now - last_dock_tick) / TICK_MS;
    if (!steps)
        return;
    if (steps > 4)
        steps = 4;              /* slow machine: skip ahead rather than crawl */
    last_dock_tick = now;
    int moving = 0;
    for (uint32_t s = 0; s < steps; s++)
        moving = dock_step();
    dock_moving = moving;
    shell_damage(dock_band());
    shell_flush();
}

/* An item's centre on screen, with the dock narrowing as it sinks (the
 * blob look when it rises). */
static int dock_squash(void)    /* 1024: full width */
{
    int f = reveal.x / (DOCK_HIDDEN > 0 ? DOCK_HIDDEN : 1);     /* 0..256 */
    if (f < 0)
        f = 0;
    return 1024 - f * 330 / 256;
}

static int item_x(int i)
{
    int c = S()->w / 2;
    return c + (items[i].pos.x / 256 - c) * dock_squash() / 1024;
}

static __attribute__((unused)) struct gfx_rect item_rect(int i)
{
    return (struct gfx_rect){ item_x(i) - DOCK_CELL / 2, dock_y() + 2, DOCK_CELL, DOCK_H - 4 };
}

static struct gfx_rect dock_rect(void)
{
    int w = dock_w.x / 256 * dock_squash() / 1024;
    return (struct gfx_rect){ (S()->w - w) / 2, dock_y(), w, DOCK_H };
}

/* Where an app's icon sits in the dock (where its window shrinks to), up. */
static struct gfx_rect dock_icon_rect(int i)
{
    int cx = items[i].pos.t / 256;
    return (struct gfx_rect){ cx - DOCK_ICON / 2, dock_base_y() + DOCK_PAD, DOCK_ICON, DOCK_ICON };
}

/* --- hit testing ---------------------------------------------------------------------- */

enum hit_kind { H_NONE, H_HOME, H_POWER, H_NET, H_BATTERY, H_WBTN, H_WCTL, H_RESTART, H_SHUTDOWN, H_LOCK, H_LOGOUT, H_DOCK, H_APPS, H_DOCK_BG, H_TILE,
                H_LIST,     /* a row of All apps */
                H_BAR };

static const enum hit_kind menu_kinds[MENU_ITEMS] = { H_RESTART, H_SHUTDOWN, H_LOCK, H_LOGOUT };

struct hit {
    enum hit_kind kind;
    int app;
};

static struct hit hover, pressed;
static uint8_t last_buttons;

static int same(struct hit a, struct hit b)
{
    return a.kind == b.kind && a.app == b.app;
}

static struct gfx_rect home_rect(void)
{
    return (struct gfx_rect){ T()->spacing, (T()->topbar_h - BAR_BUTTON_H) / 2, BAR_BUTTON_W, BAR_BUTTON_H };
}

static struct gfx_rect power_rect(void)
{
    return (struct gfx_rect){ S()->w - T()->spacing - BAR_BUTTON_W, (T()->topbar_h - BAR_BUTTON_H) / 2,
                              BAR_BUTTON_W, BAR_BUTTON_H };
}

/* The network icon, left of the power button (only with network hardware). */
static struct k86_net net_shown;
static int net_present(void) { return net_shown.wired != K86_NET_NONE || net_shown.wifi != K86_NET_NONE; }

static struct gfx_rect net_rect(void)
{
    struct gfx_rect p = power_rect();
    return (struct gfx_rect){ p.x - BAR_BUTTON_W - 4, p.y, BAR_BUTTON_W, BAR_BUTTON_H };
}

/* The battery (kernel/machine.h), left of those: its icon and how full, "87%"
 * (only with a battery). */
static struct k86_machine machine_shown = { .battery = -1 };
static int battery_present(void) { return machine_shown.battery >= 0; }

static void battery_text(char out[8])
{
    int p = machine_shown.battery, n = 0;
    if (p >= 100)
        out[n++] = '1';
    if (p >= 10)
        out[n++] = (char)('0' + p / 10 % 10);
    out[n++] = (char)('0' + p % 10);
    out[n++] = '%';
    out[n] = '\0';
}

static struct gfx_rect battery_rect(void)
{
    char text[8];
    battery_text(text);
    int w = 10 + 21 + 6 + text_width(text, TEXT_BOLD) + 10;
    int right = (net_present() ? net_rect().x : power_rect().x) - 4;
    return (struct gfx_rect){ right - w, power_rect().y, w, BAR_BUTTON_H };
}

/* Where the status icons start: what's left of it is the top bar's. */
static int status_left(void)
{
    return battery_present() ? battery_rect().x : net_present() ? net_rect().x : power_rect().x;
}

/* --- a maximised window's header, in the top bar -------------------------------------------
 * The focused window, when it's maximised, has no header bar (kernel/wm.h:
 * wm_header_hidden): its buttons follow the app's name on the left (Settings'
 * tabs: the current one with a soft background), its right-hand buttons and
 * its minimise/restore/close sit left of the network and power buttons. */
#define WBAR_H     24
#define WBAR_GAP   4
#define WCTL_SIZE  24
#define WCTL_GAP   8

static const char *focused_name(void);

static struct wm_window *merged(void)
{
    struct wm_window *w = wm_focused();
    return w && !grid_open && wm_header_hidden(w) && !wm_is_minimised(w) ? w : 0;
}

/* Where w's buttons (btn[i], in the order added) and controls (ctl[0..2]:
 * minimise, restore, close) go in the top bar. */
static void wbar_layout(struct wm_window *w, struct gfx_rect btn[WM_MAX_BUTTONS], struct gfx_rect ctl[3])
{
    const struct theme *t = T();
    int y = (t->topbar_h - WBAR_H) / 2;
    int left = t->spacing + BAR_BUTTON_W + t->spacing * 2 + text_width(focused_name(), TEXT_BOLD) + 14;
    int right = status_left() - 10;
    for (int k = 2; k >= 0; k--) {
        ctl[k] = (struct gfx_rect){ right - WCTL_SIZE, (t->topbar_h - WCTL_SIZE) / 2, WCTL_SIZE, WCTL_SIZE };
        right -= WCTL_SIZE + WCTL_GAP;
    }
    right -= 6;
    for (int i = 0; i < WM_MAX_BUTTONS; i++) {
        enum wm_side side;
        enum wm_icon icon;
        const char *label;
        int sel;
        if (wm_button_info(w, i, &side, &icon, &label, &sel)) {
            btn[i] = (struct gfx_rect){ 0, 0, 0, 0 };
            continue;
        }
        int bw = icon != WM_ICON_NONE ? 28 : text_width(label, TEXT_BOLD) + 22;
        if (side == WM_LEFT) {
            btn[i] = (struct gfx_rect){ left, y, bw, WBAR_H };
            left += bw + WBAR_GAP;
        } else {
            btn[i] = (struct gfx_rect){ right - bw, y, bw, WBAR_H };
            right -= bw + WBAR_GAP;
        }
    }
}

static struct gfx_rect menu_rect(void)
{
    return (struct gfx_rect){ S()->w - T()->spacing - MENU_W, T()->topbar_h + 4, MENU_W,
                              menu_items() * MENU_ITEM_H + 2 * MENU_PAD };
}

static struct gfx_rect menu_item_rect(int k)
{
    struct gfx_rect m = menu_rect();
    return (struct gfx_rect){ m.x + MENU_PAD, m.y + MENU_PAD + k * MENU_ITEM_H, MENU_W - 2 * MENU_PAD,
                              MENU_ITEM_H };
}

/* --- the home screen (LiteKern Go, UI-PLAN.md) ----------------------------------------
 *
 * Shown whenever no app is: a clock, then the apps in pages of 4 x 5 icons,
 * then the last page, All apps, every app A to Z. Swiping sideways turns the
 * pages; dots at the bottom show which one. No dock: Home, Back and Recent
 * apps are the phone's keys. */

#define HOME_COLS 4
#define HOME_ROWS 5
#define HOME_PER_PAGE (HOME_COLS * HOME_ROWS)
#define HOME_ICON 112
#define HOME_CLOCK_H 220        /* the clock and date, above the icons */
#define HOME_DOTS_H 48          /* the page dots, at the bottom */
#define LIST_ICON 64            /* All apps' rows */
#define SWIPE_SLOP 24           /* px a finger moves before it's a swipe, not a tap */

static struct spring pager;     /* how far the pages are turned: FX(page * screen width) */
static int page;                /* the page shown, or being turned to */
static int pager_moving;
static uint32_t last_pager_tick;

static int home_shown(void)
{
    return started && !wm_top_visible() && !covered;
}

static int home_pages(void)     /* pages of icons; All apps comes after them */
{
    return builtin_app_count ? (builtin_app_count + HOME_PER_PAGE - 1) / HOME_PER_PAGE : 1;
}

static int page_x(int p)        /* page p's left edge on screen, as turned now */
{
    return p * S()->w - pager.x / 256;
}

static struct gfx_rect tile_rect(int i)
{
    const struct theme *t = T();
    int p = i / HOME_PER_PAGE, k = i % HOME_PER_PAGE;
    int x0 = page_x(p) + (S()->w - HOME_COLS * t->tile_w) / 2;
    int y0 = t->topbar_h + HOME_CLOCK_H;
    return (struct gfx_rect){ x0 + (k % HOME_COLS) * t->tile_w, y0 + (k / HOME_COLS) * t->tile_h,
                              t->tile_w, t->tile_h };
}

static struct gfx_rect tile_icon_rect(int i)
{
    struct gfx_rect r = tile_rect(i);
    return (struct gfx_rect){ r.x + (r.w - HOME_ICON) / 2, r.y + 16, HOME_ICON, HOME_ICON };
}

/* All apps, A to Z: the k-th row's app. */
static int list_app(int k)
{
    int order[APPS_MAX], n = builtin_app_count;
    for (int i = 0; i < n; i++)
        order[i] = i;
    for (int i = 1; i < n; i++)             /* a handful of apps: insertion sort */
        for (int j = i; j > 0 && strcmp(builtin_apps[order[j - 1]].name, builtin_apps[order[j]].name) > 0; j--) {
            int tmp = order[j];
            order[j] = order[j - 1];
            order[j - 1] = tmp;
        }
    return order[k];
}

static int list_top(void)
{
    return T()->topbar_h + T()->margin + text_height(TEXT_HEADING) + T()->margin;
}

static struct gfx_rect list_rect(int k)
{
    const struct theme *t = T();
    return (struct gfx_rect){ page_x(home_pages()) + t->margin, list_top() + k * t->row_h,
                              S()->w - 2 * t->margin, t->row_h };
}

static void turn_to(int p, int instant)
{
    if (p < 0)
        p = 0;
    if (p > home_pages())
        p = home_pages();
    if (p != page)
        kprintf("home: page %d of %d%s\n", p + 1, home_pages() + 1, p == home_pages() ? " (All apps)" : "");
    page = p;
    pager.t = FX(p * S()->w);
    if (instant)
        spring_snap(&pager);
    pager_moving = pager.x != pager.t || pager.v;
    last_pager_tick = uptime_ms();
    damage_all();
}

static void pager_tick(void)
{
    if (!pager_moving)
        return;
    uint32_t now = uptime_ms(), steps = (now - last_pager_tick) / TICK_MS;
    if (!steps)
        return;
    last_pager_tick = now;
    for (uint32_t s = 0; s < (steps > 4 ? 4 : steps); s++)
        pager_moving = spring_step(&pager, 40, 90);
    damage_all();
    shell_flush();
}

/* A finger dragging the pages: where it went down, and whether it's a swipe yet. */
static int swipe_x0 = -1, swipe_on, swipe_from;

static struct hit hit_at(int x, int y)
{
    if (menu_open) {
        for (int k = 0; k < menu_items(); k++)
            if (inside(menu_item_rect(k), x, y))
                return (struct hit){ menu_kinds[k], 0 };
        if (inside(menu_rect(), x, y))
            return (struct hit){ H_BAR, 0 };
    }
    if (y < T()->topbar_h) {
        if (inside(home_rect(), x, y))
            return (struct hit){ H_HOME, 0 };
        if (inside(power_rect(), x, y))
            return (struct hit){ H_POWER, 0 };
        if (net_present() && inside(net_rect(), x, y))
            return (struct hit){ H_NET, 0 };
        if (battery_present() && inside(battery_rect(), x, y))
            return (struct hit){ H_BATTERY, 0 };
        struct wm_window *w = merged();
        if (w) {
            struct gfx_rect btn[WM_MAX_BUTTONS], ctl[3];
            wbar_layout(w, btn, ctl);
            for (int k = 0; k < 3; k++)
                if (inside(ctl[k], x, y))
                    return (struct hit){ H_WCTL, k };
            for (int i = 0; i < WM_MAX_BUTTONS; i++)
                if (inside(btn[i], x, y))
                    return (struct hit){ H_WBTN, i };
        }
        return (struct hit){ H_BAR, 0 };
    }
    if (home_shown() && !swipe_on) {
        for (int i = 0; i < builtin_app_count; i++)
            if (inside(tile_rect(i), x, y))
                return (struct hit){ H_TILE, i };
        for (int k = 0; k < builtin_app_count; k++)
            if (inside(list_rect(k), x, y))
                return (struct hit){ H_LIST, k };
    }
    return (struct hit){ H_NONE, 0 };
}

static void damage_hit(struct hit h)
{
    switch (h.kind) {
    case H_HOME:     shell_damage(home_rect()); break;
    case H_POWER:    shell_damage(power_rect()); break;
    case H_NET:      shell_damage(net_rect()); break;
    case H_BATTERY:  shell_damage(battery_rect()); break;
    case H_WBTN:
    case H_WCTL:     shell_damage((struct gfx_rect){ 0, 0, S()->w, T()->topbar_h }); break;
    case H_RESTART:
    case H_SHUTDOWN:
    case H_LOCK:
    case H_LOGOUT:   shell_damage(menu_rect()); break;
    case H_DOCK:
    case H_APPS:     shell_damage(dock_band()); break;
    case H_TILE:     shell_damage(tile_rect(h.app)); break;
    case H_LIST:     shell_damage(list_rect(h.app)); break;
    default:         break;
    }
}

/* --- drawing (into the view being composed) --------------------------------------------- */

static int visible(struct gfx_rect r)
{
    return !gfx_rect_empty(gfx_rect_intersect(r, (struct gfx_rect){ V->ox, V->oy, V->s.w, V->s.h }));
}

static void draw_icon(const struct app *a, struct gfx_rect r, uint32_t alpha)
{
    const struct theme *t = T();
    const struct icon *ic = icon_find(a->icon);
    if (ic) {
        gfx_blit_alpha_scaled(VS, (struct gfx_rect){ VX(r.x), VY(r.y), r.w, r.h }, ic->px, ic->w, ic->h,
                              ic->w, alpha);
    } else {                    /* no icon built in: its initial on an accent tile */
        char initial[2] = { a->name[0], '\0' };
        gfx_blend_round_rect(VS, VX(r.x), VY(r.y), r.w, r.h, r.w * 10 / 48, t->accent_bg, alpha);
        if (r.w >= 40)
            text_draw(VS, VX(r.x) + (r.w - text_width(initial, TEXT_HEADING)) / 2,
                      VY(r.y) + (r.h - text_height(TEXT_HEADING)) / 2, initial, TEXT_HEADING, t->accent_fg);
    }
}

static void draw_home_icon(int cx, int cy, uint32_t c)
{
    for (int i = 0; i <= 7; i++)                    /* roof */
        gfx_fill_rect(VS, VX(cx - i), VY(cy - 7 + i), 2 * i + 1, 1, c);
    gfx_fill_rect(VS, VX(cx - 5), VY(cy + 1), 11, 6, c);      /* walls */
    gfx_fill_rect(VS, VX(cx - 1), VY(cy + 3), 3, 4, T()->topbar_bg);   /* door */
}

/* The network (kernel/net.h): Wi-Fi as a fan whose arcs light up with the
 * signal, Ethernet as three linked boxes; dimmed when not connected. */
static void draw_net_icon(int cx, int cy, const struct k86_net *n, uint32_t bg)
{
    uint32_t lit = SHELL_FG, dim = gfx_mix(bg, SHELL_FG, 160);
    int wifi = n->wifi != K86_NET_NONE && (n->wifi == K86_NET_UP || n->wired != K86_NET_UP);
    if (wifi) {
        int up = n->wifi == K86_NET_UP;
        int bars = !up ? 0 : n->signal >= 66 ? 3 : n->signal >= 33 ? 2 : 1;
        int ox = cx, oy = cy + 6;
        /* Three arcs, 45 degrees either side of up, drawn pixel by pixel:
         * only above the centre, so nothing spills below the top bar. */
        for (int dy = -12; dy < 0; dy++)
            for (int dx = dy; dx <= -dy; dx++) {
                int d2 = dx * dx + dy * dy;
                for (int k = 1; k <= 3; k++)
                    if (d2 > (1 + 3 * k) * (1 + 3 * k) && d2 <= (3 + 3 * k) * (3 + 3 * k))
                        gfx_fill_rect(VS, VX(ox + dx), VY(oy + dy), 1, 1, bars >= k ? lit : dim);
            }
        gfx_fill_circle(VS, VX(ox), VY(oy - 1), 2, up ? lit : dim);
        return;
    }
    uint32_t c = n->wired == K86_NET_UP ? lit : dim;
    gfx_fill_rect(VS, VX(cx - 3), VY(cy - 8), 7, 5, c);         /* the boxes */
    gfx_fill_rect(VS, VX(cx - 10), VY(cy + 3), 7, 5, c);
    gfx_fill_rect(VS, VX(cx + 4), VY(cy + 3), 7, 5, c);
    gfx_fill_rect(VS, VX(cx), VY(cy - 3), 1, 4, c);             /* the links */
    gfx_fill_rect(VS, VX(cx - 7), VY(cy + 1), 15, 1, c);
    gfx_fill_rect(VS, VX(cx - 7), VY(cy + 1), 1, 2, c);
    gfx_fill_rect(VS, VX(cx + 7), VY(cy + 1), 1, 2, c);
}

/* A battery on its side, filled as far as it's full (red when nearly empty),
 * with a lightning bolt while it charges; then "87%". */
static void draw_battery(struct gfx_rect r, const struct k86_machine *m, uint32_t bg)
{
    int x = r.x + 10, cy = r.y + r.h / 2, low = m->battery <= 10 && m->charging == K86_BATTERY_DISCHARGING;
    uint32_t c = SHELL_FG, fill = low ? 0xff6b6b : SHELL_FG;
    gfx_fill_round_rect(VS, VX(x), VY(cy - 6), 19, 12, 3, c);         /* the case */
    gfx_fill_round_rect(VS, VX(x + 2), VY(cy - 4), 15, 8, 1, bg);
    gfx_fill_rect(VS, VX(x + 19), VY(cy - 2), 2, 4, c);               /* the terminal */
    int w = (m->battery * 13 + 50) / 100;
    if (w > 0)
        gfx_fill_rect(VS, VX(x + 3), VY(cy - 3), w, 6, fill);
    if (m->charging == K86_BATTERY_CHARGING) {
        /* The bolt, outlined in the bar's colour so it shows over the fill. */
        static const char *const bolt[] = { "..##", ".##.", "####", ".##.", "##.." };
        for (int pass = 0; pass < 2; pass++)
            for (int j = 0; j < 5; j++)
                for (int i = 0; i < 4; i++)
                    if (bolt[j][i] == '#') {
                        int bx = x + 8 + i, by = cy - 3 + j;
                        if (pass == 0)
                            gfx_fill_rect(VS, VX(bx - 1), VY(by - 1), 3, 3, bg);
                        else
                            gfx_fill_rect(VS, VX(bx), VY(by), 1, 1, c);
                    }
    }
    char text[8];
    battery_text(text);
    text_draw(VS, VX(x + 27), VY(r.y + (r.h - text_height(TEXT_BOLD)) / 2), text, TEXT_BOLD, SHELL_FG);
}

static void draw_power_icon(int cx, int cy, uint32_t c, uint32_t bg)
{
    gfx_fill_circle(VS, VX(cx), VY(cy + 1), 7, c);
    gfx_fill_circle(VS, VX(cx), VY(cy + 1), 5, bg);
    gfx_fill_rect(VS, VX(cx - 3), VY(cy - 7), 7, 5, bg);      /* the gap at the top */
    gfx_fill_rect(VS, VX(cx - 1), VY(cy - 8), 2, 8, c);       /* the stroke */
}

static uint32_t hit_bg(struct hit h, uint32_t base)
{
    if (same(h, pressed) && same(h, hover))
        return gfx_mix(WHITE, base, A_PRESSED);
    if (same(h, hover))
        return gfx_mix(WHITE, base, A_HOVER);
    return base;
}

/* "Wed 30 Sep  14:05", GNOME's format (24-hour); empty without a clock. */
static void clock_text(char text[24])
{
    static const char *const days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    struct rtc_time c = clock_shown;
    int n = 0;
    if (!rtc || !c.month) {
        text[0] = '\0';
        return;
    }
    for (const char *p = days[c.weekday % 7]; *p; p++)
        text[n++] = *p;
    text[n++] = ' ';
    if (c.day >= 10)
        text[n++] = (char)('0' + c.day / 10);
    text[n++] = (char)('0' + c.day % 10);
    text[n++] = ' ';
    for (const char *p = months[(c.month - 1) % 12]; *p; p++)
        text[n++] = *p;
    text[n++] = ' ';
    text[n++] = ' ';
    text[n++] = (char)('0' + c.hour / 10);
    text[n++] = (char)('0' + c.hour % 10);
    text[n++] = ':';
    text[n++] = (char)('0' + c.minute / 10);
    text[n++] = (char)('0' + c.minute % 10);
    text[n] = '\0';
}

static void log_clock(void)
{
    char text[24];
    clock_text(text);
    kprintf("shell: clock %s\n", text);
}

/* The clock's text and where it goes: centred, but never under a maximised
 * window's buttons in the top bar (Settings' tabs): moved right of them, and
 * only the time if even that doesn't fit. */
static void clock_place(char text[24], int *x)
{
    clock_text(text);
    int w = text_width(text, TEXT_BOLD);
    *x = (S()->w - w) / 2;
    struct wm_window *mw = merged();
    if (!mw)
        return;
    struct gfx_rect btn[WM_MAX_BUTTONS], ctl[3];
    wbar_layout(mw, btn, ctl);
    int left_end = 0, right_start = ctl[0].x;
    for (int i = 0; i < WM_MAX_BUTTONS; i++) {
        if (!btn[i].w)
            continue;
        if (btn[i].x < S()->w / 2 && btn[i].x + btn[i].w > left_end)
            left_end = btn[i].x + btn[i].w;
        else if (btn[i].x >= S()->w / 2 && btn[i].x < right_start)
            right_start = btn[i].x;
    }
    if (*x < left_end + 16)
        *x = left_end + 16;
    if (*x + w > right_start - 16) {
        const char *time = text;            /* the time follows the date's two spaces */
        while (*time && !(time[0] == ' ' && time[1] == ' '))
            time++;
        if (*time) {
            memmove(text, time + 2, strlen(time + 2) + 1);
            w = text_width(text, TEXT_BOLD);
        }
        *x = (S()->w - w) / 2 < left_end + 16 ? left_end + 16 : (S()->w - w) / 2;
        if (*x + w > right_start - 8)
            text[0] = '\0';                /* no room at all: the buttons come first */
    }
}

static struct gfx_rect clock_rect(void)
{
    char text[24];
    int x;
    clock_place(text, &x);
    int w = text_width(text, TEXT_BOLD);
    int cx = (S()->w - 220) / 2;            /* covers the centred one too: it may have moved */
    int l = x < cx ? x : cx, r = x + w > cx + 220 ? x + w : cx + 220;
    return (struct gfx_rect){ l - 4, 0, r - l + 8, T()->topbar_h };
}

/* The name of the app in front, after Home. */
static const char *focused_name(void)
{
    struct wm_window *w = wm_focused();
    const struct app *a = w ? wm_owner(w) : 0;
    return a ? a->name : w ? wm_title(w) : "";
}

static void draw_topbar(void)
{
    const struct theme *t = T();
    struct gfx_rect bar = { 0, 0, S()->w, t->topbar_h };
    if (!visible(bar))
        return;
    gfx_fill_rect(VS, VX(0), VY(0), bar.w, bar.h, t->topbar_bg);
    int ty = (t->topbar_h - text_height(TEXT_BOLD)) / 2;
    const char *name = focused_name();
    if (name[0])
        text_draw(VS, VX(t->spacing + BAR_BUTTON_W + t->spacing * 2), VY(ty), name, TEXT_BOLD, SHELL_FG);
    struct wm_window *mw = merged();
    if (mw) {                               /* its header: buttons and controls */
        struct gfx_rect btn[WM_MAX_BUTTONS], ctl[3];
        wbar_layout(mw, btn, ctl);
        for (int i = 0; i < WM_MAX_BUTTONS; i++) {
            enum wm_side side;
            enum wm_icon icon;
            const char *label;
            int sel;
            if (wm_button_info(mw, i, &side, &icon, &label, &sel))
                continue;
            struct gfx_rect b = btn[i];
            uint32_t base = sel ? t->button_bg : t->topbar_bg;
            uint32_t bbg = hit_bg((struct hit){ H_WBTN, i }, base);
            if (bbg != t->topbar_bg)
                gfx_fill_round_rect(VS, VX(b.x), VY(b.y), b.w, b.h, t->radius, bbg);
            if (icon != WM_ICON_NONE)
                wm_draw_header_icon(VS, icon, VX(b.x + b.w / 2), VY(b.y + b.h / 2), SHELL_FG);
            else
                text_draw(VS, VX(b.x + (b.w - text_width(label, TEXT_BOLD)) / 2), VY(ty), label, TEXT_BOLD, SHELL_FG);
        }
        for (int k = 0; k < 3; k++) {
            uint32_t cbg = hit_bg((struct hit){ H_WCTL, k }, t->button_bg);
            wm_draw_control(VS, k, 1, VX(ctl[k].x + WCTL_SIZE / 2), VY(ctl[k].y + WCTL_SIZE / 2), cbg, SHELL_FG);
        }
    }
    char text[24];
    int clock_x;
    clock_place(text, &clock_x);
    if (text[0])
        text_draw(VS, VX(clock_x), VY(ty), text, TEXT_BOLD, SHELL_FG);
    struct gfx_rect r = home_rect();
    uint32_t bg = hit_bg((struct hit){ H_HOME, 0 }, t->topbar_bg);
    gfx_fill_round_rect(VS, VX(r.x), VY(r.y), r.w, r.h, r.h / 2, bg);
    draw_home_icon(r.x + r.w / 2, r.y + r.h / 2, SHELL_FG);
    r = power_rect();
    bg = menu_open ? gfx_mix(WHITE, t->topbar_bg, A_HOVER) : hit_bg((struct hit){ H_POWER, 0 }, t->topbar_bg);
    gfx_fill_round_rect(VS, VX(r.x), VY(r.y), r.w, r.h, r.h / 2, bg);
    draw_power_icon(r.x + r.w / 2, r.y + r.h / 2, SHELL_FG, bg);
    if (net_present()) {
        r = net_rect();
        bg = hit_bg((struct hit){ H_NET, 0 }, t->topbar_bg);
        gfx_fill_round_rect(VS, VX(r.x), VY(r.y), r.w, r.h, r.h / 2, bg);
        draw_net_icon(r.x + r.w / 2, r.y + r.h / 2, &net_shown, bg);
    }
    if (battery_present()) {
        r = battery_rect();
        bg = hit_bg((struct hit){ H_BATTERY, 0 }, t->topbar_bg);
        gfx_fill_round_rect(VS, VX(r.x), VY(r.y), r.w, r.h, r.h / 2, bg);
        draw_battery(r, &machine_shown, bg);
    }
}

static void draw_menu(void)
{
    struct gfx_rect m = menu_rect();
    if (!visible(m))
        return;
    gfx_fill_round_rect(VS, VX(m.x), VY(m.y), m.w, m.h, 12, SHELL_POPOVER);
    static const char *const labels[MENU_ITEMS] = { "Restart", "Shut Down", "Lock", "Log Out" };
    for (int k = 0; k < menu_items(); k++) {
        struct gfx_rect r = menu_item_rect(k);
        int disabled = menu_kinds[k] == H_SHUTDOWN && !power_can_off();     /* no ACPI found */
        uint32_t bg = disabled ? SHELL_POPOVER : hit_bg((struct hit){ menu_kinds[k], 0 }, SHELL_POPOVER);
        if (bg != SHELL_POPOVER)
            gfx_fill_round_rect(VS, VX(r.x), VY(r.y), r.w, r.h, 6, bg);
        text_draw(VS, VX(r.x + 12), VY(r.y + (r.h - text_height(TEXT_BODY)) / 2), labels[k], TEXT_BODY,
                  disabled ? gfx_mix(SHELL_FG, SHELL_POPOVER, 110) : SHELL_FG);
    }
}

static void draw_home(void)
{
    const struct theme *t = T();
    /* The clock and date: on the first page, turning with it. */
    if (rtc && clock_shown.month) {
        static const char *const days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
                                            "Friday", "Saturday" };
        static const char *const months[] = { "January", "February", "March", "April", "May", "June", "July",
                                              "August", "September", "October", "November", "December" };
        char hm[6] = { (char)('0' + clock_shown.hour / 10), (char)('0' + clock_shown.hour % 10), ':',
                       (char)('0' + clock_shown.minute / 10), (char)('0' + clock_shown.minute % 10), 0 };
        char date[40];
        int n = 0;
        for (const char *q = days[clock_shown.weekday % 7]; *q; q++)
            date[n++] = *q;
        date[n++] = ' ';
        if (clock_shown.day >= 10)
            date[n++] = (char)('0' + clock_shown.day / 10);
        date[n++] = (char)('0' + clock_shown.day % 10);
        date[n++] = ' ';
        for (const char *q = months[(clock_shown.month - 1) % 12]; *q; q++)
            date[n++] = *q;
        date[n] = 0;
        int x0 = page_x(0), y = t->topbar_h + 40;
        text_draw(VS, VX(x0 + (S()->w - text_width(hm, TEXT_LARGE)) / 2), VY(y), hm, TEXT_LARGE, SHELL_FG);
        y += text_height(TEXT_LARGE) + 4;
        text_draw(VS, VX(x0 + (S()->w - text_width(date, TEXT_BODY)) / 2), VY(y), date, TEXT_BODY, SHELL_FG);
    }
    for (int i = 0; i < builtin_app_count; i++) {
        struct gfx_rect r = tile_rect(i);
        if (!visible(r))
            continue;
        struct hit h = { H_TILE, i };
        if (same(h, pressed) && same(h, hover))
            gfx_blend_round_rect(VS, VX(r.x + 8), VY(r.y), r.w - 16, r.h, TILE_RADIUS * 2, WHITE, A_PRESSED);
        const struct app *a = &builtin_apps[i];
        struct gfx_rect ir = tile_icon_rect(i);
        draw_icon(a, ir, 255);
        int ty = ir.y + ir.h + 12, max_w = r.w - 8;
        int tw = text_width(a->name, TEXT_SMALL);
        text_draw_fit(VS, VX(r.x + (r.w - (tw < max_w ? tw : max_w)) / 2), VY(ty), a->name, max_w,
                      TEXT_SMALL, SHELL_FG);
    }
    /* All apps, A to Z. */
    int x0 = page_x(home_pages());
    if (x0 < S()->w && x0 + S()->w > 0) {
        text_draw(VS, VX(x0 + t->margin), VY(t->topbar_h + t->margin), "All apps", TEXT_HEADING, SHELL_FG);
        for (int k = 0; k < builtin_app_count; k++) {
            struct gfx_rect r = list_rect(k);
            if (!visible(r))
                continue;
            struct hit h = { H_LIST, k };
            if (same(h, pressed) && same(h, hover))
                gfx_blend_round_rect(VS, VX(r.x), VY(r.y), r.w, r.h, t->radius, WHITE, A_PRESSED);
            const struct app *a = &builtin_apps[list_app(k)];
            draw_icon(a, (struct gfx_rect){ r.x + 8, r.y + (r.h - LIST_ICON) / 2, LIST_ICON, LIST_ICON }, 255);
            text_draw(VS, VX(r.x + 8 + LIST_ICON + t->margin), VY(r.y + (r.h - text_height(TEXT_BODY)) / 2),
                      a->name, TEXT_BODY, SHELL_FG);
        }
    }
    /* The page dots. */
    int n = home_pages() + 1, gap = 24, cy = S()->h - HOME_DOTS_H / 2;
    int cx0 = S()->w / 2 - (n - 1) * gap / 2;
    for (int p = 0; p < n; p++)
        gfx_fill_circle(VS, VX(cx0 + p * gap), VY(cy), 6, p == page ? SHELL_FG : DOT_GREY);
}

static __attribute__((unused)) void draw_dock(void)
{
    if (reveal.x >= FX(DOCK_HIDDEN - 2) || !visible(dock_band()))
        return;
    struct gfx_rect d = dock_rect();
    int y = dock_y();
    gfx_blend_round_rect(VS, VX(d.x), VY(d.y), d.w, d.h, 18, WHITE, A_DOCK);
    for (int i = 0; i < NITEMS; i++) {
        struct item *it = &items[i];
        if (!it->drawn || it->scale.x <= 0)
            continue;
        int cx = item_x(i), sc = it->scale.x;       /* FX(1): full size */
        if (i == I_SEP) {
            int h = 36 * sc / 256;
            gfx_blend_round_rect(VS, VX(cx), VY(y + (DOCK_H - h) / 2), 1, h, 0, WHITE, 70);
            continue;
        }
        struct hit h = { i == I_APPS ? H_APPS : H_DOCK, i };
        int lit = same(h, pressed) && same(h, hover) ? A_PRESSED : same(h, hover) ? A_HOVER
                : i == I_APPS && grid_open ? A_HOVER : 0;
        if (lit && it->in)
            gfx_blend_round_rect(VS, VX(cx - DOCK_CELL / 2 + 2), VY(y + 2), DOCK_CELL - 4, DOCK_H - 4, 12,
                                 WHITE, (uint32_t)lit);
        int size = DOCK_ICON * sc / 256;
        struct gfx_rect ir = { cx - size / 2, y + DOCK_PAD + DOCK_ICON / 2 - size / 2, size, size };
        uint32_t alpha = sc >= FX(1) ? 255 : (uint32_t)(sc * 255 / FX(1));
        if (i == I_APPS) {                          /* nine dots, like GNOME's "Show Apps" */
            for (int k = 0; k < 9; k++)
                gfx_fill_circle(VS, VX(cx - 10 * sc / 256 + (k % 3) * 10 * sc / 256),
                                VY(y + DOCK_H / 2 - 10 * sc / 256 + (k / 3) * 10 * sc / 256),
                                3 * sc / 256 > 0 ? 3 * sc / 256 : 1, SHELL_FG);
            continue;
        }
        draw_icon(&builtin_apps[i], ir, alpha);
        /* The dot: grey (minimised), white (on screen), a pill (focused). */
        if (it->dot_alpha.x > 0) {
            int dw = it->dot_w.x / 256 < 4 ? 4 : it->dot_w.x / 256;
            int white = it->dot_white.x < 0 ? 0 : it->dot_white.x > FX(1) ? 255 : it->dot_white.x * 255 / FX(1);
            int a = it->dot_alpha.x > FX(1) ? 255 : it->dot_alpha.x * 255 / FX(1);
            gfx_blend_round_rect(VS, VX(cx - dw / 2), VY(y + DOCK_H - 6), dw, 4, 2,
                                 gfx_mix(WHITE, DOT_GREY, (uint32_t)white), (uint32_t)(a * sc / FX(1) > 255 ? 255 : a * sc / FX(1)));
        }
    }
    /* The hovered item's name above it (GNOME's tooltip). */
    if ((hover.kind == H_DOCK || hover.kind == H_APPS) && dock_up() && !pressed.kind) {
        const char *name = hover.kind == H_APPS ? "Show Apps" : builtin_apps[hover.app].name;
        int lh = text_height(TEXT_BODY), w = text_width(name, TEXT_BODY) + 24;
        int x = item_x(hover.app) - w / 2, ly = y - 8 - lh - 10;
        gfx_fill_round_rect(VS, VX(x), VY(ly), w, lh + 10, 8, SHELL_POPOVER);
        text_draw(VS, VX(x + 12), VY(ly + 5), name, TEXT_BODY, SHELL_FG);
    }
}

/* --- window animations (Phase 3 §2, redone for floating windows in §6) -------------------
 *
 * A window is drawn once into a snapshot, left out of the picture, and the
 * snapshot is drawn scaled and faded over a copy of the rest, frame by
 * frame. Each frame is timed: if one takes longer than ANIM_MAX_FRAME_MS
 * (a slow machine), the rest are skipped, so an animation never makes
 * things feel slower. The cost is logged for measuring on the EeePC. */

#define ANIM_MAX_FRAME_MS 30

enum easing { EASE_OUT, EASE_BACK, EASE_IN_OUT };

struct ghost {
    struct wm_window *w;
    struct gfx_surface snap;
    struct gfx_rect from, to;
    int a0, a1;             /* alpha, 0..255 */
    int r0, r1;             /* corner radius */
};

static uint32_t *anim_save;             /* the picture without the ghosts */
static uint32_t *snap_buf[WM_MAX];      /* one snapshot per ghost, made on first use */

/* t and the result are 0..1024 (EASE_BACK overshoots a little past 1024). */
static int ease(enum easing e, int t)
{
    int64_t u = t - 1024;
    switch (e) {
    case EASE_BACK:         /* 1 + c3 u^3 + c1 u^2, c1 = 1.2 (a gentle overshoot) */
        return (int)(1024 + 2253 * u * u / 1024 * u / 1024 / 1024 + 1229 * u * u / 1024 / 1024);
    case EASE_IN_OUT:       /* smoothstep */
        return (int)((int64_t)t * t / 1024 * (3072 - 2 * t) / 1024);
    case EASE_OUT:
    default:
        return (int)(1024 + u * u / 1024 * u / 1024);
    }
}

static struct gfx_rect lerp(struct gfx_rect a, struct gfx_rect b, int e)
{
    return (struct gfx_rect){ a.x + (b.x - a.x) * e / 1024, a.y + (b.y - a.y) * e / 1024,
                              a.w + (b.w - a.w) * e / 1024, a.h + (b.h - a.h) * e / 1024 };
}

/* The window as it looks now, into ghost slot k. 0 if no memory. */
static int snapshot(int k, struct wm_window *w, struct gfx_surface *out)
{
    uint32_t bytes = (uint32_t)(S()->w * S()->h * 4);
    if (!snap_buf[k])
        snap_buf[k] = (uint32_t *)pmm_alloc_contiguous((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!snap_buf[k])
        return 0;
    struct gfx_rect r = wm_rect(w);
    *out = (struct gfx_surface){ snap_buf[k], r.w, r.h, r.w };
    gfx_fill_rect(out, 0, 0, r.w, r.h, T()->window_bg);
    wm_draw_one(out, r.x, r.y, w, r);
    return 1;
}

/* Play the ghosts (their windows already left out of the picture). */
static void anim_run(struct ghost *g, int n, int frames, int ms, enum easing e, const char *what,
                     const char *name)
{
    struct gfx_surface *s = S();
    if (!anim_save || !n || covered)
        return;                         /* covered (the lock screen, the screen saver): unseen */
    ms *= ANIM_SLOW;
    int drawn = 0;
    shell_flush();                          /* the picture as it is without them */
    compose((struct gfx_rect){ 0, 0, s->w, s->h });
    struct gfx_surface save = { anim_save, s->w, s->h, s->w };
    gfx_blit(&save, 0, 0, s, 0, 0, s->w, s->h);

    struct gfx_rect prev[WM_MAX];
    uint32_t t0 = uptime_ms(), worst = 0;
    int done = 0;
    for (int f = 1; f <= frames; f++) {
        uint32_t f0 = uptime_ms();
        int t = f * 1024 / frames, et = ease(e, t), ea = ease(EASE_OUT, t);
        for (int k = 0; k < n && done; k++) {
            gfx_blit(s, prev[k].x, prev[k].y, &save, prev[k].x, prev[k].y, prev[k].w, prev[k].h);
            screen_damage(prev[k].x, prev[k].y, prev[k].w, prev[k].h);
        }
        for (int k = 0; k < n; k++) {
            struct gfx_rect r = lerp(g[k].from, g[k].to, et);
            int alpha = g[k].a0 + (g[k].a1 - g[k].a0) * ea / 1024;
            int rad = g[k].r0 + (g[k].r1 - g[k].r0) * t / 1024;
            gfx_blit_scaled(s, r, &g[k].snap, (struct gfx_rect){ 0, 0, g[k].snap.w, g[k].snap.h },
                            (uint32_t)(alpha < 0 ? 0 : alpha > 255 ? 255 : alpha), rad);
            screen_damage(r.x, r.y, r.w, r.h);
            prev[k] = r;
        }
        done = 1;
        drawn++;
        screen_present();
        uint32_t took = uptime_ms() - f0;
        if (took > worst)
            worst = took;
        if (took > ANIM_MAX_FRAME_MS && f < frames) {
            /* Too slow here: jump to the last frame. */
            f = frames - 1;
            continue;
        }
        while (uptime_ms() - t0 < (uint32_t)(f * ms / frames))
            __asm__ volatile("pause");
    }
    for (int k = 0; k < n && done; k++)
        shell_damage(prev[k]);              /* the final picture is composed by the caller */
    kprintf("anim: %s %s, %d frames, %u ms, slowest frame %u ms\n", what, name, drawn,
            uptime_ms() - t0, worst);
}

static const char *name_of(struct wm_window *w)
{
    const struct app *a = wm_owner(w);
    return a ? a->name : wm_title(w);
}

/* The app's icon in the dock, or the middle of the bottom edge. */
static struct gfx_rect icon_target(struct wm_window *w)
{
    const struct app *a = wm_owner(w);
    if (a && app_index(a) >= 0)
        return tile_icon_rect(app_index(a));        /* LiteKern Go: its home screen icon */
    return (struct gfx_rect){ S()->w / 2 - DOCK_ICON / 2, dock_base_y() + DOCK_PAD, DOCK_ICON, DOCK_ICON };
}

static void anim_open(struct wm_window *w, struct gfx_rect from)
{
    struct ghost g = { w, { 0 }, from, wm_rect(w), 0, 255, 8, WM_RADIUS };
    if (!snapshot(0, w, &g.snap))
        return;
    wm_set_hidden(w, 1);
    anim_run(&g, 1, 12, 220, EASE_BACK, "open", name_of(w));
    wm_set_hidden(w, 0);
}

static void anim_close(struct wm_window *w)
{
    struct gfx_rect r = wm_rect(w);
    struct ghost g = { w, { 0 }, r, { r.x + r.w / 14, r.y + r.h / 14, r.w - r.w / 7, r.h - r.h / 7 },
                       255, 0, wm_is_maximised(w) ? 0 : WM_RADIUS, WM_RADIUS };
    if (!snapshot(0, w, &g.snap))
        return;
    wm_set_hidden(w, 1);
    anim_run(&g, 1, 8, 150, EASE_OUT, "close", name_of(w));
}

/* Minimise every window in list into the dock, together. */
static void minimise_windows(struct wm_window **list, int n)
{
    struct ghost g[WM_MAX];
    int ng = 0;
    for (int k = 0; k < n; k++) {
        struct wm_window *w = list[k];
        struct ghost *gh = &g[ng];
        gh->w = w;
        gh->from = wm_rect(w);
        gh->a0 = 255;
        gh->a1 = 40;
        gh->r0 = wm_is_maximised(w) ? 0 : WM_RADIUS;
        gh->r1 = 10;
        int snapped = snapshot(ng, w, &gh->snap);
        kprintf("desktop: minimise %s\n", name_of(w));
        wm_set_minimised(w, 1);
        if (snapped)
            ng++;
    }
    dock_layout(0);
    for (int k = 0; k < ng; k++)
        g[k].to = icon_target(g[k].w);
    anim_run(g, ng, 12, 240, EASE_IN_OUT, "minimise", ng == 1 ? name_of(g[0].w) : "all");
    shell_flush();
}

static void restore_window(struct wm_window *w)
{
    struct ghost g = { w, { 0 }, icon_target(w), wm_rect(w), 40, 255, 10, wm_is_maximised(w) ? 0 : WM_RADIUS };
    int snapped = snapshot(0, w, &g.snap);
    kprintf("desktop: restore %s\n", name_of(w));
    wm_set_hidden(w, 1);
    wm_set_minimised(w, 0);             /* in front and focused */
    dock_layout(0);
    if (snapped)
        anim_run(&g, 1, 12, 240, EASE_BACK, "restore", name_of(w));
    wm_set_hidden(w, 0);
    shell_flush();
}

void shell_window_minimise(struct wm_window *w)
{
    minimise_windows(&w, 1);
}

void shell_window_maximise(struct wm_window *w, int maximise, struct gfx_rect from)
{
    struct ghost g = { w, { 0 }, from, from, 255, 255, wm_is_maximised(w) ? 0 : WM_RADIUS,
                       maximise ? 0 : WM_RADIUS };
    int snapped = snapshot(0, w, &g.snap);
    kprintf("desktop: %s %s\n", maximise ? "maximise" : "unmaximise", name_of(w));
    wm_set_hidden(w, 1);
    wm_set_maximised(w, maximise);
    g.to = wm_rect(w);
    dock_layout(0);                     /* the dock hides behind a maximised window */
    if (snapped)
        anim_run(&g, 1, 10, 180, EASE_OUT, maximise ? "maximise" : "unmaximise", name_of(w));
    wm_set_hidden(w, 0);
    shell_flush();
}

void shell_window_focus_changed(void)
{
    if (!started)
        return;
    shell_damage((struct gfx_rect){ 0, 0, S()->w, T()->topbar_h });     /* the app's name */
    dock_layout(0);                     /* the dots */
}

/* --- apps ------------------------------------------------------------------------------- */

static void set_grid(int open)
{
    if (grid_open == open)
        return;
    grid_open = open;
    kprintf("desktop: app menu %s\n", open ? "open" : "closed");
    if (open)
        wm_pointer_gone();
    dock_layout(0);
    damage_all();
}

static void menu_set(int open)
{
    if (menu_open == open)
        return;
    menu_open = open;
    shell_damage(menu_rect());
    shell_damage(power_rect());
}

static void launch(int i, struct gfx_rect from)
{
    const struct app *a = &builtin_apps[i];
    struct wm_window *w = app_win[i];
    if (w) {                            /* already open: bring it back */
        if (wm_is_minimised(w))
            restore_window(w);
        else
            wm_focus(w);
        return;
    }
    kprintf("desktop: open %s\n", a->name);
    w = wm_create(a->name, a->width > 0 ? a->width : 760, a->height > 0 ? a->height : 480, a->min_width,
                  a->min_height);
    if (!w) {
        kprintf("desktop: no room for another window\n");
        return;
    }
    wm_set_owner(w, a);
    app_win[i] = w;
    app_seq[i] = ++seq;
    dock_layout(0);
    anim_open(w, from);
    if (a->open)
        a->open(w);
    if (a->lkx && lkx_start(a, w) < 0) {    /* it couldn't start: nothing to show */
        app_win[i] = 0;
        wm_destroy(w);
        dock_layout(0);
    }
    shell_flush();
}

/* The window goes, fading out. */
static void close_window(struct wm_window *w)
{
    const struct app *a = wm_owner(w);
    if (a)
        kprintf("desktop: close %s\n", a->name);
    anim_close(w);
    if (a)
        app_win[app_index(a)] = 0;
    wm_destroy(w);
    dock_layout(0);
    shell_flush();
}

void desktop_app_ended(const struct app *a, struct wm_window *w)
{
    (void)a;
    close_window(w);
}

/* Home: every window into the dock (or the app menu closes). */
static void show_desktop(void)
{
    struct wm_window *list[WM_MAX];
    int n = 0;
    for (int i = 0; i < wm_count(); i++)
        if (!wm_is_minimised(wm_at(i)))
            list[n++] = wm_at(i);
    kprintf("desktop: show desktop\n");
    if (n)
        minimise_windows(list, n);
}

/* A dock icon: open it; or bring it back; or, if it's in front already,
 * minimise it (like Windows' taskbar). */
static void dock_click(int i)
{
    struct wm_window *w = app_win[i];
    if (!w)
        launch(i, dock_icon_rect(i));
    else if (wm_is_minimised(w))
        restore_window(w);
    else if (w == wm_focused())
        minimise_windows(&w, 1);
    else
        wm_focus(w);
}

/* Before restarting or switching off: the screen fades to black. */
static void fade_out(void)
{
    struct gfx_surface *s = S();
    menu_open = 0;
    damage_all();
    shell_flush();
    cursor_hide();                          /* the pointer goes first */
    for (int i = 1; i <= 8; i++) {
        uint32_t t0 = uptime_ms();
        gfx_darken(s, 0, 0, s->w, s->h, (uint32_t)(i * 255 / 8 > 120 ? 120 : i * 255 / 8));
        screen_damage_all();
        screen_present();
        while (uptime_ms() - t0 < (uint32_t)(30 * ANIM_SLOW))
            __asm__ volatile("pause");
    }
    gfx_fill_rect(s, 0, 0, s->w, s->h, 0);
    screen_damage_all();
    screen_present();
}

static void activate(struct hit h)
{
    switch (h.kind) {
    case H_HOME:
        set_grid(0);
        show_desktop();
        turn_to(0, 0);
        break;
    case H_APPS:
        set_grid(!grid_open);
        break;
    case H_POWER:
        menu_set(!menu_open);
        break;
    case H_WBTN:                            /* a maximised window's header button */
        if (merged())
            wm_press_header_button(merged(), h.app);
        break;
    case H_WCTL:                            /* ...and its minimise, restore, close */
        if (merged())
            wm_press_control(merged(), h.app);
        break;
    case H_NET:                             /* Settings, where the Network page is */
    case H_BATTERY:                         /* ...and the System page */
        for (int i = 0; i < builtin_app_count; i++)
            if (!strcmp(builtin_apps[i].name, "Settings")) {
                launch(i, h.kind == H_NET ? net_rect() : battery_rect());
                break;
            }
        break;
    case H_LOGOUT:
        menu_set(0);
        shell_flush();
        kprintf("shell: log out\n");
        desktop_log_out();                  /* every app closed (they save as they go) */
        users_set_current(-1);
        lockscreen_login();                 /* someone logs in next */
        break;
    case H_LOCK:
        menu_set(0);
        shell_flush();
        kprintf("shell: lock\n");
        lockscreen_lock();
        break;
    case H_RESTART:
        kprintf("shell: restart\n");
        fade_out();
        power_restart();
    case H_SHUTDOWN:
        if (!power_can_off())
            break;
        kprintf("shell: shut down\n");
        fade_out();
        power_off();
    case H_TILE:
        set_grid(0);
        shell_flush();
        launch(h.app, tile_icon_rect(h.app));
        break;
    case H_LIST: {
        struct gfx_rect r = list_rect(h.app);
        launch(list_app(h.app), (struct gfx_rect){ r.x + 8, r.y + (r.h - LIST_ICON) / 2, LIST_ICON, LIST_ICON });
        break;
    }
    case H_DOCK:
        if (grid_open) {
            set_grid(0);
            shell_flush();
        }
        dock_click(h.app);
        break;
    default:
        break;
    }
}

/* --- starting, input, ticks ----------------------------------------------------------------- */

void desktop_start(void)
{
    if (screen_ready()) {       /* the animations' copy of the screen, made once */
        uint32_t bytes = (uint32_t)(S()->w * S()->h * 4);
        anim_save = (uint32_t *)pmm_alloc_contiguous((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    }
    ramdisk_init();             /* apps and wallpapers, from the boot disk */
    splash_progress(65);
    apps_init();
    rtc = device_find("rtc0");
    if (rtc && rtc->state != DEVICE_BOUND)
        rtc = 0;
    if (rtc) {
        dev_read(rtc, &clock_shown, sizeof(clock_shown));
        log_clock();
    }
    if (!screen_ready())
        return;
    /* The wallpaper chosen last time (kernel/settings.h), else the build's. */
    const char *wp = settings_wallpaper(DEFAULT_WALLPAPER);
    if (!strcmp(wp, WALLPAPER_CUSTOM)) {
        if (wallpaper_init("") == 0 && settings_load_wallpaper())
            wallpaper_set(DEFAULT_WALLPAPER);
    } else if (wallpaper_init(wp) && strcmp(wp, DEFAULT_WALLPAPER)) {
        wallpaper_set(DEFAULT_WALLPAPER);   /* that one's gone: the build's */
    }
    splash_progress(90);
    if (splash_active()) {
        splash_end();
        cursor_init();          /* hidden during the splash */
    }
    console_set_visible(0);     /* the log keeps recording, but no longer draws over us */
    wm_set_area((struct gfx_rect){ 0, T()->topbar_h, S()->w, S()->h - T()->topbar_h });
    started = 1;
    dock_layout(1);
    /* Self-tests may have left windows: they belong to no app, but show. */
    damage_all();
    shell_flush();
    /* The boot budget's finish line (docs/BOOT-BUDGET.md: <= 5 s from stage 1). */
    kprintf("[boot] desktop t=%u\n", uptime_ms());
    kprintf("desktop: ready\n");     /* everything is up: the tests wait for this */
    if (users_need_login())
        lockscreen_login();     /* someone has to log in first (kernel/users.h) */
}

int desktop_started(void)
{
    return started;
}

void desktop_cover(int on)
{
    covered = on;
    if (on)
        return;
    cursor_refresh();
    damage_all();
    shell_flush();
}

int desktop_log_out(void)
{
    for (int i = wm_count() - 1; i >= 0; i--)
        wm_request_close(wm_at(i));
    uint32_t t0 = uptime_ms();
    while (wm_count() && uptime_ms() - t0 < 3000) {
        lkx_schedule();                     /* the apps see their close, save, and end */
        desktop_handle_events();            /* the built-in ones (Log) close here */
    }
    if (wm_count())
        kprintf("desktop: log out: %d window%s didn't close\n", wm_count(), wm_count() == 1 ? "" : "s");
    return wm_count();
}

void desktop_render_quiet(void)
{
    covered = 0;
    if (!screen_ready())
        return;
    compose((struct gfx_rect){ 0, 0, S()->w, S()->h });
    ndmg = 0;                               /* all of it is drawn now */
}

void desktop_refresh(void)
{
    if (!started)
        return;
    damage_all();
    shell_flush();
}

/* Over a maximised window the dock hides; the bottom edge brings it up,
 * and it stays while the pointer is on it (and a moment after). */
static void track_peek(int x, int y)
{
    return;                     /* LiteKern Go: no dock (the home screen has the apps) */
    int want = hide_mode();
    if (!want) {
        if (peek || hide_at) {
            peek = 0;
            hide_at = 0;
        }
    } else if (y >= S()->h - REVEAL_EDGE) {
        if (!peek)
            kprintf("dock: shown (bottom edge)\n");
        peek = 1;
        hide_at = 0;
    } else if (peek) {
        struct gfx_rect d = dock_rect();
        struct gfx_rect near = { d.x - 16, d.y - 40, d.w + 32, S()->h - d.y + 40 };
        if (inside(near, x, y))
            hide_at = 0;
        else if (!hide_at)
            hide_at = uptime_ms() + HIDE_DELAY_MS;
    }
    int t = want && !peek ? FX(DOCK_HIDDEN) : 0;
    if (reveal.t != t) {
        reveal.t = t;
        dock_moving = 1;
    }
}

void desktop_input_scroll(int x, int y, int notches)
{
    if (started && (menu_open || grid_open || hit_at(x, y).kind != H_NONE))
        return;                 /* the shell's own parts don't scroll */
    wm_input_scroll(x, y, notches);
}

/* Swiping the home screen's pages: they follow the finger, then settle on
 * the nearest page, or the next one if it went a fifth of the way. 1 while
 * it's a swipe (the press on an icon under it is cancelled). */
static int home_swipe(int x, uint8_t buttons, int down, int up)
{
    if (down && home_shown() && !menu_open) {
        swipe_x0 = x;
        swipe_from = pager.x;
        swipe_on = 0;
        return 0;
    }
    if (swipe_x0 < 0)
        return 0;
    int dx = x - swipe_x0;
    if (up || !(buttons & MOUSE_LEFT)) {
        int was = swipe_on;
        swipe_x0 = -1;
        swipe_on = 0;
        if (!was)
            return 0;
        int p = page, w = S()->w;
        if (-dx > w / 5)
            p++;
        else if (dx > w / 5)
            p--;
        turn_to(p, 0);
        return 1;
    }
    if (!swipe_on && (dx > SWIPE_SLOP || dx < -SWIPE_SLOP)) {
        swipe_on = 1;
        damage_hit(pressed);
        pressed = (struct hit){ H_NONE, 0 };
        damage_hit(hover);
        hover = (struct hit){ H_NONE, 0 };
    }
    if (swipe_on) {
        int max = FX(home_pages() * S()->w), to = swipe_from - FX(dx);
        pager.x = pager.t = to < 0 ? to / 3 : to > max ? max + (to - max) / 3 : to;    /* past the ends: a stretch */
        pager.v = 0;
        pager_moving = 0;
        damage_all();
    }
    return swipe_on;
}

void desktop_input_mouse(int x, int y, uint8_t buttons)
{
    if (!started) {             /* the self-tests: just the windows */
        wm_input_mouse(x, y, buttons);
        shell_flush();
        return;
    }
    int down = (buttons & MOUSE_LEFT) && !(last_buttons & MOUSE_LEFT);
    int up = !(buttons & MOUSE_LEFT) && (last_buttons & MOUSE_LEFT);
    last_buttons = buttons;
    track_peek(x, y);
    if (home_swipe(x, buttons, down, up))
        return;
    if (wm_grabbed() && !pressed.kind) {    /* a drag or a press in a window */
        wm_input_mouse(x, y, buttons);
        return;                 /* the input loop redraws once per batch */
    }

    struct hit h = hit_at(x, y);
    if (!same(h, hover)) {
        damage_hit(hover);
        hover = h;
        damage_hit(hover);
    }
    if (h.kind != H_NONE)
        wm_pointer_gone();      /* the shell has the pointer */

    if (down) {
        if (menu_open && h.kind != H_RESTART && h.kind != H_SHUTDOWN && h.kind != H_LOCK && h.kind != H_LOGOUT &&
            h.kind != H_POWER && h.kind != H_BAR) {
            menu_set(0);        /* a click outside closes the menu */
        } else if (h.kind == H_NONE) {
            if (grid_open)
                set_grid(0);    /* a click on empty space closes the app menu */
            else
                wm_input_mouse(x, y, buttons);
        } else if (h.kind != H_BAR && h.kind != H_DOCK_BG) {
            pressed = h;
            damage_hit(h);
        }
    } else if (up && pressed.kind) {
        struct hit was = pressed;
        pressed = (struct hit){ H_NONE, 0 };
        damage_hit(was);
        if (same(was, h)) {
            if (was.kind != H_POWER)
                menu_set(0);
            shell_flush();
            activate(was);
        }
    } else if (!pressed.kind && h.kind == H_NONE && !grid_open) {
        wm_input_mouse(x, y, buttons);      /* hover and the rest, for the windows */
    }
    /* No redraw here: the input loop does one per batch of packets. */
}

/* LiteKern Go's keys (UI-PLAN.md). Until the phone screens exist they drive
 * the desktop's own: Home shows the desktop, Recent apps switches to the
 * previous app (the switcher is phase 2.10), Power locks (held: the power
 * menu), volume is only logged (no sound yet). */
#define POWER_HOLD_MS 500
static uint32_t power_down_ms;          /* Power is held since (0: it isn't) */
static int power_held;                  /* ... and the power menu opened for it */

static void recent_app(void)
{
    struct wm_window *now = wm_focused();
    for (int i = wm_count() - 1; i >= 0; i--) {
        struct wm_window *w = wm_at(i);
        if (w == now && !wm_is_minimised(w))
            continue;
        kprintf("desktop: recent app %s\n", name_of(w));
        if (wm_is_minimised(w))
            restore_window(w);
        else
            wm_focus(w);
        return;
    }
    kprintf("desktop: no recent apps\n");
}

static int phone_key(const struct key_event *k)
{
    if (k->key == KEY_PHONE_POWER) {
        if (k->pressed && !power_down_ms) {
            power_down_ms = uptime_ms() | 1;
        } else if (!k->pressed) {
            if (power_down_ms && !power_held) {
                menu_set(0);
                shell_flush();
                kprintf("shell: lock (Power)\n");
                lockscreen_lock();
            }
            power_down_ms = 0;
            power_held = 0;
        }
        return 1;
    }
    if (!k->pressed)
        return 1;
    switch (k->key) {
    case KEY_PHONE_HOME:
        kprintf("shell: Home\n");
        menu_set(0);
        set_grid(0);
        show_desktop();
        turn_to(0, 0);                  /* on the home screen already: back to its first page */
        break;
    case KEY_PHONE_RECENTS:
        kprintf("shell: Recent apps\n");
        menu_set(0);
        set_grid(0);
        recent_app();
        break;
    case KEY_VOLUME_UP:
    case KEY_VOLUME_DOWN:
        kprintf("shell: volume %s\n", k->key == KEY_VOLUME_UP ? "up" : "down");
        break;
    }
    shell_flush();
    return 1;
}

static void power_tick(void)
{
    if (power_down_ms && !power_held && uptime_ms() - power_down_ms >= POWER_HOLD_MS) {
        power_held = 1;
        kprintf("shell: power menu (Power held)\n");
        menu_set(1);
        shell_flush();
    }
}

int desktop_input_key(const struct key_event *k)
{
    if (k->key & KEY_PHONE)
        return phone_key(k);
    /* Super+L locks, as in GNOME and Windows. */
    static int super;
    if (k->key == KEY_LSUPER || k->key == KEY_RSUPER) {
        super = k->pressed;
        return 0;
    }
    if (super && k->pressed && k->key == 0x026) {      /* L */
        if (menu_open)
            menu_set(0);
        shell_flush();
        kprintf("shell: lock (Super+L)\n");
        lockscreen_lock();
        return 1;
    }
    if (!k->pressed || k->key != KEY_ESC)
        return 0;
    if (menu_open)
        menu_set(0);
    else if (grid_open)
        set_grid(0);
    else
        return 0;
    shell_flush();
    return 1;
}

static int last_machine_tick;

void desktop_tick(void)
{
    if (!started)
        return;
    dock_tick();
    pager_tick();
    power_tick();
    uint32_t ticks;
    if (!rtc || dev_ioctl(rtc, RTC_GET_TICKS, &ticks) < 0 || ticks == last_tick)
        return;
    last_tick = ticks;
    struct k86_net n;                       /* the network and battery icons: redrawn when they change */
    net_status(&n);
    struct k86_machine m = machine_shown;
    if (ticks % 5 == 0 || !last_machine_tick) {
        machine_status(&m);
        m.minutes = m.brightness = m.timezone = 0;     /* the icon shows only these */
        last_machine_tick = 1;
    }
    if (memcmp(&n, &net_shown, sizeof(n)) || memcmp(&m, &machine_shown, sizeof(m))) {
        int from = status_left();
        net_shown = n;
        machine_shown = m;
        int left = status_left() < from ? status_left() : from;
        shell_damage((struct gfx_rect){ left - 200, 0, power_rect().x - left + 200, T()->topbar_h });
        shell_flush();
    }
    struct rtc_time now;
    dev_read(rtc, &now, sizeof(now));
    if (now.minute == clock_shown.minute && now.hour == clock_shown.hour &&
        now.day == clock_shown.day && clock_shown.month)
        return;
    clock_shown = now;
    log_clock();
    shell_damage(clock_rect());
    shell_flush();
}

int desktop_busy(void)
{
    return dock_moving || hide_at || pager_moving;
}

static void log_event(const struct wm_event *ev)
{
    switch (ev->type) {
    case WM_EVENT_HEADER:
        kprintf("wm: header button %d\n", ev->id);
        break;
    case WM_EVENT_CLICK:
        kprintf("wm: click (%d, %d)\n", ev->x, ev->y);
        break;
    case WM_EVENT_KEY:
        kprintf("wm: key 0x%03x\n", ev->key.key);
        break;
    default:                    /* pointer moves are too many to log; clicks already are */
        break;
    }
}

void desktop_handle_events(void)
{
    struct wm_event ev;
again:
    for (int i = 0; i < wm_count(); i++) {
        struct wm_window *w = wm_at(i);
        const struct app *a = wm_owner(w);
        if (a && a->lkx)
            continue;           /* a ring 3 app takes its own (SYS_WAIT_EVENT) */
        while (wm_poll_event(w, &ev)) {
            if (ev.type == WM_EVENT_CLOSE) {
                kprintf("wm: close\n");
                if (a && a->close)
                    a->close(w);
                close_window(w);
                goto again;     /* the list changed */
            }
            if (a && a->event)
                a->event(w, &ev);
            else
                log_event(&ev);
        }
    }
}
