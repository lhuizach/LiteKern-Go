/* LiteKern Go: sound for apps (sdk/audio.h). Raw ALSA PCM ioctls on /dev/snd/pcmC<card>D2p,
 * the loudspeaker. No malloc, no floating point. */
#define _GNU_SOURCE
#include "sdk/audio.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sound/asound.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define SPEAKER_DEV 2                   /* "Quaternary MI2S": the loudspeaker's amplifier */
#define PHONES_DEV 0                    /* "Primary MI2S": the internal codec: headphones (and the earpiece) */
#define VOLUME_FILE "/dev/shm/lkx-volume"

static int pcm = -1, ctl = -1, on_phones, cur_rate;
static unsigned period, buffer;
static long jack_checked;               /* ms of the last look at the headphone jack */

/* The sound card's mixer, by control name (the numbers differ between kernels). */
static void ctl_id(struct snd_ctl_elem_id *id, const char *name, int iface)
{
    memset(id, 0, sizeof(*id));
    id->iface = (unsigned)iface;
    strncpy((char *)id->name, name, sizeof(id->name) - 1);
}

static int ctl_open(void)
{
    char path[32];
    for (int card = 0; card < 4 && ctl < 0; card++) {
        snprintf(path, sizeof(path), "/dev/snd/controlC%d", card);
        ctl = open(path, O_RDWR | O_CLOEXEC);
        if (ctl >= 0) {
            struct snd_ctl_card_info ci;
            if (ioctl(ctl, SNDRV_CTL_IOCTL_CARD_INFO, &ci) || strcmp((char *)ci.id, "samsunga2015")) {
                close(ctl);
                ctl = -1;
            }
        }
    }
    return ctl;
}

static void ctl_set(const char *name, int value)
{
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof(v));
    ctl_id(&v.id, name, SNDRV_CTL_ELEM_IFACE_MIXER);
    v.value.integer.value[0] = value;           /* the same bytes for an enumerated control's item */
    ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_WRITE, &v);
}

/* Are headphones (or a headset) in the jack? */
static int jack_in(void)
{
    if (ctl < 0 && ctl_open() < 0)
        return 0;
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof(v));
    ctl_id(&v.id, "Headphone Jack", SNDRV_CTL_ELEM_IFACE_CARD);
    if (ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_READ, &v) < 0)
        return 0;
    return v.value.integer.value[0] != 0;
}

/* The codec's paths for the headphones (RX1 left, RX2 right), or all off: the loudspeaker's
 * amplifier needs none of this. */
static void route(int phones)
{
    if (ctl < 0)
        return;
    ctl_set("EAR_S", 0);
    if (phones) {
        ctl_set("RX1 MIX1 INP1", 3);            /* RX1 */
        ctl_set("RX2 MIX1 INP1", 4);            /* RX2 */
        ctl_set("RDAC2 MUX", 1);                /* the right DAC takes RX2 */
        ctl_set("HPHL", 1);
        ctl_set("HPHR", 1);
    } else {
        ctl_set("HPHL", 0);
        ctl_set("HPHR", 0);
    }
}

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int k86_volume(void)
{
    char t[8];
    int fd = open(VOLUME_FILE, O_RDONLY);
    if (fd < 0)
        return 60;
    ssize_t n = read(fd, t, sizeof(t) - 1);
    close(fd);
    if (n <= 0)
        return 60;
    t[n] = '\0';
    int v = atoi(t);
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

static void set_mask(struct snd_pcm_hw_params *p, int var, unsigned bit)
{
    struct snd_mask *m = &p->masks[var - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m, 0, sizeof(*m));
    m->bits[bit >> 5] = 1u << (bit & 31);
}

static void set_interval(struct snd_pcm_hw_params *p, int var, unsigned v)
{
    struct snd_interval *i = &p->intervals[var - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    memset(i, 0, sizeof(*i));
    i->min = i->max = v;
    i->integer = 1;
}

int k86_audio_open(int rate)
{
    if (pcm >= 0)
        return 0;
    char path[48];
    int fd = -1;
    on_phones = jack_in();
    route(on_phones);
    jack_checked = now_ms();
    for (int card = 0; card < 4 && fd < 0; card++) {
        snprintf(path, sizeof(path), "/dev/snd/pcmC%dD%dp", card, on_phones ? PHONES_DEV : SPEAKER_DEV);
        fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    }
    if (fd < 0)
        return -ENODEV;

    struct snd_pcm_hw_params hw;
    memset(&hw, 0xff, sizeof(hw));              /* any: every bit set, every interval wide open */
    for (int v = 0; v <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; v++) {
        struct snd_interval *i = &hw.intervals[v];
        memset(i, 0, sizeof(*i));
        i->max = ~0u;
    }
    hw.rmask = ~0u;
    hw.cmask = 0;
    hw.info = ~0u;
    set_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    set_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    set_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    set_interval(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
    set_interval(&hw, SNDRV_PCM_HW_PARAM_RATE, (unsigned)rate);
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    period = hw.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
    buffer = hw.intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;

    struct snd_pcm_sw_params sw;
    memset(&sw, 0, sizeof(sw));
    sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step = 1;
    sw.avail_min = period;
    sw.start_threshold = period;                /* starts once a period is in */
    sw.stop_threshold = buffer;                 /* an empty ring is an underrun, not looping old sound */
    sw.xfer_align = 1;
    sw.proto = SNDRV_PCM_VERSION;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0 || ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    pcm = fd;
    cur_rate = rate;
    return 0;
}

int k86_audio_space(void)
{
    if (pcm < 0)
        return 0;
    struct snd_pcm_status st;
    memset(&st, 0, sizeof(st));
    if (ioctl(pcm, SNDRV_PCM_IOCTL_STATUS, &st) < 0)
        return 0;
    if (st.state == SNDRV_PCM_STATE_XRUN)
        return (int)buffer;                     /* write() will start it again */
    return (int)st.avail;
}

int k86_audio_write(const int16_t *frames, int count)
{
    if (pcm < 0)
        return -ENODEV;
    long now = now_ms();
    if (now - jack_checked >= 500) {            /* headphones plugged in or out: play from the other output */
        jack_checked = now;
        if (jack_in() != on_phones) {
            int rate = cur_rate;
            k86_audio_close();
            if (k86_audio_open(rate))
                return -ENODEV;
        }
    }
    /* The volume, squared (the ear is not linear), applied to a copy, a few frames at a time. */
    int v = k86_volume();
    int gain = v * v * 256 / 10000;             /* 0..256 */
    int16_t tmp[512 * 2];
    int done = 0;
    while (done < count) {
        int n = count - done < 512 ? count - done : 512;
        for (int i = 0; i < n * 2; i++)
            tmp[i] = (int16_t)(frames[done * 2 + i] * gain / 256);
        struct snd_xferi x = { .buf = tmp, .frames = (snd_pcm_uframes_t)n };
        if (ioctl(pcm, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
            if (errno == EPIPE) {               /* ran dry: start again */
                ioctl(pcm, SNDRV_PCM_IOCTL_PREPARE);
                continue;
            }
            if (errno == EAGAIN || errno == EINTR)
                break;
            return done ? done : -errno;
        }
        done += (int)x.result;
        if (x.result < (snd_pcm_sframes_t)n)
            break;
    }
    return done;
}

void k86_audio_close(void)
{
    if (pcm < 0)
        return;
    ioctl(pcm, SNDRV_PCM_IOCTL_DROP);
    close(pcm);
    pcm = -1;
}

/* A sine from a small table (quarter wave, 64 steps), no maths library. */
static const int16_t quarter[65] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787,
    21403, 22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683,
    28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571, 30852, 31113, 31356, 31580, 31785,
    31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767
};

static int16_t sine(unsigned phase)             /* phase 0..255 is one turn */
{
    unsigned q = phase >> 6, i = phase & 63;
    switch (q) {
    case 0:  return quarter[i];
    case 1:  return quarter[64 - i];
    case 2:  return (int16_t)-quarter[i];
    default: return (int16_t)-quarter[64 - i];
    }
}

void k86_audio_tone(int hz, int ms)
{
    const int rate = 48000;
    if (k86_audio_open(rate))
        return;
    int total = rate / 1000 * ms, fade = rate / 100;    /* 10 ms in and out */
    uint32_t inc = (uint32_t)((uint64_t)hz * 4294967296ull / (uint64_t)rate), phase = 0;
    int16_t buf[256 * 2];
    int done = 0;
    while (done < total) {
        int n = total - done < 256 ? total - done : 256;
        uint32_t p = phase;
        for (int i = 0; i < n; i++, p += inc) {
            int f = done + i;
            int env = f < fade ? f * 256 / fade : total - f < fade ? (total - f) * 256 / fade : 256;
            buf[i * 2] = buf[i * 2 + 1] = (int16_t)(sine(p >> 24) / 2 * env / 256);
        }
        int w = k86_audio_write(buf, n);
        if (w < 0)
            break;
        if (w == 0) {
            usleep(2000);                       /* the ring is full: let it drain */
            continue;
        }
        phase += inc * (uint32_t)w;             /* only w frames went in: carry on from there */
        done += w;
    }
    usleep((unsigned)((uint64_t)2 * period * 1000000ull / (unsigned)rate));   /* the last of it is still in the ring */
    k86_audio_close();
}
