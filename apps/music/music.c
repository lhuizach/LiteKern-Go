/* Music: plays the .wav and .mp3 files in the music folder, /extras/music on the phone (the big storage
 * area; copy files there over ssh for now). A list of tracks, and below it the player: the title,
 * the time, a bar to tap or drag to seek, Previous / Play-Pause / Next. The next track starts by itself.
 *
 * The sound goes out through sdk/audio.h (the loudspeaker, or the headphones when they're in),
 * at the volume of the volume keys. It keeps playing when you go to another app.
 *
 * .wav (8 or 16 bit, mono or stereo, any rate) and .mp3 (minimp3, third_party/minimp3); everything is
 * resampled to 48 kHz here. A track is read from the file as it plays, so it can be any length. An MP3 is
 * decoded a frame at a time; its length is estimated from the bit rate. No malloc. */
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
#include "apps/music/mp3dec.h"

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

static int ends_audio(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && (!strcasecmp(n + l - 4, ".wav") || !strcasecmp(n + l - 4, ".mp3"));
}

static void scan(void)
{
    ntracks = 0;
    mkdir(folder, 0755);
    DIR *d = opendir(folder);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && ntracks < MAX_TRACKS) {
            if (e->d_name[0] == '.' || !ends_audio(e->d_name) || strlen(e->d_name) >= K86_NAME_MAX)
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
static void wav_frame_at(uint32_t i, int *l, int *r)
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

/* --- an MP3 file ------------------------------------------------------------------------------ */
/* Decoded a frame at a time into a small window of source frames, read forward as the track plays.
 * The length is worked out from the first frame's bit rate (a constant-rate file is exact, a
 * variable-rate one is a little off), and a seek goes to the same share of the file. */

#define MP3_WIN (MP3_FRAME_MAX * 4)
static int is_mp3;
static uint32_t mp3_start;                      /* where the sound starts in the file (after any ID3 tag) */
static uint32_t mp3_bytes;                      /* the sound's bytes */
static uint8_t mp3_in[16384];                   /* the file as it's read */
static int mp3_in_pos, mp3_in_len, mp3_eof;
static int16_t mp3_win[MP3_WIN * 2];
static uint32_t mp3_first;                      /* the source frame the window starts at */
static int mp3_count;                           /* frames in the window */

static int mp3_refill(void)
{
    if (mp3_eof)
        return 0;
    if (mp3_in_pos > 0) {
        memmove(mp3_in, mp3_in + mp3_in_pos, (size_t)(mp3_in_len - mp3_in_pos));
        mp3_in_len -= mp3_in_pos;
        mp3_in_pos = 0;
    }
    size_t got = fread(mp3_in + mp3_in_len, 1, sizeof(mp3_in) - (size_t)mp3_in_len, fp);
    if (got == 0)
        mp3_eof = 1;
    mp3_in_len += (int)got;
    return (int)got;
}

/* Decodes the next frame and adds it to the window (the oldest frames drop off). 0 at the end. */
static int mp3_next(void)
{
    static int16_t pcm[MP3_FRAME_MAX * 2];
    for (int tries = 0; tries < 64; tries++) {
        if (mp3_in_len - mp3_in_pos < 4096 && !mp3_eof)
            mp3_refill();
        if (mp3_in_len - mp3_in_pos <= 4)
            return 0;
        struct mp3_info mi;
        int n = mp3_frame(mp3_in + mp3_in_pos, mp3_in_len - mp3_in_pos, pcm, &mi);
        if (mi.used <= 0)
            return 0;                           /* nothing that looks like a frame left */
        mp3_in_pos += mi.used;
        if (n <= 0)
            continue;                           /* a tag or junk, skipped */
        if (mi.channels == 1)                   /* the window is always stereo */
            for (int i = n - 1; i >= 0; i--)
                pcm[i * 2] = pcm[i * 2 + 1] = pcm[i];
        int drop = mp3_count + n - MP3_WIN;
        if (drop > 0) {
            memmove(mp3_win, mp3_win + drop * 2, (size_t)(mp3_count - drop) * 4);
            mp3_count -= drop;
            mp3_first += (uint32_t)drop;
        }
        memcpy(mp3_win + mp3_count * 2, pcm, (size_t)n * 4);
        mp3_count += n;
        return 1;
    }
    return 0;
}

/* The file's first frame gives the format; the ID3 tag in front is skipped. */
static int parse_mp3(FILE *f, uint32_t size)
{
    uint8_t h[10];
    mp3_start = 0;
    if (fseek(f, 0, SEEK_SET) || fread(h, 1, 10, f) != 10)
        return -1;
    if (!memcmp(h, "ID3", 3))
        mp3_start = 10 + ((uint32_t)(h[6] & 0x7f) << 21 | (uint32_t)(h[7] & 0x7f) << 14 |
                          (uint32_t)(h[8] & 0x7f) << 7 | (uint32_t)(h[9] & 0x7f));
    if (mp3_start >= size)
        return -1;
    mp3_bytes = size - mp3_start;
    mp3_reset();
    is_mp3 = 1;
    mp3_in_pos = mp3_in_len = mp3_eof = 0;
    mp3_count = 0;
    mp3_first = 0;
    if (fseek(f, (long)mp3_start, SEEK_SET))
        return -1;
    /* decode one frame to learn the format and bit rate */
    static int16_t pcm[MP3_FRAME_MAX * 2];
    struct mp3_info mi = { 0, 0, 0, 0 };
    for (int tries = 0; tries < 64; tries++) {
        if (mp3_in_len - mp3_in_pos < 4096 && !mp3_eof)
            mp3_refill();
        if (mp3_in_len - mp3_in_pos <= 4)
            return -1;
        int n = mp3_frame(mp3_in + mp3_in_pos, mp3_in_len - mp3_in_pos, pcm, &mi);
        if (mi.used <= 0)
            return -1;
        mp3_in_pos += mi.used;
        if (n > 0)
            break;
        mi.hz = 0;
    }
    if (mi.hz < 8000 || mi.hz > 48000 || mi.kbps <= 0 || (mi.channels != 1 && mi.channels != 2))
        return -2;
    src_rate = (uint32_t)mi.hz;
    src_channels = mi.channels;
    src_bits = 16;
    nframes = (uint32_t)((uint64_t)mp3_bytes * 8 * src_rate / ((uint64_t)mi.kbps * 1000));
    /* start over, so the first frame is played too */
    mp3_reset();
    mp3_in_pos = mp3_in_len = mp3_eof = 0;
    mp3_count = 0;
    mp3_first = 0;
    fseek(f, (long)mp3_start, SEEK_SET);
    return nframes ? 0 : -1;
}

static void mp3_frame_at(uint32_t i, int *l, int *r)
{
    if (i < mp3_first || i >= mp3_first + (uint32_t)mp3_count + 44100) {   /* a seek: the same share of the file */
        uint64_t off = (uint64_t)mp3_bytes * i / (nframes ? nframes : 1);
        mp3_reset();
        mp3_in_pos = mp3_in_len = mp3_eof = 0;
        mp3_count = 0;
        mp3_first = i;
        fseek(fp, (long)(mp3_start + off), SEEK_SET);
    }
    while (i >= mp3_first + (uint32_t)mp3_count)
        if (!mp3_next())
            break;
    if (i < mp3_first || i >= mp3_first + (uint32_t)mp3_count) {
        *l = *r = 0;
        return;
    }
    const int16_t *p = mp3_win + (size_t)(i - mp3_first) * 2;
    *l = p[0];
    *r = p[1];
}

/* One source frame as 16-bit left and right, from whichever kind of file this is. */
static void frame_at(uint32_t i, int *l, int *r)
{
    if (is_mp3)
        mp3_frame_at(i, l, r);
    else
        wav_frame_at(i, l, r);
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

/* Stops the sound. The stream is closed, not just flushed: flushed, the hardware went on playing
 * the last of the ring over and over until the stream was closed. Playing again opens it anew. */
static void stop_sound(void)
{
    playing = draining = 0;
    k86_audio_close();
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
    is_mp3 = 0;
    size_t fl = strlen(files[i].name);
    int bad = !fp;
    if (!bad)
        bad = !strcasecmp(files[i].name + fl - 4, ".mp3") ? parse_mp3(fp, files[i].size) : parse_wav(fp, files[i].size);
    if (bad) {
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

static int mic_state;                           /* the mic test (below) */

static void set_timer(void)
{
    k86_timer_set(playing || mic_state ? 40 : wg_list_gliding(&list) ? 16 : 0);
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

/* --- mic test: records 4 seconds from the phone's microphone, then plays them back ------------- */

#define MIC_RATE 48000
#define MIC_SECS 4
static int16_t mic_buf[MIC_RATE * MIC_SECS];
static int mic_state;                           /* 0 idle, 1 recording, 2 playing it back */
static int mic_len, mic_pos, mic_peak, mic_gain;
static uint32_t mic_t0;
static struct wg_button b_mic;

static void mic_stop_now(void);

static void mic_stop(void)
{
    k86_logf("music: mic test: stop (state %d)", mic_state);
    mic_stop_now();
}

static void mic_stop_now(void)
{
    k86_mic_close();
    k86_audio_close();
    mic_state = 0;
}

static void mic_start(void)
{
    if (playing) {                              /* the song is paused for the test */
        uint64_t q = queued_src();
        pos = pos > q ? pos - q : 0;
    }
    stop_sound();
    int e = k86_mic_open(MIC_RATE);
    if (e) {
        snprintf(note, sizeof(note), "Mic failed (%d)", e);
        return;
    }
    mic_len = mic_pos = mic_peak = 0;
    mic_t0 = k86_uptime_ms();
    mic_state = 1;
    k86_log("music: mic test: recording");
    snprintf(note, sizeof(note), "Recording... speak");
}

static void mic_tick(void)
{
    if (mic_state == 1) {
        int want = MIC_RATE * MIC_SECS - mic_len;
        int n = k86_mic_read(mic_buf + mic_len, want < 4096 ? want : 4096);
        if (n < 0) {
            snprintf(note, sizeof(note), "Mic read failed (%d)", n);
            mic_stop();
            return;
        }
        for (int i = 0; i < n; i++) {
            int v = mic_buf[mic_len + i];
            v = v < 0 ? -v : v;
            if (v > mic_peak)
                mic_peak = v;
        }
        mic_len += n;
        if (!mic_len && k86_uptime_ms() - mic_t0 > 1500) {
            snprintf(note, sizeof(note), "Mic gives no sound data");
            mic_stop();
            return;
        }
        if (mic_len >= MIC_RATE * MIC_SECS) {
            k86_mic_close();
            if (k86_audio_open(MIC_RATE)) {
                snprintf(note, sizeof(note), "No sound card");
                mic_state = 0;
                return;
            }
            k86_audio_flush();
            mic_gain = mic_peak > 0 ? 16000 * 256 / mic_peak : 256;     /* brought up to a good level */
            if (mic_gain > 256 * 64)
                mic_gain = 256 * 64;
            mic_state = 2;
            k86_logf("music: mic test: recorded %d frames, peak %d, gain %d; playing back", mic_len, mic_peak, mic_gain);
            snprintf(note, sizeof(note), "Mic level %d%%: playing back", mic_peak * 100 / 32768);
        }
    } else if (mic_state == 2) {
        int16_t out[512 * 2];
        for (;;) {
            int space = k86_audio_space();
            int left = mic_len - mic_pos;
            if (left <= 0) {
                if (space >= k86_audio_buffer() - 64) {
                    mic_stop();
                    snprintf(note, sizeof(note), "Mic level %d%%", mic_peak * 100 / 32768);
                }
                return;
            }
            int n = space < 512 ? space : 512;
            n = n < left ? n : left;
            if (n < 256 && left >= 256)
                return;
            for (int i = 0; i < n; i++) {
                int v = mic_buf[mic_pos + i] * mic_gain / 256;
                v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
                out[i * 2] = out[i * 2 + 1] = (int16_t)v;
            }
            int w = k86_audio_write(out, n);
            if (w <= 0)
                k86_logf("music: mic test: write gave %d (space %d, at %d of %d)", w, space, mic_pos, mic_len);
            if (w <= 0)
                return;
            mic_pos += w;
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
    b_mic = (struct wg_button){ .r = { canvas.w - 24 - 160 - 12 - 160, 20, 160, t->button_size },
                               .label = mic_state ? "Stop" : "Mic test", .style = WG_BUTTON_FLAT };
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
        wg_label_centred(&canvas, canvas.w / 2, top + 130, "Put .wav or .mp3 files in", WG_TEXT_DIM);
        wg_label_centred(&canvas, canvas.w / 2, top + 160, MUSIC_DIR, WG_TEXT_DIM);
    }
}

static void draw_all(void)
{
    const struct theme *t = theme_get();
    gfx_fill_rect(&canvas, 0, 0, canvas.w, list.r.y, t->window_bg);
    wg_label(&canvas, 24, 24, "Music", WG_TEXT_HEADING);
    b_mic.label = mic_state ? "Stop" : "Mic test";
    wg_button_draw(&canvas, &b_mic);
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
    if (mic_state) {
        static char last_note[64];
        int was = mic_state;
        mic_tick();
        if (was != mic_state || strcmp(last_note, note)) {
            snprintf(last_note, sizeof(last_note), "%s", note);
            layout();
            draw_all();
        }
    }
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
        if (wg_button_pointer(&b_mic, &p)) {
            if (mic_state)
                mic_stop();
            else
                mic_start();
            layout();
            draw_all();
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
            mic_stop();
            return 0;
        default:
            break;
        }
    }
}
