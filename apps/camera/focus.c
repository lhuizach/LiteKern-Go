/* Camera: the lens. The motor sits on its own 2-wire bus, at address 0x0c, and behaves
 * like a DW9768: register 2 = 0 for "active", then a 10-bit position, high bits in
 * register 3 and low byte in register 4 (found by test shots: 0 to about 400 goes from
 * near to far, 512 and up is blurry for a subject 25 cm away; far subjects peak past 600). One register per write. The
 * sensor's streaming supplies keep it powered, so it only works while the camera runs.
 *
 * Autofocus is two sweeps: the lens goes through its range and each stop is scored
 * by how much detail there is around the point asked for; then finer stops around
 * the best, and it goes back to the sharpest. */
#include "cam.h"
#include "sdk/kern86.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define LENS_ADDR 0x0c
#define POS_MAX 800
#define COARSE_STOPS 13
#define FINE_STOPS 7
#define SETTLE_FRAMES 5                 /* the lens rings for a moment after a move: about 80 ms */
#define SAMPLES 2                       /* frames averaged for each stop */
#define NOISE 5                         /* a difference this small is the sensor's noise */
#define WINDOW 150                      /* sensor pixels each way from the point */

static int bus = -1;
static int stop = -1, stops, wait_frames, start_pos, step_pos, best_pos, fine;
static long best_score, acc;
static int nsamp;
static int at_x = CAM_W / 2, at_y = CAM_H / 2;

static int lens_write(int reg, int value)
{
    uint8_t b[2] = { (uint8_t)reg, (uint8_t)value };
    return write(bus, b, 2) == 2 ? 0 : -1;
}

void focus_move(int pos)
{
    pos = pos < 0 ? 0 : pos > POS_MAX ? POS_MAX : pos;
    if (bus >= 0) {
        if (lens_write(2, 0) || lens_write(3, pos >> 8) || lens_write(4, pos & 0xff))
            k86_log("camera: focus motor didn't answer");
    }
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

static void sweep(int from, int step, int count, int is_fine)
{
    stop = 0;
    stops = count;
    start_pos = from;
    step_pos = step;
    fine = is_fine;
    wait_frames = SETTLE_FRAMES + (is_fine ? 0 : 4);    /* the first move may be long */
    acc = 0;
    nsamp = 0;
    focus_move(from);
}

void focus_at(int x, int y)
{
    if (bus < 0)
        return;
    at_x = x < WINDOW ? WINDOW : x > CAM_W - WINDOW - 8 ? CAM_W - WINDOW - 8 : x;
    at_y = y < WINDOW ? WINDOW : y > CAM_H - WINDOW - 1 ? CAM_H - WINDOW - 1 : y;
    best_score = -1;
    sweep(0, POS_MAX / (COARSE_STOPS - 1), COARSE_STOPS, 0);
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

void focus_frame(const uint8_t *frame)
{
    if (stop < 0)
        return;
    if (wait_frames > 0) {
        wait_frames--;
        return;
    }
    int pos = start_pos + stop * step_pos;
    acc += sharpness(frame);
    if (++nsamp < SAMPLES)
        return;                             /* a second look at this stop */
    long score = acc / SAMPLES;
    acc = 0;
    nsamp = 0;
    k86_logf("camera: focus pos %d score %d", (uint32_t)pos, (int)score);
    if (score > best_score) {
        best_score = score;
        best_pos = pos;
    }
    if (++stop < stops) {
        focus_move(start_pos + stop * step_pos);
        wait_frames = SETTLE_FRAMES;
        return;
    }
    if (!fine) {
        int step = POS_MAX / (COARSE_STOPS - 1), span = step * 2 / (FINE_STOPS - 1);
        int from = best_pos - step;
        /* The lens has two ends: the fine stops all have to be real positions. */
        if (from + span * (FINE_STOPS - 1) > POS_MAX)
            from = POS_MAX - span * (FINE_STOPS - 1);
        sweep(from < 0 ? 0 : from, span, FINE_STOPS, 1);
        return;
    }
    stop = -1;
    focus_move(best_pos);
    k86_logf("camera: focused at %d", (uint32_t)best_pos);
}
