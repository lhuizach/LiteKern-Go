/* LiteKern Go — Settings: setting, changing or removing the lock screen's
 * PIN (the user's password, kernel/users.h; the lock screen's pad types it).
 *
 * A few small dialogs: the current PIN (if there is one), then the new one
 * twice. A PIN is 4 to 16 digits. The kernel checks the current one again
 * (SYS_USER_CHANGE). Logged as "user: settings: lock PIN ..." for the tests. */
#include "apps/settings/pin.h"
#include "kernel/errno.h"
#include "kernel/string.h"
#include "kernel/widget.h"

#define PIN_MIN 4
#define PIN_MAX 16

enum step { S_CURRENT, S_NEW, S_AGAIN };

static struct gfx_surface *c;
static struct wg_dialog dialog;
static struct k86_user_change ch;
static enum step steps[3];
static int nsteps, at, active, op;
static char first[K86_PASSWORD];
static void (*finished)(const char *message, int bad);
static void (*redraw)(void);
static int appearing;                   /* the first dialog fades in (pin_start) */

static void copy(char *dst, const char *src, int size)
{
    int n = 0;
    for (; src[n] && n < size - 1; n++)
        dst[n] = src[n];
    dst[n] = '\0';
}

int pin_active(void)
{
    return active;
}

void pin_draw(void)
{
    if (active)
        wg_dialog_show(c, &dialog);
}

void pin_layout(void)
{
    if (active)
        wg_dialog_layout(&dialog, c->w, c->h);
}

static void show(const char *error)
{
    static const char *const titles[] = { "Current PIN", "New PIN", "Type It Again" };
    static const char *const bodies[] = { "The PIN you unlock the phone with now.", "4 to 16 digits.",
                                          "The same PIN again, to be sure." };
    int last = at == nsteps - 1;
    wg_dialog_init(&dialog, op == PIN_REMOVE && last ? "Remove PIN" : titles[steps[at]],
                   op == PIN_REMOVE && last ? "Type the PIN to remove it. A swipe will unlock the phone."
                                            : bodies[steps[at]],
                   last ? "Done" : "Next", op == PIN_REMOVE && last ? WG_BUTTON_DESTRUCTIVE : WG_BUTTON_SUGGESTED, 1);
    dialog.entry.secret = 1;
    if (error)
        wg_dialog_set_body(&dialog, error, 1);
    wg_dialog_layout(&dialog, c->w, c->h);
    if (appearing) {                        /* the first one fades in, the card rising */
        appearing = 0;
        dialog.appear = 0;
        wg_animate(&dialog.appear, 0, 256, 180, redraw, k86_uptime_ms);
        return;
    }
    redraw();
}

void pin_start(struct gfx_surface *canvas, int what, int has_pin, int user, void (*done)(const char *, int),
               void (*draw)(void))
{
    c = canvas;
    finished = done;
    redraw = draw;
    op = what;
    memset(&ch, 0, sizeof(ch));
    ch.op = K86_USER_PASSWORD;
    ch.user = user;
    nsteps = at = 0;
    if (has_pin)
        steps[nsteps++] = S_CURRENT;
    if (op != PIN_REMOVE) {
        steps[nsteps++] = S_NEW;
        steps[nsteps++] = S_AGAIN;
    }
    if (!nsteps)
        return;
    active = 1;
    k86_logf("settings: lock PIN %s", op == PIN_SET ? "set" : op == PIN_CHANGE ? "change" : "remove");
    appearing = 1;
    show(0);
}

static void end(const char *message, int bad)
{
    active = 0;
    memset(first, 0, sizeof(first));
    memset(&ch, 0, sizeof(ch));            /* not left lying in memory */
    finished(message, bad);
}

void pin_cancel(void)
{
    if (active)
        end("", 0);
}

static int digits_ok(const char *s)
{
    int n = 0;
    for (; s[n]; n++)
        if (s[n] < '0' || s[n] > '9')
            return 0;
    return n >= PIN_MIN && n <= PIN_MAX;
}

static void submit(void)
{
    int err = k86_user_change(&ch);
    k86_logf("settings: lock PIN %s: %s", op == PIN_REMOVE ? "removed" : "saved", err ? "refused" : "done");
    if (err == -EACCES) {                   /* the current one was wrong: ask again */
        at = 0;
        memset(ch.auth, 0, sizeof(ch.auth));
        show("That isn't the PIN. Type it again.");
        return;
    }
    end(err ? "That didn't work." : op == PIN_REMOVE ? "PIN removed. A swipe unlocks the phone." : "PIN saved.", err != 0);
}

static void next(void)
{
    const char *text = dialog.entry.text;
    switch (steps[at]) {
    case S_CURRENT:
        copy(ch.auth, text, sizeof(ch.auth));
        break;
    case S_NEW:
        if (!digits_ok(text)) {
            show("A PIN is 4 to 16 digits.");
            return;
        }
        copy(first, text, sizeof(first));
        break;
    case S_AGAIN:
        if (strcmp(text, first)) {
            at--;
            show("They weren't the same. Type the new PIN again.");
            return;
        }
        copy(ch.password, text, sizeof(ch.password));
        break;
    }
    if (++at < nsteps) {
        show(0);
        return;
    }
    submit();                               /* removing: password stays "" */
}

static void result(int r)
{
    if (r == 0)
        end("", 0);
    else if (r > 0)
        next();
    else if (dialog.dirty) {
        wg_dialog_draw(c, &dialog);
        k86_present(dialog.r.x, dialog.r.y, dialog.r.w, dialog.r.h);
    }
}

void pin_pointer(const struct k86_event *ev)
{
    struct wg_pointer p = wg_pointer_make(ev->x, ev->y, ev->buttons, ev->changed, ev->time_ms);
    result(wg_dialog_pointer(&dialog, &p));
}

void pin_key(const struct key_event *k)
{
    result(wg_dialog_key(&dialog, k));
}
