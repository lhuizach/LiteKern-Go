/* Music's Online tab (see online.h). Songs come from Audius's public API (api.audius.co): Trending, a
 * genre's trending list, or a search; a song is streamed as an MP3, downloaded into a file as it plays.
 * libcurl is loaded with dlopen, as R3lay and Weather do. */
#include "apps/music/online.h"

#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kernel/text.h"
#include "kernel/theme.h"
#include "sdk/kern86.h"

#define API "https://api.audius.co/v1"
#define APP_NAME "LiteKernGo"
#define RESULTS_FILE "/dev/shm/lkx-music-results.json"
#define ONLINE_MAX 24
#define JSON_MAX (768 * 1024)

/* --- the helper programs: one request, written into a file ------------------------------------------ */

static size_t on_data(char *p, size_t size, size_t count, void *user)
{
    FILE *f = user;
    size_t n = fwrite(p, size, count, f);
    fflush(f);                          /* a song is played while it comes: the player sees each piece */
    return n * size;
}

static int fetch_to_file(const char *url, const char *path)
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
    FILE *f = fopen(path, "wb");
    if (!h || !f)
        return -1;
    easy_setopt(h, 10002L, url);                /* URL */
    easy_setopt(h, 20011L, on_data);            /* WRITEFUNCTION */
    easy_setopt(h, 10001L, f);                  /* WRITEDATA */
    easy_setopt(h, 52L, 1L);                    /* FOLLOWLOCATION: the song comes from another address */
    easy_setopt(h, 68L, 5L);                    /* MAXREDIRS */
    easy_setopt(h, 13L, 1800L);                 /* TIMEOUT: a long song on a slow line */
    easy_setopt(h, 78L, 10L);                   /* CONNECTTIMEOUT */
    easy_setopt(h, 99L, 1L);                    /* NOSIGNAL */
    easy_setopt(h, 10018L, "LiteKernGo-Music/1.0");
    easy_setopt(h, 10097L, "/etc/ssl/certs");   /* CAPATH */
    int res = easy_perform(h);
    long status = 0;
    if (!res)
        easy_getinfo(h, 0x200002, &status);
    easy_cleanup(h);
    fclose(f);
    return !res && status == 200 ? 0 : -1;
}

int online_helper_main(int argc, char **argv)
{
    if (argc >= 4 && (!strcmp(argv[1], "--api") || !strcmp(argv[1], "--dl")))
        return fetch_to_file(argv[2], argv[3]) ? 1 : 0;
    return -1;
}

static pid_t spawn(const char *mode, const char *url, const char *out)
{
    char self[256];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0)
        return -1;
    self[n] = '\0';
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (!pid) {
        for (int fd = 3; fd < 64; fd++)         /* not the app's link to the shell */
            close(fd);
        execl(self, self, mode, url, out, (char *)0);
        _exit(127);
    }
    return pid;
}

pid_t online_download(const char *id, const char *file)
{
    char url[200];
    snprintf(url, sizeof(url), API "/tracks/%s/stream?app_name=" APP_NAME, id);
    return spawn("--dl", url, file);
}

/* --- reading what Audius sent --------------------------------------------------------------------------- */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
        p++;
    return p;
}

static const char *skip_string(const char *p)       /* p is on the opening quote */
{
    p++;
    while (*p && *p != '"')
        p += *p == '\\' && p[1] ? 2 : 1;
    return *p ? p + 1 : p;
}

static const char *skip_value(const char *p)
{
    p = skip_ws(p);
    if (*p == '"')
        return skip_string(p);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p = skip_ws(p + 1);
        while (*p && *p != close) {
            if (close == '}') {                     /* a key, a colon */
                p = skip_string(skip_ws(p));
                p = skip_ws(p);
                if (*p == ':')
                    p++;
            }
            p = skip_value(p);
            p = skip_ws(p);
            if (*p == ',')
                p = skip_ws(p + 1);
        }
        return *p ? p + 1 : p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']')
        p++;
    return p;
}

/* A string value into out (ASCII only: the others become ?). p is on the opening quote. */
static void read_string(const char *p, char *out, int max)
{
    int n = 0, last_q = 0;
    p++;
    while (*p && *p != '"' && n < max - 1) {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            char e = *p++;
            if (e == 'u') {
                p += 4;
                c = '?';
            } else {
                c = e == 'n' || e == 't' ? ' ' : (unsigned char)e;
            }
        }
        if (c >= 0x80) {
            if (last_q)
                continue;                           /* a run of foreign letters: one ? */
            c = '?';
        }
        if (c < 0x20)
            c = ' ';
        last_q = c == '?';
        out[n++] = (char)c;
    }
    out[n] = '\0';
}

struct track {
    char id[16];
    char title[64];
    char artist[40];
    int dur;
};

static int key_is(const char *p, const char *name)  /* the key at p ("...") is name */
{
    size_t n = strlen(name);
    return !strncmp(p + 1, name, n) && p[1 + n] == '"';
}

/* Fills tracks from {"data":[ {...}, ... ]}. How many. */
static int parse_tracks(const char *js, struct track *out, int max)
{
    const char *p = strstr(js, "\"data\":[");
    int n = 0;
    if (!p)
        return 0;
    p = skip_ws(p + 8);
    while (*p == '{' && n < max) {
        struct track t;
        memset(&t, 0, sizeof(t));
        int streamable = 1;
        const char *q = skip_ws(p + 1);
        while (*q && *q != '}') {
            const char *key = skip_ws(q);
            q = skip_ws(skip_string(key));
            if (*q == ':')
                q++;
            q = skip_ws(q);
            if (key_is(key, "id") && *q == '"') {
                read_string(q, t.id, sizeof(t.id));
            } else if (key_is(key, "title") && *q == '"') {
                read_string(q, t.title, sizeof(t.title));
            } else if (key_is(key, "duration")) {
                t.dur = atoi(q);
            } else if (key_is(key, "is_streamable")) {
                streamable = !strncmp(q, "true", 4);
            } else if (key_is(key, "user") && *q == '{') {
                const char *u = skip_ws(q + 1);
                while (*u && *u != '}') {
                    const char *uk = skip_ws(u);
                    u = skip_ws(skip_string(uk));
                    if (*u == ':')
                        u++;
                    u = skip_ws(u);
                    if (key_is(uk, "name") && *u == '"')
                        read_string(u, t.artist, sizeof(t.artist));
                    u = skip_ws(skip_value(u));
                    if (*u == ',')
                        u = skip_ws(u + 1);
                }
            }
            q = skip_ws(skip_value(q));
            if (*q == ',')
                q = skip_ws(q + 1);
        }
        if (t.id[0] && t.title[0] && t.dur > 0 && streamable)
            out[n++] = t;
        p = skip_ws(skip_value(p));
        if (*p == ',')
            p = skip_ws(p + 1);
    }
    return n;
}

/* --- the tab ---------------------------------------------------------------------------------------------- */

static const char *const genres[] = {
    "Electronic", "Hip-Hop/Rap", "Pop", "Rock", "Lo-Fi", "Ambient", "Jazz", "Classical", "Folk", "R&B/Soul",
    "Reggae", "Country", "Metal", "Alternative", "Acoustic", "Funk", "Latin", "Dancehall", "Trap", "House",
    "Techno", "Drum & Bass", "Dubstep", "Trance", "Hyperpop", "Soundtrack", "Blues", "Punk",
};
#define NGENRES ((int)(sizeof(genres) / sizeof(genres[0])))

static struct gfx_surface *cv;
static struct wg_entry search;
static struct wg_button b_search, b_trending, b_genres;
static struct wg_list rlist, glist;             /* the results, the genres */
static struct track tracks[ONLINE_MAX];
static int ntracks;
static int showing_genres;
static char note[64];
static char heading[48];                        /* what the list is: "Trending", "Lo-Fi", "Search: ..." */
static pid_t api_pid;
static uint32_t api_started;
static int have_pick;
static struct online_pick pick;
static int top_y, bottom_y;
static char json[JSON_MAX];

static void result_row(void *ctx, int i, struct wg_row *out)
{
    (void)ctx;
    static char detail[48];
    out->name = tracks[i].title;
    snprintf(detail, sizeof(detail), "%s", tracks[i].artist);
    out->detail = detail;
    out->icon = WG_ICON_NONE;
}

static void genre_row(void *ctx, int i, struct wg_row *out)
{
    (void)ctx;
    out->name = genres[i];
    out->detail = 0;
    out->icon = WG_ICON_NONE;
}

void online_init(struct gfx_surface *canvas)
{
    cv = canvas;
    rlist = (struct wg_list){ .selected = -1, .hover = -1, .row = result_row, .last_click_row = -1 };
    glist = (struct wg_list){ .selected = -1, .hover = -1, .row = genre_row, .last_click_row = -1 };
    wg_list_set_count(&glist, NGENRES);
    wg_entry_set(&search, "");
    snprintf(heading, sizeof(heading), "Trending");
    note[0] = '\0';
}

void online_layout(int top, int bottom)
{
    const struct theme *t = theme_get();
    int bh = t->button_size, bw = 150;
    top_y = top;
    bottom_y = bottom;
    search.r = (struct gfx_rect){ 24, top, cv->w - 48 - bw - 12, bh };
    b_search = (struct wg_button){ .r = { cv->w - 24 - bw, top, bw, bh }, .label = "Search", .style = WG_BUTTON_SUGGESTED };
    int y2 = top + bh + 12;
    b_trending = (struct wg_button){ .r = { 24, y2, 190, bh }, .label = "Trending", .style = WG_BUTTON_FLAT };
    b_genres = (struct wg_button){ .r = { 24 + 202, y2, 190, bh }, .label = "Genres", .style = WG_BUTTON_FLAT };
    int ly = y2 + bh + 12;
    rlist.r = glist.r = (struct gfx_rect){ 24, ly, cv->w - 48, bottom - ly };
}

void online_draw(void)
{
    const struct theme *t = theme_get();
    gfx_fill_rect(cv, 0, top_y - 8, cv->w, bottom_y - top_y + 8, t->window_bg);
    wg_entry_draw(cv, &search);
    wg_button_draw(cv, &b_search);
    wg_button_draw(cv, &b_trending);
    wg_button_draw(cv, &b_genres);
    if (note[0])
        wg_label(cv, 24 + 404, b_trending.r.y + (b_trending.r.h - 28) / 2, note, WG_TEXT_DIM);
    if (showing_genres) {
        wg_list_draw(cv, &glist);
    } else if (ntracks) {
        wg_list_draw(cv, &rlist);
    } else {
        wg_label_centred(cv, cv->w / 2, rlist.r.y + 60, api_pid > 0 ? "Getting songs..." : "No songs yet", WG_TEXT_TITLE);
        if (!api_pid && note[0] == '\0')
            wg_label_centred(cv, cv->w / 2, rlist.r.y + 110, "Check the Wi-Fi, then try Trending", WG_TEXT_DIM);
    }
}

/* --- asking ------------------------------------------------------------------------------------------------- */

static void url_encode(const char *in, char *out, int max)
{
    static const char hex[] = "0123456789ABCDEF";
    int n = 0;
    for (; *in && n < max - 4; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            out[n++] = (char)c;
        } else {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = '\0';
}

static void ask(const char *url, const char *what)
{
    if (api_pid > 0) {
        kill(api_pid, SIGKILL);
        waitpid(api_pid, 0, 0);
    }
    unlink(RESULTS_FILE);
    api_pid = spawn("--api", url, RESULTS_FILE);
    api_started = k86_uptime_ms();
    snprintf(heading, sizeof(heading), "%s", what);
    snprintf(note, sizeof(note), api_pid > 0 ? "Searching..." : "Can't start");
    showing_genres = 0;
}

static void ask_trending(const char *genre)
{
    char url[300], enc[64];
    if (genre) {
        url_encode(genre, enc, sizeof(enc));
        snprintf(url, sizeof(url), API "/tracks/trending?genre=%s&limit=%d&app_name=" APP_NAME, enc, ONLINE_MAX);
    } else {
        snprintf(url, sizeof(url), API "/tracks/trending?limit=%d&app_name=" APP_NAME, ONLINE_MAX);
    }
    ask(url, genre ? genre : "Trending");
}

static void ask_search(void)
{
    if (!search.len)
        return;
    char url[400], enc[200], what[48];
    url_encode(search.text, enc, sizeof(enc));
    snprintf(url, sizeof(url), API "/tracks/search?query=%s&limit=%d&app_name=" APP_NAME, enc, ONLINE_MAX);
    snprintf(what, sizeof(what), "Search");
    ask(url, what);
}

int online_tick(void)
{
    if (api_pid <= 0) {
        if (!ntracks && !note[0] && !api_started)
            ask_trending(0);                    /* the first time the tab opens: what's trending */
        return 0;
    }
    int st = 0;
    if (waitpid(api_pid, &st, WNOHANG) != api_pid) {
        if (k86_uptime_ms() - api_started > 25000) {
            kill(api_pid, SIGKILL);
            waitpid(api_pid, 0, 0);
            api_pid = 0;
            snprintf(note, sizeof(note), "Too slow");
        }
        return 1;
    }
    api_pid = 0;
    if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
        snprintf(note, sizeof(note), "No connection");
        return 1;
    }
    FILE *f = fopen(RESULTS_FILE, "rb");
    size_t got = 0;
    if (f) {
        got = fread(json, 1, sizeof(json) - 1, f);
        fclose(f);
    }
    json[got] = '\0';
    ntracks = parse_tracks(json, tracks, ONLINE_MAX);
    wg_list_set_count(&rlist, ntracks);
    rlist.selected = -1;
    rlist.top = 0;
    rlist.pos = 0;
    snprintf(note, sizeof(note), "%s: %d songs", heading, ntracks);
    return 1;
}

int online_busy(void)
{
    return api_pid > 0 || (!ntracks && !note[0] && !api_started) || wg_list_gliding(&rlist) || wg_list_gliding(&glist);
}

int online_wants_keyboard(void)
{
    return search.focused;
}

/* --- events --------------------------------------------------------------------------------------------------- */

int online_pointer(const struct wg_pointer *p)
{
    int redraw = 0;
    enum wg_entry_result er = wg_entry_pointer(&search, p);
    if (er != WG_ENTRY_NONE || search.dirty)
        redraw = 1;
    if (wg_button_pointer(&b_search, p)) {
        search.focused = 0;
        ask_search();
        return 1;
    }
    if (wg_button_pointer(&b_trending, p)) {
        search.focused = 0;
        ask_trending(0);
        return 1;
    }
    if (wg_button_pointer(&b_genres, p)) {
        search.focused = 0;
        showing_genres = !showing_genres;
        return 1;
    }
    if (b_search.dirty || b_trending.dirty || b_genres.dirty)
        redraw = 1;
    if (showing_genres) {
        enum wg_list_result r = wg_list_pointer(&glist, p);
        if (r == WG_LIST_ACTIVATE && glist.selected >= 0) {
            ask_trending(genres[glist.selected]);
            return 1;
        }
        if (r != WG_LIST_NONE || glist.dirty)
            redraw = 1;
    } else if (ntracks) {
        enum wg_list_result r = wg_list_pointer(&rlist, p);
        if (r == WG_LIST_ACTIVATE && rlist.selected >= 0) {
            snprintf(pick.id, sizeof(pick.id), "%s", tracks[rlist.selected].id);
            snprintf(pick.title, sizeof(pick.title), "%s", tracks[rlist.selected].title);
            snprintf(pick.artist, sizeof(pick.artist), "%s", tracks[rlist.selected].artist);
            pick.dur = tracks[rlist.selected].dur;
            have_pick = 1;
            return 1;
        }
        if (r != WG_LIST_NONE || rlist.dirty)
            redraw = 1;
    }
    return redraw;
}

int online_key(const struct key_event *k)
{
    if (!search.focused)
        return 0;
    enum wg_entry_result r = wg_entry_key(&search, k);
    if (r == WG_ENTRY_ACTIVATE) {
        search.focused = 0;
        ask_search();
    } else if (r == WG_ENTRY_CANCEL) {
        search.focused = 0;
    }
    return 1;
}

int online_scroll(int x, int y, int notches)
{
    return wg_list_scroll(showing_genres ? &glist : &rlist, x, y, notches);
}

int online_take_pick(struct online_pick *out)
{
    if (!have_pick)
        return 0;
    have_pick = 0;
    *out = pick;
    return 1;
}

int online_neighbour(const char *id, int dir, struct online_pick *out)
{
    for (int i = 0; i < ntracks; i++)
        if (!strcmp(tracks[i].id, id)) {
            int j = i + dir;
            if (j < 0 || j >= ntracks)
                return 0;
            snprintf(out->id, sizeof(out->id), "%s", tracks[j].id);
            snprintf(out->title, sizeof(out->title), "%s", tracks[j].title);
            snprintf(out->artist, sizeof(out->artist), "%s", tracks[j].artist);
            out->dur = tracks[j].dur;
            wg_list_select(&rlist, j);
            return 1;
        }
    return 0;
}
