/* LiteKern X — the shell: top bar, dock, app menu, windows' animations, and
 * the compositor that puts it all on screen (see kernel/desktop.h). */
#include "kernel/desktop.h"
#include "kernel/text.h"
#include "kernel/apps.h"
#include "kernel/console.h"
#include "kernel/driver.h"
#include "kernel/icon.h"
#include "kernel/input.h"
#include "kernel/keymap.h"
#include "kernel/osk.h"
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
#define MENU_W 520           /* the power menu (Power held): a sheet in the middle, big buttons */
#define MENU_ITEM_H 128
#define MENU_PAD 24
#define MENU_ITEMS 4        /* Power off, Restart, Lock, Log Out (with accounts) */
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
static void draw_drawer(void);
static int drw_shown(void);
static void draw_qs(void);
static void draw_switcher(void);
static void sw_forget(struct wm_window *w);
static int sw_open;                     /* the app switcher (the Recent apps key) */
static int qs_shown(void);
static void draw_note_icons(int x, int cy, uint32_t bg);
static int home_shown(void);
static void draw_topbar(void);
static void draw_menu(void);
static void draw_volume(void);
static int volume;
static void draw_toast(void);
static int edit_zoomed(void);           /* home edit mode: the page is a card (see "home edit mode") */
static void draw_edit(void);
/* The overlays' fades (fx_tick): 0 gone .. 255 all there. */
static int sw_alpha, menu_alpha, vol_alpha, toast_alpha;

/* --- the backdrop (the motion pass) ---------------------------------------------------
 * What's on screen, kept once as a panel, the drawer, Recent apps or the power
 * menu opens: while they move it stands in for everything under them, so a
 * frame is one copy and a panel, not the whole shell drawn again. */
static uint32_t *bd_px;
static int bd_ok;               /* bd_px holds the picture */
static int qs_taken;            /* ... for this opening of the panel */
static int qs_shown(void);

static struct gfx_surface backdrop(void)
{
    return (struct gfx_surface){ bd_px, S()->w, S()->h, S()->w };
}

static void backdrop_take(int with_bar)
{
    struct gfx_surface *s = S();
    bd_ok = 0;
    if (!bd_px) {
        uint32_t bytes = (uint32_t)(s->w * s->h * 4);
        bd_px = (uint32_t *)pmm_alloc_contiguous((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
        if (!bd_px)
            return;                     /* no memory: everything's drawn each frame */
    }
    uint32_t t0 = uptime_ms();
    struct gfx_view v = { backdrop(), 0, 0 };
    struct gfx_view *outer = V;
    V = &v;
    wallpaper_draw(VS, 0, 0, 0);
    if (home_shown())
        draw_home();
    else
        wm_draw(VS, 0, 0);
    if (with_bar)
        draw_topbar();
    V = outer;
    bd_ok = 1;
    kprintf("desktop: backdrop taken in %u ms\n", (unsigned)(uptime_ms() - t0));
}

static void backdrop_draw(void)
{
    struct gfx_surface b = backdrop();
    gfx_blit(VS, 0, 0, &b, V->ox, V->oy, VS->w, VS->h);
}

/* Everything in r, bottom to top. */
static void compose(struct gfx_rect r)
{
    struct gfx_view v = { gfx_sub(S(), r), r.x, r.y };
    struct gfx_view *outer = V;
    V = &v;
    if (started && qs_shown() && !qs_taken) {
        qs_taken = 1;
        backdrop_take(0);
    }
    if (started && bd_ok && (menu_open || menu_alpha > 0)) {
        draw_menu();                    /* it stands in for everything, the status bar too */
        draw_toast();
        V = outer;
        return;
    }
    if (bd_ok && (sw_open || sw_alpha > 0)) {
        backdrop_draw();
        draw_switcher();
    } else if (sw_open && sw_alpha >= 255) {
        wallpaper_draw(VS, v.ox, v.oy, 0);
        draw_switcher();                /* all there: nothing under it shows */
    } else if (edit_zoomed() && !qs_shown() && !drw_shown()) {
        draw_edit();
    } else if (bd_ok && (qs_shown() || drw_shown())) {
        backdrop_draw();
        if (drw_shown())
            draw_drawer();
    } else {
        wallpaper_draw(VS, v.ox, v.oy, 0);
        if (home_shown()) {
            draw_home();
            if (drw_shown())
                draw_drawer();
        } else {
            wm_draw(VS, v.ox, v.oy);
        }
        if (sw_alpha > 0)
            draw_switcher();            /* fading over what's under it */
    }
    if (started && !home_shown() && !sw_open)
        osk_draw(VS, v.ox, v.oy);
    if (started) {
        draw_qs();                      /* under the status bar, which stays on top */
        draw_topbar();
        draw_volume();
        if (menu_open || menu_alpha > 0)
            draw_menu();
        draw_toast();
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

/* --- the overlays' fades (the motion pass) ------------------------------------------------
 * The switcher, the power menu, the volume pop-up and toasts fade (and
 * slide a little) in and out on springs, FX(1) all there; fx_tick() steps
 * them a frame at a time and sets the *_alpha the drawing uses. */
static struct spring swv, menuv, volv, toastv;
/* Home edit mode (hold the home screen): wiggling icons, the page zoomed out, a bar of choices. */
static int edit_on, edit_panel, edit_sel, edit_hide_icons;
static int ed_app = -1, ed_lift, ed_px, ed_py;       /* an icon dragged: which, lifted yet, where the finger puts it */
#define BADGE_R 18
static void draw_badge(int cx, int cy, int r);
static struct spring edit_sp, panel_sp;
static struct gfx_rect clk_mask_r, clk_box_r;   /* the clock's digits, and digits + date, page coordinates */
static void show_toast(const char *text);
static int fx_moving;
static uint32_t last_fx_tick;
static uint32_t fx_began, fx_frames, fx_slowest;    /* frame stats, for the log */

static void fx_start(struct spring *s, int on)
{
    s->t = on ? FX(1) : 0;
    if (!fx_moving) {
        last_fx_tick = fx_began = uptime_ms();
        fx_frames = fx_slowest = 0;
    }
    fx_moving = 1;
}

static int fx_alpha(const struct spring *s)
{
    int a = s->x * 255 / FX(1);
    return a < 0 ? 0 : a > 255 ? 255 : a;
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

static const enum hit_kind menu_kinds[MENU_ITEMS] = { H_SHUTDOWN, H_RESTART, H_LOCK, H_LOGOUT };

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

/* LiteKern Go: the top bar is the phone's status bar (UI-PLAN.md): the time
 * on the left, the network and battery on the right, no buttons. Home and
 * Power are the phone's keys, so their buttons are gone: home_rect() and
 * power_rect() are empty, the latter only marking the right edge. */
static uint32_t bar_fg = SHELL_FG;      /* the status bar's text and icons, set as it's drawn */

static struct gfx_rect home_rect(void)
{
    return (struct gfx_rect){ 0, 0, 0, 0 };
}

static struct gfx_rect power_rect(void)
{
    return (struct gfx_rect){ S()->w - T()->margin + 4, (T()->topbar_h - BAR_BUTTON_H) / 2, 0, BAR_BUTTON_H };
}

/* The network icon, left of the power button (only with network hardware). */
static struct k86_net net_shown;
static int net_present(void) { return net_shown.wired != K86_NET_NONE || net_shown.wifi != K86_NET_NONE; }

static struct gfx_rect net_rect(void)
{
    struct gfx_rect p = power_rect();
    return (struct gfx_rect){ p.x - 2 * BAR_BUTTON_W - 4, p.y, 2 * BAR_BUTTON_W, BAR_BUTTON_H };    /* 2x icons */
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
    int w = 10 + 2 * (21 + 6) + text_width(text, TEXT_BOLD) + 10;      /* 2x icons (BAR_SCALE) */
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

/* Round glass buttons down the right edge, beside the Power key, so a thumb
 * is already there; each one's name is on its left. */
#define PW_D 136
#define PW_GAP 28
#define PW_RIGHT 28
#define PW_TOP 300
#define PW_W 440

static struct gfx_rect menu_rect(void)
{
    int n = menu_items();
    return (struct gfx_rect){ S()->w - PW_RIGHT - PW_W, PW_TOP, PW_W, n * PW_D + (n - 1) * PW_GAP };
}

static struct gfx_rect menu_item_rect(int k)
{
    return (struct gfx_rect){ S()->w - PW_RIGHT - PW_W, PW_TOP + k * (PW_D + PW_GAP), PW_W, PW_D };
}

/* --- the home screen (LiteKern Go, UI-PLAN.md) ----------------------------------------
 *
 * Shown whenever no app is: a big clock on the left, the apps in pages of
 * 4 x 4 icons, and a glass dock with four of them at the bottom, in reach of
 * a thumb. Swiping sideways turns the pages. Swiping up brings all the apps,
 * A to Z (the drawer, below). Home, Back and Recent apps are the phone's keys. */

#define HOME_COLS 4
#define HOME_ROWS 4
#define HOME_PER_PAGE (HOME_COLS * HOME_ROWS)
#define HOME_ICON 112
#define HOME_TOP 352            /* the first row of icons, under the clock and date */
#define DOCK_W_INSET 20
#define DOCK_BAND_H 176
#define DOCK_BOTTOM 24
#define DOCK_R 64
#define SWIPE_SLOP 24           /* px a finger moves before it's a swipe, not a tap */

static struct spring pager;     /* how far the pages are turned: FX(page * screen width) */
static int page;                /* the page shown, or being turned to */
static int pager_moving;
static uint32_t last_pager_tick;

static int home_shown(void)
{
    return started && !wm_top_visible() && !covered;
}

/* --- where the apps are on the home screen --------------------------------------------------------
 * Each app is in the dock, on the grid (in order, four to a row) or off the home screen (still in the
 * app drawer, and Apps in home edit mode puts it back). Kept by name in settings.txt. */
enum { W_GRID, W_DOCK, W_HIDDEN };
#define LAYOUT_MAX 32
#define LAYOUT_NAME 24
#define DOCK_MAX 5
static const char *const dock_names[] = { "Files", "Browser", "Gallery", "Settings" };   /* the first layout's dock */
static struct { char name[LAYOUT_NAME]; int where; } layout[LAYOUT_MAX];
static int nlayout, layout_seen = -1, layout_dirty = 1;
static int dock_idx[DOCK_MAX], n_dock, grid_idx[APPS_MAX], n_grid, hid_idx[APPS_MAX], n_hid;    /* app numbers */
static int off_x[APPS_MAX], off_y[APPS_MAX];    /* an icon sliding to its place: where it is from there, 1/256 px */

static int app_by_name(const char *n)
{
    for (int i = 0; i < builtin_app_count; i++)
        if (!strcmp(builtin_apps[i].name, n))
            return i;
    return -1;
}

static void layout_add(const char *name, int where)
{
    if (nlayout >= LAYOUT_MAX)
        return;
    int n = 0;
    for (; name[n] && n < LAYOUT_NAME - 1; n++)
        layout[nlayout].name[n] = name[n];
    layout[nlayout].name[n] = '\0';
    layout[nlayout].where = where;
    nlayout++;
}

static void layout_default(void)
{
    nlayout = 0;
    for (unsigned k = 0; k < sizeof(dock_names) / sizeof(dock_names[0]); k++)
        if (app_by_name(dock_names[k]) >= 0)
            layout_add(dock_names[k], W_DOCK);
    for (int i = 0; i < builtin_app_count; i++) {
        int in = 0;
        for (int k = 0; k < nlayout; k++)
            in |= !strcmp(layout[k].name, builtin_apps[i].name);
        if (!in)
            layout_add(builtin_apps[i].name, W_GRID);
    }
}

static void layout_sync(void)
{
    if (!layout_dirty && layout_seen == builtin_app_count)
        return;
    if (nlayout == 0)
        layout_default();
    for (int i = 0; i < builtin_app_count; i++) {       /* an app the layout hasn't met: on the grid */
        int in = 0;
        for (int k = 0; k < nlayout; k++)
            in |= !strcmp(layout[k].name, builtin_apps[i].name);
        if (!in)
            layout_add(builtin_apps[i].name, W_GRID);
    }
    n_dock = n_grid = n_hid = 0;
    for (int k = 0; k < nlayout; k++) {
        int i = app_by_name(layout[k].name);
        if (i < 0)
            continue;
        if (layout[k].where == W_DOCK && n_dock < DOCK_MAX)
            dock_idx[n_dock++] = i;
        else if (layout[k].where == W_HIDDEN)
            hid_idx[n_hid++] = i;
        else
            grid_idx[n_grid++] = i;
    }
    layout_dirty = 0;
    layout_seen = builtin_app_count;
    kprintf("home: %d on the grid, %d in the dock, %d off the home screen\n", n_grid, n_dock, n_hid);
}

/* The three lists are the layout now (after a move). */
static void layout_from_lists(void)
{
    nlayout = 0;
    for (int k = 0; k < n_dock; k++)
        layout_add(builtin_apps[dock_idx[k]].name, W_DOCK);
    for (int k = 0; k < n_grid; k++)
        layout_add(builtin_apps[grid_idx[k]].name, W_GRID);
    for (int k = 0; k < n_hid; k++)
        layout_add(builtin_apps[hid_idx[k]].name, W_HIDDEN);
    layout_dirty = 1;
}

static int dock_slot(int i)     /* app i's place in the dock, or -1 */
{
    layout_sync();
    for (int k = 0; k < n_dock; k++)
        if (dock_idx[k] == i)
            return k;
    return -1;
}

static int dock_count(void)
{
    layout_sync();
    return n_dock;
}

static int grid_index(int i)    /* app i's place among the grid's icons, or -1 */
{
    layout_sync();
    for (int k = 0; k < n_grid; k++)
        if (grid_idx[k] == i)
            return k;
    return -1;
}

static int home_has(int i)      /* on the home screen at all */
{
    return dock_slot(i) >= 0 || grid_index(i) >= 0;
}

static int home_pages(void)
{
    layout_sync();
    return n_grid > 0 ? (n_grid + HOME_PER_PAGE - 1) / HOME_PER_PAGE : 1;
}

/* Text for settings.txt: "Files|d;Clock|g;Log|h" */
int desktop_layout_text(char *out, int max)
{
    layout_sync();
    int n = 0;
    for (int k = 0; k < nlayout; k++) {
        for (const char *q = layout[k].name; *q && n < max - 4; q++)
            out[n++] = *q == '|' || *q == ';' ? ' ' : *q;
        if (n >= max - 4)
            break;
        out[n++] = '|';
        out[n++] = layout[k].where == W_DOCK ? 'd' : layout[k].where == W_HIDDEN ? 'h' : 'g';
        if (k + 1 < nlayout)
            out[n++] = ';';
    }
    out[n] = '\0';
    return n;
}

void desktop_layout_set(const char *text)
{
    nlayout = 0;
    while (text && *text && nlayout < LAYOUT_MAX) {
        char name[LAYOUT_NAME];
        int n = 0;
        while (*text && *text != '|' && *text != ';' && n < LAYOUT_NAME - 1)
            name[n++] = *text++;
        name[n] = '\0';
        int where = W_GRID;
        if (*text == '|') {
            text++;
            where = *text == 'd' ? W_DOCK : *text == 'h' ? W_HIDDEN : W_GRID;
            while (*text && *text != ';')
                text++;
        }
        if (*text == ';')
            text++;
        if (n)
            layout_add(name, where);
    }
    layout_dirty = 1;
}

static struct gfx_rect home_dock(void)
{
    return (struct gfx_rect){ DOCK_W_INSET, S()->h - DOCK_BOTTOM - DOCK_BAND_H, S()->w - 2 * DOCK_W_INSET, DOCK_BAND_H };
}

static int page_x(int p)        /* page p's left edge on screen, as turned now */
{
    return p * S()->w - pager.x / 256;
}

/* Where app i's tile is when it's at rest (a hidden app's: the middle of the screen, where an app that
 * opens from somewhere else grows from). */
static struct gfx_rect tile_base(int i)
{
    const struct theme *t = T();
    int slot = dock_slot(i);
    if (slot >= 0) {
        struct gfx_rect d = home_dock();
        int w = d.w / dock_count();
        return (struct gfx_rect){ d.x + slot * w, d.y, w, d.h };
    }
    int g = grid_index(i);
    if (g < 0)
        return (struct gfx_rect){ (S()->w - t->tile_w) / 2, S()->h / 2 - t->tile_h / 2, t->tile_w, t->tile_h };
    int p = g / HOME_PER_PAGE, k = g % HOME_PER_PAGE;
    int x0 = page_x(p) + (S()->w - HOME_COLS * t->tile_w) / 2;
    return (struct gfx_rect){ x0 + (k % HOME_COLS) * t->tile_w, HOME_TOP + (k / HOME_COLS) * t->tile_h,
                              t->tile_w, t->tile_h };
}

static struct gfx_rect tile_rect(int i)
{
    struct gfx_rect r = tile_base(i);
    if (i >= 0 && i < APPS_MAX) {
        r.x += off_x[i] / 256;
        r.y += off_y[i] / 256;
    }
    return r;
}

static struct gfx_rect tile_icon_rect(int i)
{
    struct gfx_rect r = tile_rect(i);
    if (dock_slot(i) >= 0)
        return (struct gfx_rect){ r.x + (r.w - HOME_ICON) / 2, r.y + (r.h - HOME_ICON) / 2, HOME_ICON, HOME_ICON };
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

/* The drawer's tiles, row by row, sliding with it. */
#define DRAWER_TOP 120
static struct spring drw;       /* FX(the screen's height): all the way open */
static int drw_moving, drw_y0 = -1, drw_from, drw_live;
static uint32_t last_drw_tick;

static int drw_shown(void) { return drw.x > 0 || drw.t > 0; }
static int drw_top(void) { return S()->h - drw.x / 256; }     /* the sheet's top edge */

static struct gfx_rect list_rect(int k)
{
    const struct theme *t = T();
    int x0 = (S()->w - HOME_COLS * t->tile_w) / 2;
    return (struct gfx_rect){ x0 + (k % HOME_COLS) * t->tile_w, drw_top() + DRAWER_TOP + (k / HOME_COLS) * t->tile_h,
                              t->tile_w, t->tile_h };
}

static void turn_to(int p, int instant)
{
    if (p < 0)
        p = 0;
    if (p > home_pages() - 1)
        p = home_pages() - 1;
    if (p != page)
        kprintf("home: page %d of %d\n", p + 1, home_pages());
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
        if (drw_shown()) {
            for (int k = 0; k < builtin_app_count; k++)
                if (inside(list_rect(k), x, y))
                    return (struct hit){ H_LIST, k };
            return (struct hit){ H_NONE, 0 };
        }
        for (int i = 0; i < builtin_app_count; i++)
            if (home_has(i) && inside(tile_rect(i), x, y))
                return (struct hit){ H_TILE, i };
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

/* Icons at the sizes on screen, shrunk once: shrinking one well takes a while,
 * and the drawer and home screen show a dozen on every frame. */
#define ICON_SIZES 48
static struct { const struct icon *ic; int w, h; uint32_t *px; } icon_sizes[ICON_SIZES];

static const uint32_t *icon_sized(const struct icon *ic, int w, int h)
{
    if (w >= ic->w || h >= ic->h)
        return 0;
    int k = 0;
    for (; k < ICON_SIZES && icon_sizes[k].ic; k++)
        if (icon_sizes[k].ic == ic && icon_sizes[k].w == w && icon_sizes[k].h == h)
            return icon_sizes[k].px;
    if (k == ICON_SIZES)
        return 0;
    uint32_t *px = (uint32_t *)pmm_alloc_contiguous(((uint32_t)(w * h * 4) + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!px)
        return 0;
    gfx_scale_argb(ic->px, ic->w, ic->h, ic->w, px, w, h);
    icon_sizes[k].ic = ic;
    icon_sizes[k].w = w;
    icon_sizes[k].h = h;
    icon_sizes[k].px = px;
    return px;
}

static void draw_icon(const struct app *a, struct gfx_rect r, uint32_t alpha)
{
    const struct theme *t = T();
    const struct icon *ic = icon_find(a->icon);
    if (ic) {
        const uint32_t *sized = alpha >= 255 ? icon_sized(ic, r.w, r.h) : 0;
        if (sized) {
            gfx_blit_alpha(VS, VX(r.x), VY(r.y), sized, r.w, r.h, r.w);
            return;
        }
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

/* An icon image (sw x sh, ARGB) drawn dw x dh about (cx, cy) in screen coordinates, turned by sn
 * (256ths of a radian), smoothly. Opaque pixels take a quick blend of four; the edges, a weighted one. */
static uint32_t lerp_rgb(uint32_t c0, uint32_t c1, int f)
{
    uint32_t rb = ((c0 & 0xff00ff) * (uint32_t)(256 - f) + (c1 & 0xff00ff) * (uint32_t)f) >> 8 & 0xff00ff;
    uint32_t g = ((c0 & 0x00ff00) * (uint32_t)(256 - f) + (c1 & 0x00ff00) * (uint32_t)f) >> 8 & 0x00ff00;
    return rb | g;
}

static void rot_icon(const uint32_t *src, int sw, int sh, int cx, int cy, int dw, int dh, int sn)
{
    int cs = 256 - sn * sn / 512;
    int inv = sw * 256 / dw;                    /* source pixels per drawn pixel, 8.8 */
    int hw = dw / 2 + 4, hh = dh / 2 + 4;
    for (int dy = -hh; dy < hh; dy++) {
        int y = cy + dy - V->oy;
        if (y < 0 || y >= V->s.h)
            continue;
        uint32_t *row = V->s.px + (size_t)y * (size_t)V->s.stride;
        for (int dx = -hw; dx < hw; dx++) {
            int x = cx + dx - V->ox;
            if (x < 0 || x >= V->s.w)
                continue;
            int rx = dx * cs + dy * sn, ry = dy * cs - dx * sn;
            int su = rx * inv / 256 + sw * 128 - 128, sv = ry * inv / 256 + sh * 128 - 128;
            int ix = su >> 8, iy = sv >> 8, fx = su & 255, fy = sv & 255;
            if (ix < -1 || iy < -1 || ix >= sw || iy >= sh)
                continue;
            uint32_t t[4];
            for (int k = 0; k < 4; k++) {
                int tx = ix + (k & 1), ty = iy + (k >> 1);
                t[k] = tx < 0 || ty < 0 || tx >= sw || ty >= sh ? 0 : src[ty * sw + tx];
            }
            if ((t[0] & t[1] & t[2] & t[3]) >> 24 == 255) {
                row[x] = lerp_rgb(lerp_rgb(t[0], t[1], fx), lerp_rgb(t[2], t[3], fx), fy);
                continue;
            }
            uint32_t w[4] = { (uint32_t)((256 - fx) * (256 - fy)), (uint32_t)(fx * (256 - fy)),
                              (uint32_t)((256 - fx) * fy), (uint32_t)(fx * fy) };
            uint64_t A = 0, R = 0, G = 0, B = 0;
            for (int k = 0; k < 4; k++) {
                uint64_t wa = (uint64_t)w[k] * (t[k] >> 24);
                A += wa;
                R += wa * (t[k] >> 16 & 255);
                G += wa * (t[k] >> 8 & 255);
                B += wa * (t[k] & 255);
            }
            if (!A)
                continue;
            uint32_t col = (uint32_t)(R / A) << 16 | (uint32_t)(G / A) << 8 | (uint32_t)(B / A);
            uint32_t al = (uint32_t)(A >> 16);
            row[x] = al >= 255 ? col : gfx_mix(col, row[x], al);
        }
    }
}

/* The red minus on an icon in edit mode: tap it to take the app off the home screen. */
static void draw_badge(int cx, int cy, int r)
{
    gfx_fill_circle(VS, VX(cx), VY(cy), r + 2, 0x2a2a2e);
    gfx_fill_circle(VS, VX(cx), VY(cy), r, 0xff453a);
    gfx_fill_rect(VS, VX(cx - r / 2), VY(cy - r / 8 - 1), r, r / 4 + 2, WHITE);
}

/* The wiggle's angle for icon idx now: its own beat, about +-2 degrees, in 256ths. */
static int wiggle_sn(int idx)
{
    uint32_t period = 150 + (uint32_t)(idx * 37 % 80), t = uptime_ms() + (uint32_t)idx * 53;
    int u = (int)(t % period * 512 / period), tri = u < 256 ? u : 511 - u;
    return (tri * 2 - 255) * 9 / 255 * (idx % 2 ? 1 : -1);
}

/* An app's icon wiggling at page size. */
static void draw_icon_wiggle(const struct app *a, struct gfx_rect r, int idx)
{
    const struct icon *ic = icon_find(a->icon);
    if (!ic) {
        draw_icon(a, r, 255);
        return;
    }
    const uint32_t *sz = icon_sized(ic, r.w, r.h);
    if (sz)
        rot_icon(sz, r.w, r.h, r.x + r.w / 2, r.y + r.h / 2, r.w, r.h, wiggle_sn(idx));
    else
        rot_icon(ic->px, ic->w, ic->h, r.x + r.w / 2, r.y + r.h / 2, r.w, r.h, wiggle_sn(idx));
}

/* The network (kernel/net.h): Wi-Fi as a fan whose arcs light up with the
 * signal, Ethernet as three linked boxes; dimmed when not connected. */
/* The status bar's icons: LiteKern X's, drawn BAR_SCALE times bigger for
 * the phone's screen (UI-PLAN.md: 2x). */
#define BAR_SCALE 2
#define B(v) ((v) * BAR_SCALE)

static void dot(int x, int y, uint32_t c)       /* one of the icons' pixels */
{
    gfx_fill_rect(VS, VX(x), VY(y), BAR_SCALE, BAR_SCALE, c);
}

static void draw_net_icon(int cx, int cy, const struct k86_net *n, uint32_t bg)
{
    uint32_t lit = bar_fg, dim = gfx_mix(bg, bar_fg, 160);
    int wifi = n->wifi != K86_NET_NONE && (n->wifi == K86_NET_UP || n->wired != K86_NET_UP);
    if (wifi) {
        int up = n->wifi == K86_NET_UP;
        int bars = !up ? 0 : n->signal >= 66 ? 3 : n->signal >= 33 ? 2 : 1;
        int ox = cx, oy = cy + B(6);
        /* Three arcs, 45 degrees either side of up, drawn pixel by pixel:
         * only above the centre, so nothing spills below the top bar. */
        for (int dy = -B(12); dy < 0; dy++)
            for (int dx = dy; dx <= -dy; dx++) {
                int d2 = dx * dx + dy * dy;
                for (int k = 1; k <= 3; k++)
                    if (d2 > B(1 + 3 * k) * B(1 + 3 * k) && d2 <= B(3 + 3 * k) * B(3 + 3 * k))
                        gfx_fill_rect(VS, VX(ox + dx), VY(oy + dy), 1, 1, bars >= k ? lit : dim);
            }
        gfx_fill_circle(VS, VX(ox), VY(oy - B(1)), B(2), up ? lit : dim);
        return;
    }
    uint32_t c = n->wired == K86_NET_UP ? lit : dim;
    gfx_fill_rect(VS, VX(cx - B(3)), VY(cy - B(8)), B(7), B(5), c);         /* the boxes */
    gfx_fill_rect(VS, VX(cx - B(10)), VY(cy + B(3)), B(7), B(5), c);
    gfx_fill_rect(VS, VX(cx + B(4)), VY(cy + B(3)), B(7), B(5), c);
    gfx_fill_rect(VS, VX(cx), VY(cy - B(3)), B(1), B(4), c);               /* the links */
    gfx_fill_rect(VS, VX(cx - B(7)), VY(cy + B(1)), B(15), B(1), c);
    gfx_fill_rect(VS, VX(cx - B(7)), VY(cy + B(1)), B(1), B(2), c);
    gfx_fill_rect(VS, VX(cx + B(7)), VY(cy + B(1)), B(1), B(2), c);
}

/* A battery on its side, filled as far as it's full (red when nearly empty),
 * with a lightning bolt while it charges; then "87%". */
static void draw_battery(struct gfx_rect r, const struct k86_machine *m, uint32_t bg)
{
    int x = r.x + 10, cy = r.y + r.h / 2, low = m->battery <= 10 && m->charging == K86_BATTERY_DISCHARGING;
    uint32_t c = bar_fg, fill = low ? 0xff6b6b : bar_fg;
    gfx_fill_round_rect(VS, VX(x), VY(cy - B(6)), B(19), B(12), B(3), c);         /* the case */
    gfx_fill_round_rect(VS, VX(x + B(2)), VY(cy - B(4)), B(15), B(8), B(1), bg);
    gfx_fill_rect(VS, VX(x + B(19)), VY(cy - B(2)), B(2), B(4), c);               /* the terminal */
    int w = (m->battery * B(13) + 50) / 100;
    if (w > 0)
        gfx_fill_rect(VS, VX(x + B(3)), VY(cy - B(3)), w, B(6), fill);
    if (m->charging == K86_BATTERY_CHARGING) {
        /* The bolt, outlined in the bar's colour so it shows over the fill. */
        static const char *const bolt[] = { "..##", ".##.", "####", ".##.", "##.." };
        for (int pass = 0; pass < 2; pass++)
            for (int j = 0; j < 5; j++)
                for (int i = 0; i < 4; i++)
                    if (bolt[j][i] == '#') {
                        int bx = x + B(8 + i), by = cy + B(j - 3);
                        if (pass == 0)
                            gfx_fill_rect(VS, VX(bx - B(1)), VY(by - B(1)), B(3), B(3), bg);
                        else
                            dot(bx, by, c);
                    }
    }
    char text[8];
    battery_text(text);
    text_draw(VS, VX(x + B(27)), VY(r.y + (r.h - text_height(TEXT_BOLD)) / 2), text, TEXT_BOLD, bar_fg);
}

static __attribute__((unused)) uint32_t hit_bg(struct hit h, uint32_t base)
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

/* The status bar's clock: the time, on the left (the date is on the home
 * screen and the lock screen). */
static void clock_place(char text[24], int *x)
{
    clock_text(text);
    const char *time = text;                /* the time follows the date's two spaces */
    while (*time && !(time[0] == ' ' && time[1] == ' '))
        time++;
    if (*time)
        memmove(text, time + 2, strlen(time + 2) + 1);
    *x = T()->margin;
}

static struct gfx_rect clock_rect(void)
{
    char text[24];
    int x;
    clock_place(text, &x);
    (void)text;
    return (struct gfx_rect){ 0, 0, x + text_width("00:00", TEXT_BOLD) + 16, T()->topbar_h };
}

/* The name of the app in front, after Home. */
static const char *focused_name(void)
{
    struct wm_window *w = wm_focused();
    const struct app *a = w ? wm_owner(w) : 0;
    return a ? a->name : w ? wm_title(w) : "";
}

/* The status bar: clear over the home screen (a soft shade keeps it
 * readable on any wallpaper), the app's header bar colour over an app, so
 * the two read as one. */
static void draw_topbar(void)
{
    const struct theme *t = T();
    struct gfx_rect bar = { 0, 0, S()->w, t->topbar_h };
    if (!visible(bar))
        return;
    uint32_t bg = t->topbar_bg;             /* black on every screen, as on Phosh */
    gfx_fill_rect(VS, VX(0), VY(0), bar.w, bar.h, bg);
    bar_fg = SHELL_FG;
    int ty = (t->topbar_h - text_height(TEXT_BOLD)) / 2;
    char text[24];
    int clock_x;
    clock_place(text, &clock_x);
    if (text[0])
        text_draw(VS, VX(clock_x), VY(ty), text, TEXT_BOLD, bar_fg);
    draw_note_icons(clock_x + text_width(text, TEXT_BOLD) + 20, t->topbar_h / 2, bg);
    if (net_present()) {
        struct gfx_rect r = net_rect();
        draw_net_icon(r.x + r.w / 2, r.y + r.h / 2, &net_shown, bg);
    }
    if (battery_present())
        draw_battery(battery_rect(), &machine_shown, bg);
    bar_fg = SHELL_FG;
}

/* The power menu's icons, drawn smooth (gfx_thick_line), about 40 px:
 * a circle's points 32 round, from 3 o'clock clockwise (y is down). */
static const int16_t circ[32] = { 1024, 1004, 946, 851, 724, 569, 392, 200, 0, -200, -392, -569, -724, -851,
                                  -946, -1004, -1024, -1004, -946, -851, -724, -569, -392, -200, 0, 200, 392,
                                  569, 724, 851, 946, 1004 };

static void arc_point(int cx, int cy, int r, int i, int *x, int *y)
{
    *x = cx + r * circ[i & 31] / 1024;
    *y = cy + r * circ[(i + 24) & 31] / 1024;     /* sin = cos a quarter turn back */
}

/* An arc from point a to point b (b > a), clockwise, w px thick. */
static void arc(int cx, int cy, int r, int a, int b, int w, uint32_t c)
{
    for (int i = a; i < b; i++) {
        int x0, y0, x1, y1;
        arc_point(cx, cy, r, i, &x0, &y0);
        arc_point(cx, cy, r, i + 1, &x1, &y1);
        gfx_thick_line(VS, VX(x0), VY(y0), VX(x1), VY(y1), w, c);
    }
}

static void menu_icon(enum hit_kind k, int cx, int cy, uint32_t c)
{
    int x, y;
    switch (k) {
    case H_SHUTDOWN:                        /* the power symbol: a ring open at the top, a bar */
        arc(cx, cy + 2, 16, 27, 53, 5, c);
        gfx_thick_line(VS, VX(cx), VY(cy - 18), VX(cx), VY(cy - 2), 5, c);
        break;
    case H_RESTART:                         /* a ring with an arrowhead: round again */
        arc(cx, cy, 16, 28, 54, 5, c);      /* from half past one round to the top left */
        arc_point(cx, cy, 16, 54, &x, &y);  /* its end, heading up and right: the arrow's tip there */
        x += 4, y -= 2;
        gfx_thick_line(VS, VX(x), VY(y), VX(x - 11), VY(y - 5), 5, c);
        gfx_thick_line(VS, VX(x), VY(y), VX(x - 5), VY(y + 11), 5, c);
        break;
    case H_LOCK:                            /* a padlock */
        arc(cx, cy - 6, 10, 16, 32, 5, c);
        gfx_thick_line(VS, VX(cx - 10), VY(cy - 6), VX(cx - 10), VY(cy), 5, c);
        gfx_thick_line(VS, VX(cx + 10), VY(cy - 6), VX(cx + 10), VY(cy), 5, c);
        gfx_fill_round_rect(VS, VX(cx - 17), VY(cy - 1), 34, 24, 6, c);
        break;
    case H_LOGOUT:                          /* a door frame and an arrow out of it */
        gfx_thick_line(VS, VX(cx - 4), VY(cy - 18), VX(cx - 16), VY(cy - 18), 4, c);
        gfx_thick_line(VS, VX(cx - 16), VY(cy - 18), VX(cx - 16), VY(cy + 18), 4, c);
        gfx_thick_line(VS, VX(cx - 16), VY(cy + 18), VX(cx - 4), VY(cy + 18), 4, c);
        gfx_thick_line(VS, VX(cx - 6), VY(cy), VX(cx + 16), VY(cy), 5, c);
        gfx_thick_line(VS, VX(cx + 16), VY(cy), VX(cx + 8), VY(cy - 8), 5, c);
        gfx_thick_line(VS, VX(cx + 16), VY(cy), VX(cx + 8), VY(cy + 8), 5, c);
        break;
    default:
        break;
    }
}

static void draw_menu(void)
{
    int al = menu_alpha;
    if (bd_ok)
        backdrop_draw();
    gfx_darken(VS, VX(0), VY(0), S()->w, S()->h, (uint32_t)(170 * al / 255));
    static const char *const labels[MENU_ITEMS] = { "Power off", "Restart", "Lock", "Log Out" };
    for (int k = 0; k < menu_items(); k++) {
        struct gfx_rect r = menu_item_rect(k);
        /* In from the Power key, one after another. */
        int dx = (255 - al) * (56 + k * 28) / 255;
        int cx = S()->w - PW_RIGHT - PW_D / 2 + dx, cy = r.y + PW_D / 2;
        if (!visible((struct gfx_rect){ r.x, r.y, r.w + 80, r.h }))
            continue;
        int disabled = menu_kinds[k] == H_SHUTDOWN && !power_can_off();     /* no ACPI found */
        int red = menu_kinds[k] == H_SHUTDOWN && !disabled;
        struct hit h = { menu_kinds[k], 0 };
        int held = same(h, pressed) && same(h, hover);
        gfx_blend_round_rect(VS, VX(cx - PW_D / 2), VY(cy - PW_D / 2), PW_D, PW_D, PW_D / 2,
                             red ? 0xc01c28 : held ? 0x5d5d61 : SHELL_POPOVER, (uint32_t)al);
        if (al > 140) {
            uint32_t fg = disabled ? 0x9a9a9c : SHELL_FG;
            menu_icon(menu_kinds[k], cx, cy, fg);
            int tw = text_width(labels[k], TEXT_BOLD);
            text_draw(VS, VX(cx - PW_D / 2 - 26 - tw), VY(cy - text_height(TEXT_BOLD) / 2), labels[k], TEXT_BOLD, fg);
        }
    }
}

static void draw_home(void)
{
    /* The clock and date: on the first page, turning with it. The same clock as the lock screen's,
     * with its own choices (hold the home screen to change them). */
    if (rtc && clock_shown.month && lockscreen_home_clock()->show)
        lockscreen_home_clock_draw(VS, V->ox - page_x(0), V->oy, &clock_shown, &clk_mask_r, &clk_box_r);
    /* The dock: a dark bar over the wallpaper, in reach of a thumb. */
    struct gfx_rect d = home_dock();
    if (dock_count() && visible(d))
        gfx_blend_round_rect(VS, VX(d.x), VY(d.y), d.w, d.h, DOCK_R, 0x1d1d20, 102);     /* 40% */
    for (int i = 0; i < builtin_app_count; i++) {
        struct gfx_rect r = tile_rect(i);
        if (!home_has(i) || !visible(r) || (edit_on && i == ed_app && ed_lift))
            continue;
        struct hit h = { H_TILE, i };
        if (!edit_on && same(h, pressed) && same(h, hover))
            gfx_blend_round_rect(VS, VX(r.x + 8), VY(r.y), r.w - 16, r.h, TILE_RADIUS * 2, WHITE, A_PRESSED);
        const struct app *a = &builtin_apps[i];
        struct gfx_rect ir = tile_icon_rect(i);
        if (edit_hide_icons)
            ;                               /* the card's picture: its icons are drawn over it, wiggling */
        else if (edit_on)
            draw_icon_wiggle(a, ir, i);
        else
            draw_icon(a, ir, 255);
        if (edit_on && !edit_hide_icons)
            draw_badge(ir.x + 10, ir.y + 10, BADGE_R);
        if (dock_slot(i) >= 0)
            continue;                       /* the dock's apps have no labels: their icons are enough */
        int ty = ir.y + ir.h + 12, max_w = r.w - 8;
        int tw = text_width(a->name, TEXT_SMALL);
        text_draw_fit(VS, VX(r.x + (r.w - (tw < max_w ? tw : max_w)) / 2), VY(ty), a->name, max_w,
                      TEXT_SMALL, SHELL_FG);
    }
    if (edit_on && ed_lift && ed_app >= 0) {            /* the icon in the finger: bigger, over the rest */
        int sz = HOME_ICON * 118 / 100;
        struct gfx_rect r = { ed_px - sz / 2, ed_py - sz / 2, sz, sz };
        draw_icon(&builtin_apps[ed_app], r, 255);
    }
    /* A handle above the dock: all apps are up there. Dots when there's more than a page. */
    int n = home_pages(), gap = 24, cy = d.y - 24;
    if (n > 1) {
        int cx0 = S()->w / 2 - (n - 1) * gap / 2;
        for (int p = 0; p < n; p++)
            gfx_fill_circle(VS, VX(cx0 + p * gap), VY(cy), 6, p == page ? SHELL_FG : DOT_GREY);
    } else {
        gfx_blend_round_rect(VS, VX(S()->w / 2 - 36), VY(cy - 4), 72, 8, 4, WHITE, 140);
    }
}

/* All apps, A to Z, on a sheet that slides up over the home screen. */
static void draw_drawer(void)
{
    const struct theme *t = T();
    int top = drw_top();
    gfx_fill_round_rect(VS, VX(0), VY(top), S()->w, S()->h - top + 64, 40, t->window_bg);
    text_draw(VS, VX(48), VY(top + t->topbar_h + 12), "Apps", TEXT_HEADING, t->fg);
    for (int k = 0; k < builtin_app_count; k++) {
        struct gfx_rect r = list_rect(k);
        if (!visible(r))
            continue;
        struct hit h = { H_LIST, k };
        if (same(h, pressed) && same(h, hover))
            gfx_fill_round_rect(VS, VX(r.x + 8), VY(r.y), r.w - 16, r.h, TILE_RADIUS * 2, t->button_bg);
        const struct app *a = &builtin_apps[list_app(k)];
        struct gfx_rect ir = { r.x + (r.w - HOME_ICON) / 2, r.y + 16, HOME_ICON, HOME_ICON };
        draw_icon(a, ir, 255);
        int ty = ir.y + ir.h + 12, max_w = r.w - 8, tw = text_width(a->name, TEXT_SMALL);
        text_draw_fit(VS, VX(r.x + (r.w - (tw < max_w ? tw : max_w)) / 2), VY(ty), a->name, max_w, TEXT_SMALL, t->fg);
    }
}

static void drw_set(int open)
{
    static int was;
    if (open && !drw_shown())
        backdrop_take(0);
    drw.t = open ? FX(S()->h) : 0;
    if (was != open)
        kprintf("drawer: %s\n", open ? "open" : "closed");
    was = open;
    drw_moving = 1;
    last_drw_tick = uptime_ms();
    damage_all();
}

static void drw_tick(void)
{
    if (!drw_moving)
        return;
    uint32_t now = uptime_ms(), steps = (now - last_drw_tick) / TICK_MS;
    if (!steps)
        return;
    last_drw_tick = now;
    for (uint32_t s = 0; s < (steps > 4 ? 4 : steps); s++)
        drw_moving = spring_step(&drw, 28, 150);
    damage_all();
    shell_flush();
}

/* --- home edit mode ---------------------------------------------------------------------------
 * Hold anywhere on the home screen: the icons start to wiggle, then the page shrinks to a card
 * and a bar of choices slides up under it: Clock, Widgets, Apps, Done. Clock opens a popup
 * sheet (like the drawer) with the lock screen's clock choices; the sheet can be closed to see the
 * result, and opened again. The clock on the card has a frame with six dots (the corners resize,
 * the top and bottom ones stretch) and can be dragged. Everything is saved as it changes. */

#define EDIT_HOLD_MS 450                /* a finger held this long on the home screen */
#define EDIT_ZOOM_DELAY_MS 380          /* the wiggle first, then the zoom out */
#define EDIT_ZOOM 184                   /* the card's size, 256ths: 72% */
#define EDIT_CARD_Y 104                 /* the card's top, under the status bar */
#define EDIT_BAR_H 120
#define PANEL_H 540
#define EDIT_FRAME_MS 45                /* the wiggle's frames */
#define HANDLE_R 11
#define HANDLE_HIT 40
#define KNOB_R 22
#define SNAP_PCT 6                      /* % from the middle where the clock snaps to it */

static uint32_t edit_start_ms, last_wiggle, hold_since;
static int hold_x, hold_y, hold_swallow;
static int edit_moving, ed_drag, ed_grab_x, ed_grab_y;
static int ed_size0, ed_stretch0, ed_dist0;
static uint32_t last_edit_tick;
static uint32_t *pg_px;                 /* the page at full size, drawn when it changes */
static int pg_ok;

enum { D_NONE, D_MOVE, D_CORNER, D_STRETCH, D_ICON, D_SLIDER };
static int panel_kind;                  /* 1 Clock, 2 Apps: the sheet that's up, or sliding away */

static int edit_p(void)                 /* how far zoomed out: 0..256 */
{
    int p = edit_sp.x / 256;
    return p < 0 ? 0 : p > 256 ? 256 : p;
}

static int edit_dyn(void)               /* something is moving on the page: the card is drawn whole each frame */
{
    if (ed_lift)
        return 1;
    for (int i = 0; i < APPS_MAX; i++)
        if (off_x[i] || off_y[i])
            return 1;
    return 0;
}

static int cc_dyn;                      /* the card's picture was made for dyn / not */

static int edit_zoomed(void)
{
    return started && home_shown() && edit_p() > 0;
}

static int edit_zoom(void)              /* the page's scale, 256ths */
{
    return 256 - (256 - EDIT_ZOOM) * edit_p() / 256;
}

static struct gfx_rect edit_card(void)
{
    int z = edit_zoom(), w = S()->w * z / 256, h = S()->h * z / 256;
    return (struct gfx_rect){ (S()->w - w) / 2, EDIT_CARD_Y * edit_p() / 256, w, h };
}

static void page_to_screen(int px, int py, int *sx, int *sy)
{
    struct gfx_rect c = edit_card();
    *sx = c.x + px * edit_zoom() / 256;
    *sy = c.y + py * edit_zoom() / 256;
}

static void screen_to_page(int sx, int sy, int *px, int *py)
{
    struct gfx_rect c = edit_card();
    *px = (sx - c.x) * 256 / edit_zoom();
    *py = (sy - c.y) * 256 / edit_zoom();
}

static uint32_t *cc_px;                 /* the card as it is now, without the icons */
static int cc_ok;
static struct gfx_rect cc_rect;         /* where it is */

static void edit_damage(void)           /* the card's picture changes */
{
    pg_ok = 0;
    cc_ok = 0;
    damage_all();
}

static void edit_frame(void)            /* only the wiggle moves */
{
    struct gfx_rect c = edit_card();
    shell_damage(c);
}

static void edit_enter(void)
{
    if (edit_on || !home_shown())
        return;
    kprintf("shell: home edit\n");
    edit_on = 1;
    edit_panel = 0;
    edit_sel = 0;
    ed_drag = D_NONE;
    edit_start_ms = last_wiggle = last_edit_tick = uptime_ms();
    drw_y0 = -1;
    swipe_x0 = -1;
    swipe_on = 0;
    damage_hit(pressed);
    pressed = (struct hit){ H_NONE, 0 };
    hold_swallow = 1;                   /* the finger that held is still down: ignore it until it lifts */
    turn_to(0, 1);
    edit_sp.t = 0;                  /* the zoom follows the wiggle, in edit_tick */
    panel_sp.t = 0;
    edit_moving = 1;
    edit_damage();
}

static void edit_exit(void)
{
    if (!edit_on)
        return;
    kprintf("shell: home edit done\n");
    edit_on = 0;
    edit_panel = 0;
    edit_sel = 0;
    ed_drag = D_NONE;
    edit_sp.t = 0;
    panel_sp.t = 0;
    edit_moving = 1;
    last_edit_tick = uptime_ms();
    edit_damage();
}

/* The page shrunk into the card: each of its pixels from four of the page's, so a shrunk letter stays smooth. */
static void edit_make_card(struct gfx_rect card)
{
    if (!pg_px)
        return;
    int pw = S()->w, ph = S()->h;
    if (!cc_px) {
        uint32_t bytes = (uint32_t)(S()->w * S()->h * 4);
        cc_px = (uint32_t *)pmm_alloc_contiguous((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
        if (!cc_px)
            return;
    }
    uint32_t step = (uint32_t)((uint64_t)65536 * 256 / (uint32_t)edit_zoom());
    static int xmap[4096];
    if (card.w > 4096)
        return;
    for (int i = 0; i < card.w; i++) {
        int sx = (int)((uint32_t)i * step >> 16);
        xmap[i] = sx > pw - 2 ? pw - 2 : sx;
    }
    for (int y = 0; y < card.h; y++) {
        int sy = (int)((uint32_t)y * step >> 16);
        sy = sy > ph - 2 ? ph - 2 : sy;
        const uint32_t *r0 = pg_px + (size_t)sy * (size_t)pw, *r1 = r0 + pw;
        uint32_t *dp = cc_px + (size_t)y * (size_t)card.w;
        for (int i = 0; i < card.w; i++) {
            int sx = xmap[i];
            uint32_t a = r0[sx], b = r0[sx + 1], c = r1[sx], d = r1[sx + 1];
            dp[i] = (a >> 2 & 0x3f3f3f) + (b >> 2 & 0x3f3f3f) + (c >> 2 & 0x3f3f3f) + (d >> 2 & 0x3f3f3f);
        }
    }
    cc_rect = card;
    cc_ok = 1;
}

static void edit_render_page(void)
{
    if (!pg_px) {
        uint32_t bytes = (uint32_t)(S()->w * S()->h * 4);
        pg_px = (uint32_t *)pmm_alloc_contiguous((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
        if (!pg_px)
            return;
    }
    struct gfx_view v = { { pg_px, S()->w, S()->h, S()->w }, 0, 0 };
    struct gfx_view *outer = V;
    V = &v;
    wallpaper_draw(VS, 0, 0, 0);
    edit_hide_icons = !edit_dyn();
    cc_dyn = edit_dyn();
    draw_home();
    edit_hide_icons = 0;
    V = outer;
    pg_ok = 1;
}

static int panel_top(void)
{
    return S()->h - panel_sp.x / 256;
}

static struct gfx_rect bar_chip(int i)
{
    int w = (S()->w - 48 - 3 * 14) / 4;
    int slide = (256 - edit_p()) * (EDIT_BAR_H + 40) / 256;
    return (struct gfx_rect){ 24 + i * (w + 14), S()->h - 24 - EDIT_BAR_H + 16 + slide, w, 88 };
}

static const char *const bar_names[4] = { "Clock", "Widgets", "Apps", "Done" };

#define PANEL_SIDE 36
#define CHIP_H 56
static struct gfx_rect pn_close(void) { return (struct gfx_rect){ S()->w - 24 - 130, panel_top() + 16, 130, CHIP_H }; }
static struct gfx_rect pn_reset(void) { return (struct gfx_rect){ S()->w - 24 - 130 - 12 - 130, panel_top() + 16, 130, CHIP_H }; }
static struct gfx_rect pn_font(int i)
{
    int w = (S()->w - 2 * PANEL_SIDE - 3 * 10) / 4;
    return (struct gfx_rect){ PANEL_SIDE + i % 4 * (w + 10), panel_top() + 96 + i / 4 * (CHIP_H + 10), w, CHIP_H };
}
static struct gfx_rect pn_style(int i)
{
    int w = (S()->w - 2 * PANEL_SIDE - 3 * 10) / 4;
    return (struct gfx_rect){ PANEL_SIDE + i * (w + 10), panel_top() + 96 + 2 * (CHIP_H + 10) + 8, w, CHIP_H };
}
static int pn_slider_y(int k)
{
    return panel_top() + 96 + 2 * (CHIP_H + 10) + 8 + CHIP_H + 14 + k * 70;
}
static struct gfx_rect pn_slider(int k)
{
    return (struct gfx_rect){ 210, pn_slider_y(k) + 29, S()->w - 210 - PANEL_SIDE - KNOB_R, 12 };
}

static const char *const font_names[7] = { "Bold", "Light", "Thin", "Condensed", "Mono", "Serif", "Slim" };
static const char *const style_names[4] = { "Standard", "Stacked", "Outline", "Glass" };
static const char *const slider_names[3] = { "Opacity", "Stretch", "Size" };
static const int slider_lo[3] = { 20, 100, 30 };
static const int slider_hi[3] = { 100, 250, 250 };

static int slider_value(int k, const struct home_clock *h)
{
    return k == 0 ? h->opacity : k == 1 ? h->stretch : h->size;
}

static void ed_chip(struct gfx_rect r, const char *label, int on, int dark)
{
    const struct theme *t = T();
    uint32_t bg = on ? t->accent_bg : dark ? SHELL_POPOVER : t->button_bg;
    uint32_t fg = on ? t->accent_fg : dark ? SHELL_FG : t->fg;
    gfx_fill_round_rect(VS, VX(r.x), VY(r.y), r.w, r.h, r.h / 2, bg);
    int mw = r.w - 16;
    enum text_style st = text_width(label, TEXT_BODY) > mw ? TEXT_SMALL : TEXT_BODY;     /* "Condensed" is long */
    int tw = text_width(label, st);
    text_draw_fit(VS, VX(r.x + (r.w - (tw < mw ? tw : mw)) / 2), VY(r.y + (r.h - text_height(st)) / 2), label, mw, st, fg);
}

static void draw_handles(void)
{
    if (!edit_sel || edit_p() < 240 || !clk_box_r.w)
        return;
    const struct theme *t = T();
    int pad = 14;
    int x0, y0, x1, y1;
    page_to_screen(clk_box_r.x - pad, clk_box_r.y - pad, &x0, &y0);
    page_to_screen(clk_box_r.x + clk_box_r.w + pad, clk_box_r.y + clk_box_r.h + pad, &x1, &y1);
    gfx_rect_outline(VS, VX(x0), VY(y0), x1 - x0, y1 - y0, WHITE);
    gfx_rect_outline(VS, VX(x0 + 1), VY(y0 + 1), x1 - x0 - 2, y1 - y0 - 2, WHITE);
    int xm = (x0 + x1) / 2;
    const int hx[6] = { x0, x1, x0, x1, xm, xm }, hy[6] = { y0, y0, y1, y1, y0, y1 };
    for (int i = 0; i < 6; i++) {
        gfx_fill_circle(VS, VX(hx[i]), VY(hy[i]), HANDLE_R + 3, t->accent_bg);
        gfx_fill_circle(VS, VX(hx[i]), VY(hy[i]), HANDLE_R, WHITE);
    }
}

static struct gfx_rect pn_app(int k)    /* the Apps sheet's k-th tile */
{
    int w = (S()->w - 2 * PANEL_SIDE) / 4;
    return (struct gfx_rect){ PANEL_SIDE + k % 4 * w, panel_top() + 96 + k / 4 * 130, w, 130 };
}

static void draw_apps_panel(void)
{
    const struct theme *t = T();
    int top = panel_top();
    layout_sync();
    text_draw(VS, VX(PANEL_SIDE), VY(top + 20), "Apps", TEXT_HEADING, t->fg);
    ed_chip(pn_reset(), "Reset", 0, 0);
    ed_chip(pn_close(), "Close", 0, 0);
    if (!n_hid) {
        const char *m = "Every app is on the home screen";
        text_draw(VS, VX((S()->w - text_width(m, TEXT_BODY)) / 2), VY(top + 200), m, TEXT_BODY, t->fg);
        return;
    }
    for (int k = 0; k < n_hid && k < 12; k++) {
        struct gfx_rect r = pn_app(k);
        struct gfx_rect ir = { r.x + (r.w - 64) / 2, r.y + 4, 64, 64 };
        draw_icon(&builtin_apps[hid_idx[k]], ir, 255);
        const char *nm = builtin_apps[hid_idx[k]].name;
        int tw = text_width(nm, TEXT_SMALL), mw = r.w - 8;
        text_draw_fit(VS, VX(r.x + (r.w - (tw < mw ? tw : mw)) / 2), VY(ir.y + 64 + 8), nm, mw, TEXT_SMALL, t->fg);
    }
}

static void draw_panel(void)
{
    int top = panel_top();
    if (top >= S()->h)
        return;
    const struct theme *t = T();
    const struct home_clock *h = lockscreen_home_clock();
    gfx_shadow(VS, VX(0), VY(top), S()->w, S()->h - top + 64, 40, 28, 110);
    gfx_fill_round_rect(VS, VX(0), VY(top), S()->w, S()->h - top + 64, 40, t->window_bg);
    if (panel_kind == 2) {
        draw_apps_panel();
        return;
    }
    text_draw(VS, VX(PANEL_SIDE), VY(top + 20), "Clock", TEXT_HEADING, t->fg);
    ed_chip(pn_reset(), "Reset", 0, 0);
    ed_chip(pn_close(), "Close", 0, 0);
    for (int i = 0; i < 7; i++)
        ed_chip(pn_font(i), font_names[i], h->font == i, 0);
    for (int i = 0; i < 4; i++)
        ed_chip(pn_style(i), style_names[i], h->style == i, 0);
    for (int k = 0; k < 3; k++) {
        struct gfx_rect r = pn_slider(k);
        int v = slider_value(k, h), kx = r.x + r.w * (v - slider_lo[k]) / (slider_hi[k] - slider_lo[k]);
        text_draw(VS, VX(PANEL_SIDE), VY(pn_slider_y(k) + (70 - text_height(TEXT_BODY)) / 2), slider_names[k],
                  TEXT_BODY, t->fg);
        gfx_fill_round_rect(VS, VX(r.x), VY(r.y), r.w, r.h, r.h / 2, t->button_bg);
        gfx_fill_round_rect(VS, VX(r.x), VY(r.y), kx - r.x + r.h / 2, r.h, r.h / 2, t->accent_bg);
        gfx_fill_circle(VS, VX(kx), VY(r.y + r.h / 2), KNOB_R, ed_drag == D_SLIDER + k ? t->accent_bg : t->fg);
    }
}

static void draw_edit(void)
{
    int p = edit_p();
    struct gfx_rect card = edit_card();
    struct gfx_rect view = { V->ox, V->oy, V->s.w, V->s.h };
    struct gfx_rect vis = gfx_rect_intersect(card, view);
    int inside_card = vis.x == view.x && vis.y == view.y && vis.w == view.w && vis.h == view.h;
    if (!inside_card) {                 /* the wiggle's frames are all inside it: no need to draw the rest */
        wallpaper_draw(VS, V->ox, V->oy, (uint32_t)(150 * p / 256));
        gfx_shadow(VS, VX(card.x), VY(card.y), card.w, card.h, 0, 26, (uint32_t)(130 * p / 256));
    }
    if (!cc_ok || cc_rect.w != card.w || cc_rect.h != card.h || cc_dyn != edit_dyn()) {
        if (!pg_ok || cc_dyn != edit_dyn())
            edit_render_page();
        edit_make_card(card);
    }
    if (cc_px && !gfx_rect_empty(vis)) {
        struct gfx_surface cc = { cc_px, card.w, card.h, card.w };
        gfx_blit(VS, VX(vis.x), VY(vis.y), &cc, vis.x - card.x, vis.y - card.y, vis.w, vis.h);
    }
    if (!inside_card)
        gfx_rect_outline(VS, VX(card.x) - 1, VY(card.y) - 1, card.w + 2, card.h + 2,
                         gfx_mix(WHITE, 0, (uint32_t)(60 * p / 256)));
    /* The icons, wiggling, at the card's scale (while one's being dragged, or sliding, they're in the card). */
    int z = edit_zoom();
    for (int i = 0; i < builtin_app_count && !edit_dyn(); i++) {
        if (!home_has(i))
            continue;
        struct gfx_rect ir = tile_icon_rect(i);
        int sx, sy, sz_w = ir.w * z / 256, sz_h = ir.h * z / 256;
        page_to_screen(ir.x + ir.w / 2, ir.y + ir.h / 2, &sx, &sy);
        struct gfx_rect box = { sx - sz_w / 2 - 6, sy - sz_h / 2 - 6, sz_w + 12, sz_h + 12 };
        if (!visible(box))
            continue;
        const struct icon *ic = icon_find(builtin_apps[i].icon);
        int sn = edit_on ? wiggle_sn(i) : 0;
        if (!ic) {
            struct gfx_rect r = { sx - sz_w / 2, sy - sz_h / 2, sz_w, sz_h };
            draw_icon(&builtin_apps[i], r, 255);
            continue;
        }
        const uint32_t *sz = icon_sized(ic, ir.w, ir.h);
        if (sz)
            rot_icon(sz, ir.w, ir.h, sx, sy, sz_w, sz_h, sn);
        else
            rot_icon(ic->px, ic->w, ic->h, sx, sy, sz_w, sz_h, sn);
        if (edit_on && p > 200) {
            int bx, by;
            page_to_screen(ir.x + 10, ir.y + 10, &bx, &by);
            draw_badge(bx, by, BADGE_R * z / 256);
        }
    }
    draw_handles();
    if (panel_sp.x < FX(PANEL_H) && p > 8) {
        for (int i = 0; i < 4; i++) {
            struct gfx_rect r = bar_chip(i);
            if (visible(r))
                ed_chip(r, bar_names[i], 0, 1);
        }
    }
    draw_panel();
}

/* The clock's choices changed: draw it, and keep them. */
static void home_clock_set(const struct home_clock *h, int save)
{
    lockscreen_set_home_clock(h);
    edit_damage();
    if (save) {
        const struct home_clock *n = lockscreen_home_clock();
        kprintf("shell: home clock font %d style %d opacity %d stretch %d size %d at %d,%d\n", n->font, n->style,
                n->opacity, n->stretch, n->size, n->x, n->y);
        settings_save();
    }
}

static int isqrt(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v)
        r++;
    return r;
}

/* Which of the frame's six dots (0-3 the corners, 4 top, 5 bottom) a screen point is on, or -1. */
static int handle_at(int x, int y)
{
    if (!edit_sel || !clk_box_r.w || edit_p() < 240)
        return -1;
    int pad = 14, x0, y0, x1, y1;
    page_to_screen(clk_box_r.x - pad, clk_box_r.y - pad, &x0, &y0);
    page_to_screen(clk_box_r.x + clk_box_r.w + pad, clk_box_r.y + clk_box_r.h + pad, &x1, &y1);
    int xm = (x0 + x1) / 2;
    const int hx[6] = { x0, x1, x0, x1, xm, xm }, hy[6] = { y0, y0, y1, y1, y0, y1 };
    for (int i = 0; i < 6; i++)
        if ((x - hx[i]) * (x - hx[i]) + (y - hy[i]) * (y - hy[i]) <= HANDLE_HIT * HANDLE_HIT)
            return i;
    return -1;
}

static void slider_to(int k, int x)
{
    struct home_clock h = *lockscreen_home_clock();
    struct gfx_rect r = pn_slider(k);
    int pc = (x - r.x) * 100 / r.w;
    pc = pc < 0 ? 0 : pc > 100 ? 100 : pc;
    int v = slider_lo[k] + pc * (slider_hi[k] - slider_lo[k]) / 100;
    if (k == 0)
        h.opacity = v;
    else if (k == 1)
        h.stretch = v;
    else
        h.size = v;
    home_clock_set(&h, 0);
}

static void edit_open_panel(int kind)   /* 0 closes it */
{
    edit_panel = kind;
    if (kind)
        panel_kind = kind;
    panel_sp.t = kind ? FX(PANEL_H) : 0;
    edit_moving = 1;
    last_edit_tick = uptime_ms();
    edit_sel = kind == 1;
    edit_damage();
}

/* Moves app a to the list `where`, at place idx, and lets the other icons slide to their new places. */
static void layout_place(int a, int where, int idx)
{
    layout_sync();
    struct gfx_rect before[APPS_MAX];
    for (int i = 0; i < builtin_app_count && i < APPS_MAX; i++)
        before[i] = tile_rect(i);
    int *lists[3] = { grid_idx, dock_idx, hid_idx };
    int *counts[3] = { &n_grid, &n_dock, &n_hid };
    for (int w = 0; w < 3; w++) {                       /* out of wherever it was */
        int *n = counts[w], k = 0;
        for (int j = 0; j < *n; j++)
            if (lists[w][j] != a)
                lists[w][k++] = lists[w][j];
        *n = k;
    }
    int *n = counts[where];
    if (where == W_DOCK && *n >= DOCK_MAX) {
        where = W_GRID;
        n = counts[where];
    }
    idx = idx < 0 ? 0 : idx > *n ? *n : idx;
    for (int j = *n; j > idx; j--)
        lists[where][j] = lists[where][j - 1];
    lists[where][idx] = a;
    (*n)++;
    layout_from_lists();
    layout_sync();
    for (int i = 0; i < builtin_app_count && i < APPS_MAX; i++) {
        if (i == a && ed_lift) {
            off_x[i] = off_y[i] = 0;
            continue;
        }
        struct gfx_rect now = tile_base(i);
        off_x[i] = home_has(i) ? (before[i].x - now.x) * 256 : 0;
        off_y[i] = home_has(i) ? (before[i].y - now.y) * 256 : 0;
    }
    edit_damage();
}

/* Where the dragged icon would go: in the dock or on the grid, and at which place. */
static void drop_target(int a, int px, int py, int *where, int *idx)
{
    layout_sync();
    int in_dock = 0, in_grid = 0;
    for (int k = 0; k < n_dock; k++)
        in_dock += dock_idx[k] != a;
    for (int k = 0; k < n_grid; k++)
        in_grid += grid_idx[k] != a;
    struct gfx_rect d = home_dock();
    if (py >= d.y - 30 && in_dock < DOCK_MAX) {
        int count = in_dock + 1, i = (px - d.x) * count / d.w;
        *where = W_DOCK;
        *idx = i < 0 ? 0 : i > count - 1 ? count - 1 : i;
        return;
    }
    const struct theme *t = T();
    int gx0 = (S()->w - HOME_COLS * t->tile_w) / 2;
    int col = (px - gx0) / t->tile_w, row = (py - HOME_TOP) / t->tile_h;
    col = col < 0 ? 0 : col > HOME_COLS - 1 ? HOME_COLS - 1 : col;
    row = row < 0 ? 0 : row > HOME_ROWS - 1 ? HOME_ROWS - 1 : row;
    int i = row * HOME_COLS + col;
    *where = W_GRID;
    *idx = i > in_grid ? in_grid : i;
}

static void layout_saved(const char *what, int app)
{
    kprintf("shell: home layout: %s %s\n", what, builtin_apps[app].name);
    settings_save();
}

static int edit_pointer(int x, int y, uint8_t buttons, int down, int up)
{
    if (hold_swallow) {
        if (up || !(buttons & MOUSE_LEFT))
            hold_swallow = 0;
        return 1;
    }
    if (!edit_on)
        return 0;
    int held = !!(buttons & MOUSE_LEFT);
    struct home_clock h = *lockscreen_home_clock();
    if (down) {
        ed_drag = D_NONE;
        if (panel_top() < S()->h && y >= panel_top() && panel_kind == 2) {      /* the Apps sheet */
            if (inside(pn_close(), x, y)) {
                edit_open_panel(0);
            } else if (inside(pn_reset(), x, y)) {
                layout_default();
                layout_dirty = 1;
                memset(off_x, 0, sizeof(off_x));
                memset(off_y, 0, sizeof(off_y));
                kprintf("shell: home layout reset\n");
                settings_save();
                edit_damage();
            } else {
                for (int k = 0; k < n_hid && k < 12; k++)
                    if (inside(pn_app(k), x, y)) {
                        int a = hid_idx[k];
                        layout_place(a, W_GRID, 1000);
                        layout_saved("added", a);
                        break;
                    }
            }
            return 1;
        }
        if (panel_top() < S()->h && y >= panel_top()) {         /* the popup */
            if (inside(pn_close(), x, y)) {
                edit_open_panel(0);
            } else if (inside(pn_reset(), x, y)) {
                h = (struct home_clock){ 1, 0, 0, 100, 100, 100, 500, -1 };
                home_clock_set(&h, 1);
            } else {
                for (int i = 0; i < 7; i++)
                    if (inside(pn_font(i), x, y)) {
                        h.font = i;
                        home_clock_set(&h, 1);
                    }
                for (int i = 0; i < 4; i++)
                    if (inside(pn_style(i), x, y)) {
                        h.style = i;
                        home_clock_set(&h, 1);
                    }
                for (int k = 0; k < 3; k++)
                    if (y >= pn_slider_y(k) && y < pn_slider_y(k) + 70 && x >= pn_slider(k).x - KNOB_R) {
                        ed_drag = D_SLIDER + k;
                        slider_to(k, x);
                    }
            }
            return 1;
        }
        if (!edit_panel && edit_p() > 200) {                    /* the bar */
            for (int i = 0; i < 4; i++)
                if (inside(bar_chip(i), x, y)) {
                    if (i == 0)
                        edit_open_panel(1);
                    else if (i == 2)
                        edit_open_panel(2);
                    else if (i == 3)
                        edit_exit();
                    else
                        show_toast("Widgets are coming");
                    return 1;
                }
        }
        int hd = handle_at(x, y);
        int px, py;
        screen_to_page(x, y, &px, &py);
        int cx = clk_mask_r.x + clk_mask_r.w / 2, cy = clk_mask_r.y + clk_mask_r.h / 2;
        if ((hd >= 0 || (clk_box_r.w && inside(clk_box_r, px, py))) && h.y < 0) {   /* from here on it's where it is */
            h.y = cy * 1000 / S()->h;
            h.x = cx * 1000 / S()->w;
            lockscreen_set_home_clock(&h);
        }
        int icon_hit = -1, badge_hit = -1;
        if (hd < 0) {
            for (int i = 0; i < builtin_app_count; i++) {
                if (!home_has(i))
                    continue;
                struct gfx_rect ir = tile_icon_rect(i);
                int bx = ir.x + 10, by = ir.y + 10;
                if ((px - bx) * (px - bx) + (py - by) * (py - by) <= 36 * 36)
                    badge_hit = i;
                else if (inside(tile_rect(i), px, py))
                    icon_hit = i;
            }
        }
        if (badge_hit >= 0) {                                   /* the minus: off the home screen */
            layout_place(badge_hit, W_HIDDEN, 1000);
            layout_saved("removed", badge_hit);
            return 1;
        }
        if (icon_hit >= 0) {                                    /* held: it may be dragged */
            struct gfx_rect ir = tile_icon_rect(icon_hit);
            ed_app = icon_hit;
            ed_lift = 0;
            ed_drag = D_ICON;
            ed_grab_x = ir.x + ir.w / 2 - px;
            ed_grab_y = ir.y + ir.h / 2 - py;
            ed_px = px + ed_grab_x;
            ed_py = py + ed_grab_y;
            hold_x = px;                /* where it went down, page coordinates */
            hold_y = py;
            return 1;
        }
        if (hd >= 0) {
            ed_size0 = h.size;
            ed_stretch0 = h.stretch;
            ed_dist0 = isqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
            if (ed_dist0 < 20)
                ed_dist0 = 20;
            ed_drag = hd >= 4 ? D_STRETCH : D_CORNER;
            ed_grab_y = py < cy ? cy - py : py - cy;
            if (ed_grab_y < 20)
                ed_grab_y = 20;
        } else if (clk_box_r.w && px >= clk_box_r.x - 20 && px < clk_box_r.x + clk_box_r.w + 20 &&
                   py >= clk_box_r.y - 20 && py < clk_box_r.y + clk_box_r.h + 20) {
            if (!edit_sel) {
                edit_sel = 1;
                edit_damage();
            }
            ed_drag = D_MOVE;
            ed_grab_x = cx - px;
            ed_grab_y = cy - py;
        } else if (edit_sel) {
            edit_sel = 0;
            edit_damage();
        }
        return 1;
    }
    if (ed_drag != D_NONE && held) {
        int px, py;
        screen_to_page(x, y, &px, &py);
        int cx = clk_mask_r.x + clk_mask_r.w / 2, cy = clk_mask_r.y + clk_mask_r.h / 2;
        if (ed_drag >= D_SLIDER) {
            slider_to(ed_drag - D_SLIDER, x);
        } else if (ed_drag == D_ICON) {
            if (!ed_lift && (px - hold_x) * (px - hold_x) + (py - hold_y) * (py - hold_y) > 24 * 24) {
                ed_lift = 1;            /* picked up */
                off_x[ed_app] = off_y[ed_app] = 0;
                edit_damage();
            }
            if (ed_lift) {
                ed_px = px + ed_grab_x;
                ed_py = py + ed_grab_y;
                int where, idx;
                drop_target(ed_app, ed_px, ed_py, &where, &idx);
                int cw = dock_slot(ed_app) >= 0 ? W_DOCK : W_GRID;
                int ci = cw == W_DOCK ? dock_slot(ed_app) : grid_index(ed_app);
                if (where != cw || idx != ci)
                    layout_place(ed_app, where, idx);
                edit_damage();
            }
        } else if (ed_drag == D_MOVE) {
            int nx = (px + ed_grab_x) * 1000 / S()->w, ny = (py + ed_grab_y) * 1000 / S()->h;
            h.x = nx < 0 ? 0 : nx > 1000 ? 1000 : nx;
            if (h.x >= 500 - SNAP_PCT * 10 && h.x <= 500 + SNAP_PCT * 10)
                h.x = 500;              /* the middle is easy to find */
            h.y = ny < 30 ? 30 : ny > 970 ? 970 : ny;
            home_clock_set(&h, 0);
        } else if (ed_drag == D_CORNER) {
            int d = isqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
            int v = ed_size0 * d / ed_dist0;
            h.size = v < 30 ? 30 : v > 250 ? 250 : v;
            home_clock_set(&h, 0);
        } else {
            int d = py < cy ? cy - py : py - cy;
            int v = ed_stretch0 * d / ed_grab_y;
            h.stretch = v < 100 ? 100 : v > 250 ? 250 : v;
            home_clock_set(&h, 0);
        }
        return 1;
    }
    if (ed_drag == D_ICON) {            /* let go: it settles into its place */
        if (ed_lift) {
            struct gfx_rect base = tile_base(ed_app), ir = { 0 };
            ir.x = ed_px - HOME_ICON / 2;
            ir.y = ed_py - HOME_ICON / 2;
            int slot_x = base.x + (base.w - HOME_ICON) / 2;
            int slot_y = base.y + (dock_slot(ed_app) >= 0 ? (base.h - HOME_ICON) / 2 : 16);
            off_x[ed_app] = (ir.x - slot_x) * 256;
            off_y[ed_app] = (ir.y - slot_y) * 256;
            ed_lift = 0;
            layout_saved("moved", ed_app);
        }
        ed_app = -1;
        ed_drag = D_NONE;
        edit_damage();
        return 1;
    }
    if (ed_drag != D_NONE) {            /* let go: keep it */
        ed_drag = D_NONE;
        home_clock_set(&h, 1);
        return 1;
    }
    (void)up;
    return 1;                           /* the home screen is not for tapping while it's being edited */
}

static void edit_tick(void)
{
    uint32_t now = uptime_ms();
    if (hold_since && now - hold_since >= EDIT_HOLD_MS) {
        hold_since = 0;
        if ((last_buttons & MOUSE_LEFT) && home_shown() && !edit_on && !menu_open && !drw_shown() && !swipe_on &&
            !drw_live)
            edit_enter();
    }
    if (edit_on && !home_shown()) {     /* an app took the screen */
        edit_on = 0;
        edit_panel = 0;
        edit_sp.x = edit_sp.t = 0;
        panel_sp.x = panel_sp.t = 0;
        edit_moving = 0;
        edit_damage();
    }
    if (edit_moving) {
        uint32_t steps = (now - last_edit_tick) / TICK_MS;
        if (steps) {
            last_edit_tick = now;
            if (edit_on && edit_sp.t == 0 && now - edit_start_ms >= EDIT_ZOOM_DELAY_MS)
                edit_sp.t = FX(256);
            int a = 1, b = 1, was_x = edit_sp.x;
            for (uint32_t s = 0; s < (steps > 4 ? 4 : steps); s++) {
                if (!(edit_on && now - edit_start_ms < EDIT_ZOOM_DELAY_MS))
                    a = spring_step(&edit_sp, 30, 150);
                b = spring_step(&panel_sp, 36, 150);
            }
            edit_moving = a || b || (edit_on && now - edit_start_ms < EDIT_ZOOM_DELAY_MS);
            if (edit_sp.x != was_x)
                edit_damage();          /* the zoom moved: the card's picture is made again */
            else
                damage_all();           /* only the sheet slid */
        }
    }
    int sliding = 0;
    for (int i = 0; i < APPS_MAX; i++)
        if (off_x[i] || off_y[i]) {
            sliding = 1;
            off_x[i] = off_x[i] / 2 + off_x[i] / 8;     /* about 60% of it left each step */
            off_y[i] = off_y[i] / 2 + off_y[i] / 8;
            if (off_x[i] > -512 && off_x[i] < 512)
                off_x[i] = 0;
            if (off_y[i] > -512 && off_y[i] < 512)
                off_y[i] = 0;
        }
    if (sliding || ed_lift) {
        edit_moving = 1;
        edit_damage();
    }
    if (edit_on && now - last_wiggle >= EDIT_FRAME_MS) {        /* the wiggle: a frame at a time */
        last_wiggle = now;
        if (edit_p() > 0)
            edit_frame();
        else
            damage_all();
    }
    if (edit_on || edit_moving)
        shell_flush();
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
 * frame. Each frame is timed: when one runs late the frames it overlapped are
 * skipped, not the rest of the animation, so a slow machine gets fewer frames
 * of the same length of movement. Fading a whole screen is the dear part, so
 * after a frame over ANIM_SOLID_MS the windows just move and scale. The cost
 * is logged for measuring. */
#define ANIM_SOLID_MS 24
static int anim_solid;

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
    uint32_t t0 = uptime_ms(), worst = 0, worst_show = 0;
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
            int alpha = anim_solid ? 255 : g[k].a0 + (g[k].a1 - g[k].a0) * ea / 1024;
            int rad = g[k].r0 + (g[k].r1 - g[k].r0) * t / 1024;
            gfx_blit_scaled(s, r, &g[k].snap, (struct gfx_rect){ 0, 0, g[k].snap.w, g[k].snap.h },
                            (uint32_t)(alpha < 0 ? 0 : alpha > 255 ? 255 : alpha), rad);
            screen_damage(r.x, r.y, r.w, r.h);
            prev[k] = r;
        }
        done = 1;
        drawn++;
        uint32_t p0 = uptime_ms();
        screen_present();
        uint32_t took = uptime_ms() - f0, showing = uptime_ms() - p0;
        if (took > worst)
            worst = took;
        if (showing > worst_show)
            worst_show = showing;
        if (took > ANIM_SOLID_MS)
            anim_solid = 1;                 /* a slow machine: no more fading, from here on */
        int due = (int)((uptime_ms() - t0) * (uint32_t)frames / (uint32_t)ms);
        if (due > f && f < frames) {        /* late: on to where it should be by now */
            f = due < frames ? due : frames - 1;
            continue;
        }
        while (uptime_ms() - t0 < (uint32_t)(f * ms / frames))
            cpu_relax();
    }
    for (int k = 0; k < n && done; k++)
        shell_damage(prev[k]);              /* the final picture is composed by the caller */
    kprintf("anim: %s %s, %d frames, %u ms, slowest frame %u ms (showing it %u ms)%s\n", what, name, drawn,
            uptime_ms() - t0, worst, worst_show, anim_solid ? ", no fade" : "");
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
    struct ghost g = { w, { 0 }, r, { r.x + r.w / 3, r.y + r.h / 3, r.w / 3, r.h / 3 },
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

/* An app chosen in Recent apps comes up out of the middle of the screen, whichever
 * icon it was opened from. */
static void open_from_centre(struct wm_window *w)
{
    struct gfx_rect to = wm_rect(w);
    int fw = S()->w * 2 / 5, fh = S()->h * 2 / 5;
    struct gfx_rect from = { (S()->w - fw) / 2, (S()->h - fh) / 2, fw, fh };
    struct ghost g = { w, { 0 }, from, to, 60, 255, 30, wm_is_maximised(w) ? 0 : WM_RADIUS };
    int snapped = snapshot(0, w, &g.snap), minimised = wm_is_minimised(w);
    if (minimised)
        kprintf("desktop: restore %s\n", name_of(w));
    wm_set_hidden(w, 1);
    if (minimised) {
        wm_set_minimised(w, 0);         /* in front and focused */
        dock_layout(0);
    } else {
        wm_focus(w);
    }
    if (snapped)
        anim_run(&g, 1, 12, 240, EASE_OUT, "open", name_of(w));
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
    if (open && !menu_alpha)
        backdrop_take(1);               /* the status bar too: the menu covers it */
    fx_start(&menuv, open);
    kprintf("shell: power menu %s\n", open ? "open" : "closed");
    if (open)
        wm_pointer_gone();
    damage_all();                       /* the dimming covers everything */
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
/* Apps closed from Recent apps: no closing animation of their own (the card said it). */
static struct wm_window *quiet_win[WM_MAX];

static void quiet_add(struct wm_window *w)
{
    for (int k = 0; k < WM_MAX; k++)
        if (!quiet_win[k]) {
            quiet_win[k] = w;
            return;
        }
}

static int quiet_take(struct wm_window *w)
{
    for (int k = 0; k < WM_MAX; k++)
        if (quiet_win[k] == w) {
            quiet_win[k] = 0;
            return 1;
        }
    return 0;
}

static void close_window(struct wm_window *w)
{
    const struct app *a = wm_owner(w);
    if (a)
        kprintf("desktop: close %s\n", a->name);
    if (!quiet_take(w))
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
    sw_forget(w);
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
            cpu_relax();
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
        drw.x = drw.t = 0;                  /* the drawer goes as the app opens */
        drw.v = 0;
        drw_moving = 0;
        launch(list_app(h.app), (struct gfx_rect){ r.x + (r.w - HOME_ICON) / 2, r.y + 16, HOME_ICON, HOME_ICON });
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
    volume = machine_volume();  /* the kept level (60 the first time) */
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
    int fade = splash_fade_begin();     /* the first screen fades in from the logo */
    console_set_visible(0);     /* the log keeps recording, but no longer draws over us */
    wm_set_area((struct gfx_rect){ 0, T()->topbar_h, S()->w, S()->h - T()->topbar_h });
    started = 1;
    dock_layout(1);
    /* Self-tests may have left windows: they belong to no app, but show. */
    damage_all();
    shell_flush();
    if (users_need_login())
        lockscreen_login();     /* someone has to log in first (kernel/users.h) */
    if (splash_active()) {
        if (fade)
            splash_fade_end();
        splash_end();
        cursor_init();          /* hidden during the splash */
    }
    /* The boot budget's finish line (docs/BOOT-BUDGET.md: <= 5 s from stage 1). */
    kprintf("[boot] desktop t=%u\n", uptime_ms());
    kprintf("desktop: ready\n");     /* everything is up, the logo gone: the tests wait for this */
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
    pg_ok = 0;
    lockscreen_home_changed();
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

/* --- the app switcher (LiteKern Go, UI-PLAN.md) -----------------------------------------
 *
 * The Recent apps key: every open app as a card (a picture of it as it was),
 * oldest on the left, the one in front in the middle. Swipe sideways to look
 * along them; tap one to go to it; swipe one up to close the app; Close all
 * closes them all. Recent apps, Back or Home leave it. */

#define SW_GAP 40
#define SW_RADIUS 24
#define SW_RIM 2                /* px, the outline around a card */
#define SW_RIM_ALPHA 90
#define SW_LABEL_H 64           /* the app's icon and name above its card */


static int sw_n;
static struct wm_window *sw_win[WM_MAX];        /* oldest first */
static struct gfx_surface sw_snap[WM_MAX];
/* What drawing a card needs, kept apart from its window: Close all lets the apps go
 * while the cards are still flying up. */
static const struct app *sw_app[WM_MAX];
static char sw_label[WM_MAX][40];
static uint32_t *sw_img[WM_MAX];                /* each card as a picture, ready to copy: made on opening */
static int sw_img_ok[WM_MAX];
static int sw_up;                               /* Close all: the cards rise as it fades away */
static int sw_scroll;                           /* px: 0 has the last card in the middle */
static int sw_x0, sw_y0, sw_scroll0, sw_card = -1, sw_mode, sw_lift;
enum { SWM_NONE, SWM_PRESS, SWM_SCROLL, SWM_LIFT };
/* Let go, the cards glide to the nearest one and settle with it in the
 * middle, like One UI's; a flick carries on to the next (sw_snap_to). */
static struct spring sw_glide;          /* FX(sw_scroll) */
static int sw_gliding, sw_vx;           /* sw_vx: FX(px) per spring tick, the finger's speed */
static int sw_last_x;
static uint32_t sw_last_ms;

static int sw_card_w(void) { return S()->w * 3 / 5; }
static int sw_card_h(void) { return sw_card_w() * (S()->h - T()->topbar_h) / S()->w; }

static struct gfx_rect sw_card_rect(int i)
{
    int w = sw_card_w(), h = sw_card_h();
    int x = (S()->w - w) / 2 + (i - (sw_n - 1)) * (w + SW_GAP) + sw_scroll;
    int y = T()->topbar_h + SW_LABEL_H + (S()->h - T()->topbar_h - SW_LABEL_H - 160 - h) / 2;
    if (i == sw_card && sw_mode == SWM_LIFT)
        y += sw_lift;
    return (struct gfx_rect){ x, y, w, h };
}

static struct gfx_rect sw_clear_rect(void)
{
    return (struct gfx_rect){ S()->w / 2 - 120, S()->h - 120, 240, 72 };
}

/* The strip the cards slide in: all that changes while they scroll. */
static void sw_damage_cards(void)
{
    int y = T()->topbar_h + SW_LABEL_H + (S()->h - T()->topbar_h - SW_LABEL_H - 160 - sw_card_h()) / 2;
    if (screen_ready())
        shell_damage((struct gfx_rect){ 0, y - SW_LABEL_H - SW_RIM, S()->w, sw_card_h() + SW_LABEL_H + 2 * SW_RIM });
}

static int sw_isqrt(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v)
        r++;
    return r;
}

/* A card's picture, made once as the switcher opens: the app scaled to the card (each
 * pixel the average of four of its own), the corners rounded, and a thin light rim round
 * it so a dark app stands out from what's behind. Then drawing a card while it scrolls is
 * one copy, not a scale of the whole app: that was what made scrolling 25 frames a second. */
static void sw_make_img(int k)
{
    int cw = sw_card_w(), ch = sw_card_h(), iw = cw + 2 * SW_RIM, ih = ch + 2 * SW_RIM, R = SW_RADIUS;
    const struct gfx_surface *sn = &sw_snap[k];
    static int xmap[1024];
    sw_img_ok[k] = 0;
    if (cw > 1000 || sn->w < 2 || sn->h < 2)
        return;
    if (!sw_img[k]) {
        uint32_t bytes = (uint32_t)(iw * ih * 4);
        sw_img[k] = (uint32_t *)pmm_alloc_contiguous((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    }
    if (!sw_img[k])
        return;
    for (int x = 0; x < cw; x++)
        xmap[x] = x * sn->w / cw;
    for (int iy = 0; iy < ih; iy++) {
        uint32_t *row = sw_img[k] + (size_t)iy * (size_t)iw;
        int dy = iy < SW_RIM + R ? SW_RIM + R - iy : iy >= ih - SW_RIM - R ? iy - (ih - SW_RIM - R - 1) : 0;
        int xi0 = SW_RIM, xi1 = iw - SW_RIM, xo0 = 0, xo1 = iw;
        if (dy > 0) {                       /* a row of a corner: where the curve is */
            int ro = sw_isqrt((R + SW_RIM) * (R + SW_RIM) - dy * dy);
            xo0 = SW_RIM + R - ro;
            xo1 = iw - SW_RIM - R + ro;
            if (dy <= R) {
                int ri = sw_isqrt(R * R - dy * dy);
                xi0 = SW_RIM + R - ri;
                xi1 = iw - SW_RIM - R + ri;
            } else {
                xi0 = xi1 = iw / 2;         /* above the card: just the rim */
            }
        }
        int sy = (iy - SW_RIM) * sn->h / ch;
        sy = sy < 0 ? 0 : sy >= sn->h ? sn->h - 1 : sy;
        const uint32_t *p0 = sn->px + (size_t)sy * (size_t)sn->stride;
        const uint32_t *p1 = sy + 1 < sn->h ? p0 + sn->stride : p0;
        for (int x = 0; x < iw; x++) {
            if (x >= xi0 && x < xi1) {
                int xs = xmap[x - SW_RIM < 0 ? 0 : x - SW_RIM >= cw ? cw - 1 : x - SW_RIM];
                int xs1 = xs + 1 < sn->w ? xs + 1 : xs;
                row[x] = 0xff000000u | (((p0[xs] & 0xfcfcfcu) >> 2) + ((p0[xs1] & 0xfcfcfcu) >> 2) +
                         ((p1[xs] & 0xfcfcfcu) >> 2) + ((p1[xs1] & 0xfcfcfcu) >> 2));
            } else if (x >= xo0 && x < xo1) {
                row[x] = ((uint32_t)SW_RIM_ALPHA << 24) | 0xffffffu;
            } else {
                row[x] = 0;
            }
        }
    }
    sw_img_ok[k] = 1;
}

static void sw_set(int open)
{
    if (sw_open == open)
        return;
    sw_open = open;
    sw_mode = SWM_NONE;
    sw_card = -1;
    sw_gliding = 0;
    if (open && !sw_alpha)
        backdrop_take(0);
    fx_start(&swv, open);               /* it fades in, or out over what's under it */
    if (open) {
        sw_n = 0;
        sw_up = 0;
        for (int i = 0; i < wm_count() && sw_n < WM_MAX; i++)
            if (snapshot(sw_n, wm_at(i), &sw_snap[sw_n])) {
                const char *name = name_of(wm_at(i));
                size_t len = strlen(name) < sizeof(sw_label[0]) - 1 ? strlen(name) : sizeof(sw_label[0]) - 1;
                sw_win[sw_n] = wm_at(i);
                sw_app[sw_n] = wm_owner(wm_at(i));
                memcpy(sw_label[sw_n], name, len);
                sw_label[sw_n][len] = '\0';
                sw_make_img(sw_n);
                sw_n++;
            }
        sw_scroll = 0;
        wm_pointer_gone();
        kprintf("switcher: open, %d app%s\n", sw_n, sw_n == 1 ? "" : "s");
        if (!sw_n)
            kprintf("desktop: no recent apps\n");
    } else {
        kprintf("switcher: closed\n");
    }
    damage_all();
}

/* An app went away (closed here, or it ended by itself). */
static void sw_forget(struct wm_window *w)
{
    if (sw_up)                          /* Close all: the cards stay until they've flown */
        return;
    for (int i = 0; i < sw_n; i++)
        if (sw_win[i] == w) {
            for (int k = i; k < sw_n - 1; k++) {
                sw_win[k] = sw_win[k + 1];
                sw_snap[k] = sw_snap[k + 1];
            }
            sw_n--;

            break;
        }
    if (sw_scroll > 0 && sw_n)
        sw_scroll = 0;
    sw_gliding = 0;
    if (sw_open)
        backdrop_take(0);               /* the app's gone from behind it too */
    damage_all();
}

static void sw_close_app(int i)
{
    struct wm_window *w = sw_win[i];
    kprintf("switcher: close %s\n", name_of(w));
    wm_set_hidden(w, 1);                /* out of the picture behind the cards, with no ceremony */
    quiet_add(w);
    sw_forget(w);
    wm_request_close(w);
}

/* Close all: every app goes at once, and the cards rise and fade together. */
static void sw_close_all(void)
{
    kprintf("switcher: close all\n");
    for (int i = 0; i < sw_n; i++) {
        wm_set_hidden(sw_win[i], 1);
        quiet_add(sw_win[i]);
        wm_request_close(sw_win[i]);
    }
    backdrop_take(0);                   /* what's behind them now: the home screen */
    sw_up = 1;
    sw_set(0);
}

static void sw_go(int i)
{
    struct wm_window *w = sw_win[i];
    kprintf("switcher: to %s\n", name_of(w));
    sw_set(0);
    spring_snap(&swv);                  /* gone at once: the picture behind it is out of date */
    sw_alpha = 0;
    shell_flush();
    open_from_centre(w);                /* it comes up out of the middle, not out of its icon */
}

static void draw_switcher(void)
{
    const struct theme *t = T();
    int al = sw_alpha, lift = sw_up ? -(255 - al) * 900 / 255 : (255 - al) * 160 / 255;
    /* fading in, the cards rise into place; fading out for Close all, they rise away */
    gfx_darken(VS, VX(0), VY(0), S()->w, S()->h, (uint32_t)(190 * al / 255));
    if (!sw_n) {
        const char *none = "No recent apps";
        if (al > 160)
            text_draw(VS, VX((S()->w - text_width(none, TEXT_BODY)) / 2), VY(S()->h / 2), none, TEXT_BODY, SHELL_FG);
        return;
    }
    for (int i = 0; i < sw_n; i++) {
        struct gfx_rect r = sw_card_rect(i);
        r.y += lift;
        if (!visible((struct gfx_rect){ r.x - SW_RIM, r.y - SW_LABEL_H, r.w + 2 * SW_RIM, r.h + SW_LABEL_H + 2 * SW_RIM }))
            continue;
        if (sw_app[i])
            draw_icon(sw_app[i], (struct gfx_rect){ r.x, r.y - SW_LABEL_H + 8, 44, 44 }, (uint32_t)al);
        if (al > 160)
            text_draw_fit(VS, VX(r.x + 56), VY(r.y - SW_LABEL_H + 8 + (44 - text_height(TEXT_BOLD)) / 2),
                          sw_label[i], r.w - 56, TEXT_BOLD, SHELL_FG);
        if (al >= 255 && sw_img_ok[i]) {
            /* Settled, or being scrolled: the ready-made picture, one copy. */
            gfx_blit_alpha(VS, VX(r.x - SW_RIM), VY(r.y - SW_RIM), sw_img[i], r.w + 2 * SW_RIM, r.h + 2 * SW_RIM,
                           r.w + 2 * SW_RIM);
            continue;
        }
        /* Fading: a thin light rim, then the app scaled at the fade's alpha. */
        gfx_blend_round_rect(VS, VX(r.x - SW_RIM), VY(r.y - SW_RIM), r.w + 2 * SW_RIM, r.h + 2 * SW_RIM,
                             SW_RADIUS + SW_RIM, 0xffffff, (uint32_t)(SW_RIM_ALPHA * al / 255));
        gfx_blit_scaled(VS, (struct gfx_rect){ VX(r.x), VY(r.y), r.w, r.h }, &sw_snap[i],
                        (struct gfx_rect){ 0, 0, sw_snap[i].w, sw_snap[i].h }, (uint32_t)al, SW_RADIUS);
    }
    if (al > 160)
        text_draw(VS, VX(48), VY(t->topbar_h + 24 + lift / 4), "Recent apps", TEXT_HEADING, SHELL_FG);
    struct gfx_rect c = sw_clear_rect();
    c.y += lift < 0 ? 0 : lift;
    gfx_blend_round_rect(VS, VX(c.x), VY(c.y), c.w, c.h, c.h / 2, t->button_bg, (uint32_t)al);
    if (al > 160)
        text_draw(VS, VX(c.x + (c.w - text_width("Close all", TEXT_BOLD)) / 2),
                  VY(c.y + (c.h - text_height(TEXT_BOLD)) / 2), "Close all", TEXT_BOLD, SHELL_FG);
}

/* Let go after a swipe: on to the card nearest where the flick would take
 * the cards (one card at most past the nearest one), on a spring. */
static void sw_snap_to(void)
{
    int step = sw_card_w() + SW_GAP, max = (sw_n - 1) * step;
    int near = (sw_scroll + step / 2) / step;
    int flung = (sw_scroll + sw_vx * 10 / 256 + step / 2) / step;     /* ~10 ticks of the flick */
    int to = flung > near + 1 ? near + 1 : flung < near - 1 ? near - 1 : flung;
    to = to < 0 ? 0 : to * step > max ? max / step : to;
    sw_glide = (struct spring){ FX(sw_scroll), sw_vx, FX(to * step) };
    sw_gliding = 1;
    if (!fx_moving) {
        last_fx_tick = fx_began = uptime_ms();
        fx_frames = fx_slowest = 0;
    }
    fx_moving = 1;
    kprintf("switcher: settles on %s\n", name_of(sw_win[sw_n - 1 - to]));
}

static void sw_pointer(int x, int y, uint8_t buttons, int down)
{
    if (down) {
        sw_gliding = 0;                 /* a finger stops the glide where it is */
        sw_x0 = x;
        sw_y0 = y;
        sw_last_x = x;
        sw_last_ms = uptime_ms();
        sw_vx = 0;
        sw_scroll0 = sw_scroll;
        sw_mode = SWM_PRESS;
        sw_card = -1;
        for (int i = 0; i < sw_n; i++)
            if (inside(sw_card_rect(i), x, y))
                sw_card = i;
        return;
    }
    if (sw_mode == SWM_NONE)
        return;
    int dx = x - sw_x0, dy = y - sw_y0, held = buttons & MOUSE_LEFT;
    if (sw_mode == SWM_PRESS && held) {
        if (dx > SWIPE_SLOP || dx < -SWIPE_SLOP)
            sw_mode = SWM_SCROLL;
        else if (dy < -SWIPE_SLOP && sw_card >= 0)
            sw_mode = SWM_LIFT;
    }
    if (sw_mode == SWM_SCROLL) {
        int max = (sw_n - 1) * (sw_card_w() + SW_GAP);
        sw_scroll = sw_scroll0 + dx;
        /* Past the ends it gives a little (a third of the way), then springs back. */
        if (sw_scroll < 0)
            sw_scroll /= 3;
        else if (sw_scroll > max)
            sw_scroll = max + (sw_scroll - max) / 3;
        uint32_t now = uptime_ms(), dt = now - sw_last_ms;
        if (x != sw_last_x && dt) {     /* the finger's speed, smoothed */
            int v = FX(x - sw_last_x) * TICK_MS / (int)dt;
            sw_vx = (sw_vx + v * 3) / 4;
            sw_last_x = x;
            sw_last_ms = now;
        }
        sw_damage_cards();              /* only the strip the cards move in */
    } else if (sw_mode == SWM_LIFT) {
        sw_lift = dy < 0 ? dy : 0;
        damage_all();
    }
    if (held)
        return;
    int mode = sw_mode, card = sw_card;
    sw_mode = SWM_NONE;
    sw_card = -1;
    sw_lift = 0;
    damage_all();
    if (mode == SWM_LIFT) {
        if (-dy > sw_card_h() / 4)
            sw_close_app(card);         /* swiped up: the app closes */
        return;
    }
    if (mode == SWM_SCROLL) {
        if (uptime_ms() - sw_last_ms > 80)
            sw_vx = 0;                  /* it stopped before letting go: no flick */
        sw_snap_to();
        return;
    }
    if (mode != SWM_PRESS)
        return;
    if (card >= 0) {
        sw_go(card);
    } else if (sw_n && inside(sw_clear_rect(), x, y)) {
        sw_close_all();
    } else {
        sw_set(0);                      /* a tap on nothing: back where it was */
    }
}

/* --- quick settings and notifications (LiteKern Go, UI-PLAN.md) -------------------------
 *
 * Pulled down from the status bar (or a tap on it, or Home held), over
 * whatever is on screen, which dims: the control center, solid modules on a
 * 4 x 4 grid: connections and Lock in a 2 x 2 block, tall brightness and
 * volume sliders, wide Do Not Disturb and Dark Mode, and four shortcuts. Over
 * them the date and a Settings button; under them the newest of the apps'
 * notifications (SYS_NOTIFY): a tap opens the app, a swipe sideways dismisses
 * one, Clear all the lot. Back, Home, a tap on the dimmed part or a swipe up
 * closes it. */

#define QS_HEAD 120             /* the date and Settings */
#define QS_SIDE 32
#define QS_CELL 146
#define QS_GAP 24
#define QS_BLOCK (2 * QS_CELL + QS_GAP)
#define QS_GRID_H (4 * QS_CELL + 3 * QS_GAP)
#define QS_ROUND 116            /* a round button in the block */
#define QS_MOD_R 52             /* a module's corners */
#define QS_NOTES 2              /* notifications that fit under the grid */
#define NOTE_MAX 8
#define NOTE_H 112
#define NOTE_GAP 12
#define QS_CLEAR_H 96
#define A_QS_DIM 215            /* black over what's behind it */

struct note {
    const struct app *app;
    char title[K86_NOTE_TITLE], body[K86_NOTE_BODY];
};
static struct note notes[NOTE_MAX];     /* newest first */
static int nnotes;
static int dnd;                         /* Do not disturb: kept, but not in the status bar */

static struct spring qs;                /* how far down the panel is: FX(px), 0 up */
static int qs_moving;
static uint32_t last_qs_tick;
/* A finger on it: where it went down, what it's doing. */
static int qs_x0, qs_y0, qs_from, qs_finger;
enum { QF_NONE, QF_PULL, QF_PANEL, QF_SLIDER, QF_NOTE, QF_DIM };
static int qs_note = -1, qs_note_dx;

static int qs_sl, qs_sl_from, qs_held = -1;     /* the slider held, its value then; the module held */

enum { T_WIFI, T_DARK, T_DND, T_LOCK, T_AIR, T_BT, T_FLASH, T_TIMER, T_CALC, T_SHOT, QS_NTILES };

static int notes_shown(void) { return nnotes < QS_NOTES ? nnotes : QS_NOTES; }

static int qs_h(void)
{
    int list = nnotes ? notes_shown() * (NOTE_H + NOTE_GAP) + QS_CLEAR_H : 90;
    return QS_HEAD + QS_GRID_H + QS_GAP + list + T()->margin;
}

static int qs_shown(void) { return qs.x > 0 || qs.t > 0; }
static int qs_top(void) { return T()->topbar_h - qs_h() + qs.x / 256; }

static struct gfx_rect qs_rect(void) { return (struct gfx_rect){ 0, qs_top(), S()->w, qs_h() }; }

static struct gfx_rect qs_gear_rect(void)
{
    return (struct gfx_rect){ S()->w - T()->margin - 88, qs_top() + 16, 88, 88 };
}

static int qs_grid_y(void) { return qs_top() + QS_HEAD; }

static struct gfx_rect qs_block_rect(void)
{
    return (struct gfx_rect){ QS_SIDE, qs_grid_y(), QS_BLOCK, QS_BLOCK };
}

static struct gfx_rect qs_tile_rect(int i)
{
    int y = qs_grid_y(), row3 = y + QS_BLOCK + QS_GAP, row4 = row3 + QS_CELL + QS_GAP;
    int right = i == T_WIFI || i == T_LOCK, low = i == T_BT || i == T_LOCK;
    int inset = (QS_BLOCK - 2 * QS_ROUND) / 3;
    switch (i) {
    case T_DND:   return (struct gfx_rect){ QS_SIDE, row3, QS_BLOCK, QS_CELL };
    case T_DARK:  return (struct gfx_rect){ QS_SIDE + QS_BLOCK + QS_GAP, row3, QS_BLOCK, QS_CELL };
    case T_FLASH: return (struct gfx_rect){ QS_SIDE, row4, QS_CELL, QS_CELL };
    case T_TIMER: return (struct gfx_rect){ QS_SIDE + QS_CELL + QS_GAP, row4, QS_CELL, QS_CELL };
    case T_CALC:  return (struct gfx_rect){ QS_SIDE + 2 * (QS_CELL + QS_GAP), row4, QS_CELL, QS_CELL };
    case T_SHOT:  return (struct gfx_rect){ QS_SIDE + 3 * (QS_CELL + QS_GAP), row4, QS_CELL, QS_CELL };
    default:
        return (struct gfx_rect){ QS_SIDE + inset + right * (QS_ROUND + inset), y + inset + low * (QS_ROUND + inset),
                                  QS_ROUND, QS_ROUND };
    }
}

static struct gfx_rect qs_slider_rect(int k)    /* 0 brightness, 1 volume */
{
    return (struct gfx_rect){ QS_SIDE + QS_BLOCK + QS_GAP + k * (QS_CELL + QS_GAP), qs_grid_y(), QS_CELL, QS_BLOCK };
}

static struct gfx_rect note_rect(int k)
{
    int y = qs_grid_y() + QS_GRID_H + QS_GAP + k * (NOTE_H + NOTE_GAP);
    return (struct gfx_rect){ QS_SIDE, y, S()->w - 2 * QS_SIDE, NOTE_H };
}

static struct gfx_rect qs_clear_rect(void)
{
    struct gfx_rect last = note_rect(notes_shown());
    return (struct gfx_rect){ S()->w / 2 - 120, last.y + 8, 240, 72 };
}

static void qs_set(int open, int instant)
{
    static int was;
    qs.t = open ? FX(qs_h()) : 0;
    if (instant)
        spring_snap(&qs);
    if (was != open) {
        kprintf("qs: %s\n", open ? "open" : "closed");
        if (open) {                             /* the brightness may have been set in Settings */
            struct k86_machine now;
            machine_status(&now);
            machine_shown.brightness = now.brightness;
        }
    }
    was = open;
    qs_moving = qs.x != qs.t || qs.v;
    last_qs_tick = uptime_ms();
    damage_all();
}

static void qs_tick(void)
{
    if (!qs_moving)
        return;
    uint32_t now = uptime_ms(), steps = (now - last_qs_tick) / TICK_MS;
    if (!steps)
        return;
    last_qs_tick = now;
    for (uint32_t s = 0; s < (steps > 4 ? 4 : steps); s++)
        qs_moving = spring_step(&qs, 50, 100);
    damage_all();
    shell_flush();
}

static void note_copy(char *dst, const char *src, int max)
{
    int n = 0;
    while (src[n] && n < max - 1) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

void desktop_notify(const void *owner, const char *title, const char *body)
{
    const struct app *a = owner;
    int k = 0;
    while (k < nnotes && !(notes[k].app == a && !strcmp(notes[k].title, title)))
        k++;                                /* the same one again: it replaces it */
    if (k == nnotes && nnotes < NOTE_MAX)
        nnotes++;
    if (k == NOTE_MAX)
        k = NOTE_MAX - 1;                   /* full: the oldest goes */
    for (; k > 0; k--)
        notes[k] = notes[k - 1];
    notes[0].app = a;
    note_copy(notes[0].title, title, sizeof(notes[0].title));
    note_copy(notes[0].body, body, sizeof(notes[0].body));
    kprintf("notify: %s: %s\n", a ? a->name : "?", notes[0].title);
    if (qs.t > 0)
        qs.t = FX(qs_h());                  /* the panel grows with it */
    damage_all();
    lockscreen_redraw();                    /* locked: its card shows there */
}

int desktop_note_count(void)
{
    return nnotes;
}

const char *desktop_note_title(int i)
{
    return i >= 0 && i < nnotes ? notes[i].title : "";
}

const char *desktop_note_app(int i)
{
    return i >= 0 && i < nnotes && notes[i].app ? notes[i].app->name : "";
}

void desktop_note_icon(int i, struct gfx_surface *s, int x, int y, int size)
{
    const struct icon *ic = i >= 0 && i < nnotes && notes[i].app ? icon_find(notes[i].app->icon) : 0;
    const uint32_t *sized = ic ? icon_sized(ic, size, size) : 0;
    if (sized)
        gfx_blit_alpha(s, x, y, sized, size, size, size);
}

static void note_remove(int k)
{
    kprintf("notify: dismissed %s\n", notes[k].title);
    for (; k < nnotes - 1; k++)
        notes[k] = notes[k + 1];
    nnotes--;
    if (qs.t > 0) {
        qs.t = FX(qs_h());
        spring_snap(&qs);
    }
    damage_all();
}

static int wifi_on(void) { return net_shown.wifi != K86_NET_NONE && net_shown.wifi != K86_NET_OFF; }

static int tile_on(int i)
{
    switch (i) {
    case T_WIFI: return wifi_on();
    case T_DARK: return !theme_is_light();
    case T_DND:  return dnd;
    default:     return 0;
    }
}

/* Airplane mode, Bluetooth and the flashlight have nothing behind them yet. */
static int tile_works(int i)
{
    if (i == T_AIR || i == T_BT || i == T_FLASH)
        return 0;
    return i != T_WIFI || net_shown.wifi != K86_NET_NONE;
}

static void show_toast(const char *text);
static void screenshot(void);
static int volume;

static void open_named(const char *name, struct gfx_rect from)
{
    qs_set(0, 1);
    for (int i = 0; i < builtin_app_count; i++)
        if (!strcmp(builtin_apps[i].name, name))
            launch(i, from);
}

static void tile_tap(int i)
{
    switch (i) {
    case T_AIR:
    case T_BT:
    case T_FLASH:
        show_toast("Not available yet");
        break;
    case T_TIMER:
        open_named("Clock", qs_tile_rect(i));
        return;
    case T_CALC:
        open_named("Calculator", qs_tile_rect(i));
        return;
    case T_SHOT:
        qs_set(0, 1);
        shell_flush();
        screenshot();
        return;
    case T_WIFI:
        if (tile_works(i))
            net_wifi_set(!wifi_on());
        break;
    case T_DARK:
        theme_set(!theme_is_light(), theme_accent_index());
        wallpaper_refresh();
        wm_theme_changed();
        settings_save();
        kprintf("qs: %s style\n", theme_is_light() ? "light" : "dark");
        break;
    case T_DND:
        dnd = !dnd;
        kprintf("qs: do not disturb %s\n", dnd ? "on" : "off");
        break;
    case T_LOCK:
        qs_set(0, 1);
        shell_flush();
        kprintf("shell: lock (quick settings)\n");
        lockscreen_lock();
        return;
    }
    damage_all();
}

/* The brightness goes to the screen's panel as commands that interrupt the picture for a
 * moment (the msm display driver's way with commands while video runs: the maintainers know
 * of it, and it's worse while the screen is redrawing). So the slider follows the finger at
 * once, and the panel hears the value once, when the finger lifts. */
static int qs_bright_want, qs_bright_sent = -1;

static void bright_flush(void)
{
    if (qs_bright_want != qs_bright_sent && machine_set_brightness(qs_bright_want) == 0)
        qs_bright_sent = qs_bright_want;
    machine_status(&machine_shown);         /* what the screen really is */
}

/* A slider dragged: from its value when the finger landed, by how far it's moved. */
static void slider_drag(int y)
{
    int least = qs_sl == 0 ? 5 : 0, p = qs_sl_from + (qs_y0 - y) * 100 / QS_BLOCK;
    p = p > 100 ? 100 : p < least ? least : p;
    if (qs_sl == 1) {
        volume = p;
        machine_set_volume(p);
        damage_all();
    } else {
        qs_bright_want = p;
        machine_shown.brightness = p;       /* the slider follows the finger */
        damage_all();
    }
}

static void draw_tile_icon(int i, int cx, int cy, uint32_t c, uint32_t bg)
{
    switch (i) {
    case T_WIFI: {
        struct k86_net n = net_shown;
        if (wifi_on()) {
            n.wifi = K86_NET_UP;
            n.signal = 100;
        }
        uint32_t keep = bar_fg;
        bar_fg = c;
        draw_net_icon(cx, cy, &n, bg);
        bar_fg = keep;
        break;
    }
    case T_DARK:                            /* a moon: a circle with a bite out of it */
        gfx_fill_circle(VS, VX(cx), VY(cy), 22, c);
        gfx_fill_circle(VS, VX(cx + 12), VY(cy - 10), 18, bg);
        break;
    case T_DND:                             /* a circle with a bar */
        gfx_fill_circle(VS, VX(cx), VY(cy), 22, c);
        gfx_fill_rect(VS, VX(cx - 12), VY(cy - 3), 24, 6, bg);
        break;
    case T_LOCK:                            /* a padlock */
        gfx_fill_round_rect(VS, VX(cx - 18), VY(cy - 2), 36, 28, 4, c);
        gfx_fill_circle(VS, VX(cx), VY(cy - 8), 13, c);
        gfx_fill_circle(VS, VX(cx), VY(cy - 8), 7, bg);
        gfx_fill_rect(VS, VX(cx - 7), VY(cy - 8), 14, 6, bg);
        break;
    case T_AIR:                             /* a plane, from above */
        gfx_thick_line(VS, VX(cx), VY(cy - 24), VX(cx), VY(cy + 24), 6, c);
        gfx_thick_line(VS, VX(cx), VY(cy - 6), VX(cx - 24), VY(cy + 10), 6, c);
        gfx_thick_line(VS, VX(cx), VY(cy - 6), VX(cx + 24), VY(cy + 10), 6, c);
        gfx_thick_line(VS, VX(cx), VY(cy + 18), VX(cx - 10), VY(cy + 25), 5, c);
        gfx_thick_line(VS, VX(cx), VY(cy + 18), VX(cx + 10), VY(cy + 25), 5, c);
        break;
    case T_BT: {                            /* the Bluetooth rune */
        static const int px[6] = { -12, 12, 0, 0, 12, -12 }, py[6] = { -12, 12, 24, -24, -12, 12 };
        for (int k = 0; k < 5; k++)
            gfx_thick_line(VS, VX(cx + px[k]), VY(cy + py[k]), VX(cx + px[k + 1]), VY(cy + py[k + 1]), 5, c);
        break;
    }
    case T_FLASH:                           /* a bolt */
        gfx_thick_line(VS, VX(cx + 8), VY(cy - 26), VX(cx - 12), VY(cy + 3), 6, c);
        gfx_thick_line(VS, VX(cx - 12), VY(cy + 3), VX(cx + 12), VY(cy - 3), 6, c);
        gfx_thick_line(VS, VX(cx + 12), VY(cy - 3), VX(cx - 8), VY(cy + 26), 6, c);
        break;
    case T_TIMER:                           /* a clock face with a button on top */
        gfx_fill_circle(VS, VX(cx), VY(cy + 3), 24, c);
        gfx_fill_circle(VS, VX(cx), VY(cy + 3), 19, bg);
        gfx_thick_line(VS, VX(cx), VY(cy + 3), VX(cx), VY(cy - 9), 4, c);
        gfx_thick_line(VS, VX(cx), VY(cy + 3), VX(cx + 9), VY(cy + 8), 4, c);
        gfx_thick_line(VS, VX(cx - 7), VY(cy - 28), VX(cx + 7), VY(cy - 28), 5, c);
        break;
    case T_CALC:                            /* a calculator */
        gfx_fill_round_rect(VS, VX(cx - 20), VY(cy - 26), 40, 52, 8, c);
        gfx_fill_round_rect(VS, VX(cx - 13), VY(cy - 19), 26, 10, 2, bg);
        for (int r = 0; r < 2; r++)
            for (int q = 0; q < 3; q++)
                gfx_fill_circle(VS, VX(cx - 11 + q * 11), VY(cy + 2 + r * 12), 3, bg);
        break;
    case T_SHOT:                            /* the four corners of the screen */
        for (int q = 0; q < 4; q++) {
            int sx = q & 1 ? 1 : -1, sy = q & 2 ? 1 : -1;
            gfx_thick_line(VS, VX(cx + sx * 23), VY(cy + sy * 23), VX(cx + sx * 9), VY(cy + sy * 23), 5, c);
            gfx_thick_line(VS, VX(cx + sx * 23), VY(cy + sy * 23), VX(cx + sx * 23), VY(cy + sy * 9), 5, c);
        }
        break;
    }
}

static void date_long(char date[40])
{
    static const char *const days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
                                        "Friday", "Saturday" };
    static const char *const months[] = { "January", "February", "March", "April", "May", "June", "July",
                                          "August", "September", "October", "November", "December" };
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
    date[n] = '\0';
}

/* One module's solid colour: lit, held, or at rest. */
static uint32_t qs_module_bg(int on, int held)
{
    const struct theme *t = T();
    return on ? t->accent_bg : held ? t->button_bg : t->headerbar_bg;
}

/* A tall slider, filled up from the bottom, its icon inside. */
static void qs_slider(int k, int value)
{
    const struct theme *t = T();
    struct gfx_rect r = qs_slider_rect(k);
    gfx_fill_round_rect(VS, VX(r.x), VY(r.y), r.w, r.h, QS_MOD_R, t->headerbar_bg);
    int lit = 0;
    if (value >= 0) {
        int h = r.h * value / 100;
        struct gfx_rect low = { r.x, r.y + r.h - h, r.w, h };
        struct gfx_view v = gfx_view_clip(V, low), *outer = V;
        V = &v;                             /* the same rounded shape: only its lower part shows */
        gfx_fill_round_rect(VS, VX(r.x), VY(r.y), r.w, r.h, QS_MOD_R, t->fg);
        V = outer;
        lit = value >= 16;
    }
    int cx = r.x + r.w / 2, cy = r.y + r.h - 56;
    uint32_t c = value < 0 ? t->fg_dim : lit ? t->headerbar_bg : t->fg;
    if (k == 0) {
        static const int dx[8] = { 0, 15, 22, 15, 0, -15, -22, -15 }, dy[8] = { -22, -15, 0, 15, 22, 15, 0, -15 };
        gfx_fill_circle(VS, VX(cx), VY(cy), 12, c);
        for (int q = 0; q < 8; q++)
            gfx_fill_circle(VS, VX(cx + dx[q]), VY(cy + dy[q]), 3, c);
    } else {
        gfx_fill_rect(VS, VX(cx - 20), VY(cy - 8), 12, 16, c);
        gfx_thick_line(VS, VX(cx - 8), VY(cy - 8), VX(cx + 6), VY(cy - 20), 5, c);
        gfx_thick_line(VS, VX(cx + 6), VY(cy - 20), VX(cx + 6), VY(cy + 20), 5, c);
        gfx_thick_line(VS, VX(cx + 6), VY(cy + 20), VX(cx - 8), VY(cy + 8), 5, c);
        gfx_thick_line(VS, VX(cx + 14), VY(cy - 8), VX(cx + 18), VY(cy), 4, c);
        gfx_thick_line(VS, VX(cx + 18), VY(cy), VX(cx + 14), VY(cy + 8), 4, c);
    }
}

/* The modules, at the panel's place. Their words sit on solid colour, so they
 * can go in the picture below; words over the dimmed screen can't. */
static void draw_qs_modules(void)
{
    const struct theme *t = T();
    struct gfx_rect g = qs_gear_rect();
    int gx = g.x + g.w / 2, gy = g.y + g.h / 2;
    uint32_t gear_bg = t->headerbar_bg;
    gfx_fill_circle(VS, VX(gx), VY(gy), g.w / 2, gear_bg);
    gfx_fill_circle(VS, VX(gx), VY(gy), 20, t->fg);                 /* a cog */
    static const int cog_dx[8] = { 0, 18, 26, 18, 0, -18, -26, -18 }, cog_dy[8] = { -26, -18, 0, 18, 26, 18, 0, -18 };
    for (int k = 0; k < 8; k++)
        gfx_fill_circle(VS, VX(gx + cog_dx[k]), VY(gy + cog_dy[k]), 7, t->fg);
    gfx_fill_circle(VS, VX(gx), VY(gy), 9, gear_bg);
    /* The connections and Lock. */
    struct gfx_rect b = qs_block_rect();
    gfx_fill_round_rect(VS, VX(b.x), VY(b.y), b.w, b.h, QS_MOD_R + 8, t->headerbar_bg);
    static const int round[4] = { T_AIR, T_WIFI, T_BT, T_LOCK };
    for (int k = 0; k < 4; k++) {
        int i = round[k], on = tile_on(i), works = tile_works(i);
        struct gfx_rect r = qs_tile_rect(i);
        uint32_t bg = on ? t->accent_bg : qs_held == i ? t->button_hover : t->button_bg;
        uint32_t fg = on ? t->accent_fg : works ? t->fg : t->fg_dim;
        gfx_fill_circle(VS, VX(r.x + r.w / 2), VY(r.y + r.h / 2), r.w / 2, bg);
        draw_tile_icon(i, r.x + r.w / 2, r.y + r.h / 2, fg, bg);
    }
    /* Brightness (none on a phone without a backlight) and volume. */
    qs_slider(0, machine_shown.brightness);
    qs_slider(1, volume);
    /* Do Not Disturb and Dark Mode. */
    static const int wide[2] = { T_DND, T_DARK };
    static const char *const wide_name[2] = { "Do Not Disturb", "Dark Mode" };
    for (int k = 0; k < 2; k++) {
        int i = wide[k], on = tile_on(i);
        struct gfx_rect r = qs_tile_rect(i);
        uint32_t bg = qs_module_bg(on, qs_held == i), fg = on ? t->accent_fg : t->fg;
        gfx_fill_round_rect(VS, VX(r.x), VY(r.y), r.w, r.h, QS_MOD_R, bg);
        draw_tile_icon(i, r.x + 48, r.y + r.h / 2, fg, bg);
        int tx = r.x + 82, by = r.y + (r.h - text_height(TEXT_BOLD) - text_height(TEXT_SMALL)) / 2;
        text_draw_fit(VS, VX(tx), VY(by), wide_name[k], r.w - 90, TEXT_BOLD, fg);
        text_draw(VS, VX(tx), VY(by + text_height(TEXT_BOLD)), on ? "On" : "Off", TEXT_SMALL, on ? fg : t->fg_dim);
    }
    /* The shortcuts. */
    static const int small[4] = { T_FLASH, T_TIMER, T_CALC, T_SHOT };
    for (int k = 0; k < 4; k++) {
        int i = small[k], works = tile_works(i);
        struct gfx_rect r = qs_tile_rect(i);
        uint32_t bg = qs_module_bg(0, qs_held == i);
        gfx_fill_round_rect(VS, VX(r.x), VY(r.y), r.w, r.h, QS_MOD_R, bg);
        draw_tile_icon(i, r.x + r.w / 2, r.y + r.h / 2, works ? t->fg : t->fg_dim, bg);
    }
    if (!nnotes)
        return;
    for (int k = 0; k < notes_shown(); k++) {
        struct gfx_rect r = note_rect(k);
        if (k == qs_note)
            r.x += qs_note_dx;
        gfx_fill_round_rect(VS, VX(r.x), VY(r.y), r.w, r.h, 44, t->headerbar_bg);
        if (notes[k].app)
            draw_icon(notes[k].app, (struct gfx_rect){ r.x + 20, r.y + (r.h - 56) / 2, 56, 56 }, 255);
        int tx = r.x + 20 + 56 + 20, tw = r.w - (tx - r.x) - 20;
        text_draw_fit(VS, VX(tx), VY(r.y + 16), notes[k].title, tw, TEXT_BOLD, t->fg);
        text_draw_fit(VS, VX(tx), VY(r.y + 16 + text_height(TEXT_BOLD) + 4), notes[k].body, tw, TEXT_SMALL,
                      t->fg_dim);
    }
    struct gfx_rect c = qs_clear_rect();
    gfx_fill_round_rect(VS, VX(c.x), VY(c.y), c.w, c.h, c.h / 2, t->headerbar_bg);
    text_draw(VS, VX(c.x + (c.w - text_width("Clear all", TEXT_BOLD)) / 2), VY(c.y + (c.h - text_height(TEXT_BOLD)) / 2),
              "Clear all", TEXT_BOLD, t->fg);
}

/* What sits straight on the dimming: drawn each frame. */
static void draw_qs_words(void)
{
    struct gfx_rect p = qs_rect();
    if (rtc && clock_shown.month) {
        char date[40];
        date_long(date);
        text_draw(VS, VX(QS_SIDE + 8), VY(p.y + (QS_HEAD - text_height(TEXT_HEADING)) / 2), date, TEXT_HEADING, SHELL_FG);
    }
    if (!nnotes) {
        const char *none = "No notifications";
        text_draw(VS, VX((S()->w - text_width(none, TEXT_BODY)) / 2), VY(note_rect(0).y + 24), none, TEXT_BODY,
                  gfx_mix(SHELL_FG, 0, 190));
    } else if (nnotes > notes_shown()) {
        struct gfx_rect c = qs_clear_rect();
        char more[12] = "+";
        more[1] = (char)('0' + nnotes - notes_shown());
        memcpy(more + 2, " more", 6);
        text_draw(VS, VX(QS_SIDE + 8), VY(c.y + (c.h - text_height(TEXT_SMALL)) / 2), more, TEXT_SMALL, SHELL_FG);
    }
}

/* While it moves the modules are one picture, drawn once and slid: drawing them
 * all again each frame (icons, words, a dozen rounded shapes) is too much here. */
#define QS_KEY 0x010203u                /* see-through: near black, as smoothed corners blend with it */
static uint32_t *qs_px;
static int qs_pic_ok;

static struct gfx_surface qs_pic(void)
{
    return (struct gfx_surface){ qs_px, S()->w, S()->h, S()->w };
}

static void qs_pic_take(void)
{
    if (!qs_px)
        qs_px = (uint32_t *)pmm_alloc_contiguous(((uint32_t)(S()->w * S()->h * 4) + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!qs_px)
        return;                         /* no memory: drawn each frame */
    struct gfx_view v = { qs_pic(), 0, 0 };
    struct gfx_view *outer = V;
    int at = qs.x;
    V = &v;
    qs.x = FX(qs_h());                  /* drawn all the way open */
    gfx_fill_rect(VS, 0, 0, VS->w, VS->h, QS_KEY);
    draw_qs_modules();
    qs.x = at;
    V = outer;
    qs_pic_ok = 1;
}

static void draw_qs(void)
{
    if (!qs_shown()) {
        qs_taken = 0;
        qs_pic_ok = 0;
        return;
    }
    int open = qs_h() ? qs.x * 255 / FX(qs_h()) : 0, settled = qs.x == FX(qs_h());
    open = open > 255 ? 255 : open;
    gfx_darken(VS, VX(0), VY(0), S()->w, S()->h, (uint32_t)(open * A_QS_DIM / 255));
    if (settled)
        qs_pic_ok = 0;                  /* open and still: tiles change, so it's drawn as it is */
    else if (!qs_pic_ok)
        qs_pic_take();
    draw_qs_words();
    if (settled || !qs_pic_ok) {
        draw_qs_modules();
        return;
    }
    struct gfx_surface pic = qs_pic();
    gfx_blit_key(VS, VX(0), VY((qs.x - FX(qs_h())) / 256), &pic, QS_KEY);
}

/* The status bar's share: up to 4 notifying apps' icons after the time, a
 * dot for more; Do not disturb shows its own sign instead. */
static void draw_note_icons(int x, int cy, uint32_t bg)
{
    if (dnd) {
        gfx_fill_circle(VS, VX(x + 14), VY(cy), 13, bar_fg);
        gfx_fill_rect(VS, VX(x + 7), VY(cy - 2), 14, 4, bg);
        return;
    }
    const struct app *shown[4];
    int n = 0, more = 0;
    for (int k = 0; k < nnotes; k++) {
        int seen = 0;
        for (int j = 0; j < n; j++)
            seen |= shown[j] == notes[k].app;
        if (seen || !notes[k].app)
            continue;
        if (n == 4) {
            more = 1;
            break;
        }
        shown[n++] = notes[k].app;
    }
    for (int j = 0; j < n; j++)
        draw_icon(shown[j], (struct gfx_rect){ x + j * 40, cy - 16, 32, 32 }, 255);
    if (more)
        gfx_fill_circle(VS, VX(x + n * 40 + 8), VY(cy), 5, bar_fg);
}

/* The finger, while the panel is out (or being pulled). 1 if it took it. */
static int qs_pointer(int x, int y, uint8_t buttons, int down)
{
    int h = qs_h();
    if (down && !qs_shown() && y < T()->topbar_h && !menu_open) {
        qs_finger = QF_PULL;                /* from the status bar */
        qs_x0 = x;
        qs_y0 = y;
        return 1;
    }
    if (!qs_shown() && qs_finger != QF_PULL)
        return 0;
    if (down) {
        qs_x0 = x;
        qs_y0 = y;
        qs_from = qs.x;
        if (!inside(qs_rect(), x, y)) {
            qs_finger = QF_DIM;
        } else if (machine_shown.brightness >= 0 && inside(qs_slider_rect(0), x, y)) {
            qs_finger = QF_SLIDER;
            qs_sl = 0;
            machine_status(&machine_shown);     /* what the screen really is, not the last look */
            qs_sl_from = machine_shown.brightness;
            qs_bright_sent = -1;
            qs_bright_want = machine_shown.brightness;
        } else if (inside(qs_slider_rect(1), x, y)) {
            qs_finger = QF_SLIDER;
            qs_sl = 1;
            qs_sl_from = volume;
        } else {
            qs_finger = QF_PANEL;
            qs_note = -1;
            qs_held = -1;
            for (int i = 0; i < QS_NTILES; i++)
                if (inside(qs_tile_rect(i), x, y))
                    qs_held = i;
            damage_all();
            for (int k = 0; k < notes_shown(); k++)
                if (inside(note_rect(k), x, y)) {
                    qs_finger = QF_NOTE;
                    qs_note = k;
                    qs_note_dx = 0;
                }
        }
        return 1;
    }
    int dx = x - qs_x0, dy = y - qs_y0, moved = dx * dx + dy * dy > SWIPE_SLOP * SWIPE_SLOP;
    int held = buttons & MOUSE_LEFT;
    switch (qs_finger) {
    case QF_PULL:                           /* the panel's bottom follows the finger */
        if (held) {
            int to = y - T()->topbar_h;
            qs.x = qs.t = FX(to < 0 ? 0 : to > h ? h : to);
            qs.v = 0;
            damage_all();
        } else {
            qs_finger = QF_NONE;
            qs_set(!moved || qs.x > FX(h) / 3, 0);  /* a tap opens it too */
        }
        return 1;
    case QF_SLIDER:
        if (held) {
            slider_drag(y);
        } else {
            qs_finger = QF_NONE;
            if (qs_sl == 0) {
                bright_flush();
                damage_all();
            }
            if (qs_sl == 1) {
                kprintf("qs: volume %d%%\n", volume);
                machine_volume_commit();
            }
        }
        return 1;
    case QF_NOTE:
        if (held) {
            if (dx > SWIPE_SLOP || dx < -SWIPE_SLOP)
                qs_note_dx = dx;
            damage_all();
            return 1;
        }
        qs_finger = QF_NONE;
        if (qs_note_dx > S()->w / 3 || qs_note_dx < -S()->w / 3) {
            note_remove(qs_note);
        } else if (!moved) {                /* a tap: its app */
            const struct app *a = notes[qs_note].app;
            note_remove(qs_note);
            qs_set(0, 1);
            if (a && app_index(a) >= 0)
                launch(app_index(a), (struct gfx_rect){ S()->w / 2 - 28, T()->topbar_h, 56, 56 });
        }
        qs_note = -1;
        qs_note_dx = 0;
        damage_all();
        return 1;
    case QF_PANEL:
        if (held) {
            if (dy < -SWIPE_SLOP) {         /* swiped up: it follows */
                int to = qs_from + FX(dy);
                qs.x = qs.t = to < 0 ? 0 : to;
                qs.v = 0;
                damage_all();
            }
            return 1;
        }
        qs_finger = QF_NONE;
        qs_held = -1;
        damage_all();
        if (dy < -SWIPE_SLOP) {
            qs_set(qs.x > FX(h) * 2 / 3, 0);
            return 1;
        }
        if (moved)
            return 1;
        if (inside(qs_gear_rect(), x, y)) {
            qs_set(0, 1);
            for (int i = 0; i < builtin_app_count; i++)
                if (!strcmp(builtin_apps[i].name, "Settings"))
                    launch(i, qs_gear_rect());
            return 1;
        }
        for (int i = 0; i < QS_NTILES; i++)
            if (inside(qs_tile_rect(i), x, y))
                tile_tap(i);
        if (nnotes && inside(qs_clear_rect(), x, y)) {
            kprintf("notify: cleared %d\n", nnotes);
            nnotes = 0;
            qs.t = FX(qs_h());
            spring_snap(&qs);
            damage_all();
        }
        return 1;
    case QF_DIM:
        if (!held) {
            qs_finger = QF_NONE;
            if (!moved)
                qs_set(0, 0);
        }
        return 1;
    }
    return 1;
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
/* Up on home brings the drawer with the finger; down on the drawer sends it
 * back. Let go past a fifth of the way and it finishes, else it springs back. */
static int drawer_swipe(int y, uint8_t buttons, int down, int up)
{
    if (down && home_shown() && !menu_open) {
        drw_y0 = y;
        drw_from = drw.x;
        drw_live = 0;
        return 0;
    }
    if (drw_y0 < 0)
        return 0;
    int dy = y - drw_y0, h = S()->h, open = drw_from > 0;
    if (up || !(buttons & MOUSE_LEFT)) {
        int was = drw_live;
        drw_y0 = -1;
        drw_live = 0;
        if (!was)
            return 0;
        drw_set(open ? dy < h / 5 : -dy > h / 5);
        return 1;
    }
    if (!drw_live && (open ? dy > SWIPE_SLOP : -dy > SWIPE_SLOP) && !swipe_on) {
        drw_live = 1;
        if (!open)
            backdrop_take(0);
        damage_hit(pressed);
        pressed = (struct hit){ H_NONE, 0 };
        damage_hit(hover);
        hover = (struct hit){ H_NONE, 0 };
    }
    if (drw_live) {
        int to = drw_from - FX(dy);
        drw.x = drw.t = to < 0 ? 0 : to > FX(h) ? FX(h) : to;
        drw.v = 0;
        drw_moving = 0;
        damage_all();
    }
    return drw_live;
}

static int home_swipe(int x, uint8_t buttons, int down, int up)
{
    if (down && home_shown() && !menu_open && !drw_shown()) {
        swipe_x0 = x;
        swipe_from = pager.x;
        swipe_on = 0;
        return 0;
    }
    if (swipe_x0 < 0 || drw_live)
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
        int max = FX((home_pages() - 1) * S()->w), to = swipe_from - FX(dx);
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
    if (!wm_grabbed() && osk_pointer(x, y, buttons))
        return;                 /* the on-screen keyboard's */
    if (!wm_grabbed() && qs_pointer(x, y, buttons, down))
        return;                 /* quick settings', or the status bar pulled */
    if (sw_open) {
        sw_pointer(x, y, buttons, down);
        return;
    }
    if (edit_pointer(x, y, buttons, down, up))
        return;
    /* A finger held still on the home screen starts edit mode (edit_tick). */
    if (down && home_shown() && !menu_open && !drw_shown() && y >= T()->topbar_h) {
        hold_since = uptime_ms() ? uptime_ms() : 1;
        hold_x = x;
        hold_y = y;
    } else if (hold_since && (up || !(buttons & MOUSE_LEFT) || (x - hold_x) * (x - hold_x) + (y - hold_y) * (y - hold_y) >
                                                                SWIPE_SLOP * SWIPE_SLOP)) {
        hold_since = 0;
    }
    if (drawer_swipe(y, buttons, down, up))
        return;
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
 * the desktop's own: Home shows the desktop, Recent apps opens the app
 * switcher, Power locks (held: the power
 * menu), volume is only logged (no sound yet). */
#define POWER_HOLD_MS 500
static uint32_t power_down_ms;          /* Power is held since (0: it isn't) */
static int power_held;                  /* ... and the power menu opened for it */

/* Home: a press goes home (or closes quick settings); held, quick settings. */
static uint32_t home_down_ms;
static int home_held;

static void home_key(void)
{
    kprintf("shell: Home\n");
    if (edit_on) {
        edit_exit();
        return;
    }
    menu_set(0);
    set_grid(0);
    if (qs_shown()) {
        qs_set(0, 0);
        return;
    }
    sw_set(0);
    if (drw_shown())
        drw_set(0);
    show_desktop();
    turn_to(0, 0);                      /* on the home screen already: back to its first page */
}

/* --- volume, screenshots, toasts (UI-PLAN.md) ------------------------------------------
 *
 * The volume keys show a slim slider on the left edge for 2 s after the last
 * press; held, they keep changing it. The level is kept, logged, and told to
 * apps (sdk/audio.h) through machine_set_volume. Power + Volume Down saves a
 * screenshot to Pictures/Screenshots; a toast says so. */

#define VOL_STEP 10
#define VOL_SHOW_MS 2000
#define VOL_REPEAT_MS 150
#define TOAST_MS 2000

static int volume = 60;                 /* 0..100 */
static uint32_t vol_until;              /* the pop-up shows until then (0: hidden) */
static uint16_t vol_key;                /* the volume key held, or 0 */
static uint32_t vol_repeat_at;
static int vol_before;                  /* the level before Volume Down: a screenshot puts it back */
static const char *toast;
static uint32_t toast_until;

static struct gfx_rect vol_rect(void)
{
    return (struct gfx_rect){ 24, T()->topbar_h + 160, 64, 440 };
}

static struct gfx_rect toast_rect(void)
{
    int w = toast ? text_width(toast, TEXT_BODY) + 64 : 0;
    return (struct gfx_rect){ (S()->w - w) / 2, S()->h - 180, w, 72 };
}

static void vol_hide(void)
{
    if (!vol_until)
        return;
    vol_until = 0;
    machine_volume_commit();            /* the changing has stopped: keep it */
    fx_start(&volv, 0);                 /* it slides back out */
    shell_damage(vol_rect());
}

static void vol_change(int up)
{
    int v = volume + (up ? VOL_STEP : -VOL_STEP);
    volume = v < 0 ? 0 : v > 100 ? 100 : v;
    machine_set_volume(volume);
    kprintf("shell: volume %s, %d%%\n", up ? "up" : "down", volume);
    vol_until = uptime_ms() + VOL_SHOW_MS;
    fx_start(&volv, 1);                 /* in from the left edge */
    shell_damage(vol_rect());
}

static void draw_volume(void)
{
    struct gfx_rect r = vol_rect();
    r.x -= (255 - vol_alpha) * (r.w + 40) / 255;        /* sliding in from the left edge */
    if (vol_alpha <= 0 || !visible(r))
        return;
    const struct theme *t = T();
    gfx_fill_round_rect(VS, VX(r.x), VY(r.y), r.w, r.h, r.w / 2, SHELL_POPOVER);
    int tx = r.x + r.w / 2 - 6, ty = r.y + 28, th = r.h - 120;  /* the track */
    gfx_fill_round_rect(VS, VX(tx), VY(ty), 12, th, 6, gfx_mix(SHELL_FG, SHELL_POPOVER, 50));
    int fill = th * volume / 100;
    if (fill)
        gfx_fill_round_rect(VS, VX(tx), VY(ty + th - fill), 12, fill, 6, t->accent_bg);
    char n[4];
    int len = 0;
    if (volume >= 100)
        n[len++] = '1';
    if (volume >= 10)
        n[len++] = (char)('0' + volume / 10 % 10);
    n[len++] = (char)('0' + volume % 10);
    n[len] = '\0';
    text_draw(VS, VX(r.x + (r.w - text_width(n, TEXT_SMALL)) / 2), VY(r.y + r.h - 72), n, TEXT_SMALL, SHELL_FG);
}

static void show_toast(const char *text)
{
    shell_damage(toast_rect());
    toast = text;
    toast_until = uptime_ms() + TOAST_MS;
    fx_start(&toastv, 1);
    shell_damage(toast_rect());
    kprintf("shell: toast \"%s\"\n", text);
}

static void draw_toast(void)
{
    struct gfx_rect r = toast_rect();
    r.y += (255 - toast_alpha) * 40 / 255;              /* rising as it fades in */
    if (!toast || toast_alpha <= 0 || !visible(r))
        return;
    gfx_blend_round_rect(VS, VX(r.x), VY(r.y), r.w, r.h, r.h / 2, SHELL_POPOVER, (uint32_t)toast_alpha);
    if (toast_alpha > 160)
        text_draw(VS, VX(r.x + 32), VY(r.y + (r.h - text_height(TEXT_BODY)) / 2), toast, TEXT_BODY, SHELL_FG);
}

static char *put_num(char *p, int v, int digits)
{
    for (int i = digits - 1; i >= 0; i--, v /= 10)
        p[i] = (char)('0' + v % 10);
    return p + digits;
}

/* The screen as it is now (without the volume pop-up), as a PNG. */
static void screenshot(void)
{
    vol_hide();
    spring_snap(&volv);                 /* gone at once: not in the picture */
    vol_alpha = 0;
    shell_damage(vol_rect());
    shell_flush();
    struct rtc_time c = { 0 };
    if (rtc)
        dev_read(rtc, &c, sizeof(c));
    char name[40] = "Screenshot_", *p = name + 11;     /* Screenshot_20261103_200512.png */
    p = put_num(p, c.year, 4);
    p = put_num(p, c.month, 2);
    p = put_num(p, c.day, 2);
    *p++ = '_';
    p = put_num(p, c.hour, 2);
    p = put_num(p, c.minute, 2);
    p = put_num(p, c.second, 2);
    memcpy(p, ".png", 5);
    uintptr_t phys;
    uint32_t bytes, len;
    int err = wallpaper_png_surface(S(), &phys, &bytes, &len);
    if (!err) {
        err = settings_save_screenshot(name, (const void *)phys, len);
        wallpaper_pack_free(phys, bytes);
    }
    if (err) {
        kprintf("shell: screenshot not saved (%d)\n", err);
        show_toast("Couldn't save the screenshot");
    } else {
        kprintf("shell: screenshot %s, %u KB\n", name, len / 1024);
        show_toast("Screenshot saved");
    }
}

static void volume_tick(void)
{
    uint32_t now = uptime_ms();
    if (vol_key && (int32_t)(now - vol_repeat_at) >= 0) {
        vol_change(vol_key == KEY_VOLUME_UP);
        vol_repeat_at = now + VOL_REPEAT_MS;
        shell_flush();
    }
    if (vol_until && (int32_t)(now - vol_until) >= 0) {
        vol_hide();
        shell_flush();
    }
    if (toast && toastv.t && (int32_t)(now - toast_until) >= 0)
        fx_start(&toastv, 0);           /* it fades out: fx_tick lets go of it then */
}

/* The overlays' springs, a frame at a time (see fx_start). */
static void fx_tick(void)
{
    if (!fx_moving)
        return;
    uint32_t now = uptime_ms(), steps = (now - last_fx_tick) / TICK_MS;
    if (!steps)
        return;
    last_fx_tick = steps > 4 ? now : last_fx_tick + steps * TICK_MS;  /* leftover ms count */
    int moving = 0, was_sw = sw_alpha, was_menu = menu_alpha, was_gliding = sw_gliding;
    for (uint32_t s = 0; s < (steps > 4 ? 4 : steps); s++) {
        moving = 0;
        moving |= spring_step(&swv, 16, 128);       /* about a quarter of a second, no overshoot */
        moving |= spring_step(&menuv, 16, 128);
        moving |= spring_step(&volv, 16, 128);
        moving |= spring_step(&toastv, 16, 128);
        if (sw_gliding)                 /* a touch springier: it settles with a little give */
            moving |= sw_gliding = spring_step(&sw_glide, 20, 104);
    }
    if (was_gliding && sw_open && sw_mode == SWM_NONE) {
        sw_scroll = sw_glide.x / 256;
        sw_damage_cards();
    }
    sw_alpha = fx_alpha(&swv);
    menu_alpha = fx_alpha(&menuv);
    vol_alpha = fx_alpha(&volv);
    toast_alpha = fx_alpha(&toastv);
    if (sw_alpha != was_sw || menu_alpha != was_menu)
        damage_all();
    struct gfx_rect v = vol_rect(), tr = toast_rect();
    shell_damage((struct gfx_rect){ v.x, v.y, S()->w - v.x, v.h });
    shell_damage((struct gfx_rect){ tr.x, tr.y, tr.w, tr.h + 40 });
    if (toast && !toastv.t && !toast_alpha)
        toast = 0;                      /* faded out */
    fx_moving = moving;
    shell_flush();
    uint32_t took = uptime_ms() - now;
    fx_frames++;
    if (took > fx_slowest)
        fx_slowest = took;
    if (!moving) {
        uint32_t ms = uptime_ms() - fx_began;
        kprintf("fx: %u frames in %u ms (%u fps), slowest frame %u ms\n", (unsigned)fx_frames, (unsigned)ms,
                (unsigned)(ms ? fx_frames * 1000 / ms : 0), (unsigned)fx_slowest);
    }
}

static int phone_key(const struct key_event *k)
{
    if (k->key == KEY_PHONE_HOME) {
        if (k->pressed && !home_down_ms) {
            home_down_ms = uptime_ms() | 1;
        } else if (!k->pressed) {
            if (home_down_ms && !home_held)
                home_key();
            home_down_ms = 0;
            home_held = 0;
            shell_flush();
        }
        return 1;
    }
    if (k->key == KEY_PHONE_POWER) {
        if (k->pressed && !power_down_ms) {
            power_down_ms = uptime_ms() | 1;
            if (vol_key == KEY_VOLUME_DOWN) {   /* Volume Down, then Power: a screenshot */
                vol_key = 0;
                volume = vol_before;            /* not a volume change after all */
                power_held = 1;                 /* nor a lock, nor the menu */
                screenshot();
            }
        } else if (!k->pressed) {
            if (power_down_ms && !power_held) {
                menu_set(0);
                shell_flush();
                kprintf("shell: lock (Power)\n");
                lockscreen_lock();
                machine_screen(0);              /* the screen off; the next Power wakes it, still locked */
            }
            power_down_ms = 0;
            power_held = 0;
        }
        return 1;
    }
    if (k->key == KEY_VOLUME_UP || k->key == KEY_VOLUME_DOWN) {
        if (!k->pressed) {
            if (vol_key == k->key)
                vol_key = 0;
        } else if (k->key == KEY_VOLUME_DOWN && power_down_ms && !power_held) {
            power_held = 1;                     /* Power, then Volume Down: a screenshot */
            screenshot();
        } else if (vol_key != k->key) {
            vol_before = volume;
            vol_key = (uint16_t)k->key;
            vol_repeat_at = uptime_ms() + POWER_HOLD_MS;
            vol_change(k->key == KEY_VOLUME_UP);
        }
        shell_flush();
        return 1;
    }
    if (!k->pressed)
        return 1;
    switch (k->key) {
    case KEY_PHONE_BACK: {
        /* The power menu first; on the home screen, back to its first page;
         * in an app, to the app if it has something to close, else home,
         * the app kept open behind (as on Android 12 and later: Recent
         * apps or the Task Manager close it; UI-PLAN.md). */
        struct wm_window *w = wm_top_visible();
        if (menu_open) {
            menu_set(0);
        } else if (sw_open) {
            sw_set(0);
        } else if (qs_shown()) {
            qs_set(0, 0);
        } else if (!w) {
            if (drw_shown())
                drw_set(0);
            turn_to(0, 0);
        } else if (wm_back_wanted(w)) {
            kprintf("shell: Back (to %s)\n", name_of(w));
            struct key_event esc = keymap_event(KEY_ESC, 1);
            wm_input_key(&esc);
            esc = keymap_event(KEY_ESC, 0);
            wm_input_key(&esc);
        } else {
            kprintf("shell: Back leaves %s open, goes home\n", name_of(w));
            minimise_windows(&w, 1);
            turn_to(0, 0);
        }
        break;
    }
    case KEY_PHONE_RECENTS:
        kprintf("shell: Recent apps\n");
        menu_set(0);
        set_grid(0);
        if (qs_shown())
            qs_set(0, 1);
        sw_set(!sw_open);
        break;
    }
    shell_flush();
    return 1;
}

/* The on-screen keyboard: up while the app in front has a text field open,
 * its window shrunk above it. */
void shell_osk_key(const struct key_event *k)
{
    if (wm_focused())
        wm_input_key(k);
}

static void keyboard_follow(void)
{
    struct wm_window *w = wm_top_visible();
    int want = w && !covered && !sw_open ? wm_keyboard_wanted(w) : 0;
    if (want == osk_mode())
        return;
    osk_show(want);
    wm_set_area((struct gfx_rect){ 0, T()->topbar_h, S()->w, S()->h - T()->topbar_h - osk_height() });
    damage_all();
    shell_flush();
}

static void power_tick(void)
{
    if (home_down_ms && !home_held && (int32_t)(uptime_ms() - home_down_ms) >= POWER_HOLD_MS) {
        home_held = 1;
        kprintf("shell: quick settings (Home held)\n");
        qs_set(1, 0);
        shell_flush();
    }
    if (power_down_ms && !power_held && (int32_t)(uptime_ms() - power_down_ms) >= POWER_HOLD_MS) {
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

static int open_later = -1;

int desktop_open_later(const char *name)
{
    for (int i = 0; i < builtin_app_count; i++)
        if (!strcmp(builtin_apps[i].name, name)) {
            open_later = i;
            return 0;
        }
    return -1;
}

void desktop_tick(void)
{
    if (!started)
        return;
    if (open_later >= 0) {
        int i = open_later;
        open_later = -1;
        launch(i, (struct gfx_rect){ S()->w / 2 - 56, S()->h / 2 - 56, 112, 112 });
    }
    dock_tick();
    pager_tick();
    edit_tick();
    qs_tick();
    power_tick();
    volume_tick();
    fx_tick();
    drw_tick();
    osk_tick();
    keyboard_follow();
    uint32_t ticks;
    if (!rtc || dev_ioctl(rtc, RTC_GET_TICKS, &ticks) < 0 || ticks == last_tick)
        return;
    last_tick = ticks;
    struct k86_net n;                       /* the network and battery icons: redrawn when they change */
    net_status(&n);
    /* The icon shows only the battery: the other fields are zeroed in both copies so that the
     * comparison ignores them, and the brightness (which the control centre's slider reads) is
     * kept apart and put back. */
    struct k86_machine m = machine_shown, was = machine_shown;
    int brightness = machine_shown.brightness;
    m.minutes = m.brightness = m.timezone = 0;
    was.minutes = was.brightness = was.timezone = 0;
    if (ticks % 5 == 0 || !last_machine_tick) {
        machine_status(&m);
        brightness = m.brightness;
        m.minutes = m.brightness = m.timezone = 0;
        last_machine_tick = 1;
    }
    machine_shown.brightness = brightness;
    if (memcmp(&n, &net_shown, sizeof(n)) || memcmp(&m, &was, sizeof(m))) {
        int from = status_left();
        net_shown = n;
        machine_shown = m;
        machine_shown.brightness = brightness;
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
    if (home_shown() && lockscreen_home_clock()->show) {
        struct gfx_rect box;
        lockscreen_home_clock_draw(0, 0, 0, &clock_shown, 0, &box);
        shell_damage((struct gfx_rect){ 0, box.y - 20, S()->w, box.h + 40 });
        pg_ok = 0;
    }
    shell_flush();
}

int desktop_busy(void)
{
    /* A key held: its long press (or repeat) is timed, so no sleeping. */
    return hold_since || edit_on || edit_moving || dock_moving || hide_at || pager_moving || qs_moving || vol_key || power_down_ms || home_down_ms || drw_moving ||
           fx_moving || osk_moving();
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
