/* LiteKern Go — Task Manager: what's running, and what it uses.
 *
 * The phone's memory at the top (in use, as a bar: the apps, the system,
 * free), then each running app: in front or in the background, its memory,
 * its share of the processor and how long it's been running, refreshed
 * every second (SYS_TASKS). Stop ends one at once, after asking (anything
 * not saved in it is lost; SYS_TASK_STOP); the Task Manager itself can't be.
 * The list scrolls with a drag and glides after a flick. Actions are logged
 * ("user: taskmgr: ...") for the tests. */
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"
#include "sdk/kern86.h"

#define MARGIN 32
#define ROW_H 150
#define TILE 80
#define REFRESH_MS 1000
#define TICK_MS 16
#define SLOP 24

static struct k86_window win;
static struct gfx_surface canvas;
static struct k86_task tasks[K86_TASKS_MAX];
static int ntasks;
static struct k86_meminfo mem;
static uint32_t next_refresh;

static int scroll, press_y, scroll0, dragging, pressed = -1;   /* pressed: a Stop button */
static int32_t glide;
static uint32_t glide_ms;
static int glide_y;

static int asking = -1;                 /* the app Stop asks about */

/* The apps' icons (SYS_TASK_ICON), fetched once per app. */
static struct {
    int32_t id;
    int state;                          /* 1 have it, -1 none */
    uint32_t px[K86_ICON * K86_ICON];
} icons[K86_TASKS_MAX];
static struct wg_dialog dialog;
static char question[64];

static void copy(char *dst, const char *src, int size)
{
    int n = 0;
    for (; src[n] && n < size - 1; n++)
        dst[n] = src[n];
    dst[n] = '\0';
}

static char *num(char *p, uint32_t v)
{
    char t[12];
    int n = 0;
    do
        t[n++] = (char)('0' + v % 10);
    while (v /= 10);
    while (n)
        *p++ = t[--n];
    *p = '\0';
    return p;
}

/* "34 MB", "1.2 GB". */
static void size_text(char *out, uint32_t kib)
{
    char *p = out;
    if (kib >= 1024 * 1024) {
        uint32_t tenths = (kib * 10u + 512u * 1024u) / (1024u * 1024u);
        p = num(p, tenths / 10);
        *p++ = '.';
        p = num(p, tenths % 10);
        copy(p, " GB", 4);
    } else {
        p = num(p, (kib + 512) / 1024);
        copy(p, " MB", 4);
    }
}

static void append(char *dst, const char *src, int size)
{
    int n = (int)strlen(dst);
    copy(dst + n, src, size - n);
}

/* --- layout -------------------------------------------------------------------------------- */

static int card_h(void)
{
    return 24 + text_height(TEXT_BOLD) + 12 + text_height(TEXT_LARGE) + 20 + 24 + 16 + text_height(TEXT_SMALL) + 28;
}

static int list_top(void)
{
    return 24 + card_h() + 40 + text_height(TEXT_BOLD) + 16;
}

static int content_h(void)
{
    return list_top() + ntasks * ROW_H + 24;
}

static int max_scroll(void)
{
    int m = content_h() - canvas.h;
    return m > 0 ? m : 0;
}

static void set_scroll(int s)
{
    int m = max_scroll();
    scroll = s < 0 ? 0 : s > m ? m : s;
}

static struct gfx_rect stop_rect(int i)
{
    const struct theme *t = theme_get();
    int y = list_top() + i * ROW_H - scroll;
    return (struct gfx_rect){ canvas.w - MARGIN - 24 - 132, y + (ROW_H - t->button_size) / 2, 132, t->button_size };
}

/* --- drawing -------------------------------------------------------------------------------- */

/* An app's icon (the home screen's), else its initial on a colour of its own. */
static void tile(int x, int y, const struct k86_task *k)
{
    int slot = -1;
    for (int i = 0; i < K86_TASKS_MAX && slot < 0; i++)
        if (icons[i].state && icons[i].id == k->id)
            slot = i;
    for (int i = 0; i < K86_TASKS_MAX && slot < 0; i++)
        if (!icons[i].state) {
            slot = i;
            icons[i].id = k->id;
            icons[i].state = k86_task_icon(k->id, icons[i].px) ? -1 : 1;
        }
    if (slot >= 0 && icons[slot].state > 0) {
        gfx_blit_alpha_scaled(&canvas, (struct gfx_rect){ x, y, TILE, TILE }, icons[slot].px, K86_ICON, K86_ICON,
                              K86_ICON, 255);
        return;
    }
    const char *name = k->name;
    static const uint32_t colours[] = { 0x3584e4, 0x2ec27e, 0xe66100, 0x9141ac, 0xc01c28, 0x1c71d8, 0x26a269 };
    uint32_t h = 0;
    for (const char *p = name; *p; p++)
        h = h * 31 + (uint8_t)*p;
    gfx_fill_round_rect(&canvas, x, y, TILE, TILE, 20, colours[h % 7]);
    char initial[2] = { name[0], 0 };
    text_draw(&canvas, x + (TILE - text_width(initial, TEXT_HEADING)) / 2, y + (TILE - text_height(TEXT_HEADING)) / 2,
              initial, TEXT_HEADING, 0xffffff);
}

static void draw(void)
{
    const struct theme *t = theme_get();
    uint32_t card = t->light ? t->view_bg : gfx_mix(0xffffff, t->window_bg, 20);
    int w = canvas.w - 2 * MARGIN;
    gfx_fill_rect(&canvas, 0, 0, canvas.w, canvas.h, t->window_bg);

    /* The memory card. */
    int y = 24 - scroll, h = card_h();
    gfx_fill_round_rect(&canvas, MARGIN, y, w, h, 28, card);
    int x = MARGIN + 28, iw = w - 56;
    text_draw(&canvas, x, y + 24, "Memory", TEXT_BOLD, t->fg_dim);
    uint32_t used = mem.total_kib > mem.available_kib ? mem.total_kib - mem.available_kib : 0, apps = 0;
    for (int i = 0; i < ntasks; i++)
        apps += tasks[i].memory_kib;
    char line[96], a[24], b[24];
    size_text(a, used);
    size_text(b, mem.total_kib);
    copy(line, a, sizeof(line));
    append(line, " of ", sizeof(line));
    append(line, b, sizeof(line));
    int ly = y + 24 + text_height(TEXT_BOLD) + 12;
    text_draw(&canvas, x, ly, line, TEXT_LARGE, t->fg);
    text_draw(&canvas, x + text_width(line, TEXT_LARGE) + 16, ly + text_height(TEXT_LARGE) - text_height(TEXT_BODY) - 8,
              "in use", TEXT_BODY, t->fg_dim);
    /* The bar: the apps, the system (the shell and everything else), free. */
    int by = ly + text_height(TEXT_LARGE) + 20;
    gfx_fill_round_rect(&canvas, x, by, iw, 24, 12, t->button_bg);
    if (mem.total_kib) {
        int wa = (int)((uint64_t)iw * apps / mem.total_kib), wu = (int)((uint64_t)iw * used / mem.total_kib);
        if (wu > iw)
            wu = iw;
        if (wa > wu)
            wa = wu;
        if (wu >= 24)
            gfx_fill_round_rect(&canvas, x, by, wu, 24, 12, gfx_mix(t->accent_bg, t->button_bg, 110));
        if (wa >= 24)
            gfx_fill_round_rect(&canvas, x, by, wa, 24, 12, t->accent_bg);
    }
    char sa[24], ss[24], sf[24];
    size_text(sa, apps);
    size_text(ss, used > apps ? used - apps : 0);
    size_text(sf, mem.available_kib);
    copy(line, "Apps ", sizeof(line));
    append(line, sa, sizeof(line));
    append(line, "    System ", sizeof(line));
    append(line, ss, sizeof(line));
    append(line, "    Free ", sizeof(line));
    append(line, sf, sizeof(line));
    text_draw_fit(&canvas, x, by + 24 + 16, line, iw, TEXT_SMALL, t->fg_dim);

    /* The apps. */
    y = 24 + h + 40 - scroll;
    copy(line, "Running", sizeof(line));
    text_draw(&canvas, MARGIN, y, line, TEXT_BOLD, t->fg);
    if (ntasks) {
        char n[8] = "";
        num(n, (uint32_t)ntasks);
        text_draw(&canvas, canvas.w - MARGIN - text_width(n, TEXT_BODY), y, n, TEXT_BODY, t->fg_dim);
    }
    for (int i = 0; i < ntasks; i++) {
        const struct k86_task *k = &tasks[i];
        int ry = list_top() + i * ROW_H - scroll;
        if (ry + ROW_H < 0 || ry > canvas.h)
            continue;
        gfx_fill_round_rect(&canvas, MARGIN, ry + 6, w, ROW_H - 12, 28, card);
        tile(MARGIN + 20, ry + (ROW_H - TILE) / 2, k);
        int tx = MARGIN + 20 + TILE + 24, tw = w - (tx - MARGIN) - 180;
        int lh = text_height(TEXT_SMALL) + 2, ty = ry + (ROW_H - text_height(TEXT_BOLD) - 4 - 2 * lh) / 2;
        text_draw_fit(&canvas, tx, ty, k->name, tw, TEXT_BOLD, t->fg);
        /* "In front, 2% CPU, 3 min" */
        copy(line, k->flags & K86_TASK_FRONT ? "In front" : "In the background", sizeof(line));
        char c[16] = "";
        char *p = num(c, k->cpu_permille / 10);
        if (k->cpu_permille < 100 && k->cpu_permille % 10) {
            *p++ = '.';
            num(p, k->cpu_permille % 10);
        }
        append(line, ", ", sizeof(line));
        append(line, c, sizeof(line));
        append(line, "% CPU", sizeof(line));
        text_draw_fit(&canvas, tx, ty + text_height(TEXT_BOLD) + 4, line, tw, TEXT_SMALL, t->fg_dim);
        size_text(a, k->memory_kib);
        text_draw(&canvas, tx, ty + text_height(TEXT_BOLD) + 4 + lh, a, TEXT_SMALL, t->fg_dim);
        struct gfx_rect s = stop_rect(i);
        if (k->flags & K86_TASK_SELF) {
            const char *me = "This app";
            text_draw(&canvas, s.x + s.w - text_width(me, TEXT_BODY), s.y + (s.h - text_height(TEXT_BODY)) / 2, me,
                      TEXT_BODY, t->fg_dim);
        } else {
            uint32_t bg = pressed == i ? t->button_active : t->button_bg;
            gfx_fill_round_rect(&canvas, s.x, s.y, s.w, s.h, s.h / 2, bg);
            text_draw(&canvas, s.x + (s.w - text_width("Stop", TEXT_BOLD)) / 2, s.y + (s.h - text_height(TEXT_BOLD)) / 2,
                      "Stop", TEXT_BOLD, t->destructive);
        }
    }
    if (asking >= 0) {
        wg_dialog_layout(&dialog, canvas.w, canvas.h);
        wg_dialog_show(&canvas, &dialog);
    }
    k86_present(0, 0, canvas.w, canvas.h);
}

/* --- doing ---------------------------------------------------------------------------------------- */

static void refresh(void)
{
    int n = k86_tasks(tasks, K86_TASKS_MAX, &mem);
    ntasks = n < 0 ? 0 : n;
    set_scroll(scroll);
    next_refresh = k86_uptime_ms() + REFRESH_MS;
}

static void ask(int i)
{
    asking = i;
    copy(question, "Stop ", sizeof(question));
    append(question, tasks[i].name, sizeof(question));
    append(question, "?", sizeof(question));
    wg_dialog_init(&dialog, question, "Anything not saved in it will be lost.", "Stop", WG_BUTTON_DESTRUCTIVE, 0);
    wg_dialog_layout(&dialog, canvas.w, canvas.h);
    dialog.appear = 0;                      /* it fades in, the card rising */
    wg_animate(&dialog.appear, 0, 256, 180, draw, k86_uptime_ms);
}

static void stop(void)
{
    int i = asking;
    asking = -1;
    int err = k86_task_stop(tasks[i].id);
    k86_logf("taskmgr: stop %s: %s", tasks[i].name, err ? "refused" : "done");
    refresh();
    draw();
}

static void dialog_result(int r)
{
    if (r == 0) {
        asking = -1;
        draw();
    } else if (r > 0) {
        stop();
    } else if (dialog.dirty) {
        wg_dialog_draw(&canvas, &dialog);
        k86_present(dialog.r.x, dialog.r.y, dialog.r.w, dialog.r.h);
    }
}

static int stop_at(int x, int y)
{
    for (int i = 0; i < ntasks; i++) {
        struct gfx_rect r = stop_rect(i);
        if (!(tasks[i].flags & K86_TASK_SELF) && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
            return i;
    }
    return -1;
}

static void pointer(const struct wg_pointer *p)
{
    if (asking >= 0) {
        dialog_result(wg_dialog_pointer(&dialog, p));
        return;
    }
    if (p->down) {
        press_y = glide_y = p->y;
        glide_ms = p->time_ms;
        glide = 0;
        scroll0 = scroll;
        dragging = 0;
        pressed = stop_at(p->x, p->y);
        draw();
        return;
    }
    if (p->held) {
        if (!dragging && (p->y - press_y > SLOP || press_y - p->y > SLOP)) {
            dragging = 1;
            pressed = -1;
        }
        if (dragging) {
            uint32_t dt = p->time_ms - glide_ms;
            if (dt > 0) {
                glide = (glide + (int32_t)((p->y - glide_y) * 256 / (int)dt)) / 2;
                glide_ms = p->time_ms;
                glide_y = p->y;
            }
            set_scroll(scroll0 - (p->y - press_y));
            draw();
        }
        return;
    }
    if (!p->up)
        return;
    int was = pressed;
    pressed = -1;
    if (dragging) {
        dragging = 0;
        if (p->time_ms - glide_ms > 80)
            glide = 0;
        glide_ms = p->time_ms;
        k86_timer_set(TICK_MS);
        return;
    }
    if (was >= 0 && was == stop_at(p->x, p->y))
        ask(was);
    else
        draw();
}

static void timer(void)
{
    uint32_t now = k86_uptime_ms();
    if (glide) {                            /* gliding after a flick, slowing down */
        uint32_t dt = now - glide_ms;
        dt = dt > 50 ? 50 : dt;
        glide_ms = now;
        int before = scroll;
        set_scroll(scroll - (int)(glide * (int32_t)dt / 256));
        for (uint32_t s = 0; s < (dt + 8) / 16; s++)
            glide = glide * 94 / 100;
        if ((glide > -8 && glide < 8) || scroll == before)
            glide = 0;
    }
    if ((int32_t)(now - next_refresh) >= 0 && asking < 0)
        refresh();
    if (asking < 0)
        draw();
    k86_timer_set(glide ? TICK_MS : REFRESH_MS / 4);
}

int main(void)
{
    if (k86_window_open(&win))
        return 1;
    canvas = (struct gfx_surface){ win.canvas, win.w, win.h, win.stride };
    refresh();
    draw();
    k86_logf("taskmgr: open, %d app%s running", ntasks, ntasks == 1 ? "" : "s");
    k86_timer_set(REFRESH_MS / 4);
    for (;;) {
        struct k86_event ev;
        k86_back_wanted(asking >= 0);
        if (k86_wait_event(&ev))
            return 1;
        switch (ev.type) {
        case K86_EVENT_POINTER: {
            struct wg_pointer p = wg_pointer_make(ev.x, ev.y, ev.buttons, ev.changed, ev.time_ms);
            pointer(&p);
            break;
        }
        case K86_EVENT_KEY:
            if (ev.key.pressed && asking >= 0)
                dialog_result(wg_dialog_key(&dialog, &ev.key));
            break;
        case K86_EVENT_SCROLL:
            set_scroll(scroll + ev.id * ROW_H);
            draw();
            break;
        case K86_EVENT_TIMER:   timer(); break;
        case K86_EVENT_THEME:   draw(); break;
        case K86_EVENT_RESIZE:  canvas.w = ev.x; canvas.h = ev.y; set_scroll(scroll); draw(); break;
        case K86_EVENT_CLOSE:   return 0;
        default:                break;
        }
    }
}
