/* LiteKern X — Settings: the Users page (kernel/users.h).
 *
 * Everyone who uses the machine, with buttons to add someone, change a
 * password, rename, make an admin (or not) and remove. Each runs as a few
 * small dialogs (a name, the new password twice, then your own password if
 * you have one), and the kernel checks the lot again (SYS_USER_CHANGE):
 * only admins add, remove or change others. Buttons you can't use are
 * greyed out. Actions are logged ("user: settings: users ...") for the tests. */
#include "apps/settings/users.h"
#include "kernel/errno.h"
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"

static struct gfx_surface *c;
static struct k86_users us;
static struct wg_list list;
static struct wg_button b_add, b_password, b_rename, b_admin, b_remove;
static struct wg_button *const bar[] = { &b_add, &b_password, &b_rename, &b_admin, &b_remove };
#define NBAR 5
static char status[96];
static int status_bad, list_y;

/* A change in progress: its op, and the dialog steps still to go. */
enum step { S_NONE, S_NAME, S_NEW, S_AGAIN, S_AUTH, S_CONFIRM, S_FILES };
static struct {
    int active;
    struct k86_user_change ch;
    enum step steps[5];
    int nsteps, at;
    char first[K86_PASSWORD];           /* the new password, typed the first time */
} flow;
static struct wg_dialog dialog;
static char body[160];

static void copy(char *dst, const char *src, int size)
{
    int n = 0;
    for (; src[n] && n < size - 1; n++)
        dst[n] = src[n];
    dst[n] = '\0';
}

static int me(void) { return us.current; }
static int admin(void) { return me() >= 0 && us.user[me()].admin; }
static int has_pw(void) { return me() >= 0 && us.user[me()].has_password; }
static int sel(void) { return list.selected; }

/* --- the list ------------------------------------------------------------------------------ */

static char details[K86_USERS_MAX][48];

static void row(void *ctx, int i, struct wg_row *out)
{
    (void)ctx;
    char *d = details[i];
    copy(d, us.user[i].admin ? "Administrator" : "Standard", 48);
    int n = (int)strlen(d);
    copy(d + n, us.user[i].has_password ? ", password" : ", no password", 48 - n);
    n = (int)strlen(d);
    if (i == me())
        copy(d + n, " (you)", 48 - n);
    *out = (struct wg_row){ us.user[i].name, d, WG_ICON_NONE };
}

static void enable(void)
{
    int s = sel(), self = s == me();
    wg_button_set_disabled(&b_add, !admin() || us.count >= K86_USERS_MAX);
    wg_button_set_disabled(&b_password, s < 0 || (!self && !admin()));
    wg_button_set_disabled(&b_rename, s < 0 || (!self && !admin()));
    wg_button_set_disabled(&b_admin, s < 0 || !admin());
    wg_button_set_disabled(&b_remove, s < 0 || self || !admin());
    b_admin.label = s >= 0 && us.user[s].admin ? "Make Standard" : "Make Admin";
}

static void reload(void)
{
    int keep = sel();
    k86_users(&us);
    wg_list_set_count(&list, us.count);
    if (keep < 0 || keep >= us.count)
        keep = me();
    wg_list_select(&list, keep);
    enable();
}

void users_page_layout(void)
{
    int w = c->w - 2 * 48, x = 48;
    if (w > 720)
        w = 720, x = (c->w - w) / 2;
    list_y = 20 + text_height(TEXT_HEADING) + 12 + text_height(TEXT_BODY) + 18;
    int bar_y = c->h - 34 - 24;
    list.r = (struct gfx_rect){ x, list_y, w, bar_y - 16 - list_y };
    int bx = x;
    for (int i = 0; i < NBAR; i++) {
        int bw = wg_button_width(i == 3 ? "Make Standard" : bar[i]->label);
        bar[i]->r = (struct gfx_rect){ bx, bar_y, bw, 34 };
        bx += bw + 10;
    }
}

void users_page_draw(void)
{
    const struct theme *t = theme_get();
    gfx_fill_rect(c, 0, 0, c->w, c->h, t->window_bg);
    text_draw(c, list.r.x, 20, "Users", TEXT_HEADING, t->fg);
    text_draw_fit(c, list.r.x, 20 + text_height(TEXT_HEADING) + 8,
                  "A password keeps people off this phone. Its storage itself isn't encrypted.", list.r.w,
                  TEXT_BODY, t->fg_dim);
    wg_list_draw(c, &list);
    for (int i = 0; i < NBAR; i++)
        wg_button_draw(c, bar[i]);
    if (status[0])
        text_draw_fit(c, list.r.x, c->h - 22, status, list.r.w, TEXT_SMALL, status_bad ? t->destructive : t->fg_dim);
    if (flow.active)
        wg_dialog_show(c, &dialog);
    k86_present(0, 0, c->w, c->h);
}

void users_page_open(struct gfx_surface *canvas)
{
    c = canvas;
    if (!list.row) {
        list = (struct wg_list){ .selected = -1, .hover = -1, .row = row, .last_click_row = -1 };
        b_add = (struct wg_button){ .label = "Add User", .style = WG_BUTTON_SUGGESTED };
        b_password = (struct wg_button){ .label = "Change Password" };
        b_rename = (struct wg_button){ .label = "Rename" };
        b_admin = (struct wg_button){ .label = "Make Admin" };
        b_remove = (struct wg_button){ .label = "Remove", .style = WG_BUTTON_DESTRUCTIVE };
    }
    users_page_layout();
    reload();
    users_page_draw();
}

/* --- the dialogs ---------------------------------------------------------------------------- */

static void show_step(const char *error)
{
    const char *who = flow.ch.op == K86_USER_ADD ? flow.ch.name : us.user[flow.ch.user].name;
    enum step s = flow.steps[flow.at];
    const char *title = "", *action = "Next";
    enum wg_button_style style = WG_BUTTON_SUGGESTED;
    int entry = 1, secret = 1;
    body[0] = '\0';
    switch (s) {
    case S_NAME:
        title = flow.ch.op == K86_USER_ADD ? "Add User" : "Rename";
        copy(body, "Their name, as the login screen shows it.", sizeof(body));
        secret = 0;
        break;
    case S_NEW:
        title = "New Password";
        copy(body, "For ", sizeof(body));
        copy(body + 4, who, sizeof(body) - 4);
        copy(body + strlen(body), ". Leave it empty for none.", sizeof(body) - strlen(body));
        break;
    case S_AGAIN:
        title = "Type It Again";
        copy(body, "The same password again, to be sure.", sizeof(body));
        break;
    case S_AUTH:
        title = "Your Password";
        copy(body, "Type your own password to make this change.", sizeof(body));
        action = "Done";
        break;
    case S_FILES:
        title = "Their Files?";
        copy(body, "Delete /Users/", sizeof(body));
        copy(body + strlen(body), who, sizeof(body) - strlen(body));
        copy(body + strlen(body), " too, or keep it for an administrator to open?", sizeof(body) - strlen(body));
        action = "Delete Files";
        style = WG_BUTTON_DESTRUCTIVE;
        entry = 0;
        break;
    case S_CONFIRM:
        title = "Remove User?";
        copy(body, who, sizeof(body));
        copy(body + strlen(body), " won't be able to log in any more.", sizeof(body) - strlen(body));
        action = "Remove";
        style = WG_BUTTON_DESTRUCTIVE;
        entry = 0;
        break;
    default:
        return;
    }
    if (flow.at == flow.nsteps - 1 && s != S_CONFIRM && s != S_FILES)
        action = "Done";
    wg_dialog_init(&dialog, title, body, action, style, entry);
    if (s == S_FILES)
        dialog.buttons[0].label = "Keep Files";   /* not a cancel here: the removal goes on */
    dialog.entry.secret = secret;
    if (s == S_NAME && flow.ch.op == K86_USER_RENAME)
        wg_entry_set(&dialog.entry, us.user[flow.ch.user].name);
    if (error)
        wg_dialog_set_body(&dialog, error, 1);
    wg_dialog_layout(&dialog, c->w, c->h);
    users_page_draw();
}

static void start(int op)
{
    memset(&flow, 0, sizeof(flow));
    flow.active = 1;
    flow.ch.op = op;
    flow.ch.user = sel();
    int s = sel(), self = s == me();
    switch (op) {
    case K86_USER_ADD:
        flow.steps[flow.nsteps++] = S_NAME;
        flow.steps[flow.nsteps++] = S_NEW;
        flow.steps[flow.nsteps++] = S_AGAIN;
        break;
    case K86_USER_PASSWORD:
        if (self && has_pw())
            flow.steps[flow.nsteps++] = S_AUTH;     /* your current one first */
        flow.steps[flow.nsteps++] = S_NEW;
        flow.steps[flow.nsteps++] = S_AGAIN;
        break;
    case K86_USER_RENAME:
        flow.steps[flow.nsteps++] = S_NAME;
        break;
    case K86_USER_ADMIN:
        flow.ch.admin = !us.user[s].admin;
        break;
    case K86_USER_REMOVE:
        flow.steps[flow.nsteps++] = S_CONFIRM;
        flow.steps[flow.nsteps++] = S_FILES;
        break;
    }
    if (has_pw() && !(op == K86_USER_PASSWORD && self))
        flow.steps[flow.nsteps++] = S_AUTH;
    if (!flow.nsteps) {                     /* nothing to ask: straight on */
        flow.at = 0;
        flow.active = 0;
    }
}

static const char *why(int err)
{
    switch (err) {
    case -EACCES: return "That isn't your password.";
    case -EPERM:  return "Only an administrator can do that.";
    case -EEXIST: return "Someone already has that name.";
    case -ENOSPC: return "That's as many users as there can be (8).";
    case -EBUSY:  return "There has to be an administrator, and you can't remove yourself.";
    case -EINVAL: return "That name or password can't be used.";
    default:      return "That didn't work.";
    }
}

static void submit(void)
{
    static const char *const what[] = { "", "add", "password", "rename", "admin", "remove" };
    int err = k86_user_change(&flow.ch);
    k86_logf("settings: users %s %s: %s", what[flow.ch.op],
             flow.ch.op == K86_USER_ADD ? flow.ch.name : us.user[flow.ch.user].name, err ? "refused" : "done");
    if (err == -EACCES) {                   /* back to "your password" */
        for (int i = 0; i < flow.nsteps; i++)
            if (flow.steps[i] == S_AUTH)
                flow.at = i;
        flow.active = 1;
        memset(flow.ch.auth, 0, sizeof(flow.ch.auth));
        show_step(why(err));
        return;
    }
    flow.active = 0;
    copy(status, err ? why(err) : "Saved.", sizeof(status));
    status_bad = err != 0;
    memset(&flow, 0, sizeof(flow));
    reload();
    users_page_draw();
}

/* The dialog's button: on to the next step, or done. */
static void step_done(void)
{
    const char *text = dialog.entry.text;
    switch (flow.steps[flow.at]) {
    case S_NAME:
        if (!text[0]) {
            show_step("Type a name.");
            return;
        }
        copy(flow.ch.name, text, sizeof(flow.ch.name));
        break;
    case S_NEW:
        copy(flow.first, text, sizeof(flow.first));
        break;
    case S_AGAIN:
        if (strcmp(text, flow.first)) {
            flow.at--;                      /* back to the first time */
            show_step("They weren't the same. Type the new password again.");
            return;
        }
        copy(flow.ch.password, text, sizeof(flow.ch.password));
        break;
    case S_AUTH:
        copy(flow.ch.auth, text, sizeof(flow.ch.auth));
        break;
    case S_FILES:
        flow.ch.admin = 1;                  /* "Delete Files" */
        break;
    default:
        break;
    }
    if (++flow.at < flow.nsteps) {
        show_step(0);
        return;
    }
    submit();
}

/* The dialog's first button: Cancel, except when it's Keep Files. */
static void first_button(void)
{
    if (flow.active && flow.steps[flow.at] == S_FILES) {
        flow.ch.admin = 0;
        if (++flow.at < flow.nsteps)
            show_step(0);
        else
            submit();
        return;
    }
    memset(&flow, 0, sizeof(flow));
    users_page_draw();
}

static void cancel(void)
{
    memset(&flow, 0, sizeof(flow));
    users_page_draw();
}

static void begin(int op)
{
    start(op);
    status[0] = '\0';
    if (flow.active)
        show_step(0);
    else
        submit();
}

/* --- input ------------------------------------------------------------------------------------ */

void users_page_pointer(const struct k86_event *ev)
{
    struct wg_pointer p = wg_pointer_make(ev->x, ev->y, ev->buttons, ev->changed, ev->time_ms);
    if (flow.active) {
        int ch = wg_dialog_pointer(&dialog, &p);
        if (ch == 0)
            first_button();
        else if (ch > 0)
            step_done();
        else if (dialog.dirty) {
            wg_dialog_draw(c, &dialog);
            k86_present(dialog.r.x, dialog.r.y, dialog.r.w, dialog.r.h);
        }
        return;
    }
    if (wg_list_pointer(&list, &p) != WG_LIST_NONE)
        enable();
    static const int ops[NBAR] = { K86_USER_ADD, K86_USER_PASSWORD, K86_USER_RENAME, K86_USER_ADMIN, K86_USER_REMOVE };
    for (int i = 0; i < NBAR; i++)
        if (wg_button_pointer(bar[i], &p)) {
            begin(ops[i]);
            return;
        }
    int dirty = list.dirty;
    for (int i = 0; i < NBAR; i++)
        dirty |= bar[i]->dirty;
    if (dirty)
        users_page_draw();
}

int users_page_busy(void)
{
    return flow.active;
}

void users_page_key(const struct key_event *k)
{
    if (flow.active) {
        int ch = wg_dialog_key(&dialog, k);
        if (ch == 0 && k->key == KEY_ESC)
            cancel();                       /* Esc always cancels */
        else if (ch == 0)
            first_button();
        else if (ch > 0)
            step_done();
        else if (dialog.dirty) {
            wg_dialog_draw(c, &dialog);
            k86_present(dialog.r.x, dialog.r.y, dialog.r.w, dialog.r.h);
        }
        return;
    }
    if (wg_list_key(&list, k) != WG_LIST_NONE) {
        enable();
        users_page_draw();
    }
}
