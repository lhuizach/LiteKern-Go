/* LiteKern X on Linux — the devices the desktop reads and draws through
 * (platform/platform.h). The same names and interfaces as the kernel's
 * drivers, so kernel/screen.c, storage.c, ramdisk.c, desktop.c and the lock
 * screen work unchanged:
 *
 *   fb0     kernel/fb.h: blits land in a framebuffer here, then the backend
 *   kbd0    kernel/input.h key events, through the shared layouts (keymap.h)
 *   mouse0  kernel/input.h pointer packets: moves, as the PS/2 mouse sends them
 *   rtc0    kernel/rtc.h: local time; a tick a second; RTC_SET_RATE sets how
 *           often main.c's loop wakes
 *   boot0   kernel/block.h, on a disk image file: the ramdisk (apps,
 *           wallpapers, fonts) and the FAT32 partition (settings, users, files) */
#include "platform/platform.h"
#include "kernel/block.h"
#include "kernel/driver.h"
#include "kernel/errno.h"
#include "kernel/fb.h"
#include "kernel/input.h"
#include "kernel/keymap.h"
#include "kernel/printk.h"
#include "kernel/rtc.h"
#include "defaults.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const struct backend *be;
static uint32_t *front;
static int fw, fh;

/* --- fb0 --------------------------------------------------------------------------------------- */

static int fb_init(device_t *dev)
{
    (void)dev;
    front = calloc((size_t)fw * (size_t)fh, 4);
    return front ? 0 : -ENOMEM;
}

static void clip_present(int x, int y, int w, int h)
{
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > fw) w = fw - x;
    if (y + h > fh) h = fh - y;
    if (w > 0 && h > 0)
        be->present(front, fw, x, y, w, h);
}

static int fb_ioctl(device_t *dev, unsigned cmd, void *arg)
{
    (void)dev;
    if (!arg)
        return -EINVAL;
    switch (cmd) {
    case FB_GET_INFO:
        *(struct fb_info *)arg = (struct fb_info){ (uint32_t)fw, (uint32_t)fh, (uint32_t)fw * 4, 32,
                                                   (uint32_t)front, 0, 0, 0 };
        return 0;
    case FB_FILL_RECT: {
        const struct fb_rect *r = arg;
        for (uint32_t y = r->y; y < r->y + r->h && y < (uint32_t)fh; y++)
            for (uint32_t x = r->x; x < r->x + r->w && x < (uint32_t)fw; x++)
                front[y * (uint32_t)fw + x] = r->colour;
        clip_present((int)r->x, (int)r->y, (int)r->w, (int)r->h);
        return 0;
    }
    case FB_BLIT: {
        const struct fb_blit *b = arg;
        if (b->x >= (uint32_t)fw || b->y >= (uint32_t)fh)
            return 0;
        uint32_t w = b->w, h = b->h;
        if (b->x + w > (uint32_t)fw) w = (uint32_t)fw - b->x;
        if (b->y + h > (uint32_t)fh) h = (uint32_t)fh - b->y;
        for (uint32_t row = 0; row < h; row++)
            memcpy(front + (b->y + row) * (uint32_t)fw + b->x, b->pixels + row * b->stride, w * 4);
        clip_present((int)b->x, (int)b->y, (int)w, (int)h);
        return 0;
    }
    default:
        return -ENOSYS;
    }
}

static const driver_t fb_driver = {
    .name = "linux-fb", .init = fb_init, .read = driver_nosys_read, .write = driver_nosys_write,
    .ioctl = fb_ioctl, .shutdown = driver_noop_shutdown,
};

const uint32_t *plat_framebuffer(int *w, int *h)
{
    *w = fw;
    *h = fh;
    return front;
}

/* --- kbd0 and mouse0: queues filled by the backend --------------------------------------------- */

#define QUEUE 256
static struct key_event keys[QUEUE];
static uint32_t khead, ktail, kdropped;
static struct mouse_event moves[QUEUE];
static uint32_t mhead, mtail, mdropped;
static int px = -1, py = -1;        /* where the pointer was last reported (-1: not yet) */
static uint8_t held;

void plat_key(uint16_t key, int pressed)
{
    if (!key)
        return;
    if (khead - ktail == QUEUE) {
        ktail++;
        kdropped++;
    }
    keys[khead++ % QUEUE] = keymap_event(key, pressed);
}

void plat_pointer(int x, int y, uint8_t buttons)
{
    if (px < 0) {                   /* the desktop's pointer starts at the centre */
        px = fw / 2;
        py = fh / 2;
    }
    int dx = x - px, dy = y - py;
    if (!dx && !dy && buttons == held)
        return;
    /* PS/2 packets carry at most 255 either way; the desktop adds them up. */
    while (dx || dy || buttons != held) {
        int sx = dx > 255 ? 255 : dx < -255 ? -255 : dx;
        int sy = dy > 255 ? 255 : dy < -255 ? -255 : dy;
        int last = sx == dx && sy == dy;
        if (mhead - mtail == QUEUE) {
            mtail++;
            mdropped++;
        }
        moves[mhead++ % QUEUE] = (struct mouse_event){ .dx = (int16_t)sx, .dy = (int16_t)sy,
                                                       .buttons = last ? buttons : held };
        dx -= sx;
        dy -= sy;
        if (last)
            held = buttons;
    }
    px = x;
    py = y;
}

void plat_wheel(int notches)
{
    while (notches) {
        int n = notches > 127 ? 127 : notches < -127 ? -127 : notches;
        if (mhead - mtail == QUEUE) {
            mtail++;
            mdropped++;
        }
        moves[mhead++ % QUEUE] = (struct mouse_event){ .buttons = held, .wheel = (int8_t)n };
        notches -= n;
    }
}

static int no_init(device_t *dev)
{
    (void)dev;
    return 0;
}

static int kbd_read(device_t *dev, void *buf, size_t len)
{
    (void)dev;
    if (len % sizeof(struct key_event))
        return -EINVAL;
    struct key_event *out = buf;
    size_t n = 0;
    while (n < len / sizeof(*out) && ktail != khead)
        out[n++] = keys[ktail++ % QUEUE];
    return (int)(n * sizeof(*out));
}

static int kbd_ioctl(device_t *dev, unsigned cmd, void *arg)
{
    (void)dev;
    if (cmd != KBD_GET_DROPPED || !arg)
        return cmd == KBD_GET_DROPPED ? -EINVAL : -ENOSYS;
    *(uint32_t *)arg = kdropped;
    return 0;
}

static int mouse_read(device_t *dev, void *buf, size_t len)
{
    (void)dev;
    if (len % sizeof(struct mouse_event))
        return -EINVAL;
    struct mouse_event *out = buf;
    size_t n = 0;
    while (n < len / sizeof(*out) && mtail != mhead)
        out[n++] = moves[mtail++ % QUEUE];
    return (int)(n * sizeof(*out));
}

static int mouse_ioctl(device_t *dev, unsigned cmd, void *arg)
{
    (void)dev;
    if (cmd != MOUSE_GET_DROPPED || !arg)
        return cmd == MOUSE_GET_DROPPED ? -EINVAL : -ENOSYS;
    *(uint32_t *)arg = mdropped;
    return 0;
}

static const driver_t kbd_driver = {
    .name = "linux-kbd", .init = no_init, .read = kbd_read, .write = driver_nosys_write,
    .ioctl = kbd_ioctl, .shutdown = driver_noop_shutdown,
};
static const driver_t mouse_driver = {
    .name = "linux-mouse", .init = no_init, .read = mouse_read, .write = driver_nosys_write,
    .ioctl = mouse_ioctl, .shutdown = driver_noop_shutdown,
};

/* Linux evdev key codes are set 1 scancodes for the main block (1-88); the
 * rest are the E0-prefixed keys. */
uint16_t plat_evdev_key(unsigned code)
{
    if (code >= 1 && code <= 88 && code != 84)
        return (uint16_t)code;
    switch (code) {
    case 96:  return KEY_EXTENDED | 0x1c;   /* keypad Enter */
    case 97:  return KEY_RCTRL;
    case 98:  return KEY_EXTENDED | 0x35;   /* keypad / */
    case 100: return KEY_RALT;              /* AltGr */
    case 102: return KEY_HOME;
    case 103: return KEY_UP;
    case 104: return KEY_PAGEUP;
    case 105: return KEY_LEFT;
    case 106: return KEY_RIGHT;
    case 107: return KEY_END;
    case 108: return KEY_DOWN;
    case 109: return KEY_PAGEDOWN;
    case 110: return KEY_INSERT;
    case 111: return KEY_DELETE;
    case 125: return KEY_LSUPER;
    case 126: return KEY_RSUPER;
    default:  return 0;
    }
}

/* --- rtc0 -------------------------------------------------------------------------------------- */

static uint32_t wake_hz;

static int rtc_read(device_t *dev, void *buf, size_t len)
{
    (void)dev;
    if (len < sizeof(struct rtc_time))
        return -EINVAL;
    time_t now = time(0);
    struct tm tm;
    localtime_r(&now, &tm);
    *(struct rtc_time *)buf = (struct rtc_time){ (uint16_t)(tm.tm_year + 1900), (uint8_t)(tm.tm_mon + 1),
                                                 (uint8_t)tm.tm_mday, (uint8_t)tm.tm_hour,
                                                 (uint8_t)tm.tm_min, (uint8_t)tm.tm_sec,
                                                 (uint8_t)tm.tm_wday };
    return (int)sizeof(struct rtc_time);
}

static int rtc_ioctl(device_t *dev, unsigned cmd, void *arg)
{
    (void)dev;
    if (!arg)
        return -EINVAL;
    switch (cmd) {
    case RTC_GET_TICKS:                     /* the chip's update interrupt: once a second */
        *(uint32_t *)arg = (uint32_t)time(0);
        return 0;
    case RTC_SET_RATE:
        wake_hz = *(const uint32_t *)arg;
        return 0;
    default:
        return -ENOSYS;
    }
}

static const driver_t rtc_driver = {
    .name = "linux-rtc", .init = no_init, .read = rtc_read, .write = driver_nosys_write,
    .ioctl = rtc_ioctl, .shutdown = driver_noop_shutdown,
};

uint32_t plat_wake_hz(void)
{
    return wake_hz;
}

/* --- boot0: a disk image file ------------------------------------------------------------------ */

static const char *disk_path;
static int disk_fd = -1, disk_ro;
static uint32_t disk_sectors;

static int disk_init(device_t *dev)
{
    (void)dev;
    disk_fd = open(disk_path, O_RDWR);
    if (disk_fd < 0) {
        disk_fd = open(disk_path, O_RDONLY);
        disk_ro = 1;
    }
    struct stat st;
    if (disk_fd < 0 || fstat(disk_fd, &st) < 0)
        return -ENODEV;
    uint64_t bytes = (uint64_t)st.st_size;
    if (S_ISBLK(st.st_mode) && ioctl(disk_fd, BLKGETSIZE64, &bytes))   /* a real disk: no file size */
        return -ENODEV;
    disk_sectors = (uint32_t)(bytes / BLK_SECTOR);
    return 0;
}

static int disk_rw(struct blk_io *io, int write)
{
    if (!io || io->lba >= disk_sectors || io->count > disk_sectors - io->lba)
        return -EINVAL;
    if (write && disk_ro)
        return -EROFS;
    off_t at = (off_t)io->lba * BLK_SECTOR;
    size_t want = (size_t)io->count * BLK_SECTOR, done = 0;
    while (done < want) {
        ssize_t n = write ? pwrite(disk_fd, (const char *)io->buf + done, want - done, at + (off_t)done)
                          : pread(disk_fd, (char *)io->buf + done, want - done, at + (off_t)done);
        if (n <= 0)
            return -EIO;
        done += (size_t)n;
    }
    /* On the stick at once, as the kernel version's BIOS writes were: Linux
     * would otherwise hold them for up to half a minute, and a setting saved
     * just before the power goes would be lost. */
    if (write && fdatasync(disk_fd))
        return -EIO;
    return 0;
}

static int disk_ioctl(device_t *dev, unsigned cmd, void *arg)
{
    (void)dev;
    switch (cmd) {
    case BLK_GET_INFO: {
        if (!arg)
            return -EINVAL;
        struct blk_info *i = arg;
        memset(i, 0, sizeof(*i));
        i->sectors = disk_sectors;
        i->read_only = (uint8_t)disk_ro;
        i->removable = 1;
        strncpy(i->model, "disk image", sizeof(i->model) - 1);
        return 0;
    }
    case BLK_READ:
        return disk_rw(arg, 0);
    case BLK_WRITE:
        return disk_rw(arg, 1);
    default:
        return -ENOSYS;
    }
}

static const driver_t disk_driver = {
    .name = "linux-disk", .init = disk_init, .read = driver_nosys_read, .write = driver_nosys_write,
    .ioctl = disk_ioctl, .shutdown = driver_noop_shutdown,
};

/* --- -------------------------------------------------------------------------------------------- */

void plat_devices_init(const struct backend *b, int w, int h, const char *disk, const char *layout)
{
    be = b;
    fw = w;
    fh = h;
    keymap_select(layout ? layout : DEFAULT_KEYBOARD);
    device_add_legacy(&fb_driver, "fb0", 0);
    device_add_legacy(&kbd_driver, "kbd0", 0);
    device_add_legacy(&mouse_driver, "mouse0", 0);
    device_add_legacy(&rtc_driver, "rtc0", 0);
    if (disk) {
        disk_path = disk;
        device_add_legacy(&disk_driver, "boot0", 0);
    }
    kprintf("kbd: layout %s\n", keymap_name());
}
