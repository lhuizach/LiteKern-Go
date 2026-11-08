/* LiteKern Go: Camera. A live picture from the rear camera, and a button that
 * saves a 720x1280 photo to Pictures/Camera, where Gallery shows it, and a little
 * picture of the last photo that opens Gallery. Exposure and white balance follow
 * the scene by themselves; focus is found on opening, and with a tap wherever you want
 * it sharp.
 *
 * Logged ("user: camera: ...") for the tests. */
#include "cam.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"
#include "sdk/kern86.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_MS 16
#define EXPOSE_EVERY 6                  /* frames: a change takes a couple to show */
#define BALANCE_EVERY 8
#define BUTTON_R 72
#define BUTTON_BELOW 150                /* its centre, up from the bottom */
#define NOTE_MS 2000

static struct k86_window win;
static struct gfx_surface canvas;
static int have_camera, shoot, busy;
static int red = 310, blue = 640;       /* a guess until the first frame */
static char note[80];
static uint32_t note_until;
static uint32_t frames;
static uint32_t thumb[THUMB * THUMB];
static int have_thumb;
static int focus_pending = 1;
static int focus_x, focus_y;            /* on screen, for the square */

static int thumb_x(void)
{
    return 56;
}

static int thumb_y(void)
{
    return canvas.h - BUTTON_BELOW - THUMB / 2;
}

static void button_draw(int pressed)
{
    int cx = canvas.w / 2, cy = canvas.h - BUTTON_BELOW;
    gfx_fill_circle(&canvas, cx, cy, BUTTON_R, 0xffffff);
    gfx_fill_circle(&canvas, cx, cy, BUTTON_R - 10, 0x202020);
    gfx_fill_circle(&canvas, cx, cy, BUTTON_R - 18, pressed ? 0xc0c0c0 : 0xffffff);
}

static void overlay(void)
{
    button_draw(busy);
    if (have_thumb) {
        struct gfx_surface t = { thumb, THUMB, THUMB, THUMB };
        gfx_fill_rect(&canvas, thumb_x() - 3, thumb_y() - 3, THUMB + 6, THUMB + 6, 0xffffff);
        gfx_blit(&canvas, thumb_x(), thumb_y(), &t, 0, 0, THUMB, THUMB);
    }
    if (focus_active()) {
        int s = 70;
        gfx_rect_outline(&canvas, focus_x - s, focus_y - s, s * 2, s * 2, 0xffffff);
        gfx_rect_outline(&canvas, focus_x - s + 1, focus_y - s + 1, s * 2 - 2, s * 2 - 2, 0xffffff);
    }
    if (note[0] && k86_uptime_ms() < note_until) {
        int w = text_width(note, TEXT_BODY) + 48, x = (canvas.w - w) / 2;
        gfx_fill_round_rect(&canvas, x, 40, w, 64, 32, 0x202020);
        text_draw(&canvas, x + 24, 40 + (64 - text_height(TEXT_BODY)) / 2, note, TEXT_BODY, 0xffffff);
    }
}

static void say(const char *text)
{
    snprintf(note, sizeof(note), "%s", text);
    note_until = k86_uptime_ms() + NOTE_MS;
}

static void draw_message(const char *text)
{
    const struct theme *t = theme_get();
    gfx_fill_rect(&canvas, 0, 0, canvas.w, canvas.h, t->window_bg);
    text_draw(&canvas, (canvas.w - text_width(text, TEXT_BODY)) / 2, 420, text, TEXT_BODY, t->fg_dim);
    k86_present(0, 0, canvas.w, canvas.h);
}

/* Focus on a point of the screen. The preview's a quarter turn from the sensor,
 * 2 screen pixels to a Bayer cell of 2 sensor pixels, so 1 to 1 once turned. */
static void focus_on(int x, int y)
{
    int top_skip = (CAM_W - canvas.h) / 2, left_skip = (CAM_H - canvas.w) / 2;
    focus_x = x;
    focus_y = y;
    focus_at(y + top_skip, CAM_H - 1 - (x + left_skip));
}

static int restreamed;                  /* take() restarted the stream: the frame in hand is gone */

/* The photo: the sensor goes to its photo mode for one frame (cam_still), which is developed
 * and saved; if that doesn't come up, the live frame in hand is saved as it is. */
static void take(const uint8_t *frame)
{
    char name[64];
    int w = 0, h = 0, rb = 0, err;
    busy = 1;
    say("Saving" TEXT_ELLIPSIS);
    overlay();
    k86_present(0, 0, canvas.w, canvas.h);
    uint32_t began = k86_uptime_ms();
    size_t live_len = (size_t)cam_stride() * CAM_H;
    uint8_t *live = malloc(live_len);           /* the live frame goes when the stream restarts */
    if (live)
        memcpy(live, frame, live_len);
    int live_stride = cam_stride();
    uint8_t *still = cam_still(&w, &h, &rb, focus_restore);
    restreamed = 1;
    focus_restore();                            /* the lens lost its power again */
    uint32_t grabbed = k86_uptime_ms();
    if (still)
        err = photo_save(still, w, h, rb, red, blue, name);
    else if (live)
        err = photo_save(live, CAM_W, CAM_H, live_stride, red, blue, name);
    else
        err = -1;
    if (err) {
        k86_log("camera: couldn't save the photo");
        say("Couldn't save the photo");
    } else {
        k86_logf("camera: saved %s (%s) in %u ms, %u ms of it the frame", name, still ? "photo mode" : "live frame",
                 k86_uptime_ms() - began, grabbed - began);
        say("Saved to Gallery");
        have_thumb = photo_thumb(thumb);
    }
    free(still);
    free(live);
    busy = 0;
    shoot = 0;
}

static void timer(void)
{
    int slot, got = -1;
    const uint8_t *frame = 0, *newest;
    while ((newest = cam_next(&slot))) {        /* behind: only the newest matters */
        if (got >= 0)
            cam_release(got);
        got = slot;
        frame = newest;
    }
    if (got < 0)
        return;
    frames++;
    if (focus_pending && frames > 12) {         /* the exposure's settled by then */
        focus_pending = 0;
        focus_on(canvas.w / 2, canvas.h / 2);
    }
    focus_frame(frame);
    if (frames % EXPOSE_EVERY == 0 && !focus_active())
        cam_expose(frame);
    if (frames % BALANCE_EVERY == 1)
        photo_balance(frame, &red, &blue);
    static uint32_t since, work, skipped;
    uint32_t t0 = k86_uptime_ms();
    photo_preview(frame, red, blue, canvas.px, canvas.w, canvas.h, canvas.stride);
    if (shoot)
        take(frame);
    overlay();
    k86_present(0, 0, canvas.w, canvas.h);
    if (restreamed)
        restreamed = 0;                         /* a new stream: its buffers are all queued already */
    else
        cam_release(got);
    work += k86_uptime_ms() - t0;
    if (!since)
        since = t0;
    if (frames % 60 == 0) {                     /* how fast it's going, for the log */
        uint32_t span = k86_uptime_ms() - since;
        k86_logf("camera: %u frames shown in %u ms, %u ms of work each", 60u, span, work / 60);
        since = k86_uptime_ms();
        work = 0;
    }
    (void)skipped;
}

static void pointer(const struct k86_event *ev)
{
    struct wg_pointer p = wg_pointer_make(ev->x, ev->y, ev->buttons, ev->changed, ev->time_ms);
    int dx = p.x - canvas.w / 2, dy = p.y - (canvas.h - BUTTON_BELOW);
    if (!p.up || !have_camera)
        return;
    if (dx * dx + dy * dy < (BUTTON_R + 24) * (BUTTON_R + 24)) {
        shoot = 1;
    } else if (have_thumb && p.x >= thumb_x() - 16 && p.x < thumb_x() + THUMB + 16 &&
               p.y >= thumb_y() - 16 && p.y < thumb_y() + THUMB + 16) {
        k86_log("camera: opening Gallery");
        k86_app_open("Gallery");
    } else if (!focus_active()) {
        focus_on(p.x, p.y);
    }
}

int main(void)
{
    struct k86_header h;
    if (k86_window_open(&win))
        return 1;
    canvas = (struct gfx_surface){ win.canvas, win.w, win.h, win.stride };
    memset(&h, 0, sizeof(h));
    strcpy(h.title, "Camera");
    k86_header(&h);
    have_camera = cam_open() == 0;
    if (have_camera) {
        k86_timer_set(FRAME_MS);
        focus_open();
        have_thumb = photo_thumb(thumb);
        k86_log("camera: open");
    } else {
        cam_close();
        k86_log("camera: no camera here");
        draw_message("No camera found");
    }
    for (;;) {
        struct k86_event ev;
        if (k86_wait_event(&ev)) {
            cam_close();
            return 1;
        }
        switch (ev.type) {
        case K86_EVENT_POINTER:
            pointer(&ev);
            break;
        case K86_EVENT_TIMER:
            timer();
            break;
        case K86_EVENT_RESIZE:
            canvas.w = ev.x;
            canvas.h = ev.y;
            if (!have_camera)
                draw_message("No camera found");
            break;
        case K86_EVENT_THEME:
            if (!have_camera)
                draw_message("No camera found");
            break;
        case K86_EVENT_CLOSE:
            focus_close();
            cam_close();
            return 0;
        default:
            break;
        }
    }
}
