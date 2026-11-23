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
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <wayland-server.h>
#include <wpe/webkit.h>
#include <wpe/fdo.h>
#include <wpe/fdo-egl.h>
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
static double vscale = 1.0;                 /* picture pixels per app pixel: the app shows a smaller picture enlarged */
static double zoom = 1.0;                  /* the page's zoom: 2 = a 360 px wide page on the 720 px picture */
static unsigned char *shm;                  /* the mapped FRAME_FILE */
static struct frame_header *fh;
static unsigned char *last;                 /* --shot mode: newest frame */
static unsigned frames;
static const char *shot;
static GMainLoop *loop;
static gint64 last_scroll_us;               /* when the last scroll command came (see command()) */
static char *start_url;                     /* loaded once the ad filter is in (or has failed) */
static int started;
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

/* ---- the GPU path ---------------------------------------------------------------------------------
 * With the software path above, WebKit composites with llvmpipe on the CPU (one thread at 99%). Here this host
 * makes an EGL display on the phone's GPU (the Adreno 306, through Mesa's freedreno, on the render node with
 * GBM) and gives it to WPEBackend-fdo: WebKit then draws and composites on the GPU and hands over each frame
 * as an EGLImage (a dmabuf). We draw it once into a small framebuffer, swapping red and blue, and read the
 * pixels back into FRAME_FILE, so the app needs no change. If anything fails we use the software path. */
static int use_gpu;
static EGLDisplay edpy = EGL_NO_DISPLAY;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;
static GLuint g_prog, g_tex, g_fbo, g_out;       /* program, the frame's texture, framebuffer, its picture */
static int g_w, g_h;
static unsigned char *g_tmp;

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[300]; glGetShaderInfoLog(s, sizeof(log), NULL, log); fprintf(stderr, "lkx-webd: shader: %s\n", log); return 0; }
    return s;
}

static int gpu_init(void)
{
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("lkx-webd: gpu: renderD128"); return -1; }
    struct gbm_device *gbm = gbm_create_device(fd);
    if (!gbm) { fprintf(stderr, "lkx-webd: gpu: no gbm device\n"); return -1; }
    edpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    EGLint maj, min;
    if (edpy == EGL_NO_DISPLAY || !eglInitialize(edpy, &maj, &min)) { fprintf(stderr, "lkx-webd: gpu: eglInitialize failed\n"); return -1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(edpy, ca, &cfg, 1, &n) || n < 1) { fprintf(stderr, "lkx-webd: gpu: no EGL config\n"); return -1; }
    static const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(edpy, cfg, EGL_NO_CONTEXT, xa);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "lkx-webd: gpu: no GL context (0x%x)\n", eglGetError());
        return -1;
    }
    image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!image_target) { fprintf(stderr, "lkx-webd: gpu: no glEGLImageTargetTexture2DOES\n"); return -1; }
    GLuint vs = compile(GL_VERTEX_SHADER, "attribute vec2 p; attribute vec2 t; varying vec2 uv;"
                                          "void main(){ uv = t; gl_Position = vec4(p, 0.0, 1.0); }");
    GLuint fs = compile(GL_FRAGMENT_SHADER, "precision mediump float; varying vec2 uv; uniform sampler2D s;"
                                            "void main(){ gl_FragColor = texture2D(s, uv).bgra; }");
    if (!vs || !fs) return -1;
    g_prog = glCreateProgram();
    glAttachShader(g_prog, vs);
    glAttachShader(g_prog, fs);
    glBindAttribLocation(g_prog, 0, "p");
    glBindAttribLocation(g_prog, 1, "t");
    glLinkProgram(g_prog);
    GLint ok = 0;
    glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "lkx-webd: gpu: link failed\n"); return -1; }
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &g_fbo);
    glGenTextures(1, &g_out);
    fprintf(stderr, "lkx-webd: gpu: %s, %s\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    return 0;
}

/* an exported image to pixels in the frame buffer (top row first, BGRA) */
static void gpu_readback(EGLImageKHR img, int iw, int ih)
{
    if (iw != g_w || ih != g_h) {                   /* (re)make the framebuffer's picture for this size */
        glBindTexture(GL_TEXTURE_2D, g_out);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, iw, ih, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_out, 0);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) fprintf(stderr, "lkx-webd: gpu: framebuffer incomplete (0x%x)\n", st);
        g_w = iw;
        g_h = ih;
        free(g_tmp);
        g_tmp = NULL;
    }
    glBindTexture(GL_TEXTURE_2D, g_tex);
    image_target(GL_TEXTURE_2D, img);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, iw, ih);
    glUseProgram(g_prog);
    static const GLfloat pos[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    static const GLfloat tex[] = { 0, 0, 1, 0, 0, 1, 1, 1 };   /* the image's first row lands in the framebuffer's first row */
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, tex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    unsigned char *dst = shm ? shm + HEADER_BYTES : last;
    if (!dst) dst = last = calloc((size_t)W * H, 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (iw == W && ih <= H) {
        glReadPixels(0, 0, iw, ih, GL_RGBA, GL_UNSIGNED_BYTE, dst);
    } else {
        if (!g_tmp) g_tmp = malloc((size_t)iw * ih * 4);
        glReadPixels(0, 0, iw, ih, GL_RGBA, GL_UNSIGNED_BYTE, g_tmp);
        for (int y = 0; y < ih && y < H; y++)
            memcpy(dst + (size_t)y * W * 4, g_tmp + (size_t)y * iw * 4, (size_t)(iw < W ? iw : W) * 4);
    }
}

static void on_egl_image(void *data, struct wpe_fdo_egl_exported_image *im)
{
    (void)data;
    gpu_readback(wpe_fdo_egl_exported_image_get_egl_image(im),
                 (int)wpe_fdo_egl_exported_image_get_width(im), (int)wpe_fdo_egl_exported_image_get_height(im));
    if (fh) { __sync_synchronize(); fh->seq++; }
    frames++;
    wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(exportable, im);
    wpe_view_backend_exportable_fdo_dispatch_frame_complete(exportable);
}

static void on_egl_shm(void *data, struct wpe_fdo_shm_exported_buffer *b)   /* WebKit may still send a few in memory */
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
    wpe_view_backend_exportable_fdo_egl_dispatch_release_shm_exported_buffer(exportable, b);
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

/* After a tap: is the thing that has the focus one you type into? Then the app brings up the keyboard. */
static void editable_done(GObject *o, GAsyncResult *res, gpointer d)
{
    (void)d;
    GError *err = NULL;
    JSCValue *v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(o), res, &err);
    int yes = v && jsc_value_is_boolean(v) && jsc_value_to_boolean(v);
    if (v) g_object_unref(v);
    if (err) g_error_free(err);
    say("editable %d", yes);
}

static gboolean check_editable(gpointer d)
{
    (void)d;
    webkit_web_view_evaluate_javascript(view,
        "(function(){var e=document.activeElement;if(!e)return false;var t=(e.tagName||'').toLowerCase();"
        "if(t==='textarea'||e.isContentEditable)return true;"
        "if(t==='input'){var k=(e.type||'text').toLowerCase();return !/^(button|submit|checkbox|radio|range|color|file|image|reset)$/.test(k);}"
        "return false;})()",
        -1, NULL, NULL, NULL, editable_done, NULL);
    return G_SOURCE_REMOVE;
}

static const char *mobile_ua = "Mozilla/5.0 (iPhone; CPU iPhone OS 16_4 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/16.4 Mobile/15E148 Safari/604.1";

static void command(char *line)
{
    int x, y, dy, d;
    double z;
    unsigned sym, down;
    if (!strncmp(line, "load ", 5)) webkit_web_view_load_uri(view, line + 5);
    else if (!strcmp(line, "back")) webkit_web_view_go_back(view);
    else if (!strcmp(line, "forward")) webkit_web_view_go_forward(view);
    else if (!strcmp(line, "reload")) webkit_web_view_reload(view);
    else if (!strcmp(line, "stop")) webkit_web_view_stop_loading(view);
    else if (!strcmp(line, "quit")) g_main_loop_quit(loop);
    else if (sscanf(line, "zoom %lf", &z) == 1) {                    /* a pinch: the page's zoom now */
        if (z < 0.4) z = 0.4;
        if (z > 6) z = 6;
        zoom = z;
        webkit_web_view_set_zoom_level(view, zoom);
    } else if (sscanf(line, "desktop %d %lf", &d, &z) == 2) {        /* the desktop or the phone's site, and its zoom */
        WebKitSettings *st = webkit_web_view_get_settings(view);
        webkit_settings_set_user_agent(st, d ? NULL : mobile_ua);       /* NULL: the engine's own (a desktop browser) */
        zoom = z > 0.4 ? z : 1.0;
        webkit_web_view_set_zoom_level(view, zoom);
        webkit_web_view_reload(view);
    }
    else if (sscanf(line, "click %d %d", &x, &y) == 2) {
        x = (int)(x * vscale); y = (int)(y * vscale);      /* the app's pixels to the picture's */
        pointer(wpe_input_pointer_event_type_motion, x, y, 0, 0);
        pointer(wpe_input_pointer_event_type_button, x, y, 1, 1);
        pointer(wpe_input_pointer_event_type_button, x, y, 1, 0);
        g_timeout_add(200, check_editable, NULL);
    } else if (sscanf(line, "scroll %d %d %d", &x, &y, &dy) == 3) {
        /* WPE's wheel events move the page a fixed 40 px a notch whatever the value, too coarse for a finger
         * following the page, so a small script moves the nearest scrollable thing under the finger (or the page).
         * The thing is looked up only when a drag starts (more than 250 ms since the last scroll) and kept in
         * window.__lkxT after that: walking the page with getComputedStyle on every move was slow on the A53. */
        gint64 now = g_get_monotonic_time();
        int fresh = now - last_scroll_us > 250000;
        last_scroll_us = now;
        char js[900];
        snprintf(js, sizeof(js),
                 "(function(x,y,dy,f){var t=window.__lkxT;"
                 "if(f||t===undefined||(t&&!t.isConnected)){t=null;var e=document.elementFromPoint(x,y);"
                 "while(e&&e!==document.body&&e!==document.documentElement){var s=getComputedStyle(e);"
                 "if(/(auto|scroll)/.test(s.overflowY)&&e.scrollHeight>e.clientHeight){t=e;break;}"
                 "e=e.parentElement;}window.__lkxT=t;}"
                 "if(t){var b=t.scrollTop;t.scrollTop+=dy;if(t.scrollTop!==b)return;}"
                 "window.scrollBy(0,dy);})(%f,%f,%f,%d)", x * vscale / zoom, y * vscale / zoom, dy * vscale / zoom, fresh);   /* the page's own pixels */
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

/* ---- ad and tracker blocking -----------------------------------------------------------------
 * Most of a page's JavaScript on a slow CPU is ads and trackers. WebKit's content filter blocks requests whose
 * address contains one of these names, before they are fetched. Kept short and to well-known ad/tracking hosts. */
static const char *const blocked[] = {
    "doubleclick.net", "googlesyndication.com", "googleadservices.com", "googletagservices.com",
    "googletagmanager.com", "google-analytics.com", "adservice.google.", "2mdn.net", "facebook.net/en_US/fbevents",
    "scorecardresearch.com", "quantserve.com", "taboola.com", "outbrain.com", "criteo.com", "criteo.net",
    "adnxs.com", "amazon-adsystem.com", "hotjar.com", "moatads.com", "adsrvr.org", "rubiconproject.com",
    "pubmatic.com", "openx.net", "casalemedia.com", "mathtag.com", "bluekai.com", "krxd.net", "chartbeat.com",
    "yieldmo.com", "adform.net", "smartadserver.com", "zedo.com", "serving-sys.com",
    "adsafeprotected.com", "imrworldwide.com", "ads-twitter.com", "analytics.twitter.com", "ads.linkedin.com",
    "snap.licdn.com", "bat.bing.com", "clarity.ms", "fullstory.com", "mixpanel.com", "amplitude.com",
    "optimizely.com", "segment.io", "cdn.segment.com", "mc.yandex.ru", "an.yandex.ru", "adroll.com",
    "bidswitch.net", "sharethrough.com", "smaato.net", "teads.tv", "triplelift.com", "id5-sync.com",
    "everesttech.net", "demdex.net", "omtrdc.net", "adobedtm.com", "tiqcdn.com", "cookielaw.org",
    "onetrust.com", "cookiebot.com", "trustarc.com", "quantcount.com", "nr-data.net",
    "sentry.io", "bugsnag.com", "datadoghq.com", "tapad.com", "liadm.com", "ml314.com", "3lift.com",
    "gumgum.com", "indexww.com", "contextweb.com", "lijit.com", "sovrn.com", "conversantmedia.com",
};

static gboolean begin_load(gpointer d)
{
    (void)d;
    if (started) return G_SOURCE_REMOVE;
    started = 1;
    if (start_url) webkit_web_view_load_uri(view, start_url);
    return G_SOURCE_REMOVE;
}

static void filter_saved(GObject *o, GAsyncResult *res, gpointer d)
{
    (void)d;
    GError *err = NULL;
    WebKitUserContentFilter *f = webkit_user_content_filter_store_save_finish(WEBKIT_USER_CONTENT_FILTER_STORE(o), res, &err);
    if (f) {
        webkit_user_content_manager_add_filter(webkit_web_view_get_user_content_manager(view), f);
        webkit_user_content_filter_unref(f);
        fprintf(stderr, "lkx-webd: ad filter on (%d hosts)\n", (int)(sizeof(blocked) / sizeof(blocked[0])));
    } else {
        fprintf(stderr, "lkx-webd: ad filter failed: %s\n", err ? err->message : "?");
    }
    if (err) g_error_free(err);
    begin_load(NULL);
}

static void install_filter(void)
{
    GString *js = g_string_new("[");
    for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
        if (i) g_string_append_c(js, ',');
        g_string_append(js, "{\"trigger\":{\"url-filter\":\"");
        for (const char *c = blocked[i]; *c; c++) {
            if (*c == '.') g_string_append(js, "\\\\.");      /* a dot in the regular expression, in JSON */
            else g_string_append_c(js, *c);
        }
        g_string_append(js, "\"},\"action\":{\"type\":\"block\"}}");
    }
    g_string_append_c(js, ']');
    WebKitUserContentFilterStore *store = webkit_user_content_filter_store_new("/root/.cache/lkx-filters");
    webkit_user_content_filter_store_save(store, "ads", g_string_free_to_bytes(js), NULL, filter_saved, NULL);
    g_timeout_add_seconds(5, begin_load, NULL);               /* never wait longer than this for it */
}

/* ---- --bench: how many frames a second while the page scrolls (10 s of 20 ms steps, no finger needed) ---- */
static int bench, bench_i;
static gint64 bench_t0;
static unsigned bench_f0;

static gboolean bench_step(gpointer d)
{
    (void)d;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "scroll 360 400 %d", (bench_i / 80) % 2 ? -40 : 40);
    command(cmd);
    if (++bench_i < 500) return G_SOURCE_CONTINUE;
    double s = (g_get_monotonic_time() - bench_t0) / 1e6;
    fprintf(stderr, "lkx-webd: bench: %u frames in %.1f s = %.1f fps\n", frames - bench_f0, s, (frames - bench_f0) / s);
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static gboolean bench_start(gpointer d)
{
    (void)d;
    bench_t0 = g_get_monotonic_time();
    bench_f0 = frames;
    g_timeout_add(20, bench_step, NULL);
    return G_SOURCE_REMOVE;
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
    const char *ua = NULL;
    int desktop = 0;
    double zoom_arg = 0;
    int force_shm = 0;
    const char *url = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--wait") && i + 1 < argc) wait_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--serve")) serve = 1;
        else if (!strcmp(argv[i], "--ua") && i + 1 < argc) ua = argv[++i];
        else if (!strcmp(argv[i], "--desktop")) desktop = 1;
        else if (!strcmp(argv[i], "--bench")) bench = 1;
        else if (!strcmp(argv[i], "--shm")) force_shm = 1;          /* the software path, as before */
        else if (!strcmp(argv[i], "--zoom") && i + 1 < argc) zoom_arg = atof(argv[++i]);
        else if (!strcmp(argv[i], "--vscale") && i + 1 < argc) vscale = atof(argv[++i]);
        else url = argv[i];
    }
    if (!serve && !url) { fprintf(stderr, "usage: lkx-webd --serve [--size WxH] [URL] | [--wait S] --shot F.ppm URL\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);

    wpe_loader_init("libWPEBackend-fdo-1.0.so.1");
    if (!force_shm && gpu_init() == 0) {
        if (wpe_fdo_initialize_for_egl_display(edpy)) {
            static const struct wpe_view_backend_exportable_fdo_egl_client eclient = {
                .export_fdo_egl_image = on_egl_image, .export_shm_buffer = on_egl_shm };
            exportable = wpe_view_backend_exportable_fdo_egl_create(&eclient, NULL, (uint32_t)W, (uint32_t)H);
            use_gpu = 1;
        } else {
            fprintf(stderr, "lkx-webd: wpe_fdo_initialize_for_egl_display failed, using software\n");
        }
    }
    if (!use_gpu) {
        if (!wpe_fdo_initialize_shm()) { fprintf(stderr, "lkx-webd: wpe_fdo_initialize_shm failed\n"); return 1; }
        static const struct wpe_view_backend_exportable_fdo_client fclient = { .export_shm_buffer = on_shm };
        exportable = wpe_view_backend_exportable_fdo_create(&fclient, NULL, W, H);
    }
    fprintf(stderr, "lkx-webd: drawing with the %s\n", use_gpu ? "GPU" : "CPU (software GL)");
    vb = wpe_view_backend_exportable_fdo_get_view_backend(exportable);
    WebKitWebViewBackend *wb = webkit_web_view_backend_new(vb, NULL, NULL);
    view = WEBKIT_WEB_VIEW(webkit_web_view_new(wb));
    wpe_view_backend_add_activity_state(vb, wpe_view_activity_state_visible | wpe_view_activity_state_focused | wpe_view_activity_state_in_window);
    /* A phone's way: the sites that have a mobile version get it. The identity string is Safari on an iPhone (the
     * same engine family) and the page is shown at twice the size, so the page is 360 wide, as on a phone, and
     * the text and buttons are big enough to tap. --desktop turns both off. */
    if (!desktop) {
        zoom = 2.0;
        webkit_web_view_set_zoom_level(view, zoom);
        if (!ua) ua = mobile_ua;
    }
    if (zoom_arg > 0) { zoom = zoom_arg; webkit_web_view_set_zoom_level(view, zoom); }
    if (ua) webkit_settings_set_user_agent(webkit_web_view_get_settings(view), ua);
    /* Less work for the A53: no WebGL, no smooth-scroll animation (our drag already moves the page in small
     * steps), and no video starting by itself. */
    WebKitSettings *st = webkit_web_view_get_settings(view);
    webkit_settings_set_enable_webgl(st, FALSE);
    webkit_settings_set_enable_smooth_scrolling(st, FALSE);
    webkit_settings_set_media_playback_requires_user_gesture(st, TRUE);
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
        g_timeout_add_seconds((guint)wait_s, bench ? bench_start : finish, NULL);
    }
    start_url = url ? g_strdup(url) : NULL;
    install_filter();                       /* the first page loads when the filter is in, or after 5 s */
    g_main_loop_run(loop);
    unlink(SOCK_FILE);
    _exit(0);                               /* the web processes go with us: nothing else holds the caller's pipes */
}
