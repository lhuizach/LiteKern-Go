/* LiteKern Go, web: lkx-webd, the browser engine's host. Runs inside the Alpine sandbox (musl) and
 * is built there (phone/sandbox/build-webd.sh). It loads pages with WPE WebKit and hands the picture
 * to the LiteKern app over a shared file; v0 here: load a page, write what it drew to a .ppm.
 *
 *   lkx-webd [--size WxH] [--wait SECONDS] [--shot FILE.ppm] URL
 *
 * Pixels: WPEBackend-fdo's shm export (software drawing, no GPU). Each finished frame arrives in
 * export_shm_buffer(); it is copied out and released, and the web process told the frame is done. */
#define _GNU_SOURCE
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server.h>
#include <wpe/webkit.h>
#include <wpe/fdo.h>
#include <wpe/unstable/fdo-shm.h>

static struct wpe_view_backend_exportable_fdo *exportable;
static int W = 720, H = 1224;
static unsigned char *last;           /* the newest frame, BGRA, W*H*4 */
static unsigned frames;
static const char *shot;
static GMainLoop *loop;

static void on_shm(void *data, struct wpe_fdo_shm_exported_buffer *b)
{
    (void)data;
    struct wl_shm_buffer *sb = wpe_fdo_shm_exported_buffer_get_shm_buffer(b);
    int w = wl_shm_buffer_get_width(sb), h = wl_shm_buffer_get_height(sb), st = wl_shm_buffer_get_stride(sb);
    wl_shm_buffer_begin_access(sb);
    const unsigned char *p = wl_shm_buffer_get_data(sb);
    if (!last) last = calloc((size_t)W * H, 4);
    for (int y = 0; y < h && y < H; y++) memcpy(last + (size_t)y * W * 4, p + (size_t)y * st, (size_t)(w < W ? w : W) * 4);
    wl_shm_buffer_end_access(sb);
    frames++;
    wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(exportable, b);
    wpe_view_backend_exportable_fdo_dispatch_frame_complete(exportable);
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

static void on_load(WebKitWebView *v, WebKitLoadEvent e, gpointer d)
{
    (void)d;
    if (e == WEBKIT_LOAD_FINISHED) fprintf(stderr, "lkx-webd: loaded %s (%s)\n", webkit_web_view_get_uri(v), webkit_web_view_get_title(v) ? webkit_web_view_get_title(v) : "");
}

int main(int argc, char **argv)
{
    int wait_s = 8;
    const char *url = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--wait") && i + 1 < argc) wait_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else url = argv[i];
    }
    if (!url) { fprintf(stderr, "usage: lkx-webd [--size WxH] [--wait S] [--shot F.ppm] URL\n"); return 2; }

    wpe_loader_init("libWPEBackend-fdo-1.0.so.1");
    if (!wpe_fdo_initialize_shm()) { fprintf(stderr, "lkx-webd: wpe_fdo_initialize_shm failed\n"); return 1; }
    static const struct wpe_view_backend_exportable_fdo_client client = { .export_shm_buffer = on_shm };
    exportable = wpe_view_backend_exportable_fdo_create(&client, NULL, W, H);
    struct wpe_view_backend *vb = wpe_view_backend_exportable_fdo_get_view_backend(exportable);
    WebKitWebViewBackend *wb = webkit_web_view_backend_new(vb, NULL, NULL);
    WebKitWebView *view = WEBKIT_WEB_VIEW(webkit_web_view_new(wb));
    wpe_view_backend_add_activity_state(vb, wpe_view_activity_state_visible | wpe_view_activity_state_focused | wpe_view_activity_state_in_window);
    g_signal_connect(view, "load-changed", G_CALLBACK(on_load), NULL);
    webkit_web_view_load_uri(view, url);
    loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add_seconds(wait_s, finish, NULL);
    g_main_loop_run(loop);
    return 0;
}
