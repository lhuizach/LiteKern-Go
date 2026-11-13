/* LiteKern Go, web: lkx-webd, the browser engine's host. Runs inside the Alpine sandbox (musl) and is
 * built there (phone/sandbox/build-webd.sh). It loads pages with WPE WebKit and hands the picture to the
 * LiteKern Browser app (apps/browser) over a shared file; the app sends what the user does over a socket.
 *
 *   lkx-webd --serve [--size WxH] [URL]          the real thing, run by the Browser app
 *   lkx-webd [--size WxH] [--wait S] --shot F.ppm URL   one picture of a page (shot.sh)
 *
 * Frames: WPEBackend-fdo's shm export (software drawing, no GPU). Each finished frame is copied into
 * FRAME_FILE (a header, then BGRA rows; "seq" goes up when a frame is complete) and released.
 * Commands, one line each, on SOCK_FILE (from the app):
 *   load URL | back | forward | reload | stop | quit
 *   click X Y            a tap at X,Y of the picture
 *   scroll X Y DY        move the page: DY > 0 shows lower parts of it (the finger moved up)
 *   key KEYSYM 1|0       a key down or up (X11 keysyms: 0xff0d Enter, 0xff08 Backspace, or the character)
 * Back to the app, one line each: ready W H | title T | uri U | loading 1|0 | progress N | nav BACK FWD */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server.h>
#include <wpe/webkit.h>
#include <wpe/fdo.h>
#include <wpe/unstable/fdo-shm.h>

#define FRAME_FILE "/dev/shm/lkxweb.frame"
#define SOCK_FILE "/dev/shm/lkxweb.sock"
#define FRAME_MAGIC 0x4657574cu             /* "LWWF" */
#define HEADER_BYTES 64

struct frame_header {                       /* at the start of FRAME_FILE */
    uint32_t magic, seq, w, h, stride, reserved[3];
};

static struct wpe_view_backend_exportable_fdo *exportable;
static struct wpe_view_backend *vb;
static WebKitWebView *view;
static int W = 720, H = 1032;
static unsigned char *shm;                  /* the mapped FRAME_FILE */
static struct frame_header *fh;
static unsigned char *last;                 /* --shot mode: newest frame */
static unsigned frames;
static const char *shot;
static GMainLoop *loop;
static int client = -1;
static char inbuf[4096];
static size_t inlen;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char line[1200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (client < 0 || n <= 0) return;
    if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
    for (int i = 0; i < n; i++) if (line[i] == '\n') line[i] = ' ';
    line[n++] = '\n';
    if (write(client, line, (size_t)n) < 0 && errno != EAGAIN) { close(client); client = -1; }
}

static void on_shm(void *data, struct wpe_fdo_shm_exported_buffer *b)
{
    (void)data;
    struct wl_shm_buffer *sb = wpe_fdo_shm_exported_buffer_get_shm_buffer(b);
    int w = wl_shm_buffer_get_width(sb), h = wl_shm_buffer_get_height(sb), st = wl_shm_buffer_get_stride(sb);
    wl_shm_buffer_begin_access(sb);
    const unsigned char *p = wl_shm_buffer_get_data(sb);
    unsigned char *dst = shm ? shm + HEADER_BYTES : last;
    if (!dst) dst = last = calloc((size_t)W * H, 4);
    for (int y = 0; y < h && y < H; y++)
        memcpy(dst + (size_t)y * W * 4, p + (size_t)y * st, (size_t)(w < W ? w : W) * 4);
    wl_shm_buffer_end_access(sb);
    if (fh) { __sync_synchronize(); fh->seq++; }
    frames++;
    wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(exportable, b);
    wpe_view_backend_exportable_fdo_dispatch_frame_complete(exportable);
}

static void pointer(int type, int x, int y, uint32_t button, uint32_t state)
{
    struct wpe_input_pointer_event e = { .type = type, .time = (uint32_t)(g_get_monotonic_time() / 1000), .x = x, .y = y,
                                         .button = button, .state = state, .modifiers = 0 };
    wpe_view_backend_dispatch_pointer_event(vb, &e);
}

static void key(uint32_t sym, int down)
{
    struct wpe_input_keyboard_event e = { .time = (uint32_t)(g_get_monotonic_time() / 1000), .key_code = sym,
                                          .hardware_key_code = 0, .pressed = down != 0, .modifiers = 0 };
    wpe_view_backend_dispatch_keyboard_event(vb, &e);
}

static void command(char *line)
{
    int x, y, dy;
    unsigned sym, down;
    if (!strncmp(line, "load ", 5)) webkit_web_view_load_uri(view, line + 5);
    else if (!strcmp(line, "back")) webkit_web_view_go_back(view);
    else if (!strcmp(line, "forward")) webkit_web_view_go_forward(view);
    else if (!strcmp(line, "reload")) webkit_web_view_reload(view);
    else if (!strcmp(line, "stop")) webkit_web_view_stop_loading(view);
    else if (!strcmp(line, "quit")) g_main_loop_quit(loop);
    else if (sscanf(line, "click %d %d", &x, &y) == 2) {
        pointer(wpe_input_pointer_event_type_motion, x, y, 0, 0);
        pointer(wpe_input_pointer_event_type_button, x, y, 1, 1);
        pointer(wpe_input_pointer_event_type_button, x, y, 1, 0);
    } else if (sscanf(line, "scroll %d %d %d", &x, &y, &dy) == 3) {
        /* WPE's wheel events move the page a fixed 40 px a notch whatever the value, too coarse for a finger
         * following the page, so a small script moves the nearest scrollable thing under the finger (or the page) */
        char js[600];
        snprintf(js, sizeof(js),
                 "(function(x,y,dy){var e=document.elementFromPoint(x,y);"
                 "while(e&&e!==document.body&&e!==document.documentElement){var s=getComputedStyle(e);"
                 "if(/(auto|scroll)/.test(s.overflowY)&&e.scrollHeight>e.clientHeight){var b=e.scrollTop;e.scrollTop+=dy;if(e.scrollTop!==b)return;}"
                 "e=e.parentElement;}window.scrollBy(0,dy);})(%d,%d,%d)", x, y, dy);
        webkit_web_view_evaluate_javascript(view, js, -1, NULL, NULL, NULL, NULL, NULL);
    } else if (sscanf(line, "key %i %u", &sym, &down) == 2) key(sym, (int)down);
}

static gboolean on_client(gint fd, GIOCondition cond, gpointer d)
{
    (void)cond; (void)d;
    ssize_t n = read(fd, inbuf + inlen, sizeof(inbuf) - 1 - inlen);
    if (n <= 0) {
        if (n < 0 && errno == EAGAIN) return G_SOURCE_CONTINUE;
        close(fd);
        if (client == fd) client = -1;
        inlen = 0;
        return G_SOURCE_REMOVE;
    }
    inlen += (size_t)n;
    inbuf[inlen] = 0;
    char *start = inbuf, *nl;
    while ((nl = strchr(start, '\n'))) { *nl = 0; command(start); start = nl + 1; }
    inlen = (size_t)(inbuf + inlen - start);
    memmove(inbuf, start, inlen);
    if (inlen >= sizeof(inbuf) - 1) inlen = 0;
    return G_SOURCE_CONTINUE;
}

static gboolean on_listener(gint fd, GIOCondition cond, gpointer d)
{
    (void)cond; (void)d;
    int c = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (c < 0) return G_SOURCE_CONTINUE;
    if (client >= 0) close(client);
    client = c;
    inlen = 0;
    g_unix_fd_add(c, G_IO_IN | G_IO_HUP, on_client, NULL);
    say("ready %d %d", W, H);
    const char *u = webkit_web_view_get_uri(view);
    if (u) say("uri %s", u);
    const char *t = webkit_web_view_get_title(view);
    if (t) say("title %s", t);
    return G_SOURCE_CONTINUE;
}

static void nav_state(void)
{
    say("nav %d %d", webkit_web_view_can_go_back(view), webkit_web_view_can_go_forward(view));
}

static void on_load(WebKitWebView *v, WebKitLoadEvent e, gpointer d)
{
    (void)d;
    if (e == WEBKIT_LOAD_STARTED) say("loading 1");
    if (e == WEBKIT_LOAD_FINISHED) {
        say("loading 0");
        fprintf(stderr, "lkx-webd: loaded %s\n", webkit_web_view_get_uri(v));
    }
    if (e == WEBKIT_LOAD_COMMITTED) say("uri %s", webkit_web_view_get_uri(v));
    nav_state();
}

static void on_title(GObject *o, GParamSpec *p, gpointer d)
{
    (void)o; (void)p; (void)d;
    const char *t = webkit_web_view_get_title(view);
    say("title %s", t ? t : "");
}

static void on_progress(GObject *o, GParamSpec *p, gpointer d)
{
    (void)o; (void)p; (void)d;
    say("progress %d", (int)(webkit_web_view_get_estimated_load_progress(view) * 100));
}

static gboolean on_load_failed(WebKitWebView *v, WebKitLoadEvent e, gchar *uri, GError *err, gpointer d)
{
    (void)v; (void)e; (void)d;
    fprintf(stderr, "lkx-webd: failed %s: %s\n", uri, err ? err->message : "?");
    say("error %s", err ? err->message : "load failed");
    return FALSE;
}

static gboolean finish(gpointer d)
{
    (void)d;
    if (shot && last) {
        FILE *f = fopen(shot, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", W, H);
            for (int i = 0; i < W * H; i++) { fputc(last[i * 4 + 2], f); fputc(last[i * 4 + 1], f); fputc(last[i * 4], f); }
            fclose(f);
        }
    }
    fprintf(stderr, "lkx-webd: %u frames, last %s\n", frames, last ? "yes" : "none");
    g_main_loop_quit(loop);
    return FALSE;
}

static int listen_socket(void)
{
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strncpy(a.sun_path, SOCK_FILE, sizeof(a.sun_path) - 1);
    unlink(SOCK_FILE);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 1)) { perror("lkx-webd: socket"); return -1; }
    return s;
}

static int map_frame(void)
{
    size_t size = HEADER_BYTES + (size_t)W * H * 4;
    int fd = open(FRAME_FILE, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0 || ftruncate(fd, (off_t)size)) { perror("lkx-webd: frame file"); return -1; }
    shm = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shm == MAP_FAILED) { shm = NULL; perror("lkx-webd: mmap"); return -1; }
    fh = (struct frame_header *)shm;
    memset(shm, 255, size);
    *fh = (struct frame_header){ FRAME_MAGIC, 0, (uint32_t)W, (uint32_t)H, (uint32_t)W * 4, { 0, 0, 0 } };
    return 0;
}

int main(int argc, char **argv)
{
    int wait_s = 8, serve = 0;
    const char *url = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--wait") && i + 1 < argc) wait_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--serve")) serve = 1;
        else url = argv[i];
    }
    if (!serve && !url) { fprintf(stderr, "usage: lkx-webd --serve [--size WxH] [URL] | [--wait S] --shot F.ppm URL\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);

    wpe_loader_init("libWPEBackend-fdo-1.0.so.1");
    if (!wpe_fdo_initialize_shm()) { fprintf(stderr, "lkx-webd: wpe_fdo_initialize_shm failed\n"); return 1; }
    static const struct wpe_view_backend_exportable_fdo_client fclient = { .export_shm_buffer = on_shm };
    exportable = wpe_view_backend_exportable_fdo_create(&fclient, NULL, W, H);
    vb = wpe_view_backend_exportable_fdo_get_view_backend(exportable);
    WebKitWebViewBackend *wb = webkit_web_view_backend_new(vb, NULL, NULL);
    view = WEBKIT_WEB_VIEW(webkit_web_view_new(wb));
    wpe_view_backend_add_activity_state(vb, wpe_view_activity_state_visible | wpe_view_activity_state_focused | wpe_view_activity_state_in_window);
    g_signal_connect(view, "load-changed", G_CALLBACK(on_load), NULL);
    g_signal_connect(view, "load-failed", G_CALLBACK(on_load_failed), NULL);
    g_signal_connect(view, "notify::title", G_CALLBACK(on_title), NULL);
    g_signal_connect(view, "notify::estimated-load-progress", G_CALLBACK(on_progress), NULL);
    loop = g_main_loop_new(NULL, FALSE);
    if (serve) {
        if (map_frame()) return 1;
        int s = listen_socket();
        if (s < 0) return 1;
        g_unix_fd_add(s, G_IO_IN, on_listener, NULL);
        fprintf(stderr, "lkx-webd: serving %dx%d on %s\n", W, H, SOCK_FILE);
    } else {
        g_timeout_add_seconds((guint)wait_s, finish, NULL);
    }
    if (url) webkit_web_view_load_uri(view, url);
    g_main_loop_run(loop);
    unlink(SOCK_FILE);
    _exit(0);                               /* the web processes go with us: nothing else holds the caller's pipes */
}
