/* Music: plays the .wav files in the music folder, /extras/music on the phone (the big storage
 * area; copy files there over ssh for now). A list of tracks, and below it the player: the title,
 * the time, a bar to tap or drag to seek, Previous / Play-Pause / Next. The next track starts by itself.
 *
 * The sound goes out through sdk/audio.h (the loudspeaker, or the headphones when they're in),
 * at the volume of the volume keys. It keeps playing when you go to another app.
 *
 * Only .wav for now (8 or 16 bit, mono or stereo, any rate: it's resampled to 48 kHz here). A track
 * is read from the file as it plays, so it can be any length. MP3 would take far less space, and is
 * the next step. No floating point, no malloc. */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"
#include "sdk/audio.h"
#include "sdk/kern86.h"

#define MAX_TRACKS 128
#define MUSIC_DIR "/extras/music"
#define RBUF_FRAMES 8192                        /* source frames read from the file at a time */
#define OUT_RATE 48000
#define CHUNK 1024                              /* frames made at a time */
#define PLAYER_H 300                            /* the player's panel, at the bottom */

static struct k86_window win;
static struct gfx_surface canvas;
static const char *folder = MUSIC_DIR;

struct track {
    char name[K86_NAME_MAX];
    uint32_t size;
};
static struct track files[MAX_TRACKS];          /* the .wav files, sorted */
static int ntracks;
static struct wg_list list;
static struct wg_button b_prev, b_play, b_next, b_refresh;

/* The track that's loaded (-1: none) and where it is. */
static int cur = -1;
static FILE *fp;                                /* the open file */
static uint32_t data_off, pcm_bytes, nframes, src_rate;
static int src_channels, src_bits;
static uint64_t pos;                            /* the next source frame to make, 32.32 */
static uint64_t step;                           /* source frames per output frame, 32.32 */
static int playing, draining;
static char note[64];                           /* "Loading...", "Can't play this", or empty */

static int scrubbing, scrub_x;                  /* the finger on the bar */
static uint32_t shown_sec = ~0u;                /* the second the player shows */

/* --- the folder ------------------------------------------------------------------------------- */

static int ends_wav(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && n[l - 4] == '.' && (n[l - 3] | 32) == 'w' && (n[l - 2] | 32) == 'a' && (n[l - 1] | 32) == 'v';
}

static void scan(void)
{
    ntracks = 0;
    mkdir(folder, 0755);
    DIR *d = opendir(folder);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && ntracks < MAX_TRACKS) {
            if (e->d_name[0] == '.' || !ends_wav(e->d_name) || strlen(e->d_name) >= K86_NAME_MAX)
                continue;
            char path[K86_PATH_MAX + K86_NAME_MAX + 8];
            struct stat st;
            snprintf(path, sizeof(path), "%s/%s", folder, e->d_name);
            if (stat(path, &st) || !S_ISREG(st.st_mode))
                continue;
            snprintf(files[ntracks].name, sizeof(files[ntracks].name), "%s", e->d_name);
            files[ntracks].size = st.st_size > 0xffffffffll ? 0xffffffffu : (uint32_t)st.st_size;
            ntracks++;
        }
        closedir(d);
    }
    for (int i = 1; i < ntracks; i++) {         /* by name */
        struct track t = files[i];
        int j = i;
        while (j > 0 && strcasecmp(files[j - 1].name, t.name) > 0) {
            files[j] = files[j - 1];
            j--;
        }
        files[j] = t;
    }
    if (fp) {
        fclose(fp);
        fp = 0;
    }
    cur = -1;
    playing = draining = 0;
    wg_list_set_count(&list, ntracks);
}

/* "Some Song.wav" -> "Some Song" */
static void title_of(int i, char *out, int max)
{
    snprintf(out, (size_t)max, "%s", files[i].name);
    size_t l = strlen(out);
    if (l > 4)
        out[l - 4] = '\0';
}

static void row(void *ctx, int i, struct wg_row *out)
{
    static char name[K86_NAME_MAX], detail[16];
    (void)ctx;
    title_of(i, name, sizeof(name));
    snprintf(detail, sizeof(detail), "%u MB", (unsigned)((files[i].size + 524288) / 1048576));
    out->name = name;
    out->detail = detail;
    out->icon = i == cur && playing ? WG_ICON_FILE : WG_ICON_NONE;
}

/* --- a WAV file ------------------------------------------------------------------------------- */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

/* Reads the header of the open file: the format, where the sound data starts, and how long it is. */
static int parse_wav(FILE *f, uint32_t size)
{
    uint8_t h[12];
    if (size < 44 || fseek(f, 0, SEEK_SET) || fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        return -1;
    uint32_t off = 12;
    int have_fmt = 0;
    while ((uint64_t)off + 8 <= size) {
        uint8_t ch[8];
        if (fseek(f, (long)off, SEEK_SET) || fread(ch, 1, 8, f) != 8)
            return -1;
        uint32_t len = le32(ch + 4);
        if (!memcmp(ch, "fmt ", 4) && len >= 16) {
            uint8_t b[28];
            uint32_t n = len < sizeof(b) ? len : (uint32_t)sizeof(b);
            if (fread(b, 1, n, f) != n)
                return -1;
            uint32_t tag = le16(b);
            src_channels = (int)le16(b + 2);
            src_rate = le32(b + 4);
            src_bits = (int)le16(b + 14);
            if (tag == 0xFFFE && n >= 26)
                tag = le16(b + 24);             /* extensible: the real format code */
            if (tag != 1)
                return -2;                      /* not plain PCM */
            have_fmt = 1;
        } else if (!memcmp(ch, "data", 4)) {
            if (!have_fmt)
                return -1;
            if ((uint64_t)off + 8 + len > size)
                len = size - off - 8;           /* a header that claims more than there is */
            if ((src_channels != 1 && src_channels != 2) || (src_bits != 8 && src_bits != 16) ||
                src_rate < 8000 || src_rate > 96000)
                return -2;
            data_off = off + 8;
            pcm_bytes = len;
            nframes = len / (uint32_t)(src_channels * src_bits / 8);
            return nframes ? 0 : -1;
        }
        off += 8 + len + (len & 1);
    }
    return -1;
}

/* The file is read in blocks of RBUF_FRAMES frames; a frame outside the block in memory fetches the block it's in. */
static uint8_t rbuf[RBUF_FRAMES * 4];
static uint32_t rb_first, rb_count;

static void fill(uint32_t i)
{
    int fsz = src_channels * src_bits / 8;
    uint32_t n = nframes - i < RBUF_FRAMES ? nframes - i : RBUF_FRAMES;
    rb_count = 0;
    if (fseek(fp, (long)(data_off + (uint64_t)i * (uint64_t)fsz), SEEK_SET))
        return;
    rb_first = i;
    rb_count = (uint32_t)fread(rbuf, (size_t)fsz, n, fp);
}

/* One source frame as 16-bit left and right. */
static void frame_at(uint32_t i, int *l, int *r)
{
    if (i < rb_first || i >= rb_first + rb_count) {
        fill(i);
        if (i < rb_first || i >= rb_first + rb_count) {        /* the file is shorter than it says */
            *l = *r = 0;
            return;
        }
    }
    if (src_bits == 16) {
        const uint8_t *p = rbuf + (size_t)(i - rb_first) * (size_t)src_channels * 2;
        *l = (int16_t)le16(p);
        *r = src_channels == 2 ? (int16_t)le16(p + 2) : *l;
    } else {
        const uint8_t *p = rbuf + (size_t)(i - rb_first) * (size_t)src_channels;
        *l = (p[0] - 128) << 8;
        *r = src_channels == 2 ? (p[1] - 128) << 8 : *l;
    }
}

/* --- playing ---------------------------------------------------------------------------------- */

/* Frames of the current track that are in the ring but haven't been played. */
static uint64_t queued_src(void)
{
    int q = k86_audio_buffer() - k86_audio_space();
    return q > 0 ? (uint64_t)q * step : 0;
}

/* Where the track really is (what's heard), in source frames. */
static uint32_t heard_frame(void)
{
    uint64_t q = playing ? queued_src() : 0;
    uint64_t p = pos > q ? pos - q : 0;
    uint32_t f = (uint32_t)(p >> 32);
    return f > nframes ? nframes : f;
}

static void stop_sound(void)
{
    playing = draining = 0;
    k86_audio_flush();
}

static int load(int i)
{
    stop_sound();
    cur = -1;
    if (fp) {
        fclose(fp);
        fp = 0;
    }
    char path[K86_PATH_MAX + K86_NAME_MAX + 8];
    snprintf(path, sizeof(path), "%s/%s", folder, files[i].name);
    fp = fopen(path, "rb");
    if (!fp || parse_wav(fp, files[i].size)) {
        if (fp) {
            fclose(fp);
            fp = 0;
        }
        snprintf(note, sizeof(note), "Can't play this file");
        return -1;
    }
    note[0] = '\0';
    rb_count = 0;
    cur = i;
    pos = 0;
    step = ((uint64_t)src_rate << 32) / OUT_RATE;
    shown_sec = ~0u;
    return 0;
}

static void set_timer(void)
{
    k86_timer_set(playing ? 40 : wg_list_gliding(&list) ? 16 : 0);
}

static void start(int i)
{
    if (i < 0 || i >= ntracks)
        return;
    if (load(i)) {
        set_timer();
        return;
    }
    if (k86_audio_open(OUT_RATE)) {
        snprintf(note, sizeof(note), "No sound card");
        return;
    }
    k86_audio_flush();
    playing = 1;
    wg_list_select(&list, i);
    k86_logf("music: playing %s (%u Hz, %d ch, %d bit)", files[i].name, (unsigned)src_rate, src_channels, src_bits);
    set_timer();
}

static void pause_play(void)
{
    if (cur < 0) {
        if (ntracks)
            start(list.selected >= 0 ? list.selected : 0);
        return;
    }
    if (playing) {                              /* back to where it's heard, and stop at once */
        uint64_t q = queued_src();
        pos = pos > q ? pos - q : 0;
        stop_sound();
    } else if (!k86_audio_open(OUT_RATE)) {
        if ((uint32_t)(pos >> 32) >= nframes)
            pos = 0;
        playing = 1;
    }
    set_timer();
}

static void go_next(int dir)
{
    if (!ntracks)
        return;
    int i = cur < 0 ? 0 : (cur + dir + ntracks) % ntracks;
    start(i);
}

static void seek_to(int x, int x0, int w)
{
    if (cur < 0 || w <= 0)
        return;
    int d = x - x0;
    d = d < 0 ? 0 : d > w ? w : d;
    pos = ((uint64_t)((uint64_t)nframes * (uint64_t)d / (uint64_t)w)) << 32;
    k86_audio_flush();
    draining = 0;
}

/* Makes sound for the ring while there's room. Returns 1 when the track ended. */
static int feed(void)
{
    int16_t buf[CHUNK * 2];
    for (;;) {
        int space = k86_audio_space();
        if (space < 256)
            return 0;
        uint32_t first = (uint32_t)(pos >> 32);
        if (first >= nframes)
            return 1;
        int n = space < CHUNK ? space : CHUNK;
        int made = 0;
        while (made < n) {
            uint32_t i = (uint32_t)(pos >> 32);
            if (i >= nframes)
                break;
            int l0, r0, l1, r1;
            frame_at(i, &l0, &r0);
            frame_at(i + 1 < nframes ? i + 1 : i, &l1, &r1);
            int frac = (int)((pos >> 16) & 0xffff);
            buf[made * 2] = (int16_t)(l0 + (int)(((int64_t)(l1 - l0) * frac) >> 16));
            buf[made * 2 + 1] = (int16_t)(r0 + (int)(((int64_t)(r1 - r0) * frac) >> 16));
            pos += step;
            made++;
        }
        int w = k86_audio_write(buf, made);
        if (w < 0)
            return 0;
        if (w < made) {                         /* not all taken: those frames come again */
            pos -= (uint64_t)(made - w) * step;
            return 0;
        }
    }
}

/* --- drawing ---------------------------------------------------------------------------------- */

static struct gfx_rect bar_rect(void)
{
    return (struct gfx_rect){ 36, canvas.h - PLAYER_H + 112, canvas.w - 72, 16 };
}

static void layout(void)
{
    const struct theme *t = theme_get();
    int bh = t->button_size * 3 / 2, gap = 16, w = (canvas.w - 48 - 2 * gap) / 3;
    int y = canvas.h - bh - 28;
    b_prev = (struct wg_button){ .r = { 24, y, w, bh }, .label = "Previous" };
    b_play = (struct wg_button){ .r = { 24 + w + gap, y, w, bh }, .label = playing ? "Pause" : "Play",
                                 .style = WG_BUTTON_SUGGESTED };
    b_next = (struct wg_button){ .r = { 24 + 2 * (w + gap), y, w, bh }, .label = "Next" };
    b_refresh = (struct wg_button){ .r = { canvas.w - 24 - 160, 20, 160, t->button_size }, .label = "Refresh",
                                    .style = WG_BUTTON_FLAT };
    int top = 20 + t->button_size + 20;
    list.r = (struct gfx_rect){ 24, top, canvas.w - 48, canvas.h - PLAYER_H - top - 8 };
}

static void fmt_time(char *out, int max, uint32_t sec)
{
    snprintf(out, (size_t)max, "%u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
}

static void draw_player(void)
{
    const struct theme *t = theme_get();
    int top = canvas.h - PLAYER_H;
    gfx_fill_rect(&canvas, 0, top, canvas.w, PLAYER_H, t->window_bg);
    gfx_fill_rect(&canvas, 0, top, canvas.w, 1, t->border);

    char title[K86_NAME_MAX];
    if (cur >= 0)
        title_of(cur, title, sizeof(title));
    else
        snprintf(title, sizeof(title), "%s", note[0] ? note : ntracks ? "Pick a track" : "No music yet");
    gfx_fill_rect(&canvas, 0, top + 24, canvas.w, 40, t->window_bg);
    int tw = wg_label_width(title, WG_TEXT_TITLE);
    if (tw > canvas.w - 48)
        wg_text_fit(&canvas, 24, top + 24, title, canvas.w - 48, t->fg);
    else
        wg_label_centred(&canvas, canvas.w / 2, top + 24, title, WG_TEXT_TITLE);
    if (cur >= 0 && note[0])
        wg_label_centred(&canvas, canvas.w / 2, top + 64, note, WG_TEXT_DIM);

    struct gfx_rect br = bar_rect();
    uint32_t total = cur >= 0 ? nframes : 0, at = scrubbing && total ? 0 : heard_frame();
    int fill = 0;
    if (scrubbing && br.w > 0) {
        int d = scrub_x - br.x;
        fill = d < 0 ? 0 : d > br.w ? br.w : d;
        at = (uint32_t)((uint64_t)total * (uint64_t)fill / (uint64_t)br.w);
    } else if (total) {
        fill = (int)((uint64_t)br.w * at / total);
    }
    gfx_fill_round_rect(&canvas, br.x, br.y, br.w, br.h, br.h / 2, gfx_mix(t->fg, t->window_bg, 40));
    if (fill > br.h)
        gfx_fill_round_rect(&canvas, br.x, br.y, fill, br.h, br.h / 2, t->accent_bg);
    else if (fill > 0)
        gfx_fill_round_rect(&canvas, br.x, br.y, br.h, br.h, br.h / 2, t->accent_bg);

    char a[16], b[16];
    uint32_t rate = src_rate ? src_rate : 1;
    shown_sec = at / rate;
    fmt_time(a, sizeof(a), cur >= 0 ? at / rate : 0);
    fmt_time(b, sizeof(b), cur >= 0 ? total / rate : 0);
    wg_label(&canvas, br.x, br.y + 28, a, WG_TEXT_SMALL);
    wg_label(&canvas, br.x + br.w - wg_label_width(b, WG_TEXT_SMALL), br.y + 28, b, WG_TEXT_SMALL);

    b_play.label = playing ? "Pause" : "Play";
    wg_button_draw(&canvas, &b_prev);
    wg_button_draw(&canvas, &b_play);
    wg_button_draw(&canvas, &b_next);
}

static void draw_list(void)
{
    const struct theme *t = theme_get();
    int top = list.r.y;
    gfx_fill_rect(&canvas, 0, top, canvas.w, canvas.h - PLAYER_H - top, t->window_bg);
    if (ntracks) {
        wg_list_draw(&canvas, &list);
    } else {
        wg_label_centred(&canvas, canvas.w / 2, top + 80, "No music yet", WG_TEXT_TITLE);
        wg_label_centred(&canvas, canvas.w / 2, top + 130, "Put .wav files in", WG_TEXT_DIM);
        wg_label_centred(&canvas, canvas.w / 2, top + 160, MUSIC_DIR, WG_TEXT_DIM);
    }
}

static void draw_all(void)
{
    const struct theme *t = theme_get();
    gfx_fill_rect(&canvas, 0, 0, canvas.w, list.r.y, t->window_bg);
    wg_label(&canvas, 24, 24, "Music", WG_TEXT_HEADING);
    wg_button_draw(&canvas, &b_refresh);
    draw_list();
    draw_player();
    k86_present(0, 0, canvas.w, canvas.h);
}

static void present_player(void)
{
    k86_present(0, canvas.h - PLAYER_H, canvas.w, PLAYER_H);
}

static void present_list(void)
{
    k86_present(0, list.r.y, canvas.w, canvas.h - PLAYER_H - list.r.y);
}

/* --- events ----------------------------------------------------------------------------------- */

static void on_timer(void)
{
    if (wg_list_gliding(&list)) {
        wg_list_tick(&list, k86_uptime_ms());
        draw_list();
        present_list();
    }
    if (playing) {
        if (!draining && feed()) {
            draining = 1;                       /* all of it is in the ring: wait for it to play */
        }
        if (draining && k86_audio_space() >= k86_audio_buffer() - 64) {
            draining = 0;
            if (ntracks > 1) {
                go_next(1);
            } else {
                playing = 0;
                pos = 0;
                stop_sound();
            }
            layout();
            draw_list();
            draw_player();
            present_list();
            present_player();
            set_timer();
            return;
        }
        uint32_t rate = src_rate ? src_rate : 1;
        if (!scrubbing && heard_frame() / rate != shown_sec) {      /* the second changed */
            draw_player();
            present_player();
        }
    }
    set_timer();
}

static void on_pointer(const struct k86_event *ev)
{
    struct wg_pointer p = wg_pointer_make(ev->x, ev->y, ev->buttons, ev->changed, ev->time_ms);
    int redraw_player = 0, redraw_list = 0;

    /* The seek bar: a finger down on it (a taller target than the bar), dragging, lifted. */
    struct gfx_rect br = bar_rect();
    if (cur >= 0 && p.down && p.x >= br.x - 12 && p.x < br.x + br.w + 12 && p.y >= br.y - 28 && p.y < br.y + br.h + 28) {
        scrubbing = 1;
        scrub_x = p.x;
        redraw_player = 1;
    } else if (scrubbing) {
        scrub_x = p.x;
        if (p.up) {
            scrubbing = 0;
            seek_to(p.x, br.x, br.w);
        }
        redraw_player = 1;
    } else {
        if (wg_button_pointer(&b_refresh, &p)) {
            scan();
            layout();
            draw_all();
            k86_present(0, 0, canvas.w, canvas.h);
            set_timer();
            return;
        }
        if (wg_button_pointer(&b_play, &p)) {
            pause_play();
            layout();
            redraw_player = redraw_list = 1;
        }
        if (wg_button_pointer(&b_prev, &p)) {
            go_next(-1);
            redraw_player = redraw_list = 1;
        }
        if (wg_button_pointer(&b_next, &p)) {
            go_next(1);
            redraw_player = redraw_list = 1;
        }
        if (ntracks) {
            enum wg_list_result r = wg_list_pointer(&list, &p);
            if (r == WG_LIST_ACTIVATE && list.selected >= 0) {
                start(list.selected);
                redraw_player = 1;
            }
            if (r != WG_LIST_NONE || list.dirty)
                redraw_list = 1;
        }
        if (b_play.dirty || b_prev.dirty || b_next.dirty)
            redraw_player = 1;
    }
    if (redraw_list) {
        draw_list();
        present_list();
    }
    if (redraw_player) {
        draw_player();
        present_player();
    }
    set_timer();
}

int main(void)
{
    if (k86_window_open(&win))
        return 1;
    canvas = (struct gfx_surface){ win.canvas, win.w, win.h, win.stride };
    list = (struct wg_list){ .selected = -1, .hover = -1, .row = row, .last_click_row = -1 };
    layout();
    scan();
    layout();
    draw_all();
    k86_log("music: open");
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
            if (ntracks && wg_list_scroll(&list, ev.x, ev.y, ev.id)) {
                draw_list();
                present_list();
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
            stop_sound();
            k86_audio_close();
            return 0;
        default:
            break;
        }
    }
}
