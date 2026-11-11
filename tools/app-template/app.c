/* @TITLE@: a starting point for a LiteKern Go app. It opens a window, draws a heading, a counter and a
 * button, counts taps, follows the theme and the window's size, and keeps the count in a file in the
 * user's home folder, so it's still there next time.
 *
 * Read docs/NEW-APP.md first, then docs/APPS.md; kernel/kern86_abi.h lists every call the system gives
 * an app. Keep to the limits there: no malloc (static arrays), whole numbers, and go back to
 * k86_wait_event() quickly, because apps take turns. Log lines (k86_log) show up in the Log app. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"
#include "sdk/kern86.h"

#define SAVE_FILE "@NAME@.dat"

static struct k86_window win;
static struct gfx_surface canvas;
static struct wg_button button;
static char home[K86_PATH_MAX];         /* the user's home folder, and the volume it's on */
static int vol = -1;
static int count;

static void load(void)
{
    int32_t saved = 0;
    vol = k86_home(home);
    if (vol >= 0 && k86_read(vol, home, SAVE_FILE, &saved, sizeof(saved)) == (int)sizeof(saved))
        count = saved;
}

static void save(void)
{
    int32_t v = count;
    if (vol >= 0)
        k86_write(vol, home, SAVE_FILE, &v, sizeof(v));
}

static void layout(void)
{
    const struct theme *t = theme_get();
    int w = canvas.w - 48;
    button.r = (struct gfx_rect){ 24, canvas.h / 2 + 40, w, t->button_size * 2 };
    button.label = "Tap me";
    button.style = WG_BUTTON_SUGGESTED;
}

static void draw(void)
{
    const struct theme *t = theme_get();
    char text[32];
    gfx_fill_rect(&canvas, 0, 0, canvas.w, canvas.h, t->window_bg);      /* colours come from the theme: never fixed */
    wg_label_centred(&canvas, canvas.w / 2, 120, "@TITLE@", WG_TEXT_HEADING);
    snprintf(text, sizeof(text), "%d", count);
    wg_label_centred(&canvas, canvas.w / 2, canvas.h / 2 - 60, text, WG_TEXT_HEADING);
    wg_button_draw(&canvas, &button);
    k86_present(0, 0, canvas.w, canvas.h);                               /* show what changed (here: all) */
}

int main(void)
{
    if (k86_window_open(&win))
        return 1;
    canvas = (struct gfx_surface){ win.canvas, win.w, win.h, win.stride };
    load();
    layout();
    draw();
    k86_log("@NAME@: open");
    for (;;) {
        struct k86_event ev;
        if (k86_wait_event(&ev))                                         /* sleeps until something happens */
            return 1;
        switch (ev.type) {
        case K86_EVENT_POINTER: {                                        /* a touch: x, y in the canvas */
            struct wg_pointer p = wg_pointer_make(ev.x, ev.y, ev.buttons, ev.changed, ev.time_ms);
            if (wg_button_pointer(&button, &p)) {                        /* pressed and let go on the button */
                count++;
                save();
                k86_logf("@NAME@: count %d", count);
            }
            draw();
            break;
        }
        case K86_EVENT_THEME:                                            /* the style or accent changed */
            draw();
            break;
        case K86_EVENT_RESIZE:                                           /* the window's size changed */
            canvas.w = ev.x;
            canvas.h = ev.y;
            layout();
            draw();
            break;
        case K86_EVENT_CLOSE:
            return 0;
        default:
            break;
        }
    }
}
