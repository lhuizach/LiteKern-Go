/* LiteKern Go: a second finger, for pinching. The shell's platform keeps where the fingers are in
 * /dev/shm/lkx-touch2 (platform/drm.c); k86_touch2() reads it. Positions are in screen pixels: the pointer's
 * finger is the one the app's K86_EVENT_POINTER events follow, the same finger as (x1, y1). Compare a
 * pointer event's own x, y with (x1, y1) to turn screen pixels into canvas pixels. n is how many fingers are
 * down; x2, y2 are 0 unless n >= 2. Returns 0 if there's nothing there (a PC without a touchscreen). */
#ifndef K86_TOUCH2_H
#define K86_TOUCH2_H

#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

struct k86_touch2 {
    int32_t seq, n, x1, y1, x2, y2;
};

static inline int k86_touch2(struct k86_touch2 *t)
{
    int fd = open("/dev/shm/lkx-touch2", O_RDONLY | O_CLOEXEC);
    int ok = fd >= 0 && pread(fd, t, sizeof(*t), 0) == (ssize_t)sizeof(*t);
    if (fd >= 0)
        close(fd);
    if (!ok)
        *t = (struct k86_touch2){ 0 };
    return ok;
}

#endif
