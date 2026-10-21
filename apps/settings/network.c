/* LiteKern X — Settings: the Network page (kernel/net.h, the Linux port's
 * phase L5), laid out like the Users page.
 *
 * What's connected (Wi-Fi, Ethernet, the address), then the Wi-Fi networks
 * in range, strongest first, with Connect, Forget and Rescan. Connect asks
 * for the password of a secured network it doesn't know (or whose password
 * was refused); one it remembers joins with the password it has. The state
 * and the list refresh every 2 s while the page shows. Actions are logged
 * ("user: settings: network ...") for the tests. */
#include "apps/settings/network.h"
#include "kernel/errno.h"
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"

static struct gfx_surface *c;
static struct k86_net net;
static struct k86_wifi wifi[K86_WIFI_MAX];
static int nwifi;
static struct wg_list list;
static struct wg_button b_connect, b_forget, b_rescan;
static struct wg_button *const bar[] = { &b_connect, &b_forget, &b_rescan };
#define NBAR 3
static char status[96];
static int status_bad, list_y;
static struct wg_dialog dialog;
static int asking;                      /* the password dialog shows */
static char asking_for[K86_SSID_MAX];
static char body[160];

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

int network_page_available(void)
{
    struct k86_net n;
    memset(&n, 0, sizeof(n));
    int err = k86_net_status(&n);
    int yes = !err && (n.wired != K86_NET_NONE || n.wifi != K86_NET_NONE);
    k86_logf("settings: network %s (wired %d, Wi-Fi %d, %d)", yes ? "page" : "none", n.wired, n.wifi, err);
    return yes;
}

static int sel(void) { return list.selected; }

/* --- the list ------------------------------------------------------------------------------- */

static char details[K86_WIFI_MAX][48];

static const char *strength(int pct)
{
    return pct >= 75 ? "excellent" : pct >= 50 ? "good" : pct >= 25 ? "fair" : "weak";
}

static void row(void *ctx, int i, struct wg_row *out)
{
    (void)ctx;
    const struct k86_wifi *w = &wifi[i];
    char *d = details[i];
    int wrong = net.wifi == K86_NET_WRONG_KEY && !strcmp(net.ssid, w->ssid);
    copy(d, w->connected ? "Connected" : wrong ? "Wrong password" : w->known ? "Saved"
                                       : w->secure ? "Secured" : "Open", 48);
    append(d, ", signal ", 48);
    append(d, strength(w->signal), 48);
    *out = (struct wg_row){ w->ssid, d, WG_ICON_NONE };
}

static void enable(void)
{
    int s = sel();
    wg_button_set_disabled(&b_connect, s < 0 || wifi[s].connected);
    wg_button_set_disabled(&b_forget, s < 0 || !wifi[s].known);
    wg_button_set_disabled(&b_rescan, net.wifi == K86_NET_NONE);
}

/* Read the state and the networks; keep the selection on the same network. */
static int reload(void)
{
    char keep[K86_SSID_MAX] = "";
    if (sel() >= 0 && sel() < nwifi)
        copy(keep, wifi[sel()].ssid, sizeof(keep));
    struct k86_net before = net;
    struct k86_wifi old[K86_WIFI_MAX];
    int nold = nwifi;
    memcpy(old, wifi, sizeof(old));
    k86_net_status(&net);
    int n = net.wifi != K86_NET_NONE ? k86_net_scan(wifi, K86_WIFI_MAX) : 0;
    nwifi = n < 0 ? 0 : n > K86_WIFI_MAX ? K86_WIFI_MAX : n;
    wg_list_set_count(&list, nwifi);
    int pick = -1;
    for (int i = 0; i < nwifi && keep[0]; i++)
        if (!strcmp(wifi[i].ssid, keep))
            pick = i;
    wg_list_select(&list, pick);
    enable();
    return memcmp(&before, &net, sizeof(net)) || nold != nwifi || memcmp(old, wifi, sizeof(old));
}

void network_page_layout(void)
{
    int w = c->w - 2 * 48, x = 48;
    if (w > 720)
        w = 720, x = (c->w - w) / 2;
    list_y = 20 + text_height(TEXT_HEADING) + 12 + 2 * (text_height(TEXT_BODY) + 6) + 12;
    int bar_y = c->h - 34 - 24;
    list.r = (struct gfx_rect){ x, list_y, w, bar_y - 16 - list_y };
    int bx = x;
    for (int i = 0; i < NBAR; i++) {
        bar[i]->r = (struct gfx_rect){ bx, bar_y, wg_button_width(bar[i]->label), 34 };
        bx += bar[i]->r.w + 10;
    }
    if (asking)
        wg_dialog_layout(&dialog, c->w, c->h);
}

static const char *state_name(int s, int wifi_kind)
{
    switch (s) {
    case K86_NET_NONE:       return wifi_kind ? "none on this machine" : "no port";
    case K86_NET_OFF:        return wifi_kind ? "switched off (Fn+F2 switches it on)" : "no cable";
    case K86_NET_DOWN:       return "not connected";
    case K86_NET_CONNECTING: return "connecting...";
    case K86_NET_UP:         return "connected";
    default:                 return "the password was refused";
    }
}

void network_page_draw(void)
{
    const struct theme *t = theme_get();
    char line[120];
    gfx_fill_rect(c, 0, 0, c->w, c->h, t->window_bg);
    text_draw(c, list.r.x, 20, "Network", TEXT_HEADING, t->fg);
    int y = 20 + text_height(TEXT_HEADING) + 8;
    copy(line, "Wi-Fi: ", sizeof(line));
    append(line, state_name(net.wifi, 1), sizeof(line));
    if (net.ssid[0] && net.wifi >= K86_NET_CONNECTING) {
        append(line, " (", sizeof(line));
        append(line, net.ssid, sizeof(line));
        append(line, ")", sizeof(line));
    }
    text_draw_fit(c, list.r.x, y, line, list.r.w, TEXT_BODY, net.wifi == K86_NET_WRONG_KEY ? t->destructive : t->fg_dim);
    y += text_height(TEXT_BODY) + 6;
    copy(line, "Ethernet: ", sizeof(line));
    append(line, state_name(net.wired, 0), sizeof(line));
    if (net.address[0]) {
        append(line, ". Address: ", sizeof(line));
        append(line, net.address, sizeof(line));
    }
    text_draw_fit(c, list.r.x, y, line, list.r.w, TEXT_BODY, t->fg_dim);
    wg_list_draw(c, &list);
    if (!nwifi) {
        const char *empty = net.wifi == K86_NET_NONE ? "No Wi-Fi on this machine."
                          : net.wifi == K86_NET_OFF ? "Wi-Fi is switched off." : "Looking for networks...";
        text_draw(c, list.r.x + 16, list.r.y + 14, empty, TEXT_BODY, t->fg_dim);
    }
    for (int i = 0; i < NBAR; i++)
        wg_button_draw(c, bar[i]);
    if (status[0])
        text_draw_fit(c, list.r.x, c->h - 22, status, list.r.w, TEXT_SMALL, status_bad ? t->destructive : t->fg_dim);
    if (asking)
        wg_dialog_show(c, &dialog);
    k86_present(0, 0, c->w, c->h);
}

void network_page_open(struct gfx_surface *canvas)
{
    c = canvas;
    if (!list.row) {
        list = (struct wg_list){ .selected = -1, .hover = -1, .row = row, .last_click_row = -1 };
        b_connect = (struct wg_button){ .label = "Connect", .style = WG_BUTTON_SUGGESTED };
        b_forget = (struct wg_button){ .label = "Forget" };
        b_rescan = (struct wg_button){ .label = "Rescan" };
    }
    network_page_layout();
    reload();
    network_page_draw();
}

void network_page_tick(void)
{
    if (asking || !reload())
        return;
    if (!strcmp(status, "Connecting...") && net.wifi == K86_NET_UP)
        copy(status, "Connected.", sizeof(status));
    else if (!strcmp(status, "Connecting...") && net.wifi == K86_NET_WRONG_KEY)
        status[0] = '\0';                   /* the line above says why */
    network_page_draw();
}

/* --- joining --------------------------------------------------------------------------------- */

static void join(const char *ssid, const char *password)
{
    int err = k86_net_join(ssid, password);
    k86_logf("settings: network join %s: %s", ssid, err ? "refused" : "asked");
    copy(status, err == -EINVAL ? "A Wi-Fi password is 8 to 63 characters." : err ? "That didn't work." : "Connecting...",
         sizeof(status));
    status_bad = err != 0;
    reload();
    network_page_draw();
}

static void ask_password(const char *ssid, const char *error)
{
    copy(asking_for, ssid, sizeof(asking_for));
    copy(body, "The password for ", sizeof(body));
    append(body, ssid, sizeof(body));
    append(body, ".", sizeof(body));
    wg_dialog_init(&dialog, "Wi-Fi Password", body, "Connect", WG_BUTTON_SUGGESTED, 1);
    dialog.entry.secret = 1;
    if (error)
        wg_dialog_set_body(&dialog, error, 1);
    asking = 1;
    wg_dialog_layout(&dialog, c->w, c->h);
    network_page_draw();
}

static void connect_selected(void)
{
    int s = sel();
    if (s < 0)
        return;
    const struct k86_wifi *w = &wifi[s];
    int wrong = net.wifi == K86_NET_WRONG_KEY && !strcmp(net.ssid, w->ssid);
    if (w->secure && (!w->known || wrong))
        ask_password(w->ssid, wrong ? "That password was refused. Type it again." : 0);
    else
        join(w->ssid, "");                  /* open, or remembered: no password to ask */
}

static void password_done(void)
{
    const char *p = dialog.entry.text;
    int n = (int)strlen(p);
    if (n < 8 || n > 63) {
        wg_dialog_set_body(&dialog, "A Wi-Fi password is 8 to 63 characters.", 1);
        wg_dialog_draw(c, &dialog);
        k86_present(dialog.r.x, dialog.r.y, dialog.r.w, dialog.r.h);
        return;
    }
    asking = 0;
    join(asking_for, p);
}

static void forget_selected(void)
{
    int s = sel();
    if (s < 0)
        return;
    int err = k86_net_forget(wifi[s].ssid);
    k86_logf("settings: network forget %s: %s", wifi[s].ssid, err ? "refused" : "done");
    copy(status, err ? "That didn't work." : "Forgotten.", sizeof(status));
    status_bad = err != 0;
    reload();
    network_page_draw();
}

/* --- input ------------------------------------------------------------------------------------ */

static void dialog_result(int ch)
{
    if (ch == 0) {
        asking = 0;
        network_page_draw();
    } else if (ch > 0) {
        password_done();
    } else if (dialog.dirty) {
        wg_dialog_draw(c, &dialog);
        k86_present(dialog.r.x, dialog.r.y, dialog.r.w, dialog.r.h);
    }
}

void network_page_pointer(const struct k86_event *ev)
{
    struct wg_pointer p = wg_pointer_make(ev->x, ev->y, ev->buttons, ev->changed, ev->time_ms);
    if (asking) {
        dialog_result(wg_dialog_pointer(&dialog, &p));
        return;
    }
    enum wg_list_result r = wg_list_pointer(&list, &p);
    if (r != WG_LIST_NONE)
        enable();
    if (r == WG_LIST_ACTIVATE) {
        connect_selected();
        return;
    }
    if (wg_button_pointer(&b_connect, &p)) {
        connect_selected();
        return;
    }
    if (wg_button_pointer(&b_forget, &p)) {
        forget_selected();
        return;
    }
    if (wg_button_pointer(&b_rescan, &p)) {
        copy(status, "Looking for networks...", sizeof(status));
        status_bad = 0;
        reload();
        network_page_draw();
        return;
    }
    int dirty = list.dirty;
    for (int i = 0; i < NBAR; i++)
        dirty |= bar[i]->dirty;
    if (dirty)
        network_page_draw();
}

void network_page_scroll(const struct k86_event *ev)
{
    if (!asking && wg_list_scroll(&list, ev->x, ev->y, ev->id))
        network_page_draw();
}

void network_page_key(const struct key_event *k)
{
    if (asking) {
        dialog_result(wg_dialog_key(&dialog, k));
        return;
    }
    enum wg_list_result r = wg_list_key(&list, k);
    if (r == WG_LIST_ACTIVATE) {
        connect_selected();
        return;
    }
    if (r != WG_LIST_NONE) {
        enable();
        network_page_draw();
    }
}
