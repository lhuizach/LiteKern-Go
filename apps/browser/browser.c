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

#define ENTER "/extras/enter.sh"
#define WEBD "/extras/alpine/usr/local/bin/lkx-webd"
#define FRAME_FILE "/dev/shm/lkxweb.frame"
#define SOCK_FILE "/dev/shm/lkxweb.sock"
#define HOME_URL "https://duckduckgo.com/"
#define BAR 96                          /* the address bar on top and the buttons at the bottom */
#define HEADER_BYTES 64
#define FRAME_MAGIC 0x4657574cu
#define DRAG_SLOP 14
#define URL_MAX 512
#define NBTN 4

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
static char url_edit[URL_MAX];
static int url_len;

static int ptr_down, ptr_in_view, dragged, sx, sy, last_y;
static int btn_down = -1;
static uint32_t last_change_ms;
static uint32_t timer_ms;

static const char *const btn_label[NBTN] = { "Back", "Next", "Reload", "Home" };

static int view_h(void) { return canvas.h - 2 * BAR; }

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

static void engine_spawn(void)
{
    char size[24];
    snprintf(size, sizeof(size), "%dx%d", canvas.w, view_h());
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
              "LIBGL_ALWAYS_SOFTWARE=1", "/usr/local/bin/lkx-webd", "--serve", "--size", size, HOME_URL, (char *)NULL);
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
        int n = snprintf(url, sizeof(url), "https://duckduckgo.com/?q=");
        for (const char *c = text; *c && n < (int)sizeof(url) - 4; c++) {
            if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')) url[n++] = *c;
            else n += snprintf(url + n, 4, "%%%02X", (unsigned char)*c);
        }
        url[n] = 0;
    }
    say("load %s", url);
}

/* ---- drawing ------------------------------------------------------------------------------- */

static void draw_bars(void)
{
    const struct theme *t = theme_get();
    gfx_fill_rect(&canvas, 0, 0, canvas.w, BAR, t->headerbar_bg);
    gfx_fill_rect(&canvas, 0, BAR - 1, canvas.w, 1, t->headerbar_border);
    gfx_fill_rect(&canvas, 16, 16, canvas.w - 32, BAR - 32, editing ? t->view_bg : t->button_bg);
    gfx_round_corners(&canvas, 16, 16, canvas.w - 32, BAR - 32, 14, t->headerbar_bg);
    const char *shown = editing ? url_edit : (uri[0] ? uri : (title[0] ? title : "Search or type an address"));
    uint32_t col = (editing || uri[0]) ? t->fg : t->fg_dim;
    int th = text_height(TEXT_BODY);
    text_draw_fit(&canvas, 34, 16 + (BAR - 32 - th) / 2, shown, canvas.w - 68 - (editing ? 14 : 0), TEXT_BODY, col);
    if (editing) {
        int cx = 34 + text_width(url_edit, TEXT_BODY);
        if (cx > canvas.w - 40) cx = canvas.w - 40;
        gfx_fill_rect(&canvas, cx + 2, 16 + (BAR - 32 - th) / 2, 2, th, t->accent);
    }
    if (loading)
        gfx_fill_rect(&canvas, 0, BAR - 4, canvas.w * progress / 100, 4, t->accent);

    int y = canvas.h - BAR;
    gfx_fill_rect(&canvas, 0, y, canvas.w, BAR, t->headerbar_bg);
    gfx_fill_rect(&canvas, 0, y, canvas.w, 1, t->headerbar_border);
    int bw = canvas.w / NBTN;
    for (int i = 0; i < NBTN; i++) {
        const char *label = btn_label[i];
        int off = (i == 0 && !can_back) || (i == 1 && !can_fwd);
        if (i == 2 && loading) label = "Stop";
        if (btn_down == i)
            gfx_fill_rect(&canvas, i * bw + 6, y + 8, bw - 12, BAR - 16, t->button_active);
        wg_label_centred(&canvas, i * bw + bw / 2, y + (BAR - wg_label_height(WG_TEXT_BODY)) / 2, label, WG_TEXT_BODY);
        if (off)                         /* a greyed label: draw it over again dimmer */
            gfx_fill_rect(&canvas, i * bw + 8, y + 10, bw - 16, BAR - 20, t->headerbar_bg),
            wg_label_centred(&canvas, i * bw + bw / 2, y + (BAR - wg_label_height(WG_TEXT_BODY)) / 2, label, WG_TEXT_SMALL);
    }
}

static void draw_message(const char *a, const char *b)
{
    const struct theme *t = theme_get();
    gfx_fill_rect(&canvas, 0, BAR, canvas.w, view_h(), t->window_bg);
    wg_label_centred(&canvas, canvas.w / 2, BAR + view_h() / 2 - 40, a, WG_TEXT_HEADING);
    if (b)
        wg_label_centred(&canvas, canvas.w / 2, BAR + view_h() / 2 + 20, b, WG_TEXT_BODY);
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
    const uint32_t *src = (const uint32_t *)(frame + HEADER_BYTES);
    int w = (int)frame_w < canvas.w ? (int)frame_w : canvas.w;
    int h = (int)frame_h < view_h() ? (int)frame_h : view_h();
    for (int y = 0; y < h; y++)
        memcpy(canvas.px + (size_t)(BAR + y) * canvas.stride, src + (size_t)y * frame_w, (size_t)w * 4);
}

static void redraw(void)
{
    draw_view();
    draw_bars();
    k86_present(0, 0, canvas.w, canvas.h);
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
            if (url_len) url_edit[--url_len] = 0;
        } else if (k->ascii >= 32 && k->ascii < 127 && url_len < URL_MAX - 2) {
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

static void press_button(int i)
{
    if (i == 0 && can_back) say("back");
    else if (i == 1 && can_fwd) say("forward");
    else if (i == 2) say(loading ? "stop" : "reload");
    else if (i == 3) say("load %s", HOME_URL);
}

static void on_pointer(const struct k86_event *ev)
{
    int down = ev->buttons & 1, changed = ev->changed & 1;
    int x = ev->x, y = ev->y;
    if (down && changed) {              /* finger down */
        ptr_down = 1; ptr_in_view = 0; dragged = 0; btn_down = -1;
        sx = x; sy = last_y = y;
        if (y < BAR) {
            editing = 1;
            snprintf(url_edit, sizeof(url_edit), "%s", uri);
            url_len = (int)strlen(url_edit);
            page_kb = 0;
        } else if (y >= canvas.h - BAR) {
            btn_down = x * NBTN / canvas.w;
        } else {
            ptr_in_view = 1;
            if (editing) editing = 0;
        }
    } else if (down && ptr_down && ptr_in_view) {   /* moving */
        if (!dragged && (abs(y - sy) > DRAG_SLOP || abs(x - sx) > DRAG_SLOP))
            dragged = 1;
        if (dragged && y != last_y) {
            say("scroll %d %d %d", x, y - BAR, last_y - y);
            last_y = y;
        }
    } else if (!down && changed && ptr_down) {      /* finger up */
        if (ptr_in_view && !dragged && sock >= 0) say("click %d %d", x, y - BAR);
        if (btn_down >= 0 && y >= canvas.h - BAR && x * NBTN / canvas.w == btn_down)
            press_button(btn_down);
        ptr_down = 0; btn_down = -1;
    }
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
    int changed = 0;
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
            changed = 1;
        }
    }
    if (changed) {
        last_change_ms = k86_uptime_ms();
        redraw();
    }
}

int main(void)
{
    if (k86_window_open(&win))
        return 1;
    canvas = (struct gfx_surface){ win.canvas, win.w, win.h, win.stride };
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
            on_pointer(&ev);
            last_change_ms = k86_uptime_ms();
            redraw();
            break;
        case K86_EVENT_KEY:
            on_key(&ev.key);
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
