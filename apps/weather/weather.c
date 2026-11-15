/* Weather: today's weather and the next three days for a city, from open-meteo.com (no key, no account).
 * Change picks the city from a list; Refresh asks again. The numbers come from a small helper, this same
 * program started as `weather --fetch` (no window): it asks the internet, writes /dev/shm/lkx-weather and
 * exits, so a slow network never holds the screen. The home screen's Weather widget starts it the same way
 * and shows what it left there. The city is kept in /extras/weather.cfg.
 *
 * libcurl is loaded with dlopen, as R3lay does. Temperatures are kept in tenths of a degree: no floating
 * point. */
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"
#include "kernel/wxicon.h"
#include "sdk/kern86.h"

#define STATE_FILE "/dev/shm/lkx-weather"
#define STATE_TMP "/dev/shm/lkx-weather.tmp"
#define CONFIG_FILE "/extras/weather.cfg"
#define STALE_S 1200                    /* data older than this is asked for again */
#define DAYS 4

struct city {
    const char *name;
    int lat, lon;                       /* ten-thousandths of a degree */
};
static const struct city cities[] = {
    { "London", 515072, -1276 },        { "Manchester", 534808, -22426 },  { "Birmingham", 524862, -18904 },
    { "Leeds", 538008, -15491 },        { "Liverpool", 534084, -29916 },   { "Bristol", 514545, -25879 },
    { "Sheffield", 533811, -14701 },    { "Newcastle", 549783, -16178 },   { "Nottingham", 529548, -11581 },
    { "Glasgow", 558642, -42518 },      { "Edinburgh", 559533, -31883 },   { "Aberdeen", 571497, -20943 },
    { "Cardiff", 514816, -31791 },      { "Belfast", 545973, -59301 },     { "Dublin", 533498, -62603 },
    { "Paris", 488566, 23522 },         { "Berlin", 525200, 134050 },      { "Madrid", 404168, -37038 },
    { "Rome", 419028, 124964 },         { "New York", 407128, -740060 },   { "Los Angeles", 340522, -1182437 },
    { "Toronto", 436532, -793832 },     { "Tokyo", 356762, 1396503 },      { "Dubai", 252048, 552708 },
    { "Delhi", 286139, 772090 },        { "Lagos", 65244, 33792 },         { "Cape Town", -339249, 184241 },
    { "Sydney", -338688, 1512093 },
};
#define NCITIES ((int)(sizeof(cities) / sizeof(cities[0])))

struct wx {
    int ok;
    long when;                          /* when it was fetched (time()) */
    char city[32];
    int temp, code, day;                /* tenths of a degree; WMO code; 1 daytime */
    int hi[DAYS], lo[DAYS], dcode[DAYS];
};

static int cur_lat = 515072, cur_lon = -1276;
static char cur_city[32] = "London";

/* --- the city, kept ------------------------------------------------------------------------------ */

static void load_config(void)
{
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f)
        return;
    int lat, lon;
    char name[32];
    if (fscanf(f, "%d %d %31[^\n]", &lat, &lon, name) == 3) {
        cur_lat = lat;
        cur_lon = lon;
        memcpy(cur_city, name, sizeof(cur_city));
    }
    fclose(f);
}

static void save_config(void)
{
    FILE *f = fopen(CONFIG_FILE, "w");
    if (!f)
        return;
    fprintf(f, "%d %d %s\n", cur_lat, cur_lon, cur_city);
    fclose(f);
}

/* --- the numbers, as open-meteo sends them --------------------------------------------------------- */

/* "-3.4" -> -34 (tenths), "12" -> 120. The characters used, 0 if it isn't a number. */
static int parse_tenths(const char *p, int *out)
{
    const char *s = p;
    int neg = 0, v = 0, frac = 0;
    if (*p == '-') {
        neg = 1;
        p++;
    }
    if (*p < '0' || *p > '9')
        return 0;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (*p++ - '0');
    if (*p == '.') {
        p++;
        if (*p >= '0' && *p <= '9')
            frac = *p - '0';
        while (*p >= '0' && *p <= '9')
            p++;
    }
    v = v * 10 + frac;
    *out = neg ? -v : v;
    return (int)(p - s);
}

/* The array `key` of numbers (tenths) inside `where`, up to n of them. */
static int parse_array(const char *where, const char *key, int *out, int n)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":[", key);
    const char *p = strstr(where, pat);
    if (!p)
        return 0;
    p += strlen(pat);
    int k = 0;
    while (k < n) {
        int len = parse_tenths(p, &out[k]);
        if (!len)
            break;
        k++;
        p += len;
        if (*p != ',')
            break;
        p++;
    }
    return k;
}

static int parse_value(const char *where, const char *key, int *out)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(where, pat);
    return p && parse_tenths(p + strlen(pat), out) ? 1 : 0;
}

static int parse_reply(const char *js, struct wx *w)
{
    const char *cur = strstr(js, "\"current\":{"), *day = strstr(js, "\"daily\":{");
    int code10, day10;
    if (!cur || !day || !parse_value(cur, "temperature_2m", &w->temp) || !parse_value(cur, "weather_code", &code10))
        return -1;
    w->code = code10 / 10;
    w->day = parse_value(cur, "is_day", &day10) ? day10 / 10 : 1;
    int codes[DAYS];
    int nh = parse_array(day, "temperature_2m_max", w->hi, DAYS);
    int nl = parse_array(day, "temperature_2m_min", w->lo, DAYS);
    int nc = parse_array(day, "weather_code", codes, DAYS);
    if (nh < 1 || nl < 1 || nc < 1)
        return -1;
    for (int i = 0; i < DAYS; i++)
        w->dcode[i] = i < nc ? codes[i] / 10 : w->code;
    w->ok = 1;
    return 0;
}

/* --- the file the shell's widget reads ------------------------------------------------------------- */

static void write_state(const struct wx *w)
{
    FILE *f = fopen(STATE_TMP, "w");
    if (!f)
        return;
    fprintf(f, "ok %ld\n%s\n%d %d %d\n", w->when, w->city, w->temp, w->code, w->day);
    for (int i = 0; i < DAYS; i++)
        fprintf(f, "%d %d %d\n", w->hi[i], w->lo[i], w->dcode[i]);
    fclose(f);
    rename(STATE_TMP, STATE_FILE);
}

static int read_state(struct wx *w)
{
    memset(w, 0, sizeof(*w));
    FILE *f = fopen(STATE_FILE, "r");
    if (!f)
        return -1;
    int bad = 0;
    char head[8];
    if (fscanf(f, "%7s %ld\n", head, &w->when) != 2 || strcmp(head, "ok"))
        bad = 1;
    if (!bad && !fgets(w->city, sizeof(w->city), f))
        bad = 1;
    if (!bad) {
        size_t n = strlen(w->city);
        while (n && (w->city[n - 1] == '\n' || w->city[n - 1] == '\r'))
            w->city[--n] = '\0';
        if (fscanf(f, "%d %d %d", &w->temp, &w->code, &w->day) != 3)
            bad = 1;
    }
    for (int i = 0; !bad && i < DAYS; i++)
        if (fscanf(f, "%d %d %d", &w->hi[i], &w->lo[i], &w->dcode[i]) != 3)
            bad = 1;
    fclose(f);
    w->ok = !bad;
    return bad ? -1 : 0;
}

/* --- asking the internet (the helper, `weather --fetch`) ---------------------------------------------- */

struct body {
    char *p;
    size_t n, max;
};

static size_t on_data(char *p, size_t size, size_t count, void *user)
{
    struct body *b = user;
    size_t n = size * count;
    if (b->n + n + 1 > b->max)
        return 0;
    memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = '\0';
    return n;
}

static int http_get(const char *url, char *out, size_t max)
{
    static const char *const names[] = { "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so" };
    void *lib = 0;
    for (unsigned i = 0; !lib && i < sizeof(names) / sizeof(names[0]); i++)
        lib = dlopen(names[i], RTLD_NOW);
    if (!lib)
        return -1;
    int (*global_init)(long);
    void *(*easy_init)(void);
    int (*easy_setopt)(void *, long, ...);
    int (*easy_perform)(void *);
    int (*easy_getinfo)(void *, int, ...);
    void (*easy_cleanup)(void *);
#define LOAD(var, name) if (!(*(void **)&var = dlsym(lib, name))) return -1
    LOAD(global_init, "curl_global_init");
    LOAD(easy_init, "curl_easy_init");
    LOAD(easy_setopt, "curl_easy_setopt");
    LOAD(easy_perform, "curl_easy_perform");
    LOAD(easy_getinfo, "curl_easy_getinfo");
    LOAD(easy_cleanup, "curl_easy_cleanup");
#undef LOAD
    global_init(3L);
    void *h = easy_init();
    if (!h)
        return -1;
    struct body b = { out, 0, max };
    out[0] = '\0';
    easy_setopt(h, 10002L, url);                /* URL */
    easy_setopt(h, 20011L, on_data);            /* WRITEFUNCTION */
    easy_setopt(h, 10001L, &b);                 /* WRITEDATA */
    easy_setopt(h, 13L, 20L);                   /* TIMEOUT */
    easy_setopt(h, 78L, 8L);                    /* CONNECTTIMEOUT */
    easy_setopt(h, 99L, 1L);                    /* NOSIGNAL */
    easy_setopt(h, 10018L, "LiteKernGo-Weather/1.0");
    easy_setopt(h, 10097L, "/etc/ssl/certs");   /* CAPATH */
    int res = easy_perform(h);
    long status = 0;
    if (!res)
        easy_getinfo(h, 0x200002, &status);
    easy_cleanup(h);
    return !res && status == 200 ? 0 : -1;
}

static void fmt_deg(char *out, int v)
{
    int a = v < 0 ? -v : v;
    sprintf(out, "%s%d.%04d", v < 0 ? "-" : "", a / 10000, a % 10000);
}

/* `weather --fetch`: no window. 0 when it left fresh numbers. */
static int fetch_main(void)
{
    static char reply[16384];
    char url[400], lat[16], lon[16];
    load_config();
    fmt_deg(lat, cur_lat);
    fmt_deg(lon, cur_lon);
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&current=temperature_2m,weather_code,is_day"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min&forecast_days=%d&timezone=auto",
             lat, lon, DAYS);
    struct wx w;
    memset(&w, 0, sizeof(w));
    if (http_get(url, reply, sizeof(reply)) || parse_reply(reply, &w))
        return 1;
    memcpy(w.city, cur_city, sizeof(w.city));
    w.when = (long)time(0);
    write_state(&w);
    return 0;
}

/* --- the window ------------------------------------------------------------------------------------ */

static struct k86_window win;
static struct gfx_surface canvas;
static struct wx wx;
static pid_t fetch_pid;                 /* the helper that's asking, or 0 */
static uint32_t fetch_started;
static int picking;                     /* the city list is up */
static struct wg_list list;
static struct wg_button b_change, b_refresh;
static int fetch_failed;

static const char *const day_names[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

static void row(void *ctx, int i, struct wg_row *out)
{
    (void)ctx;
    out->name = cities[i].name;
    out->detail = 0;
    out->icon = WG_ICON_NONE;
}

static int whole(int tenths)            /* tenths of a degree -> whole degrees, rounded */
{
    return tenths >= 0 ? (tenths + 5) / 10 : -((-tenths + 5) / 10);
}

static void start_fetch(void)
{
    if (fetch_pid > 0)
        return;
    char self[256];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0)
        return;
    self[n] = '\0';
    pid_t pid = fork();
    if (pid < 0)
        return;
    if (!pid) {
        for (int fd = 3; fd < 64; fd++)         /* not the app's link to the shell */
            close(fd);
        execl(self, self, "--fetch", (char *)0);
        _exit(127);
    }
    fetch_pid = pid;
    fetch_started = k86_uptime_ms();
    fetch_failed = 0;
}

static void layout(void)
{
    const struct theme *t = theme_get();
    int bw = 168;
    b_refresh = (struct wg_button){ .r = { canvas.w - 24 - bw, 20, bw, t->button_size }, .label = fetch_pid > 0 ? "Updating" : "Refresh",
                                    .style = WG_BUTTON_FLAT };
    b_change = (struct wg_button){ .r = { canvas.w - 24 - 2 * bw - 12, 20, bw, t->button_size }, .label = "Change",
                                   .style = WG_BUTTON_FLAT };
    int top = 20 + t->button_size + 20;
    list.r = (struct gfx_rect){ 24, top, canvas.w - 48, canvas.h - top - 24 };
}

static void draw_temp(int x, int y, int tenths, enum text_style st, uint32_t col)
{
    char s[24];
    snprintf(s, sizeof(s), "%d\xb0", whole(tenths));
    text_draw(&canvas, x, y, s, st, col);
}

static void draw_all(void)
{
    const struct theme *t = theme_get();
    gfx_fill_rect(&canvas, 0, 0, canvas.w, canvas.h, t->window_bg);
    if (picking) {
        wg_label(&canvas, 24, 24, "Choose a city", WG_TEXT_HEADING);
        wg_list_draw(&canvas, &list);
        k86_present(0, 0, canvas.w, canvas.h);
        return;
    }
    char title[48];
    snprintf(title, sizeof(title), "%s", wx.ok ? wx.city : cur_city);
    wg_text_fit(&canvas, 24, 24, title, canvas.w - 24 - 2 * 168 - 36 - 24, t->fg);
    wg_button_draw(&canvas, &b_change);
    wg_button_draw(&canvas, &b_refresh);
    if (!wx.ok) {
        wg_label_centred(&canvas, canvas.w / 2, 300, fetch_pid > 0 ? "Getting the weather..." : "No weather yet", WG_TEXT_TITLE);
        if (fetch_failed)
            wg_label_centred(&canvas, canvas.w / 2, 360, "Check the Wi-Fi, then Refresh", WG_TEXT_DIM);
        k86_present(0, 0, canvas.w, canvas.h);
        return;
    }
    int cat = wx_category(wx.code);
    gfx_fill_round_rect(&canvas, 24, 120, canvas.w - 48, 400, 40, t->button_bg);
    wx_draw(&canvas, 190, 290, 100, cat, !wx.day);
    draw_temp(360, 190, wx.temp, TEXT_LARGE, t->fg);
    text_draw(&canvas, 360, 270, wx_text(cat), TEXT_HEADING, t->fg);
    char hl[64];
    snprintf(hl, sizeof(hl), "High %d\xb0   Low %d\xb0", whole(wx.hi[0]), whole(wx.lo[0]));
    text_draw(&canvas, 360, 330, hl, TEXT_BODY, gfx_mix(t->fg, t->button_bg, 170));
    struct k86_time now;
    int wd = k86_time(&now) ? 0 : now.weekday;
    for (int i = 1; i < DAYS; i++) {
        int y = 560 + (i - 1) * 120;
        gfx_fill_round_rect(&canvas, 24, y, canvas.w - 48, 104, 28, t->button_bg);
        text_draw(&canvas, 56, y + 30, i == 1 ? "Tomorrow" : day_names[(wd + i) % 7], TEXT_HEADING, t->fg);
        wx_draw(&canvas, 400, y + 52, 34, wx_category(wx.dcode[i]), 0);
        char hi[24], lo[24];
        snprintf(hi, sizeof(hi), "%d\xb0", whole(wx.hi[i]));
        snprintf(lo, sizeof(lo), "%d\xb0", whole(wx.lo[i]));
        text_draw(&canvas, canvas.w - 56 - text_width(lo, TEXT_HEADING), y + 30, lo, TEXT_HEADING,
                  gfx_mix(t->fg, t->button_bg, 150));
        text_draw(&canvas, canvas.w - 56 - 90 - text_width(hi, TEXT_HEADING), y + 30, hi, TEXT_HEADING, t->fg);
    }
    char foot[64];
    long age = (long)time(0) - wx.when;
    if (age < 90)
        snprintf(foot, sizeof(foot), "Updated just now");
    else
        snprintf(foot, sizeof(foot), "Updated %ld min ago", age / 60);
    wg_label_centred(&canvas, canvas.w / 2, canvas.h - 60, foot, WG_TEXT_SMALL);
    k86_present(0, 0, canvas.w, canvas.h);
}

static void reload(void)
{
    read_state(&wx);
}

static void need_fresh(void)
{
    long age = (long)time(0) - wx.when;
    if (!wx.ok || age > STALE_S || strcmp(wx.city, cur_city))
        start_fetch();
}

static void set_timer(void)
{
    k86_timer_set(fetch_pid > 0 ? 500 : wg_list_gliding(&list) ? 16 : 30000);
}

static void on_timer(void)
{
    if (fetch_pid > 0) {
        int st = 0;
        if (waitpid(fetch_pid, &st, WNOHANG) == fetch_pid) {
            fetch_pid = 0;
            fetch_failed = !(WIFEXITED(st) && WEXITSTATUS(st) == 0);
            read_state(&wx);
        } else if (k86_uptime_ms() - fetch_started > 30000) {
            kill(fetch_pid, SIGKILL);
            fetch_failed = 1;
        }
        layout();
        draw_all();
    } else if (picking && wg_list_gliding(&list)) {
        wg_list_tick(&list, k86_uptime_ms());
        draw_all();
    } else {
        reload();
        need_fresh();
        layout();
        draw_all();
    }
    set_timer();
}

static void choose(int i)
{
    cur_lat = cities[i].lat;
    cur_lon = cities[i].lon;
    snprintf(cur_city, sizeof(cur_city), "%s", cities[i].name);
    save_config();
    picking = 0;
    k86_back_wanted(0);
    memset(&wx, 0, sizeof(wx));
    unlink(STATE_FILE);
    start_fetch();
    layout();
    draw_all();
    set_timer();
}

static void on_pointer(const struct k86_event *ev)
{
    struct wg_pointer p = wg_pointer_make(ev->x, ev->y, ev->buttons, ev->changed, ev->time_ms);
    if (picking) {
        enum wg_list_result r = wg_list_pointer(&list, &p);
        if (r == WG_LIST_ACTIVATE && list.selected >= 0) {
            choose(list.selected);
            return;
        }
        if (r != WG_LIST_NONE || list.dirty)
            draw_all();
        set_timer();
        return;
    }
    if (wg_button_pointer(&b_change, &p)) {
        picking = 1;
        k86_back_wanted(1);
        draw_all();
    } else if (wg_button_pointer(&b_refresh, &p)) {
        start_fetch();
        layout();
        draw_all();
        set_timer();
    } else if (b_change.dirty || b_refresh.dirty) {
        draw_all();
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--fetch"))
        return fetch_main();
    if (k86_window_open(&win))
        return 1;
    canvas = (struct gfx_surface){ win.canvas, win.w, win.h, win.stride };
    list = (struct wg_list){ .selected = -1, .hover = -1, .row = row, .last_click_row = -1 };
    wg_list_set_count(&list, NCITIES);
    load_config();
    read_state(&wx);
    need_fresh();
    layout();
    draw_all();
    set_timer();
    for (;;) {
        struct k86_event ev;
        if (k86_wait_event(&ev))
            return 1;
        switch (ev.type) {
        case K86_EVENT_POINTER:
            on_pointer(&ev);
            break;
        case K86_EVENT_TIMER:
            on_timer();
            break;
        case K86_EVENT_SCROLL:
            if (picking && wg_list_scroll(&list, ev.x, ev.y, ev.id))
                draw_all();
            break;
        case K86_EVENT_KEY:
            if (picking && ev.key.pressed && ev.key.key == KEY_ESC) {
                picking = 0;
                k86_back_wanted(0);
                layout();
                draw_all();
            }
            break;
        case K86_EVENT_THEME:
            layout();
            draw_all();
            break;
        case K86_EVENT_RESIZE:
            canvas.w = ev.x;
            canvas.h = ev.y;
            layout();
            draw_all();
            break;
        case K86_EVENT_CLOSE:
            if (fetch_pid > 0)
                kill(fetch_pid, SIGKILL);
            return 0;
        default:
            break;
        }
    }
}
