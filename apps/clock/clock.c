/* LiteKern Go — Clock: alarms, a timer and a stopwatch (ROADMAP.md phase 3).
 *
 * Three tabs along the bottom of the screen:
 *   Alarm      the time now, then the alarms (a daily time each, with a
 *              switch); a tap on one edits it, + Add alarm makes one. The
 *              editor sets the hour and minute with + and - (held, they keep
 *              going). Kept in Clock/alarms.txt in the home folder.
 *   Timer      typed on a keypad as on phones (digits push in from the
 *              right: 5 is 5 seconds, 500 is 5 minutes), then Start; Pause,
 *              Resume, Cancel.
 *   Stopwatch  Start, Stop, Lap, Reset, and the laps, newest first.
 * When an alarm or the timer goes off, the screen says so (Snooze 5 min or
 * Stop for an alarm) and a notification is posted. There's no sound yet,
 * and alarms only ring while Clock is running (open, or in the app
 * switcher): there are no background services (UI-PLAN.md).
 * Actions are logged ("user: clock: ...") for the tests. */
#include "kernel/string.h"
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"
#include "sdk/kern86.h"

#define MARGIN 32
#define TAB_H 120
#define PILL_H 96
#define MAX_ALARMS 10
#define MAX_LAPS 99
#define MAX_BUTTONS 40
#define REPEAT_DELAY_MS 400
#define REPEAT_MS 90
#define SNOOZE_MIN 5
#define BACK_ID 1

enum tab { T_ALARM, T_TIMER, T_STOPWATCH, NTABS };
static const char *const tab_names[NTABS] = { "Alarm", "Timer", "Stopwatch" };

/* What can be tapped (made again with each drawing). */
enum {
    B_TAB = 100,                        /* + the tab */
    B_SW_LEFT = 110, B_SW_RIGHT,
    B_DIGIT = 200,                      /* + the digit */
    B_DEL = 210, B_START, B_PAUSE, B_CANCEL, B_DISMISS,
    B_ALARM = 300,                      /* + the alarm: edit it */
    B_SWITCH = 320,                     /* + the alarm: on or off */
    B_ADD = 340, B_H_UP, B_H_DOWN, B_M_UP, B_M_DOWN, B_SAVE, B_DELETE,
    B_SNOOZE = 360, B_STOP_RING,
};
enum { S_PLAIN, S_ACCENT, S_RED, S_QUIET };

struct button {
    struct gfx_rect r;
    int id;
};
static struct button buttons[MAX_BUTTONS];
static int nbuttons, pressed = -1;      /* pressed: an id */
static uint32_t pressed_ms, repeat_at;

static struct k86_window win;
static struct gfx_surface canvas;
static enum tab tab = T_ALARM;
static uint32_t rate;                   /* the timer's interval now */

/* The stopwatch. */
static int sw_running;
static uint32_t sw_base, sw_started;    /* ms before the last start; when it started */
static uint32_t laps[MAX_LAPS];         /* lap times, oldest first */
static int nlaps;

/* The timer. */
enum { TM_SETTING, TM_RUNNING, TM_PAUSED, TM_DONE };
static int tm_state;
static char tm_digits[7];               /* typed, "" to "HHMMSS" */
static uint32_t tm_end, tm_left;        /* running: when it ends; paused: what's left */

/* The alarms. */
struct alarm {
    int h, m, on;
    int fired_day, fired_month;         /* rung today already */
};
static struct alarm alarms[MAX_ALARMS];
static int nalarms;
static int editing = -1;                /* the alarm being edited, MAX_ALARMS: a new one, -1: none */
static int edit_h, edit_m;
static int snoozing, snooze_h, snooze_m;
static int ringing;                     /* 1: an alarm, 2: the timer */
static int ring_h, ring_m;
static struct k86_time now;
static int have_time;

/* --- text --------------------------------------------------------------------------------- */

static char *two(char *p, int v)
{
    *p++ = (char)('0' + v / 10 % 10);
    *p++ = (char)('0' + v % 10);
    *p = '\0';
    return p;
}

static void hm_text(char *out, int h, int m)
{
    char *p = two(out, h);
    *p++ = ':';
    two(p, m);
}

/* mm:ss.cc (or h:mm:ss.cc past an hour). */
static void sw_text(char *out, uint32_t ms)
{
    uint32_t cs = ms / 10 % 100, s = ms / 1000 % 60, m = ms / 60000 % 60, h = ms / 3600000;
    char *p = out;
    if (h) {
        p = two(p, (int)h);
        *p++ = ':';
    }
    p = two(p, (int)m);
    *p++ = ':';
    p = two(p, (int)s);
    *p++ = '.';
    two(p, (int)cs);
}

/* h:mm:ss, from whole seconds. */
static void hms_text(char *out, uint32_t secs)
{
    char *p = two(out, (int)(secs / 3600));
    *p++ = ':';
    p = two(p, (int)(secs / 60 % 60));
    *p++ = ':';
    two(p, (int)(secs % 60));
}

static void copy(char *dst, const char *src, int size)
{
    int n = 0;
    for (; src[n] && n < size - 1; n++)
        dst[n] = src[n];
    dst[n] = '\0';
}

/* Text twice the size of TEXT_LARGE, centred on cx (drawn small, then scaled). */
static void big_text(const char *s, int cx, int y, uint32_t fg, uint32_t bg)
{
    static uint32_t px[640 * 96];
    int w = text_width(s, TEXT_LARGE) + 4, h = text_height(TEXT_LARGE);
    if (w > 640)
        w = 640;
    if (h > 96)
        h = 96;
    struct gfx_surface tmp = { px, w, h, w };
    gfx_fill_rect(&tmp, 0, 0, w, h, bg);
    text_draw(&tmp, 2, 0, s, TEXT_LARGE, fg);
    gfx_blit_scaled(&canvas, (struct gfx_rect){ cx - w, y, 2 * w, 2 * h }, &tmp, (struct gfx_rect){ 0, 0, w, h }, 255, 0);
}

static int big_height(void)
{
    return 2 * text_height(TEXT_LARGE);
}

/* --- buttons ------------------------------------------------------------------------------ */

static void add(int id, struct gfx_rect r)
{
    if (nbuttons < MAX_BUTTONS)
        buttons[nbuttons++] = (struct button){ r, id };
}

static void pill(int id, struct gfx_rect r, const char *label, int style, enum text_style st)
{
    const struct theme *t = theme_get();
    uint32_t bg = style == S_ACCENT ? t->accent_bg : style == S_RED ? t->destructive
                : style == S_QUIET ? t->window_bg : t->button_bg;
    uint32_t fg = style == S_ACCENT || style == S_RED ? t->accent_fg : t->fg;
    if (pressed == id)
        bg = gfx_mix(t->fg, bg, 50);
    gfx_fill_round_rect(&canvas, r.x, r.y, r.w, r.h, (r.h < r.w ? r.h : r.w) / 2, bg);
    text_draw(&canvas, r.x + (r.w - text_width(label, st)) / 2, r.y + (r.h - text_height(st)) / 2, label, st, fg);
    add(id, r);
}

static int button_at(int x, int y)
{
    for (int i = nbuttons - 1; i >= 0; i--) {   /* the last drawn is on top */
        struct gfx_rect r = buttons[i].r;
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
            return buttons[i].id;
    }
    return -1;
}

static int repeats(int id)
{
    return id == B_H_UP || id == B_H_DOWN || id == B_M_UP || id == B_M_DOWN;
}

/* --- the clock and the alarms' file ----------------------------------------------------------- */

static void read_time(void)
{
    have_time = k86_time(&now) == 0;
}

static int home_vol = -1;
static char home_dir[K86_PATH_MAX];

static void alarms_dir(char *out)
{
    int n = (int)strlen(home_dir);
    copy(out, home_dir, K86_PATH_MAX);
    copy(out + n, n > 1 ? "/Clock" : "Clock", K86_PATH_MAX - n);
}

static void save_alarms(void)
{
    char text[MAX_ALARMS * 16 + 1], dir[K86_PATH_MAX];
    int n = 0;
    for (int i = 0; i < nalarms; i++) {
        hm_text(text + n, alarms[i].h, alarms[i].m);
        n += 5;
        copy(text + n, alarms[i].on ? " on\n" : " off\n", (int)sizeof(text) - n);
        n += alarms[i].on ? 4 : 5;
    }
    if (home_vol < 0)
        return;
    k86_create(home_vol, home_dir, "Clock", 1);        /* -EEXIST after the first time */
    alarms_dir(dir);
    int err = k86_write(home_vol, dir, "alarms.txt", text, (uint32_t)n);
    k86_logf("clock: alarms saved (%d): %s", nalarms, err ? "refused" : "done");
}

static void load_alarms(void)
{
    char text[MAX_ALARMS * 16 + 1], dir[K86_PATH_MAX];
    if (home_vol < 0)
        return;
    alarms_dir(dir);
    int n = k86_read(home_vol, dir, "alarms.txt", text, sizeof(text) - 1);
    if (n <= 0)
        return;
    text[n] = '\0';
    for (char *p = text; *p && nalarms < MAX_ALARMS;) {
        if (p[0] >= '0' && p[0] <= '2' && p[2] == ':') {
            struct alarm *a = &alarms[nalarms++];
            *a = (struct alarm){ (p[0] - '0') * 10 + p[1] - '0', (p[3] - '0') * 10 + p[4] - '0', p[6] == 'o' && p[7] == 'n', 0, 0 };
            if (a->h > 23 || a->m > 59)
                nalarms--;
        }
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
    }
    k86_logf("clock: %d alarm%s", nalarms, nalarms == 1 ? "" : "s");
}

/* Earliest first. */
static void sort_alarms(void)
{
    for (int i = 1; i < nalarms; i++)
        for (int k = i; k > 0 && alarms[k].h * 60 + alarms[k].m < alarms[k - 1].h * 60 + alarms[k - 1].m; k--) {
            struct alarm a = alarms[k];
            alarms[k] = alarms[k - 1];
            alarms[k - 1] = a;
        }
}

/* --- drawing ------------------------------------------------------------------------------ */

static void draw_tabs(void)
{
    const struct theme *t = theme_get();
    int y = canvas.h - TAB_H, w = canvas.w / NTABS;
    gfx_fill_rect(&canvas, 0, y, canvas.w, TAB_H, t->headerbar_bg);
    for (int i = 0; i < NTABS; i++) {
        struct gfx_rect r = { i * w, y, w, TAB_H };
        int sel = (int)tab == i;
        if (sel)
            gfx_fill_round_rect(&canvas, r.x + 24, r.y + 20, r.w - 48, 8, 4, t->accent_bg);
        text_draw(&canvas, r.x + (w - text_width(tab_names[i], TEXT_BOLD)) / 2,
                  r.y + (TAB_H - text_height(TEXT_BOLD)) / 2 + 8, tab_names[i], TEXT_BOLD, sel ? t->fg : t->fg_dim);
        add(B_TAB + i, r);
    }
}

static void draw_stopwatch(void)
{
    const struct theme *t = theme_get();
    uint32_t ms = sw_base + (sw_running ? k86_uptime_ms() - sw_started : 0);
    char text[24];
    sw_text(text, ms);
    big_text(text, canvas.w / 2, 120, t->fg, t->window_bg);
    int y = 120 + big_height() + 60, bw = (canvas.w - 2 * MARGIN - 24) / 2;
    pill(B_SW_LEFT, (struct gfx_rect){ MARGIN, y, bw, PILL_H }, sw_running ? "Lap" : "Reset", S_PLAIN, TEXT_HEADING);
    pill(B_SW_RIGHT, (struct gfx_rect){ MARGIN + bw + 24, y, bw, PILL_H }, sw_running ? "Stop" : ms ? "Resume" : "Start",
         sw_running ? S_RED : S_ACCENT, TEXT_HEADING);
    y += PILL_H + 40;
    int rh = text_height(TEXT_BODY) + 28;
    for (int i = nlaps - 1; i >= 0 && y + rh < canvas.h - TAB_H; i--, y += rh) {
        char name[16] = "Lap ", time[24];
        int n = 4;
        if (i + 1 >= 10)
            name[n++] = (char)('0' + (i + 1) / 10);
        name[n++] = (char)('0' + (i + 1) % 10);
        name[n] = '\0';
        sw_text(time, laps[i] - (i ? laps[i - 1] : 0));
        text_draw(&canvas, MARGIN, y + 14, name, TEXT_BODY, t->fg_dim);
        text_draw(&canvas, canvas.w - MARGIN - text_width(time, TEXT_BODY), y + 14, time, TEXT_BODY, t->fg);
        gfx_fill_rect(&canvas, MARGIN, y + rh - 1, canvas.w - 2 * MARGIN, 1, gfx_mix(t->fg, t->window_bg, 30));
    }
}

/* The typed digits as seconds: "130" is 1 min 30 s. */
static uint32_t tm_typed_secs(void)
{
    char d[7] = "000000";
    int n = (int)strlen(tm_digits);
    for (int i = 0; i < n; i++)
        d[6 - n + i] = tm_digits[i];
    int h = (d[0] - '0') * 10 + d[1] - '0', m = (d[2] - '0') * 10 + d[3] - '0', s = (d[4] - '0') * 10 + d[5] - '0';
    return (uint32_t)(h * 3600 + m * 60 + s);
}

static uint32_t tm_left_ms(void)
{
    if (tm_state == TM_RUNNING)
        return (int32_t)(tm_end - k86_uptime_ms()) > 0 ? tm_end - k86_uptime_ms() : 0;
    return tm_state == TM_PAUSED ? tm_left : 0;
}

static void draw_timer(void)
{
    const struct theme *t = theme_get();
    char text[24];
    if (tm_state == TM_SETTING) {
        char d[7] = "000000";
        int n = (int)strlen(tm_digits);
        for (int i = 0; i < n; i++)
            d[6 - n + i] = tm_digits[i];
        char *p = text;                     /* "00h 05m 00s" */
        *p++ = d[0], *p++ = d[1], *p++ = 'h', *p++ = ' ', *p++ = d[2], *p++ = d[3], *p++ = 'm', *p++ = ' ';
        *p++ = d[4], *p++ = d[5], *p++ = 's', *p = '\0';
        text_draw(&canvas, (canvas.w - text_width(text, TEXT_LARGE)) / 2, 60, text, TEXT_LARGE, n ? t->fg : t->fg_dim);
        int top = 60 + text_height(TEXT_LARGE) + 48, kw = (canvas.w - 2 * MARGIN - 2 * 24) / 3, kh = 120;
        static const char *const labels[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "", "0", "Del" };
        for (int k = 0; k < 12; k++) {
            if (k == 9)
                continue;
            struct gfx_rect r = { MARGIN + k % 3 * (kw + 24), top + k / 3 * (kh + 20), kw, kh };
            int id = k == 11 ? B_DEL : B_DIGIT + (k == 10 ? 0 : k + 1);
            pill(id, r, labels[k], k == 11 ? S_QUIET : S_PLAIN, k == 11 ? TEXT_BOLD : TEXT_LARGE);
        }
        int y = top + 4 * (kh + 20) + 12;
        if (n)
            pill(B_START, (struct gfx_rect){ MARGIN, y, canvas.w - 2 * MARGIN, PILL_H }, "Start", S_ACCENT, TEXT_HEADING);
        return;
    }
    uint32_t left = tm_left_ms();
    hms_text(text, (left + 999) / 1000);
    int done = tm_state == TM_DONE;
    big_text(done ? "00:00:00" : text, canvas.w / 2, 200, done ? t->accent_bg : t->fg, t->window_bg);
    if (done)
        text_draw(&canvas, (canvas.w - text_width("Time's up", TEXT_HEADING)) / 2, 200 + big_height() + 24,
                  "Time's up", TEXT_HEADING, t->fg);
    int y = 200 + big_height() + 120, bw = (canvas.w - 2 * MARGIN - 24) / 2;
    if (done) {
        pill(B_DISMISS, (struct gfx_rect){ MARGIN, y, canvas.w - 2 * MARGIN, PILL_H }, "Stop", S_RED, TEXT_HEADING);
        return;
    }
    pill(B_CANCEL, (struct gfx_rect){ MARGIN, y, bw, PILL_H }, "Cancel", S_PLAIN, TEXT_HEADING);
    pill(B_PAUSE, (struct gfx_rect){ MARGIN + bw + 24, y, bw, PILL_H }, tm_state == TM_RUNNING ? "Pause" : "Resume",
         S_ACCENT, TEXT_HEADING);
}

static void draw_switch(struct gfx_rect r, int on)
{
    const struct theme *t = theme_get();
    gfx_fill_round_rect(&canvas, r.x, r.y, r.w, r.h, r.h / 2, on ? t->accent_bg : t->button_bg);
    gfx_fill_circle(&canvas, on ? r.x + r.w - r.h / 2 : r.x + r.h / 2, r.y + r.h / 2, r.h / 2 - 6, 0xffffff);
}

static void draw_alarms(void)
{
    const struct theme *t = theme_get();
    int y = 32;
    if (have_time) {
        static const char *const days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
        char hm[8];
        hm_text(hm, now.hour, now.minute);
        text_draw(&canvas, MARGIN, y, hm, TEXT_LARGE, t->fg);
        text_draw(&canvas, MARGIN + text_width(hm, TEXT_LARGE) + 20, y + text_height(TEXT_LARGE) - text_height(TEXT_BODY) - 6,
                  days[now.weekday % 7], TEXT_BODY, t->fg_dim);
        y += text_height(TEXT_LARGE) + 32;
    }
    int rh = 132;
    if (!nalarms)
        text_draw(&canvas, MARGIN, y + 24, "No alarms yet.", TEXT_BODY, t->fg_dim);
    for (int i = 0; i < nalarms && y + rh < canvas.h - TAB_H - PILL_H - 40; i++, y += rh + 16) {
        struct gfx_rect r = { MARGIN, y, canvas.w - 2 * MARGIN, rh };
        gfx_fill_round_rect(&canvas, r.x, r.y, r.w, r.h, 24, pressed == B_ALARM + i ? t->button_active : t->button_bg);
        add(B_ALARM + i, r);
        char hm[8];
        hm_text(hm, alarms[i].h, alarms[i].m);
        text_draw(&canvas, r.x + 32, r.y + 14, hm, TEXT_LARGE, alarms[i].on ? t->fg : t->fg_dim);
        text_draw(&canvas, r.x + 32, r.y + 14 + text_height(TEXT_LARGE), alarms[i].on ? "Every day" : "Off", TEXT_SMALL,
                  t->fg_dim);
        struct gfx_rect sw = { r.x + r.w - 32 - 104, r.y + (rh - 56) / 2, 104, 56 };
        draw_switch(sw, alarms[i].on);
        add(B_SWITCH + i, (struct gfx_rect){ sw.x - 24, r.y, sw.w + 56, rh });    /* a big target round it */
    }
    if (snoozing) {
        char line[40] = "Snoozed until ";
        hm_text(line + 14, snooze_h, snooze_m);
        text_draw(&canvas, MARGIN, canvas.h - TAB_H - PILL_H - 40 - text_height(TEXT_BODY) - 12, line, TEXT_BODY, t->fg_dim);
    }
    if (nalarms < MAX_ALARMS)
        pill(B_ADD, (struct gfx_rect){ MARGIN, canvas.h - TAB_H - PILL_H - 24, canvas.w - 2 * MARGIN, PILL_H },
             "+ Add alarm", S_ACCENT, TEXT_HEADING);
}

static void draw_editor(void)
{
    const struct theme *t = theme_get();
    int cw = (canvas.w - 2 * MARGIN - 40) / 2, top = 80;
    static const char *const heads[2] = { "Hour", "Minute" };
    for (int c = 0; c < 2; c++) {
        int x = MARGIN + c * (cw + 40), v = c ? edit_m : edit_h;
        text_draw(&canvas, x + (cw - text_width(heads[c], TEXT_BOLD)) / 2, top, heads[c], TEXT_BOLD, t->fg_dim);
        pill(c ? B_M_UP : B_H_UP, (struct gfx_rect){ x, top + 60, cw, PILL_H }, "+", S_PLAIN, TEXT_LARGE);
        char d[4];
        two(d, v);
        big_text(d, x + cw / 2, top + 60 + PILL_H + 24, t->fg, t->window_bg);
        pill(c ? B_M_DOWN : B_H_DOWN, (struct gfx_rect){ x, top + 60 + PILL_H + 48 + big_height(), cw, PILL_H }, "-",
             S_PLAIN, TEXT_LARGE);
    }
    int y = top + 60 + 2 * PILL_H + 48 + big_height() + 80;
    pill(B_SAVE, (struct gfx_rect){ MARGIN, y, canvas.w - 2 * MARGIN, PILL_H }, "Save", S_ACCENT, TEXT_HEADING);
    if (editing < MAX_ALARMS)
        pill(B_DELETE, (struct gfx_rect){ MARGIN, y + PILL_H + 24, canvas.w - 2 * MARGIN, PILL_H }, "Delete alarm", S_QUIET,
             TEXT_HEADING);
}

static void draw_ringing(void)
{
    const struct theme *t = theme_get();
    gfx_fill_rect(&canvas, 0, 0, canvas.w, canvas.h, t->accent_bg);
    char hm[8];
    if (ringing == 1)
        hm_text(hm, ring_h, ring_m);
    else
        copy(hm, "00:00", sizeof(hm));
    const char *what = ringing == 1 ? "Alarm" : "Time's up";
    text_draw(&canvas, (canvas.w - text_width(what, TEXT_HEADING)) / 2, 200, what, TEXT_HEADING, t->accent_fg);
    big_text(hm, canvas.w / 2, 280, t->accent_fg, t->accent_bg);
    int y = canvas.h - 2 * PILL_H - 120;
    if (ringing == 1)
        pill(B_SNOOZE, (struct gfx_rect){ MARGIN, y, canvas.w - 2 * MARGIN, PILL_H }, "Snooze 5 min", S_PLAIN, TEXT_HEADING);
    pill(B_STOP_RING, (struct gfx_rect){ MARGIN, y + PILL_H + 32, canvas.w - 2 * MARGIN, PILL_H }, "Stop", S_RED,
         TEXT_HEADING);
}

static void header(void)
{
    struct k86_header h;
    memset(&h, 0, sizeof(h));
    copy(h.title, ringing ? "Clock" : editing >= 0 ? (editing == MAX_ALARMS ? "New Alarm" : "Edit Alarm") : "Clock",
         sizeof(h.title));
    if (editing >= 0 && !ringing) {
        h.nbuttons = 1;
        h.buttons[0].icon = 1;              /* back */
        h.buttons[0].id = BACK_ID;
    }
    k86_header(&h);
}

static void draw(void)
{
    gfx_fill_rect(&canvas, 0, 0, canvas.w, canvas.h, theme_get()->window_bg);
    nbuttons = 0;
    if (ringing) {
        draw_ringing();
    } else if (editing >= 0) {
        draw_editor();
    } else {
        if (tab == T_ALARM)
            draw_alarms();
        else if (tab == T_TIMER)
            draw_timer();
        else
            draw_stopwatch();
        draw_tabs();
    }
    k86_present(0, 0, canvas.w, canvas.h);
}

/* Fast while something counts, held or flashes; once a second for the alarms. */
static void set_rate(void)
{
    uint32_t want = sw_running || tm_state == TM_RUNNING || (pressed >= 0 && repeats(pressed)) ? 40 : 1000;
    if (want != rate) {
        rate = want;
        k86_timer_set(rate);
    }
}

/* --- going off ------------------------------------------------------------------------------- */

static void ring(int what, int h, int m)
{
    ringing = what;
    ring_h = h;
    ring_m = m;
    editing = -1;
    pressed = -1;
    char hm[8];
    hm_text(hm, h, m);
    k86_logf("clock: %s", what == 1 ? "alarm rings" : "timer done");
    if (what == 1)
        k86_logf("clock: alarm %s", hm);
    k86_notify(what == 1 ? "Alarm" : "Timer", what == 1 ? hm : "Time's up");
    header();
}

static void check_alarms(void)
{
    if (!have_time || ringing)
        return;
    for (int i = 0; i < nalarms; i++) {
        struct alarm *a = &alarms[i];
        if (a->on && a->h == now.hour && a->m == now.minute &&
            !(a->fired_day == now.day && a->fired_month == now.month)) {
            a->fired_day = now.day;
            a->fired_month = now.month;
            ring(1, a->h, a->m);
            return;
        }
    }
    if (snoozing && snooze_h == now.hour && snooze_m == now.minute) {
        snoozing = 0;
        ring(1, snooze_h, snooze_m);
    }
}

/* --- doing ------------------------------------------------------------------------------------ */

static void edit(int i)
{
    editing = i;
    if (i == MAX_ALARMS) {                  /* a new one: at the time now */
        edit_h = have_time ? now.hour : 7;
        edit_m = have_time ? now.minute : 0;
    } else {
        edit_h = alarms[i].h;
        edit_m = alarms[i].m;
    }
    k86_logf("clock: %s alarm", i == MAX_ALARMS ? "new" : "edit");
    header();
}

static void stop_editing(void)
{
    editing = -1;
    header();
}

static void step(int id)
{
    if (id == B_H_UP)
        edit_h = (edit_h + 1) % 24;
    else if (id == B_H_DOWN)
        edit_h = (edit_h + 23) % 24;
    else if (id == B_M_UP)
        edit_m = (edit_m + 1) % 60;
    else if (id == B_M_DOWN)
        edit_m = (edit_m + 59) % 60;
}

static void act(int id)
{
    char text[24];
    if (id >= B_TAB && id < B_TAB + NTABS) {
        tab = (enum tab)(id - B_TAB);
        k86_logf("clock: tab %s", tab_names[tab]);
    } else if (id == B_SW_RIGHT) {
        if (sw_running) {
            sw_base += k86_uptime_ms() - sw_started;
            sw_running = 0;
        } else {
            sw_started = k86_uptime_ms();
            sw_running = 1;
        }
        k86_logf("clock: stopwatch %s", sw_running ? "start" : "stop");
    } else if (id == B_SW_LEFT) {
        if (sw_running && nlaps < MAX_LAPS) {
            laps[nlaps] = sw_base + k86_uptime_ms() - sw_started;
            sw_text(text, laps[nlaps] - (nlaps ? laps[nlaps - 1] : 0));
            nlaps++;
            k86_logf("clock: stopwatch lap %d %s", nlaps, text);
        } else if (!sw_running) {
            sw_base = 0;
            nlaps = 0;
            k86_log("clock: stopwatch reset");
        }
    } else if (id >= B_DIGIT && id < B_DIGIT + 10) {
        int n = (int)strlen(tm_digits);
        if (n < 6 && !(n == 0 && id == B_DIGIT)) {      /* no leading 0 */
            tm_digits[n] = (char)('0' + id - B_DIGIT);
            tm_digits[n + 1] = '\0';
        }
    } else if (id == B_DEL) {
        int n = (int)strlen(tm_digits);
        if (n)
            tm_digits[n - 1] = '\0';
    } else if (id == B_START) {
        uint32_t secs = tm_typed_secs();
        if (!secs)
            return;
        tm_end = k86_uptime_ms() + secs * 1000;
        tm_state = TM_RUNNING;
        hms_text(text, secs);
        k86_logf("clock: timer start %s", text);
    } else if (id == B_PAUSE) {
        if (tm_state == TM_RUNNING) {
            tm_left = tm_left_ms();
            tm_state = TM_PAUSED;
        } else {
            tm_end = k86_uptime_ms() + tm_left;
            tm_state = TM_RUNNING;
        }
        k86_logf("clock: timer %s", tm_state == TM_RUNNING ? "resume" : "pause");
    } else if (id == B_CANCEL || id == B_DISMISS) {
        tm_state = TM_SETTING;
        k86_log(id == B_CANCEL ? "clock: timer cancel" : "clock: timer stop");
    } else if (id >= B_ALARM && id < B_ALARM + MAX_ALARMS) {
        edit(id - B_ALARM);
    } else if (id >= B_SWITCH && id < B_SWITCH + MAX_ALARMS) {
        struct alarm *a = &alarms[id - B_SWITCH];
        a->on = !a->on;
        hm_text(text, a->h, a->m);
        k86_logf("clock: alarm %s %s", text, a->on ? "on" : "off");
        save_alarms();
    } else if (id == B_ADD) {
        edit(MAX_ALARMS);
    } else if (repeats(id)) {
        step(id);
    } else if (id == B_SAVE) {
        if (editing == MAX_ALARMS)
            alarms[nalarms++] = (struct alarm){ edit_h, edit_m, 1, 0, 0 };
        else
            alarms[editing] = (struct alarm){ edit_h, edit_m, 1, 0, 0 };
        hm_text(text, edit_h, edit_m);
        k86_logf("clock: alarm set %s", text);
        sort_alarms();
        save_alarms();
        stop_editing();
    } else if (id == B_DELETE && editing >= 0 && editing < nalarms) {
        hm_text(text, alarms[editing].h, alarms[editing].m);
        for (int i = editing; i < nalarms - 1; i++)
            alarms[i] = alarms[i + 1];
        nalarms--;
        k86_logf("clock: alarm delete %s", text);
        save_alarms();
        stop_editing();
    } else if (id == B_SNOOZE) {
        int m = ring_h * 60 + ring_m + SNOOZE_MIN;
        if (have_time)
            m = now.hour * 60 + now.minute + SNOOZE_MIN;
        snooze_h = m / 60 % 24;
        snooze_m = m % 60;
        snoozing = 1;
        ringing = 0;
        hm_text(text, snooze_h, snooze_m);
        k86_logf("clock: snooze until %s", text);
        header();
    } else if (id == B_STOP_RING) {
        if (ringing == 2)
            tm_state = TM_SETTING;
        ringing = 0;
        k86_log("clock: alarm stopped");
        header();
    }
}

/* --- events ------------------------------------------------------------------------------------ */

static void pointer(const struct k86_event *ev)
{
    struct wg_pointer p = wg_pointer_make(ev->x, ev->y, ev->buttons, ev->changed, ev->time_ms);
    if (p.down) {
        pressed = button_at(p.x, p.y);
        pressed_ms = k86_uptime_ms();
        repeat_at = pressed_ms + REPEAT_DELAY_MS;
        if (repeats(pressed))
            step(pressed);                  /* at once, then it repeats while held */
        set_rate();
        if (pressed >= 0)
            draw();
        return;
    }
    if (!p.up || pressed < 0)
        return;
    int was = pressed;
    pressed = -1;
    set_rate();
    if (was == button_at(p.x, p.y) && !repeats(was))
        act(was);
    draw();
}

static void key(const struct key_event *k)
{
    if (!k->pressed)
        return;
    if (k->key == KEY_ESC) {                /* the phone's Back */
        if (ringing)
            act(B_STOP_RING);
        else if (editing >= 0)
            stop_editing();
    } else if (ringing || editing >= 0) {
        return;
    } else if (tab == T_TIMER && tm_state == TM_SETTING && k->ascii >= '0' && k->ascii <= '9') {
        act(B_DIGIT + k->ascii - '0');
    } else if (tab == T_TIMER && tm_state == TM_SETTING && k->key == KEY_BACKSPACE) {
        act(B_DEL);
    } else if (tab == T_TIMER && k->key == KEY_ENTER) {
        act(tm_state == TM_SETTING ? B_START : tm_state == TM_DONE ? B_DISMISS : B_PAUSE);
    } else if (tab == T_STOPWATCH && k->key == KEY_SPACE) {
        act(B_SW_RIGHT);
    } else if (tab == T_STOPWATCH && k->ascii == 'l') {
        act(B_SW_LEFT);
    } else if (k->key == KEY_LEFT && tab > 0) {
        act(B_TAB + tab - 1);
    } else if (k->key == KEY_RIGHT && tab + 1 < NTABS) {
        act(B_TAB + tab + 1);
    } else {
        return;
    }
    set_rate();
    draw();
}

static void tick(void)
{
    int before = have_time ? now.minute : -1;
    read_time();
    check_alarms();
    if (tm_state == TM_RUNNING && !tm_left_ms()) {
        tm_state = TM_DONE;
        ring(2, 0, 0);
    }
    uint32_t t = k86_uptime_ms();
    if (pressed >= 0 && repeats(pressed) && (int32_t)(t - repeat_at) >= 0) {
        step(pressed);
        repeat_at = t + REPEAT_MS;
    }
    set_rate();
    int moving = sw_running || tm_state == TM_RUNNING || (pressed >= 0 && repeats(pressed));
    if (moving || ringing || (tab == T_ALARM && now.minute != before))
        draw();
}

int main(void)
{
    if (k86_window_open(&win))
        return 1;
    canvas = (struct gfx_surface){ win.canvas, win.w, win.h, win.stride };
    home_vol = k86_home(home_dir);
    if (home_vol < 0)
        home_vol = -1;
    read_time();
    load_alarms();
    sort_alarms();
    header();
    set_rate();
    draw();
    k86_log("clock: open");
    for (;;) {
        struct k86_event ev;
        k86_back_wanted(editing >= 0 || ringing);
        if (k86_wait_event(&ev))
            return 1;
        switch (ev.type) {
        case K86_EVENT_POINTER: pointer(&ev); break;
        case K86_EVENT_KEY:     key(&ev.key); break;
        case K86_EVENT_TIMER:   tick(); break;
        case K86_EVENT_HEADER:
            if (ev.id == BACK_ID) {
                stop_editing();
                draw();
            }
            break;
        case K86_EVENT_THEME:   draw(); break;
        case K86_EVENT_RESIZE:  canvas.w = ev.x; canvas.h = ev.y; draw(); break;
        case K86_EVENT_CLOSE:   return 0;
        default:                break;
        }
    }
}
