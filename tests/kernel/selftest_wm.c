/* LiteKern Go — window system self-test, for the phone's window manager
 * (kernel/wm.c: every window full-screen under the status bar, its header
 * bar always there, no window controls). Test builds only
 * (-DLKX_SELFTEST_WM); tests/linux/test-linux.sh runs it.
 *
 * Checks a window's place and header bar pixel by pixel, that input becomes
 * the right events (header buttons by id, content clicks in content
 * coordinates, pointer moves merged, keys to the focused window), then two
 * windows: the one in front covers the other, minimising hands the focus
 * back, and when the area shrinks (the on-screen keyboard) both are refitted
 * and told (RESIZE). Leaves a Files-style mock-up open for the screenshot. */
#include "kernel/desktop.h"
#include "kernel/errno.h"
#include "kernel/font.h"
#include "kernel/printk.h"
#include "kernel/screen.h"
#include "kernel/theme.h"
#include "kernel/wm.h"

static int passed, failed;

static void check(int ok, const char *what)
{
    kprintf("selftest: %s %s\n", ok ? "ok  " : "FAIL", what);
    if (ok)
        passed++;
    else
        failed++;
}

static uint32_t px(int x, int y)
{
    struct gfx_surface *s = screen_surface();
    return s->px[y * s->stride + x];
}

/* The next event other than pointer updates (checked on their own below). */
static int next(struct wm_window *w, struct wm_event *ev)
{
    while (wm_poll_event(w, ev))
        if (ev->type != WM_EVENT_POINTER)
            return 1;
    return 0;
}

static void drain(struct wm_window *w)
{
    struct wm_event ev;
    while (wm_poll_event(w, &ev))
        ;
}

static void mouse(int x, int y, uint8_t b)
{
    wm_input_mouse(x, y, b);
    wm_present();
}

/* A Files-style list, drawn the Adwaita way: a rounded "boxed list" card
 * with separators, one selected row, a folder glyph per row. */
static void mockup(struct wm_window *w)
{
    const struct theme *t = theme_get();
    struct gfx_surface *c = wm_content(w);
    static const char *const names[] = { "Documents", "Music", "Pictures", "notes.txt",
                                         "todo.txt", "kernel.bin" };
    static const char *const sizes[] = { "3 items", "12 items", "48 items", "2 KB", "512 bytes",
                                         "28 KB" };
    int x = 32, y = 24, wd = c->w - 64, n = 6;

    gfx_fill_rect(c, 0, 0, c->w, c->h, t->window_bg);
    gfx_text(c, x, y, "Home", t->fg_dim, GFX_TRANSPARENT);
    y += FONT_H + t->spacing;
    gfx_fill_round_rect(c, x, y, wd, n * t->row_h, 12, t->view_bg);
    for (int i = 0; i < n; i++) {
        int ry = y + i * t->row_h;
        if (i == 1)
            gfx_fill_rect(c, x, ry, wd, t->row_h, t->row_selected);
        if (i)
            gfx_fill_rect(c, x + 12, ry, wd - 24, 1, t->border);
        uint32_t icon = i < 3 ? t->accent : t->fg_dim;
        gfx_fill_round_rect(c, x + 14, ry + (t->row_h - 28) / 2, 36, 28, 5, icon);
        gfx_text(c, x + 64, ry + (t->row_h - FONT_H) / 2, names[i], t->fg, GFX_TRANSPARENT);
        gfx_text(c, x + wd - 14 - gfx_text_width(sizes[i]), ry + (t->row_h - FONT_H) / 2,
                 sizes[i], t->fg_dim, GFX_TRANSPARENT);
    }
    wm_damage(w, 0, 0, c->w, c->h);
    wm_present();
}

void selftest_wm_run(void)
{
    const struct theme *t = theme_get();
    struct wm_event ev;
    int hh = t->headerbar_h;

    struct wm_window *w = wm_create("Files", 760, 480, 0, 0);
    wm_present();
    check(w && wm_focused() == w, "a window opens, focused");
    struct gfx_rect r = wm_rect(w), a = wm_area();
    check(r.x == a.x && r.y == a.y && r.w == a.w && r.h == a.h && wm_is_maximised(w),
          "it opens full-screen under the status bar, whatever size it asked for");
    struct gfx_surface *c = wm_content(w);
    check(c->w == r.w && c->h == r.h - hh, "content: the whole width, under the header bar");
    int ct = r.y + hh;                                  /* first content row on screen */
    check(px(r.x + 2, r.y + 2) == t->headerbar_bg, "the header bar uses the theme");     /* beside its glass */
    check(px(r.x + 20, ct + 20) == t->window_bg, "content starts as the window background");
    int bright = 0;
    for (int x = r.x + r.w / 2 - 60; x < r.x + r.w / 2 + 60; x++)
        for (int y = r.y + 10; y < r.y + hh - 10; y++)
            bright += px(x, y) == t->fg;
    check(bright > 20, "the title is drawn in the middle of the header bar");
    int cy = r.y + hh / 2, marks = 0;
    for (int x = r.x + r.w - 110; x < r.x + r.w - 30; x++)     /* the capsule's plain glass */
        marks += px(x, cy) == t->fg;
    check(!marks, "no minimise, maximise or close: the phone's keys do that");
    wm_set_maximised(w, 0);
    wm_present();
    check(wm_is_maximised(w) && wm_rect(w).w == a.w && wm_rect(w).h == a.h, "it can't be made smaller");

    check(wm_add_button(w, WM_LEFT, WM_ICON_BACK, 0, 1) == 0 && wm_add_button(w, WM_RIGHT, WM_ICON_ADD, 0, 3) == 0 &&
              wm_add_button(w, WM_RIGHT, WM_ICON_NONE, "Rename", 4) == 0,
          "header buttons: icons and text, on the left and right");
    wm_present();
    int bx = r.x + t->spacing + t->button_size / 2;    /* the back button's centre */
    drain(w);
    mouse(bx, cy, MOUSE_LEFT);
    mouse(bx, cy, 0);
    check(next(w, &ev) && ev.type == WM_EVENT_HEADER && ev.id == 1, "a header button reports its id");
    mouse(bx, cy, MOUSE_LEFT);
    mouse(bx + 200, cy + 300, MOUSE_LEFT);
    mouse(bx + 200, cy + 300, 0);
    check(!next(w, &ev), "a press dragged off its button does nothing");

    drain(w);
    mouse(r.x + 100, ct + 50, MOUSE_LEFT);
    mouse(r.x + 100, ct + 50, 0);
    check(next(w, &ev) && ev.type == WM_EVENT_CLICK && ev.x == 100 && ev.y == 50,
          "a content tap arrives in content coordinates");

    drain(w);
    mouse(r.x + 300, ct + 100, 0);
    mouse(r.x + 310, ct + 110, 0);
    mouse(r.x + 320, ct + 120, 0);
    check(wm_poll_event(w, &ev) && ev.type == WM_EVENT_POINTER && ev.x == 320 && ev.y == 120 &&
              !ev.changed && !wm_poll_event(w, &ev),
          "pointer moves reach the app, merged into the latest position");
    mouse(r.x + 320, ct + 120, MOUSE_LEFT);
    mouse(r.x + 330, ct + 120, MOUSE_LEFT);
    mouse(r.x + 330, ct + 120, 0);
    int ok_down = wm_poll_event(w, &ev) && ev.type == WM_EVENT_POINTER && ev.changed == MOUSE_LEFT &&
                  ev.buttons == MOUSE_LEFT;
    while (ok_down && wm_poll_event(w, &ev) && ev.type != WM_EVENT_POINTER)
        ;                       /* the CLICK convenience event sits in between */
    int ok_drag = ev.type == WM_EVENT_POINTER && ev.x == 330 && !ev.changed;
    int ok_up = wm_poll_event(w, &ev) && ev.type == WM_EVENT_POINTER && ev.changed == MOUSE_LEFT &&
                !ev.buttons;
    check(ok_down && ok_drag && ok_up, "press, drag and release arrive in order");

    struct key_event k = { .key = 0x1e, .pressed = 1, .ascii = 'a' };
    wm_input_key(&k);
    k.pressed = 0;
    wm_input_key(&k);
    check(next(w, &ev) && ev.type == WM_EVENT_KEY && ev.key.ascii == 'a' && !next(w, &ev),
          "keys go to the focused window (presses only)");

    int ok = 1;
    for (int i = 3; i < WM_MAX_BUTTONS; i++)
        ok &= wm_add_button(w, WM_RIGHT, WM_ICON_NONE, "x", 10 + i) == 0;
    check(ok && wm_add_button(w, WM_RIGHT, WM_ICON_NONE, "x", 99) == -ENOMEM, "button limit -> -ENOMEM");
    wm_clear_buttons(w);

    gfx_fill_rect(c, -10, -10, 50, 50, 0xff00ff);
    wm_damage(w, 0, 0, 50, 50);
    wm_present();
    check(px(r.x + 6, ct - 2) == t->headerbar_bg && px(r.x + 6, ct + 5) == 0xff00ff,
          "drawing into the content can't reach the header bar");

    wm_request_close(w);
    check(next(w, &ev) && ev.type == WM_EVENT_CLOSE && !next(w, &ev), "Back's close reaches the app as CLOSE");

    /* A second window: full-screen too, in front, and it takes the keys. */
    struct wm_window *w2 = wm_create("Second", 400, 300, 0, 0);
    wm_present();
    struct gfx_rect r2 = wm_rect(w2);
    check(w2 && r2.x == a.x && r2.y == a.y && r2.w == a.w && wm_focused() == w2 && wm_at(1) == w2,
          "a second window opens full-screen, in front and focused");
    check(px(r.x + 6, ct + 5) == t->window_bg, "it covers the first");
    k.pressed = 1;
    wm_input_key(&k);
    check(!next(w, &ev) && next(w2, &ev) && ev.type == WM_EVENT_KEY, "keys follow the focus");

    wm_set_minimised(w2, 1);
    wm_present();
    check(wm_is_minimised(w2) && wm_focused() == w && px(r.x + 6, ct + 5) == 0xff00ff,
          "minimised (Home): the one behind shows again and gets the focus");
    wm_set_minimised(w2, 0);
    wm_present();
    check(!wm_is_minimised(w2) && wm_focused() == w2, "brought back (the app switcher), it's in front again");

    /* The on-screen keyboard takes the bottom: the windows are refitted above it. */
    drain(w);
    drain(w2);
    struct gfx_rect small = { a.x, a.y, a.w, a.h - 400 };
    wm_set_area(small);
    wm_present();
    check(wm_rect(w2).h == small.h && next(w2, &ev) && ev.type == WM_EVENT_RESIZE && ev.y == small.h - hh &&
              wm_content(w2)->h == ev.y,
          "the area shrinks (the keyboard): the window fits above it, and hears its new size");
    check(wm_rect(w).h == small.h && next(w, &ev) && ev.type == WM_EVENT_RESIZE,
          "the one behind is refitted too");
    wm_set_area(a);
    wm_present();
    drain(w);
    drain(w2);

    /* Clean up: the second window goes; the first, as Files will look,
     * stays for the screenshot. */
    wm_destroy(w2);
    check(wm_count() == 1 && wm_focused() == w, "closing a window leaves the rest, focused");
    wm_set_title(w, "Files");
    wm_add_button(w, WM_LEFT, WM_ICON_BACK, 0, 1);
    wm_add_button(w, WM_RIGHT, WM_ICON_ADD, 0, 3);
    mockup(w);
    drain(w);

    kprintf("selftest: wm %d/%d passed\n", passed, passed + failed);
    if (failed)
        panic("window system self-test: %d checks failed", failed);
}
