/* Browser: a web browser with real JavaScript. The page is drawn by WPE WebKit, which runs in a small
 * Alpine sandbox in the phone's "extras" area (docs/BROWSER-PLAN.md) as the program lkx-webd
 * (phone/sandbox/webd.c). This app starts it, shows its picture (a shared frame file), and sends it
 * what the user does over a socket: taps, drags, keys, and the address bar and buttons. The engine is
 * not part of the system image: without /extras it says so. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"
#include "sdk/kern86.h"
#include "sdk/touch2.h"

#define ENTER "/extras/enter.sh"
#define WEBD "/extras/alpine/usr/local/bin/lkx-webd"
#define FRAME_FILE "/dev/shm/lkxweb.frame"
#define SOCK_FILE "/dev/shm/lkxweb.sock"
#define HOME_URL "https://html.duckduckgo.com/html/"   /* DuckDuckGo's page without JavaScript: much quicker here */
#define SEARCH_URL "https://html.duckduckgo.com/html/?q="
#define BAR 0                           /* the page starts at the top: the buttons and the address bar are in the
                                         * window's header bar (publish_header), where the app's name was */
#define HEADER_BYTES 64
#define FRAME_MAGIC 0x4657574cu
#define DRAG_SLOP 14
#define URL_MAX 512

/* the header buttons' ids */
enum { HB_BACK = 1, HB_FWD, HB_RELOAD, HB_HOME, HB_ADDR, HB_TABS };

struct frame_header { uint32_t magic, seq, w, h, stride, reserved[3]; };

enum { ENGINE_NONE, ENGINE_STARTING, ENGINE_READY, ENGINE_FAILED };

static struct k86_window win;
static struct gfx_surface canvas;

static int engine = ENGINE_NONE;
static pid_t webd_pid;
static int sock = -1;
static uint32_t started_ms;
static const unsigned char *frame;      /* the mapped FRAME_FILE, or NULL */
static size_t frame_len;
static uint32_t seen_seq, frame_w, frame_h;
static int got_frame;
static char inbuf[2048];
static size_t inlen;

static char title[128], uri[URL_MAX], errtext[128];
static int loading, progress, can_back, can_fwd, page_kb;
static int editing;                     /* the address bar has the keyboard */
static int edit_fresh;                  /* nothing typed yet: the first key replaces the address */
static char url_edit[URL_MAX];
static int url_len;

static int ptr_down, ptr_in_view, dragged, sx, sy, last_y;
static uint32_t last_change_ms;
static uint32_t timer_ms;

static int view_h(void) { return canvas.h - 2 * BAR; }

/* ---- tabs and the page's zoom ---------------------------------------------------------------------------------
 * A tab is an address and a title. There is one engine (the phone has little memory for more), so a tab you
 * come back to is loaded again, from the top. */
#define TABS_MAX 8
struct tab { char uri[URL_MAX]; char title[64]; };
static struct tab tabs[TABS_MAX];
static int ntabs = 1, cur_tab;
static int sheet;                       /* the tabs sheet is open */
static int desktop_site;                /* 1: asks sites for their desktop versions */
static int pinching;
static double zoom_mul = 1.0;           /* the pinch's factor on the page's usual zoom */
static double pinch_d0, pinch_m0;
static double applied_mul = 1.0;       /* the factor the engine has drawn the page at */
static int pinch_cx, pinch_cy;          /* where the fingers are, in the view */
static uint32_t pinch_draw_ms;
static struct gfx_rect sheet_close[TABS_MAX], sheet_card[TABS_MAX], sheet_new, sheet_desk;

/* The view's tallest height so far (the keyboard down). The engine draws for it, and when the on-screen keyboard
 * makes the window shorter the page is not squashed to fit: the keyboard covers the bottom of it. */
static int full_view_h;
static int ref_h(void)
{
    if (view_h() > full_view_h)
        full_view_h = view_h();
    return full_view_h;
}

/* ---- the engine ----------------------------------------------------------------------------- */

static int engine_installed(void)
{
    return access(ENTER, X_OK) == 0 && access(WEBD, X_OK) == 0;
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char line[URL_MAX + 40];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (sock < 0 || n <= 0)
        return;
    if (n > (int)sizeof(line) - 2)
        n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    if (write(sock, line, (size_t)n) < 0 && errno != EAGAIN) {
        close(sock);
        sock = -1;
    }
}

static int map_frame(void)
{
    struct stat st;
    int fd = open(FRAME_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) || st.st_size < HEADER_BYTES) { close(fd); return -1; }
    const struct frame_header *h = mmap(NULL, HEADER_BYTES, PROT_READ, MAP_SHARED, fd, 0);
    if (h == MAP_FAILED) { close(fd); return -1; }
    uint32_t w = h->w, ht = h->h, magic = h->magic;
    munmap((void *)h, HEADER_BYTES);
    size_t len = HEADER_BYTES + (size_t)w * ht * 4;
    if (magic != FRAME_MAGIC || w == 0 || ht == 0 || (size_t)st.st_size < len) { close(fd); return -1; }
    void *m = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
        return -1;
    frame = m;
    frame_len = len;
    frame_w = w;
    frame_h = ht;
    return 0;
}

static int try_connect(void)
{
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strncpy(a.sun_path, SOCK_FILE, sizeof(a.sun_path) - 1);
    if (s < 0)
        return -1;
    if (connect(s, (struct sockaddr *)&a, sizeof(a))) { close(s); return -1; }
    if (map_frame()) { close(s); return -1; }
    sock = s;
    inlen = 0;
    return 0;
}

/* How big the engine draws, as a percent of the view. WebKit's cost goes with the pixels it paints (on the
 * phone: 25 fps at 100, 41 at 75, 80 at 50 while scrolling), and the app enlarges the picture, which is cheap.
 * 85 by default; a number from 40 to 100 in /extras/web-scale changes it (takes effect when the engine starts). */
static int render_pct = 85;

static void load_render_pct(void)
{
    FILE *f = fopen("/extras/web-scale", "r");
    int v;
    if (f) {
        if (fscanf(f, "%d", &v) == 1 && v >= 40 && v <= 100)
            render_pct = v;
        fclose(f);
    }
}

static void engine_spawn(void)
{
    char size[24], zoomarg[16], vscalearg[16];
    load_render_pct();
    snprintf(size, sizeof(size), "%dx%d", (canvas.w * render_pct / 100) & ~1, (ref_h() * render_pct / 100) & ~1);
    snprintf(zoomarg, sizeof(zoomarg), "%.3f", 2.0 * render_pct / 100);
    snprintf(vscalearg, sizeof(vscalearg), "%.3f", render_pct / 100.0);
    mkdir("/extras/alpine/run", 0755);
    mkdir("/extras/alpine/run/u", 0700);
    unlink(SOCK_FILE);
    unlink(FRAME_FILE);
    pid_t p = fork();
    if (p < 0) { engine = ENGINE_FAILED; snprintf(errtext, sizeof(errtext), "can't start it (fork)"); return; }
    if (p == 0) {
        setsid();
        int in = open("/dev/null", O_RDONLY), out = open("/tmp/lkx-web.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (in >= 0) dup2(in, 0);
        if (out >= 0) { dup2(out, 1); dup2(out, 2); }
        for (int fd = 3; fd < 64; fd++) close(fd);
        execl(ENTER, ENTER, "env", "XDG_RUNTIME_DIR=/run/u", "HOME=/root", "WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1",
              "/usr/local/bin/lkx-webd", "--serve", "--size", size,
              "--zoom", zoomarg, "--vscale", vscalearg, HOME_URL, (char *)NULL);
        _exit(127);
    }
    webd_pid = p;
    started_ms = k86_uptime_ms();
    engine = ENGINE_STARTING;
    k86_log("browser: starting the engine");
}

static void engine_start(void)
{
    if (!engine_installed()) {
        engine = ENGINE_NONE;
        return;
    }
    if (try_connect() == 0) {           /* one is running already (the app was stopped, not closed) */
        engine = ENGINE_READY;
        say("reload");
        return;
    }
    engine_spawn();
}

static void engine_stop(void)
{
    say("quit");
    if (sock >= 0)
        close(sock);
    sock = -1;
    if (webd_pid > 0) {
        for (int i = 0; i < 20; i++) {
            if (waitpid(webd_pid, NULL, WNOHANG) == webd_pid) { webd_pid = 0; break; }
            usleep(50000);
        }
        if (webd_pid > 0) { kill(-webd_pid, SIGKILL); waitpid(webd_pid, NULL, 0); }
    }
    unlink(SOCK_FILE);
}

/* a line from the engine */
static void on_line(char *l)
{
    int a, b;
    if (!strncmp(l, "title ", 6)) snprintf(title, sizeof(title), "%s", l + 6);
    else if (!strncmp(l, "uri ", 4)) snprintf(uri, sizeof(uri), "%s", l + 4);
    else if (!strncmp(l, "loading ", 8)) loading = atoi(l + 8);
    else if (!strncmp(l, "progress ", 9)) progress = atoi(l + 9);
    else if (sscanf(l, "nav %d %d", &a, &b) == 2) { can_back = a; can_fwd = b; }
    else if (sscanf(l, "editable %d", &a) == 1) page_kb = a;
    else if (!strncmp(l, "error ", 6)) snprintf(errtext, sizeof(errtext), "%s", l + 6);
    else if (!strncmp(l, "ready", 5)) { engine = ENGINE_READY; errtext[0] = 0; }
}

static int pump(void)
{
    int changed = 0;
    if (sock < 0)
        return 0;
    for (;;) {
        ssize_t n = read(sock, inbuf + inlen, sizeof(inbuf) - 1 - inlen);
        if (n < 0 && errno == EINTR) continue;
        if (n == 0 || (n < 0 && errno != EAGAIN)) {
            close(sock);
            sock = -1;
            engine = ENGINE_FAILED;
            snprintf(errtext, sizeof(errtext), "the engine stopped");
            return 1;
        }
        if (n < 0)
            break;
        inlen += (size_t)n;
        inbuf[inlen] = 0;
        char *start = inbuf, *nl;
        while ((nl = strchr(start, '\n'))) { *nl = 0; on_line(start); start = nl + 1; changed = 1; }
        inlen = (size_t)(inbuf + inlen - start);
        memmove(inbuf, start, inlen);
        if (inlen >= sizeof(inbuf) - 1)
            inlen = 0;
    }
    return changed;
}

/* ---- address ------------------------------------------------------------------------------- */

static void go(const char *text)
{
    char url[URL_MAX + 64];
    int space = strchr(text, ' ') != NULL;
    if (strstr(text, "://")) snprintf(url, sizeof(url), "%s", text);
    else if (!space && strchr(text, '.')) snprintf(url, sizeof(url), "https://%s", text);
    else {                              /* a search */
        int n = snprintf(url, sizeof(url), "%s", SEARCH_URL);
        for (const char *c = text; *c && n < (int)sizeof(url) - 4; c++) {
            if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')) url[n++] = *c;
            else n += snprintf(url + n, 4, "%%%02X", (unsigned char)*c);
        }
        url[n] = 0;
    }
    say("load %s", url);
}

/* ---- drawing ------------------------------------------------------------------------------- */

/* The buttons and the address bar are the window's header bar, drawn by the shell (kernel/wm.c) where the app's
 * name was: back, forward, reload or stop, the address (the title, in a field) and home. The header is sent
 * again only when something in it changed. */
static void publish_header(void)
{
    static struct k86_header last;
    static int have_last;
    struct k86_header h;
    memset(&h, 0, sizeof(h));
    if (editing) {                      /* the end of what is typed, with a cursor */
        size_t n = strlen(url_edit), room = sizeof(h.title) - 2;
        snprintf(h.title, sizeof(h.title), "%.46s|", n > room ? url_edit + (n - room) : url_edit);
    } else if (uri[0]) {                /* the address without its https:// and www. */
        const char *u = uri;
        if (!strncmp(u, "https://", 8)) u += 8;
        else if (!strncmp(u, "http://", 7)) u += 7;
        if (!strncmp(u, "www.", 4)) u += 4;
        snprintf(h.title, sizeof(h.title), "%.47s", u);
    } else {
        snprintf(h.title, sizeof(h.title), "Search or type URL");
    }
    h.nbuttons = 6;
    h.buttons[0].side = 0; h.buttons[0].icon = 1; h.buttons[0].id = HB_BACK;   h.buttons[0].selected = can_back ? 0 : 2;
    h.buttons[1].side = 0; h.buttons[1].icon = 4; h.buttons[1].id = HB_FWD;    h.buttons[1].selected = can_fwd ? 0 : 2;
    h.buttons[2].side = 0; h.buttons[2].icon = loading ? 6 : 5; h.buttons[2].id = HB_RELOAD;
    h.buttons[3].side = 1; h.buttons[3].icon = 7; h.buttons[3].id = HB_HOME;
    h.buttons[4].side = 2; h.buttons[4].icon = 0; h.buttons[4].id = HB_ADDR;   h.buttons[4].selected = editing ? 1 : 0;
    h.buttons[5].side = 1; h.buttons[5].icon = 0; h.buttons[5].id = HB_TABS;   h.buttons[5].selected = sheet ? 1 : 0;
    snprintf(h.buttons[5].label, sizeof(h.buttons[5].label), "%d", ntabs);
    if (have_last && !memcmp(&h, &last, sizeof(h)))
        return;
    k86_header(&h);
    last = h;
    have_last = 1;
}

/* A thin line along the top of the page while it loads. */
static void draw_progress(void)
{
    if (loading && progress > 0)
        gfx_fill_rect(&canvas, 0, 0, canvas.w * progress / 100, 4, theme_get()->accent);
}

static void draw_message(const char *a, const char *b)
{
    const struct theme *t = theme_get();
    gfx_fill_rect(&canvas, 0, BAR, canvas.w, view_h(), t->window_bg);
    wg_label_centred(&canvas, canvas.w / 2, BAR + view_h() / 2 - 40, a, WG_TEXT_HEADING);
    if (b)
        wg_label_centred(&canvas, canvas.w / 2, BAR + view_h() / 2 + 20, b, WG_TEXT_BODY);
}

/* While the fingers pinch: the engine's last picture, scaled about the fingers by what the pinch has done since. */
static void draw_view_scaled(void)
{
    const uint32_t *src = (const uint32_t *)(frame + HEADER_BYTES);
    int dw = canvas.w, dh = view_h(), ref = ref_h();
    int f = (int)(zoom_mul * 1000 / applied_mul);
    if (f < 100)
        f = 100;
    static uint16_t xm[2048];
    if (dw > 2048 || frame_w == 0 || frame_h == 0)
        return;
    for (int x = 0; x < dw; x++) {
        int vx = pinch_cx + (x - pinch_cx) * 1000 / f;
        xm[x] = vx < 0 || vx >= dw ? 0xffff : (uint16_t)((uint32_t)vx * frame_w / (uint32_t)dw);
    }
    uint32_t bg = theme_get()->window_bg;
    for (int y = 0; y < dh; y++) {
        uint32_t *dst = (uint32_t *)(canvas.px + (size_t)(BAR + y) * canvas.stride);
        int vy = pinch_cy + (y - pinch_cy) * 1000 / f;
        if (vy < 0 || vy >= dh) {
            for (int x = 0; x < dw; x++)
                dst[x] = bg;
            continue;
        }
        uint32_t sy = (uint32_t)vy * frame_h / (uint32_t)ref;
        const uint32_t *row = src + (size_t)(sy < frame_h ? sy : frame_h - 1) * frame_w;
        for (int x = 0; x < dw; x++)
            dst[x] = xm[x] == 0xffff ? bg : row[xm[x]];
    }
}

static void draw_view(void)
{
    if (engine == ENGINE_NONE) {
        draw_message("No browser engine", "Install it in the extras area first");
        return;
    }
    if (engine == ENGINE_FAILED) {
        draw_message("The engine isn't running", errtext[0] ? errtext : NULL);
        return;
    }
    if (!frame || !got_frame) {
        draw_message("Starting the engine...", "The first page takes a few seconds");
        return;
    }
    if (pinching) {
        draw_view_scaled();
        return;
    }
    const uint32_t *src = (const uint32_t *)(frame + HEADER_BYTES);
    int dw = canvas.w, dh = view_h();
    if ((int)frame_w == dw) {           /* full size: straight copy */
        int h = (int)frame_h < dh ? (int)frame_h : dh;
        for (int y = 0; y < h; y++)
            memcpy(canvas.px + (size_t)(BAR + y) * canvas.stride, src + (size_t)y * frame_w, (size_t)dw * 4);
        return;
    }
    /* A smaller picture (render_pct): enlarged to the view, nearest pixel. A column map is made once, a row that
     * maps to the same source row as the one above is a copy of it. */
    static uint16_t xmap[2048];
    static int map_dw, map_fw;
    if (dw > 2048 || frame_w == 0 || frame_h == 0)
        return;
    if (map_dw != dw || map_fw != (int)frame_w) {
        for (int x = 0; x < dw; x++)
            xmap[x] = (uint16_t)((uint32_t)x * frame_w / (uint32_t)dw);
        map_dw = dw;
        map_fw = (int)frame_w;
    }
    int prev_sy = -1;
    uint32_t *prev = NULL;
    int ref = ref_h();
    for (int y = 0; y < dh; y++) {
        int sy = (int)((uint32_t)y * frame_h / (uint32_t)ref);
        uint32_t *dst = (uint32_t *)(canvas.px + (size_t)(BAR + y) * canvas.stride);
        if (sy == prev_sy && prev) {
            memcpy(dst, prev, (size_t)dw * 4);
        } else {
            const uint32_t *row = src + (size_t)sy * frame_w;
            for (int x = 0; x < dw; x++)
                dst[x] = row[xmap[x]];
        }
        prev_sy = sy;
        prev = dst;
    }
}

static void draw_sheet(void)
{
    const struct theme *t = theme_get();
    int w = canvas.w, h = canvas.h, pad = 24;
    gfx_darken(&canvas, 0, 0, w, h, 170);
    int ch = 112, y = 32;
    memset(sheet_close, 0, sizeof(sheet_close));
    memset(sheet_card, 0, sizeof(sheet_card));
    for (int i = 0; i < ntabs; i++) {
        struct gfx_rect r = { pad, y, w - 2 * pad, ch };
        sheet_card[i] = r;
        sheet_close[i] = (struct gfx_rect){ r.x + r.w - 96, r.y, 96, r.h };
        gfx_fill_round_rect(&canvas, r.x, r.y, r.w, r.h, 26, i == cur_tab ? t->accent_bg : t->view_bg);
        uint32_t fg = i == cur_tab ? t->accent_fg : t->fg;
        const char *tt = i == cur_tab ? title : tabs[i].title;
        const char *uu = i == cur_tab ? uri : tabs[i].uri;
        text_draw_fit(&canvas, r.x + 28, r.y + 18, tt[0] ? tt : "New tab", r.w - 150, TEXT_BOLD, fg);
        text_draw_fit(&canvas, r.x + 28, r.y + 62, uu[0] ? uu : "Search or type URL", r.w - 150, TEXT_SMALL, fg);
        int cx = r.x + r.w - 48, cy = r.y + r.h / 2;
        gfx_thick_line(&canvas, cx - 10, cy - 10, cx + 10, cy + 10, 3, fg);
        gfx_thick_line(&canvas, cx + 10, cy - 10, cx - 10, cy + 10, 3, fg);
        y += ch + 14;
    }
    sheet_new = (struct gfx_rect){ pad, y, w - 2 * pad, 96 };
    gfx_fill_round_rect(&canvas, sheet_new.x, sheet_new.y, sheet_new.w, sheet_new.h, 48, ntabs < TABS_MAX ? t->view_bg : t->window_bg);
    text_draw(&canvas, w / 2 - text_width("New tab", TEXT_BOLD) / 2, sheet_new.y + (96 - text_height(TEXT_BOLD)) / 2, "New tab", TEXT_BOLD,
              ntabs < TABS_MAX ? t->fg : t->fg_dim);
    y += 96 + 14;
    sheet_desk = (struct gfx_rect){ pad, y, w - 2 * pad, 96 };
    gfx_fill_round_rect(&canvas, sheet_desk.x, sheet_desk.y, sheet_desk.w, sheet_desk.h, 48, t->view_bg);
    text_draw(&canvas, sheet_desk.x + 36, sheet_desk.y + (96 - text_height(TEXT_BOLD)) / 2, "Desktop site", TEXT_BOLD, t->fg);
    struct gfx_rect sw = { sheet_desk.x + sheet_desk.w - 36 - 96, sheet_desk.y + 28, 96, 40 };
    gfx_fill_round_rect(&canvas, sw.x, sw.y, sw.w, sw.h, 20, desktop_site ? t->accent_bg : t->window_bg);
    gfx_fill_circle(&canvas, desktop_site ? sw.x + sw.w - 20 : sw.x + 20, sw.y + 20, 16, desktop_site ? t->accent_fg : t->fg_dim);
}

static void redraw(void)
{
    draw_view();
    if (sheet)
        draw_sheet();
    draw_progress();
    publish_header();
    k86_present(0, 0, canvas.w, canvas.h);
}

/* A new picture from the engine and nothing else changed: copy and show only the page area. */
static void redraw_view(void)
{
    if (sheet) {                        /* the sheet is over the page: the page behind it needn't move */
        return;
    }
    draw_view();
    draw_progress();
    k86_present(0, BAR, canvas.w, view_h());
}

/* Finger moves are sent to the engine in batches (at most one every SCROLL_MS, the distance added up): one
 * script run and one repaint per move event was more than the phone's CPU could keep up with. */
#define SCROLL_MS 45
static int pend_dy, pend_x, pend_y;
static uint32_t scroll_sent_ms;

static void flush_scroll(int force)
{
    if (!pend_dy || sock < 0)
        return;
    uint32_t now = k86_uptime_ms();
    if (!force && now - scroll_sent_ms < SCROLL_MS)
        return;
    say("scroll %d %d %d", pend_x, pend_y, pend_dy);
    pend_dy = 0;
    scroll_sent_ms = now;
}

/* ---- input --------------------------------------------------------------------------------- */

static uint32_t keysym(const struct key_event *k)
{
    if (k->key == KEY_ENTER || k->ascii == '\r' || k->ascii == '\n') return 0xff0d;
    if (k->key == KEY_BACKSPACE || k->ascii == 8) return 0xff08;
    if (k->key == KEY_TAB || k->ascii == '\t') return 0xff09;
    if (k->ascii >= 32 && k->ascii < 127) return k->ascii;
    return 0;
}

static void on_key(const struct key_event *k)
{
    if (!k->pressed)
        return;
    if (editing) {
        if (k->key == KEY_ESC) { editing = 0; return; }
        if (k->key == KEY_ENTER || k->ascii == '\r' || k->ascii == '\n') {
            editing = 0;
            if (url_len)
                go(url_edit);
        } else if (k->key == KEY_BACKSPACE || k->ascii == 8) {
            if (edit_fresh) { url_len = 0; url_edit[0] = 0; edit_fresh = 0; }
            else if (url_len) url_edit[--url_len] = 0;
        } else if (k->ascii >= 32 && k->ascii < 127 && url_len < URL_MAX - 2) {
            if (edit_fresh) { url_len = 0; edit_fresh = 0; }
            url_edit[url_len++] = (char)k->ascii;
            url_edit[url_len] = 0;
        }
        return;
    }
    if (k->key == KEY_ESC) {            /* Back */
        if (page_kb) page_kb = 0;
        else if (can_back) say("back");
        return;
    }
    uint32_t sym = keysym(k);
    if (sym && sock >= 0) {
        say("key 0x%x 1", sym);
        say("key 0x%x 0", sym);
    }
}

/* A press in the header bar (the shell sends the button's id). */
static void tab_save(void)
{
    snprintf(tabs[cur_tab].uri, sizeof(tabs[cur_tab].uri), "%.*s", URL_MAX - 1, uri);
    snprintf(tabs[cur_tab].title, sizeof(tabs[cur_tab].title), "%.63s", title);
}

static void tab_open(int i)             /* come to tab i: its page is loaded again */
{
    tab_save();
    cur_tab = i;
    sheet = 0;
    zoom_mul = 1.0;
    say("load %s", tabs[i].uri[0] ? tabs[i].uri : HOME_URL);
}

static void tab_new(void)
{
    if (ntabs >= TABS_MAX)
        return;
    tab_save();
    cur_tab = ntabs++;
    tabs[cur_tab].uri[0] = tabs[cur_tab].title[0] = 0;
    sheet = 0;
    zoom_mul = 1.0;
    say("load %s", HOME_URL);
}

static void tab_close(int i)
{
    tab_save();
    for (int k = i; k < ntabs - 1; k++)
        tabs[k] = tabs[k + 1];
    if (ntabs > 1)
        ntabs--;
    else
        tabs[0].uri[0] = tabs[0].title[0] = 0;
    if (cur_tab >= ntabs || cur_tab > i)
        cur_tab = cur_tab > i ? cur_tab - 1 : ntabs - 1;
    if (i == cur_tab || ntabs == 1) {
        zoom_mul = 1.0;
        say("load %s", tabs[cur_tab].uri[0] ? tabs[cur_tab].uri : HOME_URL);
    }
}

static double base_zoom(void)
{
    return (desktop_site ? 1.0 : 2.0) * render_pct / 100;
}

static void set_desktop(int on)
{
    desktop_site = on;
    zoom_mul = 1.0;
    say("desktop %d %.3f", on, base_zoom());
}

static void on_header(int id)
{
    if (id != HB_ADDR)
        editing = 0;
    if (id != HB_TABS)
        sheet = 0;
    switch (id) {
    case HB_TABS:
        sheet = !sheet;
        if (sheet)
            tab_save();
        break;
    case HB_BACK: if (can_back) say("back"); break;
    case HB_FWD: if (can_fwd) say("forward"); break;
    case HB_RELOAD: say(loading ? "stop" : "reload"); break;
    case HB_HOME: say("load %s", HOME_URL); break;
    case HB_ADDR:
        editing = 1;
        edit_fresh = 1;                 /* what is typed replaces the address */
        snprintf(url_edit, sizeof(url_edit), "%s", uri);
        url_len = (int)strlen(url_edit);
        page_kb = 0;
        break;
    }
}

/* Returns 0 when the move only scrolled the page (nothing of ours to redraw). */
static int on_sheet_pointer(const struct k86_event *ev)
{
    if (!(ev->changed & 1) || (ev->buttons & 1))
        return 1;                       /* acts when the finger lifts */
    int x = ev->x, y = ev->y;
#define IN(r) (x >= (r).x && y >= (r).y && x < (r).x + (r).w && y < (r).y + (r).h)
    for (int i = 0; i < ntabs; i++) {
        if (IN(sheet_close[i])) { tab_close(i); return 1; }
        if (IN(sheet_card[i])) { tab_open(i); return 1; }
    }
    if (IN(sheet_new)) { tab_new(); return 1; }
    if (IN(sheet_desk)) { set_desktop(!desktop_site); sheet = 0; return 1; }
#undef IN
    sheet = 0;                          /* a tap outside closes it */
    return 1;
}

static double fsqrt(double v)           /* (the apps have no maths library) */
{
    double r = v > 1 ? v / 2 : 1;
    for (int i = 0; i < 24; i++)
        r = (r + v / r) / 2;
    return r;
}

static void pinch_update(const struct k86_event *ev)
{
    struct k86_touch2 t;
    if (!k86_touch2(&t) || t.n < 2) {
        if (pinching) {                 /* the fingers lifted: the engine draws the page at the new zoom */
            pinching = 0;
            dragged = 1;
            applied_mul = zoom_mul;
            say("zoom %.3f", base_zoom() * zoom_mul);
        }
        return;
    }
    double dx = t.x1 - t.x2, dy = t.y1 - t.y2, d = dx * dx + dy * dy;
    d = d > 0 ? fsqrt(d) : 1;
    pinch_cx = (t.x1 + t.x2) / 2 + (ev->x - t.x1);
    pinch_cy = (t.y1 + t.y2) / 2 + (ev->y - t.y1) - BAR;
    if (!pinching) {
        pinching = 1;
        dragged = 1;                    /* not a tap, not a scroll */
        pend_dy = 0;
        pinch_d0 = d;
        pinch_m0 = zoom_mul;
        applied_mul = zoom_mul;
        return;
    }
    double m = pinch_m0 * d / pinch_d0;
    zoom_mul = m < 0.5 ? 0.5 : m > 2.5 ? 2.5 : m;
}

static int on_pointer(const struct k86_event *ev)
{
    if (sheet)
        return on_sheet_pointer(ev);
    int down = ev->buttons & 1, changed = ev->changed & 1;
    int x = ev->x, y = ev->y;
    pinch_update(ev);
    if (pinching) {                     /* redraw the scaled picture, at most about 25 times a second */
        uint32_t now = k86_uptime_ms();
        if (now - pinch_draw_ms < 40)
            return 0;
        pinch_draw_ms = now;
        return 1;
    }
    if (down && changed) {              /* finger down */
        ptr_down = 1; ptr_in_view = 1; dragged = 0;
        sx = x; sy = last_y = y;
        if (editing) editing = 0;
    } else if (down && ptr_down && ptr_in_view) {   /* moving */
        if (!dragged && (abs(y - sy) > DRAG_SLOP || abs(x - sx) > DRAG_SLOP))
            dragged = 1;
        if (dragged && y != last_y) {
            pend_dy += last_y - y;
            pend_x = x;
            pend_y = y - BAR;
            last_y = y;
            flush_scroll(0);
        }
        return 0;
    } else if (!down && changed && ptr_down) {      /* finger up */
        flush_scroll(1);
        if (ptr_in_view && !dragged && sock >= 0) say("click %d %d", x, y - BAR);
        ptr_down = 0;
    }
    return 1;
}

/* ---- main ---------------------------------------------------------------------------------- */

static void set_timer(void)
{
    uint32_t want = (loading || engine == ENGINE_STARTING || k86_uptime_ms() - last_change_ms < 1500) ? 33 : 250;
    if (want != timer_ms) {
        timer_ms = want;
        k86_timer_set(want);
    }
}

static void tick(void)
{
    int changed = 0, frame_new = 0;
    flush_scroll(0);
    if (engine == ENGINE_STARTING) {
        if (sock < 0 && try_connect() == 0)
            changed = 1;
        if (sock < 0 && k86_uptime_ms() - started_ms > 90000) {
            engine = ENGINE_FAILED;
            snprintf(errtext, sizeof(errtext), "it didn't start (see /tmp/lkx-web.log)");
            changed = 1;
        }
    }
    if (pump()) changed = 1;
    if (frame && frame_len) {
        uint32_t seq = ((const volatile struct frame_header *)frame)->seq;
        if (seq != seen_seq) {
            seen_seq = seq;
            got_frame = 1;
            frame_new = 1;
        }
    }
    if (changed || frame_new) {
        last_change_ms = k86_uptime_ms();
        if (changed)
            redraw();
        else
            redraw_view();
    }
}

int main(void)
{
    if (k86_window_open(&win))
        return 1;
    canvas = (struct gfx_surface){ win.canvas, win.w, win.h, win.stride };
    ref_h();
    engine_start();
    last_change_ms = k86_uptime_ms();
    redraw();
    k86_log("browser: open");
    for (;;) {
        struct k86_event ev;
        k86_keyboard_wanted(editing || page_kb);
        k86_back_wanted(editing || page_kb || can_back);
        set_timer();
        if (k86_wait_event(&ev))
            break;
        switch (ev.type) {
        case K86_EVENT_POINTER:
            if (on_pointer(&ev))
                redraw();
            last_change_ms = k86_uptime_ms();
            break;
        case K86_EVENT_KEY:
            on_key(&ev.key);
            last_change_ms = k86_uptime_ms();
            redraw();
            break;
        case K86_EVENT_HEADER:
            on_header(ev.id);
            last_change_ms = k86_uptime_ms();
            redraw();
            break;
        case K86_EVENT_THEME:
            redraw();
            break;
        case K86_EVENT_RESIZE:
            canvas.w = ev.x;
            canvas.h = ev.y;
            redraw();
            break;
        case K86_EVENT_TIMER:
            tick();
            break;
        case K86_EVENT_CLOSE:
            engine_stop();
            return 0;
        default:
            break;
        }
        tick();
    }
    engine_stop();
    return 0;
}
