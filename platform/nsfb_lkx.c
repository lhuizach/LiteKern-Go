/* LiteKern X on Linux — libnsfb's "lkx" surface: NetSurf's framebuffer front
 * end drawing into a LiteKern X window (LiteKernX-Roadmap/06, phase L6).
 *
 * linux/package/netsurf-lkx copies this file into libnsfb as
 * src/surface/lkx.c; netsurf-fb is started with "-f lkx" by the Web app's
 * launcher (linux/overlay/usr/lib/litekern/apps/browser). It's a LiteKern X
 * app like the others: KERN86 calls through platform/libk86.c, included here
 * whole, so libnsfb needs nothing else.
 *
 *   pixels   the window's canvas is the surface's memory (0x00RRGGBB, which
 *            is libnsfb's XRGB8888); update() presents the changed box
 *   size     always the window's content size; a resize is NSFB_EVENT_RESIZE
 *   input    SYS_WAIT_EVENT, with SYS_TIMER_SET for NetSurf's timeouts;
 *            keys arrive as the characters LiteKern X's layout (AZERTY)
 *            made, so the front end's own US key table isn't used for them
 *   pointer  the desktop's own: the surface never draws NetSurf's */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libnsfb.h"
#include "libnsfb_plot.h"
#include "libnsfb_event.h"

#include "nsfb.h"
#include "surface.h"
#include "plot.h"

#include "platform/libk86.c"

static struct k86_window win;
static int opened, cur_w, cur_h;

/* Events made from one LiteKern X event (a press is a move, then a button). */
#define QUEUE 32
static nsfb_event_t queue[QUEUE];
static int qhead, qtail;
static uint8_t buttons;

static void push(enum nsfb_event_type_e type, int code)
{
    if ((qhead + 1) % QUEUE == qtail)
        return;
    nsfb_event_t *e = &queue[qhead];
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->value.keycode = code;
    qhead = (qhead + 1) % QUEUE;
}

static void push_move(int x, int y)
{
    if ((qhead + 1) % QUEUE == qtail)
        return;
    nsfb_event_t *e = &queue[qhead];
    memset(e, 0, sizeof(*e));
    e->type = NSFB_EVENT_MOVE_ABSOLUTE;
    e->value.vector.x = x;
    e->value.vector.y = y;
    qhead = (qhead + 1) % QUEUE;
}

static int open_window(void)
{
    if (opened)
        return 0;
    if (k86_call(SYS_WINDOW_OPEN, (uint32_t)&win, 0, 0, 0))
        return -1;
    opened = 1;
    cur_w = win.w;
    cur_h = win.h;
    static const char hello[] = "browser: open";
    k86_call(SYS_DEBUG_WRITE, (uint32_t)hello, sizeof(hello) - 1, 0, 0);
    return 0;
}

static int lkx_defaults(nsfb_t *nsfb)
{
    if (open_window())
        return -1;
    nsfb->width = cur_w;
    nsfb->height = cur_h;
    nsfb->format = NSFB_FMT_XRGB8888;
    select_plotters(nsfb);
    return 0;
}

static int lkx_initialise(nsfb_t *nsfb)
{
    if (open_window())
        return -1;
    nsfb->ptr = (uint8_t *)win.canvas;
    nsfb->linelen = win.stride * 4;
    return 0;
}

/* The size asked for doesn't matter: it's the window's. */
static int lkx_set_geometry(nsfb_t *nsfb, int width, int height, enum nsfb_format_e format)
{
    (void)width, (void)height, (void)format;
    nsfb->width = cur_w;
    nsfb->height = cur_h;
    nsfb->format = NSFB_FMT_XRGB8888;
    nsfb->linelen = win.stride * 4;
    select_plotters(nsfb);
    return 0;
}

static int lkx_finalise(nsfb_t *nsfb)
{
    (void)nsfb;
    return 0;
}

/* A character LiteKern X's layout produced, as the front end's tables want
 * it: the code it reads as that character, and whether Shift must look held
 * (capitals; and the few its unshifted table lacks). */
static void push_char(int c)
{
    int code = c, shift = 0;
    if (c >= 'A' && c <= 'Z')
        code = c + 32, shift = 1;
    else if (c == '%')
        code = '5', shift = 1;
    else if (c == '^')
        code = '6', shift = 1;
    else if (c == '{' || c == '|' || c == '}')
        code = c - 32, shift = 1;
    else if (c == '~')
        code = '^';
    if (shift)
        push(NSFB_EVENT_KEY_DOWN, NSFB_KEY_LSHIFT);
    push(NSFB_EVENT_KEY_DOWN, code);
    push(NSFB_EVENT_KEY_UP, code);
    if (shift)
        push(NSFB_EVENT_KEY_UP, NSFB_KEY_LSHIFT);
}

static void key(const struct key_event *k)
{
    static const struct { uint16_t lkx; int nsfb; } special[] = {
        { KEY_UP, NSFB_KEY_UP }, { KEY_DOWN, NSFB_KEY_DOWN }, { KEY_LEFT, NSFB_KEY_LEFT },
        { KEY_RIGHT, NSFB_KEY_RIGHT }, { KEY_HOME, NSFB_KEY_HOME }, { KEY_END, NSFB_KEY_END },
        { KEY_PAGEUP, NSFB_KEY_PAGEUP }, { KEY_PAGEDOWN, NSFB_KEY_PAGEDOWN },
        { KEY_DELETE, NSFB_KEY_DELETE }, { KEY_INSERT, NSFB_KEY_INSERT },
        { KEY_ENTER, NSFB_KEY_RETURN }, { KEY_EXTENDED | 0x1c, NSFB_KEY_RETURN },
        { KEY_BACKSPACE, NSFB_KEY_BACKSPACE }, { KEY_TAB, NSFB_KEY_TAB }, { KEY_ESC, NSFB_KEY_ESCAPE },
    };
    for (unsigned i = 0; i < sizeof(special) / sizeof(special[0]); i++)
        if (k->key == special[i].lkx) {
            push(k->pressed ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP, special[i].nsfb);
            return;
        }
    if (k->key >= KEY_F1 && k->key < KEY_F1 + 10) {
        push(k->pressed ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP, NSFB_KEY_F1 + (k->key - KEY_F1));
        return;
    }
    if (!k->pressed)
        return;                             /* characters: sent whole on the press */
    if (k->ascii >= 1 && k->ascii <= 26 && (k->mods & MOD_CTRL)) {    /* Ctrl+C, Ctrl+V... */
        push(NSFB_EVENT_KEY_DOWN, NSFB_KEY_LCTRL);
        push(NSFB_EVENT_KEY_DOWN, 'a' + k->ascii - 1);
        push(NSFB_EVENT_KEY_UP, 'a' + k->ascii - 1);
        push(NSFB_EVENT_KEY_UP, NSFB_KEY_LCTRL);
    } else if (k->ascii >= 32 && k->ascii < 127) {
        push_char(k->ascii);
    }
}

static void pointer(const struct k86_event *ev)
{
    push_move(ev->x, ev->y);
    static const struct { uint8_t bit; int code; } b[] = {
        { MOUSE_LEFT, NSFB_KEY_MOUSE_1 }, { MOUSE_MIDDLE, NSFB_KEY_MOUSE_2 }, { MOUSE_RIGHT, NSFB_KEY_MOUSE_3 },
    };
    for (int i = 0; i < 3; i++)
        if ((ev->buttons ^ buttons) & b[i].bit)
            push(ev->buttons & b[i].bit ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP, b[i].code);
    buttons = ev->buttons;
}

static bool lkx_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    (void)nsfb;
    while (qtail == qhead) {
        /* No waiting with a limit in KERN86: a timer does it (0: a moment). */
        if (timeout >= 0)
            k86_timer_set(timeout > 0 ? (uint32_t)timeout : 1);
        struct k86_event ev;
        int err = k86_call(SYS_WAIT_EVENT, (uint32_t)&ev, 0, 0, 0);
        if (timeout >= 0)
            k86_timer_set(0);
        if (err)
            return false;
        switch (ev.type) {
        case K86_EVENT_TIMER:
            event->type = NSFB_EVENT_CONTROL;
            event->value.controlcode = NSFB_CONTROL_TIMEOUT;
            return true;
        case K86_EVENT_CLOSE:
            event->type = NSFB_EVENT_CONTROL;
            event->value.controlcode = NSFB_CONTROL_QUIT;
            return true;
        case K86_EVENT_RESIZE:
            cur_w = ev.x;
            cur_h = ev.y;
            event->type = NSFB_EVENT_RESIZE;
            event->value.resize.w = ev.x;
            event->value.resize.h = ev.y;
            return true;
        case K86_EVENT_POINTER:
            pointer(&ev);
            break;
        case K86_EVENT_KEY:
            key(&ev.key);
            break;
        case K86_EVENT_SCROLL: {            /* the wheel: X11's buttons 4 and 5, 100 px a notch */
            push_move(ev.x, ev.y);
            int n = ev.id < 0 ? -ev.id : ev.id, code = ev.id < 0 ? NSFB_KEY_MOUSE_4 : NSFB_KEY_MOUSE_5;
            for (int i = 0; i < n && i < 8; i++) {
                push(NSFB_EVENT_KEY_DOWN, code);
                push(NSFB_EVENT_KEY_UP, code);
            }
            break;
        }
        default:
            break;                          /* the theme, header buttons: nothing here */
        }
    }
    *event = queue[qtail];
    qtail = (qtail + 1) % QUEUE;
    return true;
}

/* The Web app's downloads (linux/package/netsurf-lkx/download_lkx.c, in
 * NetSurf's front end): the file saved in the user's Downloads folder
 * (made if it isn't there), as "name", or "name (2)"... when that's taken.
 * 0 and the path in `saved`, or a negative errno. */
int lkx_save_download(const char *name, const void *data, unsigned len, char *saved, int max);
int lkx_save_download(const char *name, const void *data, unsigned len, char *saved, int max)
{
    char home[K86_PATH_MAX], folder[K86_PATH_MAX], file[K86_NAME_MAX];
    int vol = k86_call(SYS_HOME, (uint32_t)home, 0, 0, 0);
    if (vol < 0)
        return vol;
    k86_call(SYS_FS_CREATE, (uint32_t)vol, (uint32_t)home, (uint32_t)"Downloads", 1);    /* -EEXIST: fine */
    snprintf(folder, sizeof(folder), "%s%sDownloads", home, strcmp(home, "/") ? "/" : "");
    /* "report.pdf", then "report (2).pdf", "report (3).pdf"... */
    const char *dot = strrchr(name, '.');
    int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
    char probe;
    for (int n = 1; n < 100; n++) {
        if (n == 1)
            snprintf(file, sizeof(file), "%s", name);
        else
            snprintf(file, sizeof(file), "%.*s (%d)%s", stem > 48 ? 48 : stem, name, n, name + stem);
        if (k86_call5(SYS_FS_READ, (uint32_t)vol, (uint32_t)folder, (uint32_t)file, (uint32_t)&probe, 1) < 0)
            break;                              /* not there: this name */
    }
    int err = k86_call5(SYS_FS_WRITE, (uint32_t)vol, (uint32_t)folder, (uint32_t)file, (uint32_t)data, len);
    if (err >= 0)
        snprintf(saved, (size_t)max, "%s/%s", folder, file);
    return err < 0 ? err : 0;
}

static int lkx_claim(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    (void)nsfb, (void)box;
    return 0;
}

static int lkx_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    (void)nsfb;
    int x = box->x0 < 0 ? 0 : box->x0, y = box->y0 < 0 ? 0 : box->y0;
    int w = box->x1 - x, h = box->y1 - y;
    if (w > 0 && h > 0)
        k86_present(x, y, w, h);
    return 0;
}

static int lkx_cursor(nsfb_t *nsfb, struct nsfb_cursor_s *cursor)
{
    (void)nsfb, (void)cursor;
    return 0;                               /* the desktop draws the pointer */
}

const nsfb_surface_rtns_t lkx_rtns = {
    .defaults = lkx_defaults,
    .initialise = lkx_initialise,
    .finalise = lkx_finalise,
    .input = lkx_input,
    .claim = lkx_claim,
    .update = lkx_update,
    .cursor = lkx_cursor,
    .geometry = lkx_set_geometry,
};

/* Registered as the Linux framebuffer's kind (libnsfb 3.10 builds none of
 * its own here), under its own name: "-f lkx". */
NSFB_SURFACE_DEF(lkx, NSFB_SURFACE_LINUX, &lkx_rtns)
