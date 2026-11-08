/* Camera: the lens. The motor sits on its own 2-wire bus, at address 0x0c, and behaves
 * like a DW9768: register 2 = 0 for "active", then a 10-bit position, high bits in
 * register 3 and low byte in register 4 (found by test shots: 0 to about 400 goes from
 * near to far, 512 and up is blurry for a subject 25 cm away; far subjects peak past 600). One register per write. The
 * sensor's streaming supplies keep it powered, so it only works while the camera runs.
 *
 * Autofocus: the lens goes through its range in 7 steps, each scored by how much detail
 * there is around the point asked for; it stops early once the detail has clearly fallen
 * away past the best, and a parabola through the scores round the best gives the peak. */
#include "cam.h"
#include "sdk/kern86.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define LENS_ADDR 0x0c
#define POS_MAX 800
#define STOPS 7                         /* 0 to POS_MAX in steps of 133 */
#define SETTLE_FRAMES 3                 /* after a move: the lens rings for a moment and the frames in flight are old */
#define SAMPLES 1                       /* frames looked at for each stop */
#define GIVE_UP 60                      /* per cent of the best: two stops below it in a row past the peak and it's found */
#define NOISE 5                         /* a difference this small is the sensor's noise */
#define WINDOW 150                      /* sensor pixels each way from the point */

static int bus = -1;
static int stop = -1, wait_frames, best_pos, below;
static long scores[STOPS];
static uint32_t began;
static long best_score, acc;
static int nsamp;
static int at_x = CAM_W / 2, at_y = CAM_H / 2;

static int lens_write(int reg, int value)
{
    uint8_t b[2] = { (uint8_t)reg, (uint8_t)value };
    return write(bus, b, 2) == 2 ? 0 : -1;
}

static int lens_pos = -1;               /* where the lens was last sent (-1: never) */

void focus_move(int pos)
{
    pos = pos < 0 ? 0 : pos > POS_MAX ? POS_MAX : pos;
    lens_pos = pos;
    if (bus >= 0) {
        if (lens_write(2, 0) || lens_write(3, pos >> 8) || lens_write(4, pos & 0xff))
            k86_log("camera: focus motor didn't answer");
    }
}

/* The lens loses its power whenever the sensor stops streaming (a photo does that twice):
 * this sends it back to where it was, and waits for it to settle. */
void focus_restore(void)
{
    if (bus < 0 || lens_pos < 0)
        return;
    usleep(60 * 1000);                  /* the lens's supply comes up with the stream */
    focus_move(lens_pos);
    usleep(150 * 1000);
}

int focus_open(void)
{
    DIR *d = opendir("/sys/class/i2c-dev");
    struct dirent *e;
    while (d && bus < 0 && (e = readdir(d))) {
        char path[300], name[64] = "";
        snprintf(path, sizeof(path), "/sys/class/i2c-dev/%s/name", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        if (!fgets(name, sizeof(name), f))
            name[0] = 0;
        fclose(f);
        if (!strstr(name, "focus"))
            continue;
        snprintf(path, sizeof(path), "/dev/%s", e->d_name);
        bus = open(path, O_RDWR);
        if (bus >= 0 && ioctl(bus, I2C_SLAVE, LENS_ADDR)) {
            close(bus);
            bus = -1;
        }
    }
    if (d)
        closedir(d);
    if (bus < 0) {
        k86_log("camera: no focus motor");
        return -1;
    }
    k86_log("camera: focus motor found");
    return 0;
}

void focus_close(void)
{
    if (bus >= 0)
        close(bus);
    bus = -1;
}

int focus_active(void)
{
    return stop >= 0;
}

static void sweep(void)
{
    stop = 0;
    below = 0;
    began = k86_uptime_ms();
    best_score = -1;
    wait_frames = SETTLE_FRAMES + 2;        /* the first move may be long */
    acc = 0;
    nsamp = 0;
    focus_move(0);
}

void focus_at(int x, int y)
{
    if (bus < 0)
        return;
    at_x = x < WINDOW ? WINDOW : x > CAM_W - WINDOW - 8 ? CAM_W - WINDOW - 8 : x;
    at_y = y < WINDOW ? WINDOW : y > CAM_H - WINDOW - 1 ? CAM_H - WINDOW - 1 : y;
    sweep();
}

/* How much fine detail there is round the point: the differences between greens two
 * pixels apart along a row, and between rows two apart. On the odd rows pixels 0 and 2 of
 * each group of four (five bytes) are green; their high 8 bits are the first bytes.
 * Differences within the sensor's noise are left out, and the rest squared, so one sharp
 * edge counts for more than a lot of faint texture. */
static long sharpness(const uint8_t *frame)
{
    int stride = cam_stride();
    long score = 0;
    for (int y = (at_y - WINDOW) | 1; y < at_y + WINDOW; y += 2) {
        const uint8_t *row = frame + (size_t)y * (size_t)stride, *down = row + 2 * (size_t)stride;
        for (int x = (at_x - WINDOW) & ~3; x < at_x + WINDOW; x += 4) {
            int g = x / 4 * 5;
            int a = row[g], b = row[g + 2], c = row[g + 5], v = down[g];
            int d[3] = { a - b, b - c, a - v };
            for (int k = 0; k < 3; k++) {
                int m = d[k] < 0 ? -d[k] : d[k];
                if (m > NOISE)
                    score += m * m;
            }
        }
    }
    return score;
}

/* The best stop is the peak to within a step; the scores either side of it say which way
 * the real peak lies. A parabola through the logs of three scores (detail falls off like a
 * bell, and the log of a bell is a parabola) gives where its top is. */
static int refine(int i)
{
    int step = POS_MAX / (STOPS - 1);
    int pos = i * step;
    if (i <= 0 || i >= STOPS - 1 || scores[i - 1] <= 0 || scores[i + 1] <= 0 || scores[i] <= 0)
        return pos;
    double l = log((double)scores[i - 1]), m = log((double)scores[i]), r = log((double)scores[i + 1]);
    double d = l - 2 * m + r;
    if (d >= 0)
        return pos;                         /* no peak: flat or a dip */
    double off = 0.5 * (l - r) / d;         /* in steps, from the best stop */
    off = off < -0.5 ? -0.5 : off > 0.5 ? 0.5 : off;
    return pos + (int)(off * step);
}

void focus_frame(const uint8_t *frame)
{
    if (stop < 0)
        return;
    if (wait_frames > 0) {
        wait_frames--;
        return;
    }
    int step = POS_MAX / (STOPS - 1), pos = stop * step;
    acc += sharpness(frame);
    if (++nsamp < SAMPLES)
        return;
    long score = acc / SAMPLES;
    acc = 0;
    nsamp = 0;
    scores[stop] = score;
    k86_logf("camera: focus pos %d score %d", (uint32_t)pos, (int)score);
    if (score > best_score) {
        best_score = score;
        best_pos = stop;
        below = 0;
    } else if (score * 100 < best_score * GIVE_UP) {
        below++;
    } else {
        below = 0;                          /* it came back up: not past the peak */
    }
    /* Done when all the stops are looked at, or two in a row past the best are well below it. */
    if (++stop < STOPS && below < 2) {
        focus_move(stop * step);
        wait_frames = SETTLE_FRAMES;
        return;
    }
    for (int i = stop; i < STOPS; i++)
        scores[i] = 0;
    stop = -1;
    int at = refine(best_pos);
    focus_move(at);
    k86_logf("camera: focused at %d in %u ms", (uint32_t)at, k86_uptime_ms() - began);
}
