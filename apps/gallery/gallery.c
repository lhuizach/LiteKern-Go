/* LiteKern Go — Gallery: the pictures on the phone, newest first.
 *
 * A grid of square thumbnails (three a row) of the pictures in Pictures,
 * Pictures/Screenshots, Pictures/Camera and Downloads in the home folder.
 * It scrolls with a drag, by the pixel, and keeps going after a flick,
 * slowing down. The thumbnails are made in the background, a few at a
 * time, the ones on screen first. A tap opens a picture fitted to the
 * screen: a swipe sideways goes to the next or previous one, and the
 * buttons under it make it the wallpaper or the lock screen's, or delete
 * it (asked first). Back goes back to the grid, then closes Gallery.
 *
 * Decoding is Files' (apps/files/image.c, picture.c, compiled in here by
 * the one-line files next to this one). Actions are logged ("user:
 * gallery: ...") for the tests. */
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"
#include "sdk/kern86.h"
#include "apps/files/picture.h"

#define MAX_PICS 96
#define COLS 3
#define GAP 6
#define THUMB 236                       /* a thumbnail's side, as drawn on the phone's 720 px */
#define SLOP 24                         /* px a finger moves before it's a drag */
#define SWIPE 80                        /* px sideways that turn to the next picture */
#define FLING_MIN 8                     /* px/s * 256 / 1000 below which a fling stops */
#define TICK_MS 16
#define BACK_ID 1

static const char *const folders[] = { "Pictures", "Pictures/Screenshots", "Pictures/Camera", "Downloads" };
#define NFOLDERS 4

struct pic {
    char dir[K86_PATH_MAX];
    struct k86_dirent e;
    int thumb;                          /* 0 not yet, 1 made, -1 couldn't be */
};
static struct pic pics[MAX_PICS];
static int npics;
static uint32_t thumbs[MAX_PICS][THUMB * THUMB];

static struct k86_window win;
static struct gfx_surface canvas;
static int vol = -1;
static char home[K86_PATH_MAX];

/* The grid's scrolling: its top in px, and a fling's speed (px per ms, x 256). */
static int scroll, press_x, press_y, scroll0, dragging, pressed = -1;
static int32_t fling;
static uint32_t last_ms;
static int last_y;

/* The viewer. */
static int viewing = -1;                /* the picture shown, or -1: the grid */
static struct wg_button b_lock, b_wall, b_delete;
static int asking;                      /* Delete's question */
static struct wg_dialog dialog;
static char status[96];
static int status_bad;

static void copy(char *dst, const char *src, int size)
{
    int n = 0;
    for (; src[n] && n < size - 1; n++)
        dst[n] = src[n];
    dst[n] = '\0';
}

static void join(char *out, const char *a, const char *b)
{
    int n = (int)strlen(a);
    copy(out, a, K86_PATH_MAX);
    if (n && a[n - 1] != '/')
        out[n++] = '/';
    copy(out + n, b, K86_PATH_MAX - n);
}

/* --- finding the pictures ---------------------------------------------------------------------- */

static int newer(const struct k86_dirent *a, const struct k86_dirent *b)
{
    if (a->date != b->date)
        return a->date > b->date;
    if (a->time != b->time)
        return a->time > b->time;
    return strcmp(a->name, b->name) > 0;    /* screenshots: their names are their times */
}

static void scan(void)
{
    static struct k86_dirent list[256];
    npics = 0;
    if (vol < 0)
        return;
    for (int f = 0; f < NFOLDERS; f++) {
        char dir[K86_PATH_MAX];
        join(dir, home, folders[f]);
        int n = k86_list(vol, dir, list, 256);
        for (int i = 0; i < n && i < 256; i++) {
            if (list[i].is_dir || !picture_named(list[i].name))
                continue;
            int k = npics < MAX_PICS ? npics++ : MAX_PICS - 1;
            if (k == MAX_PICS - 1 && !newer(&list[i], &pics[k].e))
                continue;                   /* full: only newer ones push the oldest out */
            struct pic p;
            copy(p.dir, dir, sizeof(p.dir));
            p.e = list[i];
            p.thumb = 0;
            while (k > 0 && newer(&p.e, &pics[k - 1].e)) {   /* newest first */
                pics[k] = pics[k - 1];
                k--;
            }
            pics[k] = p;
        }
    }
    k86_logf("gallery: %d picture%s", npics, npics == 1 ? "" : "s");
}

/* --- the grid --------------------------------------------------------------------------------------- */

static int tile(void)
{
    return (canvas.w - (COLS - 1) * GAP) / COLS;
}

static int content_h(void)
{
    int rows = (npics + COLS - 1) / COLS;
    return rows * (tile() + GAP);
}

static int max_scroll(void)
{
    int m = content_h() - canvas.h;
    return m > 0 ? m : 0;
}

static struct gfx_rect tile_rect(int i)
{
    int t = tile();
    return (struct gfx_rect){ i % COLS * (t + GAP), i / COLS * (t + GAP) - scroll, t, t };
}

static int tile_at(int x, int y)
{
    int t = tile();
    int col = x / (t + GAP), row = (y + scroll) / (t + GAP);
    if (col >= COLS || x % (t + GAP) >= t || (y + scroll) % (t + GAP) >= t)
        return -1;
    int i = row * COLS + col;
    return i < npics ? i : -1;
}

static void draw_grid(void)
{
    const struct theme *t = theme_get();
    gfx_fill_rect(&canvas, 0, 0, canvas.w, canvas.h, t->window_bg);
    if (!npics) {
        const char *a = "No pictures yet.", *b = "Screenshots (Power and Volume Down) come here.";
        text_draw(&canvas, (canvas.w - text_width(a, TEXT_HEADING)) / 2, canvas.h / 3, a, TEXT_HEADING, t->fg);
        text_draw_fit(&canvas, 32, canvas.h / 3 + text_height(TEXT_HEADING) + 16, b, canvas.w - 64, TEXT_BODY,
                      t->fg_dim);
    }
    for (int i = 0; i < npics; i++) {
        struct gfx_rect r = tile_rect(i);
        if (r.y + r.h < 0 || r.y > canvas.h)
            continue;
        if (pics[i].thumb == 1) {
            struct gfx_surface th = { thumbs[i], THUMB, THUMB, THUMB };
            if (r.w == THUMB)
                gfx_blit(&canvas, r.x, r.y, &th, 0, 0, THUMB, THUMB);
            else
                gfx_blit_scaled(&canvas, r, &th, (struct gfx_rect){ 0, 0, THUMB, THUMB }, 255, 0);
        } else {
            gfx_fill_rect(&canvas, r.x, r.y, r.w, r.h, gfx_mix(t->fg, t->window_bg, pics[i].thumb < 0 ? 40 : 16));
        }
        if (i == pressed)
            gfx_darken(&canvas, r.x, r.y, r.w, r.h, 70);
    }
    if (max_scroll() > 0) {                 /* where we are */
        int track = canvas.h - 16, thumb = track * canvas.h / content_h();
        thumb = thumb < 40 ? 40 : thumb;
        int y = 8 + (track - thumb) * scroll / max_scroll();
        gfx_fill_round_rect(&canvas, canvas.w - 10, y, 6, thumb, 3, gfx_mix(t->fg, t->window_bg, 110));
    }
    k86_present(0, 0, canvas.w, canvas.h);
}

/* Make the next missing thumbnail: one on screen first. 1 if there's more to do. */
static int thumb_step(void)
{
    int pick = -1;
    for (int i = 0; i < npics && pick < 0; i++) {
        struct gfx_rect r = tile_rect(i);
        if (!pics[i].thumb && r.y + r.h >= 0 && r.y <= canvas.h)
            pick = i;
    }
    for (int i = 0; i < npics && pick < 0; i++)
        if (!pics[i].thumb)
            pick = i;
    if (pick < 0)
        return 0;
    const char *why = 0;
    picture_app = 0;                        /* thumbnails: no log line each */
    int ok = !picture_open(vol, pics[pick].dir, &pics[pick].e, &why) && !picture_thumb(thumbs[pick], THUMB);
    picture_app = "gallery";
    picture_close();
    pics[pick].thumb = ok ? 1 : -1;
    return 1;
}

static void set_scroll(int s)
{
    int m = max_scroll();
    scroll = s < 0 ? 0 : s > m ? m : s;
}

/* --- the viewer --------------------------------------------------------------------------------------- */

static void header(void)
{
    struct k86_header h;
    memset(&h, 0, sizeof(h));
    copy(h.title, viewing >= 0 ? pics[viewing].e.name : "Gallery", sizeof(h.title));
    if (viewing >= 0) {
        h.nbuttons = 1;
        h.buttons[0].icon = 1;              /* back */
        h.buttons[0].id = BACK_ID;
    }
    k86_header(&h);
}

static void layout_viewer(void)
{
    const struct theme *t = theme_get();
    int y = canvas.h - t->button_size - 24, w = (canvas.w - 2 * 24 - 2 * 12) / 3;
    b_lock.r = (struct gfx_rect){ 24, y, w, t->button_size };
    b_wall.r = (struct gfx_rect){ 24 + w + 12, y, w, t->button_size };
    b_delete.r = (struct gfx_rect){ 24 + 2 * (w + 12), y, w, t->button_size };
    if (asking)
        wg_dialog_layout(&dialog, canvas.w, canvas.h);
}

static void draw_viewer(void)
{
    const struct theme *t = theme_get();
    gfx_fill_rect(&canvas, 0, 0, canvas.w, canvas.h, t->window_bg);
    if (status[0])
        text_draw_fit(&canvas, 24, b_lock.r.y - text_height(TEXT_SMALL) - 12, status, canvas.w - 48, TEXT_SMALL,
                      status_bad ? t->destructive : t->fg_dim);
    wg_button_draw(&canvas, &b_lock);
    wg_button_draw(&canvas, &b_wall);
    wg_button_draw(&canvas, &b_delete);
    if (asking)
        wg_dialog_show(&canvas, &dialog);
    k86_present(0, 0, canvas.w, canvas.h);
    if (!asking)
        picture_draw(&canvas, (struct gfx_rect){ 0, 16, canvas.w, b_lock.r.y - 16 - text_height(TEXT_SMALL) - 24 });
}

static void open_viewer(int i)
{
    const char *why = 0;
    status[0] = '\0';
    if (picture_open(vol, pics[i].dir, &pics[i].e, &why)) {
        copy(status, why, sizeof(status));
        status_bad = 1;
    }
    viewing = i;
    k86_logf("gallery: open %s", pics[i].e.name);
    header();
    layout_viewer();
    draw_viewer();
}

static void close_viewer(void)
{
    picture_close();
    viewing = -1;
    asking = 0;
    header();
    draw_grid();
}

static void step_viewer(int step)
{
    int i = viewing + step;
    if (i < 0 || i >= npics)
        return;
    open_viewer(i);
}

static void wallpaper(int where)
{
    const char *why = 0;
    int ok = !picture_wallpaper(where, &why);
    copy(status, ok ? (where == K86_WALLPAPER_LOCK ? "It's on the lock screen now." : "It's the wallpaper now.") : why,
         sizeof(status));
    status_bad = !ok;
    k86_logf("gallery: %s %s", where == K86_WALLPAPER_LOCK ? "lock screen" : "wallpaper", ok ? "set" : "refused");
    draw_viewer();
}

static void delete_now(void)
{
    struct pic *p = &pics[viewing];
    int err = k86_delete(vol, p->dir, p->e.name);
    k86_logf("gallery: delete %s: %s", p->e.name, err ? "refused" : "done");
    asking = 0;
    if (err) {
        copy(status, "It couldn't be deleted.", sizeof(status));
        status_bad = 1;
        draw_viewer();
        return;
    }
    int i = viewing;
    for (int k = i; k < npics - 1; k++) {
        pics[k] = pics[k + 1];
        memcpy(thumbs[k], thumbs[k + 1], sizeof(thumbs[k]));
    }
    npics--;
    set_scroll(scroll);
    if (!npics) {
        close_viewer();
        return;
    }
    open_viewer(i < npics ? i : npics - 1);     /* the next one (or the one before, at the end) */
}

/* --- input ---------------------------------------------------------------------------------------------- */

static void timer(void)
{
    int busy = 0;
    if (viewing < 0 && fling && !dragging) {    /* the fling slows down, then stops */
        set_scroll(scroll - fling * TICK_MS / 256);
        fling = fling * 94 / 100;
        if (fling > -FLING_MIN && fling < FLING_MIN)
            fling = 0;
        if (scroll == 0 || scroll == max_scroll())
            fling = 0;                      /* at an end */
        busy = 1;
    }
    if (viewing < 0 && thumb_step())
        busy = 1;
    if (viewing < 0)
        draw_grid();
    k86_timer_set(busy || fling ? TICK_MS : 0);
}

static void grid_pointer(const struct wg_pointer *p, uint32_t ms)
{
    if (p->down) {
        press_x = p->x;
        press_y = last_y = p->y;
        last_ms = ms;
        scroll0 = scroll;
        dragging = 0;
        fling = 0;
        pressed = tile_at(p->x, p->y);
        draw_grid();
        return;
    }
    if (p->held) {
        if (!dragging && (p->y - press_y > SLOP || press_y - p->y > SLOP)) {
            dragging = 1;
            pressed = -1;
        }
        if (dragging) {
            uint32_t dt = ms - last_ms;
            if (dt > 0) {                   /* the finger's speed, for a fling */
                int32_t v = (int32_t)((p->y - last_y) * 256 / (int)dt);
                fling = (fling + v) / 2;
            }
            last_ms = ms;
            last_y = p->y;
            set_scroll(scroll0 - (p->y - press_y));
            draw_grid();
        }
        return;
    }
    if (!p->up)
        return;
    int was = pressed;
    pressed = -1;
    if (dragging) {
        dragging = 0;
        if (ms - last_ms > 80)
            fling = 0;                      /* it stopped before it let go: no fling */
        if (fling)
            k86_timer_set(TICK_MS);
        draw_grid();
        return;
    }
    if (was >= 0 && was == tile_at(p->x, p->y))
        open_viewer(was);
    else
        draw_grid();
}

static void viewer_pointer(const struct wg_pointer *p)
{
    if (asking) {
        int r = wg_dialog_pointer(&dialog, p);
        if (r == 0) {
            asking = 0;
            draw_viewer();
        } else if (r > 0) {
            delete_now();
        } else if (dialog.dirty) {
            wg_dialog_draw(&canvas, &dialog);
            k86_present(dialog.r.x, dialog.r.y, dialog.r.w, dialog.r.h);
        }
        return;
    }
    if (wg_button_pointer(&b_lock, p)) {
        wallpaper(K86_WALLPAPER_LOCK);
        return;
    }
    if (wg_button_pointer(&b_wall, p)) {
        wallpaper(K86_WALLPAPER_DESKTOP);
        return;
    }
    if (wg_button_pointer(&b_delete, p)) {
        asking = 1;
        wg_dialog_init(&dialog, "Delete this picture?", "It will be gone for good.", "Delete", WG_BUTTON_DESTRUCTIVE,
                       0);
        layout_viewer();
        draw_viewer();
        return;
    }
    if (b_lock.dirty || b_wall.dirty || b_delete.dirty) {
        wg_button_draw(&canvas, &b_lock);
        wg_button_draw(&canvas, &b_wall);
        wg_button_draw(&canvas, &b_delete);
        k86_present(0, b_lock.r.y, canvas.w, b_lock.r.h);
    }
    if (p->down && p->y < b_lock.r.y - 12) {
        press_x = p->x;
        dragging = 1;
    } else if (p->up && dragging) {
        dragging = 0;
        int dx = p->x - press_x;
        if (dx < -SWIPE || dx > SWIPE)
            step_viewer(dx < 0 ? 1 : -1);   /* a swipe left: the next one */
    }
}

static void key(const struct key_event *k)
{
    if (!k->pressed)
        return;
    if (asking) {
        int r = wg_dialog_key(&dialog, k);
        if (r == 0) {
            asking = 0;
            draw_viewer();
        } else if (r > 0) {
            delete_now();
        }
        return;
    }
    if (viewing >= 0) {
        if (k->key == KEY_ESC)
            close_viewer();                 /* the phone's Back */
        else if (k->key == KEY_RIGHT)
            step_viewer(1);
        else if (k->key == KEY_LEFT)
            step_viewer(-1);
        return;
    }
    if (k->key == KEY_DOWN || k->key == KEY_UP) {
        set_scroll(scroll + (k->key == KEY_DOWN ? 1 : -1) * (tile() + GAP));
        draw_grid();
    }
}

int main(void)
{
    if (k86_window_open(&win))
        return 1;
    canvas = (struct gfx_surface){ win.canvas, win.w, win.h, win.stride };
    picture_app = "gallery";
    b_lock = (struct wg_button){ .label = "Lock Screen" };
    b_wall = (struct wg_button){ .label = "Wallpaper", .style = WG_BUTTON_SUGGESTED };
    b_delete = (struct wg_button){ .label = "Delete", .style = WG_BUTTON_DESTRUCTIVE };
    vol = k86_home(home);
    scan();
    header();
    draw_grid();
    k86_timer_set(npics ? TICK_MS : 0);    /* the thumbnails, in the background */
    k86_log("gallery: open");
    for (;;) {
        struct k86_event ev;
        k86_back_wanted(viewing >= 0);
        if (k86_wait_event(&ev))
            return 1;
        switch (ev.type) {
        case K86_EVENT_POINTER: {
            struct wg_pointer p = wg_pointer_make(ev.x, ev.y, ev.buttons, ev.changed, ev.time_ms);
            if (viewing >= 0)
                viewer_pointer(&p);
            else
                grid_pointer(&p, ev.time_ms);
            break;
        }
        case K86_EVENT_SCROLL:
            if (viewing < 0) {
                set_scroll(scroll + ev.id * (tile() + GAP));
                draw_grid();
            }
            break;
        case K86_EVENT_KEY:     key(&ev.key); break;
        case K86_EVENT_TIMER:   timer(); break;
        case K86_EVENT_HEADER:
            if (ev.id == BACK_ID)
                close_viewer();
            break;
        case K86_EVENT_THEME:
            if (viewing >= 0)
                draw_viewer();
            else
                draw_grid();
            break;
        case K86_EVENT_RESIZE:
            canvas.w = ev.x;
            canvas.h = ev.y;
            set_scroll(scroll);
            if (viewing >= 0) {
                layout_viewer();
                draw_viewer();
            } else {
                draw_grid();
            }
            break;
        case K86_EVENT_CLOSE:   return 0;
        default:                break;
        }
    }
}
