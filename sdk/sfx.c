/* LiteKern Go: the sound mixer (sdk/sfx.h). Voices (a waveform, a pitch that can slide, a volume that rises and
 * falls, for noise a filter that can sweep) are summed and written to the speaker from the app's timer. */
#include "sdk/sfx.h"

#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "sdk/audio.h"
#include "sdk/kern86.h"

#define RATE 48000
#define VOICES 20
#define CHUNK 256
#define TARGET 4800                     /* frames kept queued ahead: about 100 ms, to ride out a late timer tick (a gap in the sound is a click or a buzz) */

enum { W_SINE, W_TRI, W_SQUARE, W_SAW, W_NOISE };

struct voice {
    int live, wave, delay, len, pos, amp, att_n, lp, lpc, dlpc;
    uint32_t phase;
    int64_t inc, dinc;
    int env_step;                       /* for the rise at the start */
    int aenv;
};

static struct voice v[VOICES];
static int16_t sine_t[256];
static int ready, opened, idle, music = -1, mstep, msamp;
static uint32_t rng = 0x2545f491u;

static void init(void)
{
    int64_t c = 65516, s = 1608, x = 65536, y = 0;      /* cos and sin of 1/256 of a turn, 16.16 */
    for (int i = 0; i < 256; i++) {
        sine_t[i] = (int16_t)(y * 32000 >> 16);
        int64_t nx = (x * c - y * s) >> 16, ny = (x * s + y * c) >> 16;
        x = nx;
        y = ny;
    }
    ready = 1;
}

static int64_t hz_inc(int hz, int semis)
{
    static const int ratio[12] = { 65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715 };
    int64_t f = (int64_t)hz * ratio[(semis % 12 + 12) % 12] >> 16;
    int o = (semis - (semis % 12 + 12) % 12) / 12;
    f = o >= 0 ? f << o : f >> -o;
    return f * 89478;                   /* 2^32 / 48000, a little under */
}

/* Starts a voice. Frequencies in Hz (noise: f0 and f1 are the filter's cutoff, Hz), times in ms, amp 0..255. */
static void start(int delay_ms, int wave, int f0, int f1, int ms, int amp, int att_ms, int semis)
{
    int slot = -1, quiet = 1 << 30;
    for (int i = 0; i < VOICES; i++) {
        if (!v[i].live) {
            slot = i;
            break;
        }
        int left = v[i].len - v[i].pos;
        if (left < quiet) {                     /* all busy: take the one nearly done */
            quiet = left;
            slot = i;
        }
    }
    struct voice *p = &v[slot];
    memset(p, 0, sizeof(*p));
    p->live = 1;
    p->wave = wave;
    p->delay = delay_ms * (RATE / 1000);
    p->len = ms * (RATE / 1000);
    p->amp = amp;
    p->att_n = att_ms * (RATE / 1000);
    if (p->att_n < 1)
        p->att_n = 1;
    p->env_step = 65536 / p->att_n;
    if (wave == W_NOISE) {                      /* cutoff Hz to a one-pole coefficient (16 bit) */
        int c0 = (int)((int64_t)f0 * 411775 / RATE), c1 = (int)((int64_t)f1 * 411775 / RATE);
        p->lpc = c0 > 65535 ? 65535 : c0;
        p->dlpc = ((c1 > 65535 ? 65535 : c1) - p->lpc) / p->len;
    } else {
        p->inc = hz_inc(f0, semis);
        p->dinc = (hz_inc(f1, semis) - p->inc) / p->len;
    }
}

static void bell(int delay, int hz, int ms, int amp, int semis)         /* a struck bell: a few partials, each dying faster */
{
    start(delay, W_SINE, hz, hz, ms, amp, 1, semis);
    start(delay, W_SINE, hz * 5 / 2, hz * 5 / 2, ms * 2 / 3, amp / 2, 1, semis);
    start(delay, W_SINE, hz * 4, hz * 4, ms / 3, amp / 3, 1, semis);
}

void k86_sfx_play(int id, int semis)
{
    if (!ready)
        init();
    idle = 0;
    switch (id) {
    case SFX_TAP:
        start(0, W_SINE, 1500, 1100, 50, 110, 1, semis);
        start(0, W_TRI, 3000, 2200, 25, 40, 1, semis);
        break;
    case SFX_BACK:
        start(0, W_SINE, 900, 500, 70, 110, 1, semis);
        break;
    case SFX_WHOOSH:
        start(0, W_NOISE, 500, 7000, 190, 250, 50, 0);
        start(0, W_SINE, 300, 800, 150, 25, 40, semis);
        break;
    case SFX_THUD:
        start(0, W_SINE, 190, 55, 150, 200, 1, semis);
        start(0, W_NOISE, 6000, 1500, 55, 130, 1, 0);
        start(0, W_SQUARE, 1800, 900, 12, 45, 1, semis);
        break;
    case SFX_CLINK:
        bell(0, 1760, 420, 100, semis);
        bell(8, 2349, 300, 70, semis);
        start(0, W_NOISE, 12000, 5000, 35, 120, 1, 0);
        break;
    case SFX_SPLAT:
        start(0, W_NOISE, 9000, 700, 260, 150, 4, 0);
        start(0, W_SINE, 280, 70, 220, 170, 2, semis);
        start(60, W_NOISE, 3000, 600, 140, 70, 4, 0);
        break;
    case SFX_LEVELUP:
        for (int i = 0; i < 4; i++) {
            static const int n[4] = { 523, 659, 784, 1047 };
            start(i * 85, W_TRI, n[i], n[i], 280, 120, 2, semis);
            start(i * 85, W_SINE, n[i] * 2, n[i] * 2, 160, 35, 2, semis);
        }
        start(340, W_TRI, 1319, 1319, 600, 70, 4, semis);
        start(340, W_TRI, 1568, 1568, 600, 60, 4, semis);
        start(340, W_SINE, 2093, 2093, 600, 45, 4, semis);
        break;
    case SFX_FAIL:
        start(0, W_TRI, 420, 130, 520, 150, 6, semis);
        start(0, W_SQUARE, 210, 65, 520, 40, 6, semis);
        start(0, W_SINE, 110, 38, 460, 150, 3, semis);
        break;
    case SFX_WIN: {
        static const int n[6] = { 523, 659, 784, 1047, 784, 1047 };
        static const int t[6] = { 0, 110, 220, 330, 520, 630 };
        for (int i = 0; i < 6; i++) {
            start(t[i], W_SQUARE, n[i], n[i], i < 5 ? 200 : 900, 55, 3, semis);
            start(t[i], W_TRI, n[i], n[i], i < 5 ? 220 : 900, 110, 3, semis);
        }
        bell(760, 1568, 800, 80, semis);
        bell(900, 2093, 900, 70, semis);
        break;
    }
    case SFX_CRASH:
        start(0, W_NOISE, 9000, 500, 420, 250, 2, 0);
        start(0, W_SINE, 130, 34, 520, 230, 2, semis);
        bell(0, 520, 260, 80, semis);
        start(40, W_NOISE, 15000, 3000, 120, 120, 2, 0);
        break;
    case SFX_PAUSE:
        start(0, W_SINE, 700, 650, 90, 110, 2, semis);
        start(90, W_SINE, 500, 380, 130, 110, 2, semis);
        break;
    case SFX_RESUME:
        start(0, W_SINE, 500, 520, 90, 110, 2, semis);
        start(90, W_SINE, 750, 800, 130, 110, 2, semis);
        break;
    case SFX_COIN:
        start(0, W_SQUARE, 988, 988, 90, 50, 1, semis);
        start(70, W_SQUARE, 1319, 1319, 220, 55, 1, semis);
        start(0, W_SINE, 1976, 1976, 90, 40, 1, semis);
        start(70, W_SINE, 2637, 2637, 220, 45, 1, semis);
        break;
    default:
        break;
    }
}

/* ---- the tunes: 32 eighth-note steps; lead notes are semitones above the root, 99 = rest; the bass changes every
 *      `bstep` steps ---- */
#define R 99
struct tune {
    int bpm, root, bstep, lead_wave, bass_wave, lead_amp, bass_amp, lead_ms;
    signed char lead[32], bass[16];
};
static const struct tune tunes[4] = {
    { 126, 262, 4, W_TRI, W_TRI, 70, 90, 200,                                     /* bouncy and bright */
      { 0, R, 4, R, 7, R, 4, R, 9, R, 7, R, 4, R, 2, R, 0, R, 4, R, 7, R, 12, R, 9, 7, 4, R, 2, R, 0, R },
      { -12, -15, -19, -17, -12, -15, -19, -17 } },
    { 88, 220, 4, W_SINE, W_SINE, 80, 80, 520,                                    /* slow and icy */
      { 0, R, 3, R, 7, R, 10, R, 12, R, 10, R, 7, R, 3, R, 0, R, 5, R, 8, R, 12, R, 10, R, 7, R, 3, R, R, R },
      { -12, -12, -7, -5, -12, -12, -7, -9 } },
    { 138, 165, 2, W_SQUARE, W_SAW, 60, 110, 150,                                  /* driving and dark */
      { 0, 0, R, 3, 0, R, 5, R, 7, R, 5, R, 3, R, 0, R, 0, 0, R, 3, 0, R, 10, R, 8, R, 7, R, 5, R, 3, R },
      { -12, -12, -12, -12, -10, -10, -7, -7, -12, -12, -12, -12, -5, -5, -7, -7 } },
    { 100, 294, 4, W_TRI, W_SINE, 80, 85, 700,                                    /* dreamy and spacey */
      { 0, R, R, 7, R, R, 12, R, 14, R, R, 12, R, R, 7, R, 5, R, R, 9, R, R, 12, R, 10, R, R, 7, R, R, 3, R },
      { -12, -12, -7, -7, -9, -9, -14, -14 } },
};

void k86_sfx_music(int tune)
{
    if (!ready)
        init();
    if (tune == music)
        return;
    music = tune >= 0 && tune < 4 ? tune : -1;
    mstep = msamp = 0;
    idle = 0;
}

static void music_step(void)
{
    const struct tune *t = &tunes[music];
    int s = mstep & 31, n = t->lead[s];
    if (n != R)
        start(0, t->lead_wave, t->root, t->root, t->lead_ms, t->lead_amp, 6, n);
    if (s % t->bstep == 0) {
        int b = t->bass[(s / t->bstep) & 15];
        start(0, t->bass_wave, t->root, t->root, t->bstep * 60 * 1000 / t->bpm / 2 * 9 / 10, t->bass_amp, 8, b);
        if (t->bass_wave == W_SINE)
            start(0, W_TRI, t->root, t->root, 200, 30, 8, b + 12);       /* a little edge for the soft bass */
    }
    mstep++;
}

static int next_noise(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (int)(rng >> 16) - 32768;
}

static int voice_sample(struct voice *p)
{
    if (p->delay > 0) {
        p->delay--;
        return 0;
    }
    if (p->pos >= p->len) {
        p->live = 0;
        return 0;
    }
    int s;
    switch (p->wave) {
    case W_SINE:
        s = sine_t[p->phase >> 24];
        break;
    case W_TRI: {
        int q = (int)(p->phase >> 16);
        s = (q < 32768 ? q * 2 - 32768 : 98303 - q * 2) * 3 / 4;
        break;
    }
    case W_SQUARE:
        s = p->phase >> 31 ? -20000 : 20000;
        break;
    case W_SAW:
        s = ((int)(p->phase >> 16) - 32768) * 3 / 5;
        break;
    default:
        p->lp += (int)(((int64_t)(next_noise() - p->lp) * p->lpc) >> 16);
        s = p->lp;
        p->lpc += p->dlpc;
        break;
    }
    p->phase += (uint32_t)p->inc;
    p->inc += p->dinc;
    int rise = p->pos < p->att_n ? p->pos * p->env_step : 65536;
    int fall = (int)((int64_t)(p->len - p->pos) * 65536 / p->len);           /* 1 to 0 over the voice's life */
    int env = (int)((int64_t)fall * fall >> 16);
    p->pos++;
    return (int)((int64_t)s * (rise > 65535 ? 65535 : rise) >> 16) * (env >> 4) / 4096 * p->amp / 256;
}

/* The next n frames of everything that is sounding (mono, put in both channels). */
static void render(int16_t *buf, int n)
{
    for (int i = 0; i < n; i++) {
        if (music >= 0 && msamp <= 0) {
            music_step();
            msamp = RATE * 60 / tunes[music].bpm / 2;
        }
        msamp--;
        int mix = 0;
        for (int k = 0; k < VOICES; k++)
            if (v[k].live)
                mix += voice_sample(&v[k]);
        mix = mix * 3 / 4;                      /* headroom for several voices at once */
        buf[i * 2] = buf[i * 2 + 1] = (int16_t)(mix > 32767 ? 32767 : mix < -32768 ? -32768 : mix);
    }
}

int k86_sfx_busy(void)
{
    if (music >= 0)
        return 1;
    for (int i = 0; i < VOICES; i++)
        if (v[i].live)
            return 1;
    return opened;                      /* (the tail of the last sound is still in the speaker's ring) */
}

void k86_sfx_pump(void)
{
    int any = music >= 0;
    for (int i = 0; i < VOICES && !any; i++)
        any |= v[i].live;
    if (!any) {
        if (opened && ++idle > 20) {            /* a bit under a second of quiet: let go of the speaker */
            k86_audio_close();
            opened = 0;
        }
        return;
    }
    if (!opened) {
        if (k86_audio_open(RATE))
            return;
        opened = 1;
    }
    idle = 0;
    static int lastv = -1;
    int cv = k86_volume();
    if (cv != lastv) {                          /* the Log app shows the level the sound is being played at */
        k86_logf("sfx: volume %d%%", cv);
        lastv = cv;
    }
    int space = k86_audio_space(), queued = k86_audio_buffer() - space, want = TARGET - queued;
    if (want > space)
        want = space;
    int16_t buf[CHUNK * 2];
    while (want > 0) {
        int n = want < CHUNK ? want : CHUNK;
        render(buf, n);
        int w = k86_audio_write(buf, n);
        if (w <= 0)
            break;
        want -= n;
    }
}

/* ---- fire and forget: for apps whose timer isn't always running ---- */
static pid_t shot_child;

void k86_sfx_shot(int id, int semis)
{
    static int ignoring;
    if (!ignoring) {
        signal(SIGCHLD, SIG_IGN);               /* nobody waits for the helper: no zombies */
        ignoring = 1;
    }
    int big = id == SFX_FAIL || id == SFX_WIN || id == SFX_LEVELUP || id == SFX_CRASH;
    if (shot_child > 0 && kill(shot_child, 0) == 0) {
        if (!big)
            return;                             /* a small sound asked for while one plays is dropped */
        kill(shot_child, SIGKILL);              /* a big one takes over */
        usleep(15000);
    }
    shot_child = fork();
    if (shot_child != 0)
        return;
    memset(v, 0, sizeof(v));
    music = -1;
    k86_sfx_play(id, semis);
    if (k86_audio_open(RATE))
        _exit(0);
    int16_t buf[CHUNK * 2];
    for (;;) {
        int live = 0;
        for (int i = 0; i < VOICES; i++)
            live |= v[i].live;
        if (!live)
            break;
        int space = k86_audio_space();
        if (space < 64) {
            usleep(3000);
            continue;
        }
        int n = space < CHUNK ? space : CHUNK;
        render(buf, n);
        if (k86_audio_write(buf, n) < 0)
            break;
    }
    for (int i = 0; i < 100 && k86_audio_space() < k86_audio_buffer(); i++)    /* the last of it plays out */
        usleep(5000);
    k86_audio_close();
    _exit(0);
}

void k86_sfx_close(void)
{
    memset(v, 0, sizeof(v));
    music = -1;
    if (opened) {
        k86_audio_close();
        opened = 0;
    }
}
