/* Camera: the lens. The motor sits on its own 2-wire bus, at address 0x0c, and looks
 * like an AK7345: a 9-bit position in the top of registers 0 and 1, and 0 in register 2
 * for "active" (Samsung's msm_actuator.c writes those; Linux's ak7375.c has the same
 * layout). One byte to a write: it refuses a third byte. The sensor's streaming
 * supplies keep it powered, so it only works while the camera runs.
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
#define POS_MAX 511
#define COARSE_STOPS 12
#define FINE_STOPS 7
#define SETTLE_FRAMES 2                 /* a frame or two before a move shows */
#define WINDOW 150                      /* sensor pixels each way from the point */

static int bus = -1;
static int stop = -1, stops, wait_frames, start_pos, step_pos, best_pos, fine;
static long best_score;
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
        int v = pos << 7;
        if (lens_write(0, v >> 8) || lens_write(1, v & 0xff) || lens_write(2, 0))
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
    wait_frames = SETTLE_FRAMES + (is_fine ? 0 : 2);    /* the first move may be long */
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

/* How much detail: the difference between greens two along, from the odd rows, where
 * green starts each pair. Small differences are left out, as noise. */
static long sharpness(const uint8_t *frame)
{
    int stride = cam_stride();
    long score = 0;
    for (int y = (at_y - WINDOW) | 1; y < at_y + WINDOW; y += 2) {
        const uint8_t *row = frame + (size_t)y * (size_t)stride;
        for (int x = (at_x - WINDOW) & ~3; x < at_x + WINDOW; x += 4) {
            int d = row[x / 4 * 5] - row[x / 4 * 5 + 5];
            d = d < 0 ? -d : d;
            if (d > 3)
                score += d * d;
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
    long score = sharpness(frame);
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
        sweep(best_pos - step, span, FINE_STOPS, 1);
        return;
    }
    stop = -1;
    focus_move(best_pos);
    k86_logf("camera: focused at %d", (uint32_t)best_pos);
}
