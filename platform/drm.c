/* LiteKern X on Linux — the drm backend: the real screen and input, on the
 * EeePC (phase L3) and in QEMU. Linux's own interfaces, no libraries:
 *
 *   screen  DRM/KMS: the connected display's preferred mode (the EeePC's
 *           panel: 1024x600), one "dumb" buffer scanned out as it is; present
 *           copies the changed rectangles into it
 *   input   evdev: every keyboard and pointer in /dev/input (taken for
 *           ourselves, so the text console behind doesn't see the keys), and
 *           ones plugged in later (inotify on /dev/input)
 *
 * Linux's evdev key codes become the kernel's (plat_evdev_key()); the layout
 * (AZERTY) is LiteKern X's own. Pointers: relative ones (the touchpad, as a
 * PS/2 mouse: psmouse.proto=exps, and USB mice) move the pointer by what they
 * send, as the kernel version's PS/2 driver did; absolute ones (tablets)
 * place it. */
#include "platform/platform.h"
#include "kernel/input.h"
#include "kernel/printk.h"

#include <dirent.h>
#include <errno.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int card = -1, ep = -1, ino = -1, tty = -1;
static uint8_t *scan;               /* the dumb buffer, mapped */
static uint32_t pitch, fb_id;
static int sw, sh;

/* --- the screen -------------------------------------------------------------------------------- */

static int open_display(int *w, int *h)
{
    card = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (card < 0) {
        kprintf("drm: no /dev/dri/card0\n");
        return -1;
    }
    struct drm_mode_card_res res = { 0 };
    if (ioctl(card, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_connectors || !res.count_crtcs) {
        kprintf("drm: the card has no display outputs\n");
        return -1;
    }
    uint32_t conns[16], crtcs[16], encs[16];
    res.count_connectors = res.count_connectors > 16 ? 16 : res.count_connectors;
    res.count_crtcs = res.count_crtcs > 16 ? 16 : res.count_crtcs;
    res.count_encoders = res.count_encoders > 16 ? 16 : res.count_encoders;
    res.count_fbs = 0;
    res.connector_id_ptr = (uintptr_t)conns;
    res.crtc_id_ptr = (uintptr_t)crtcs;
    res.encoder_id_ptr = (uintptr_t)encs;
    if (ioctl(card, DRM_IOCTL_MODE_GETRESOURCES, &res))
        return -1;

    /* A connected output: the built-in panel (LVDS, eDP) first. */
    struct drm_mode_get_connector best = { 0 };
    struct drm_mode_modeinfo mode = { 0 };
    int found = 0;
    for (uint32_t i = 0; i < res.count_connectors; i++) {
        struct drm_mode_get_connector c = { .connector_id = conns[i] };
        if (ioctl(card, DRM_IOCTL_MODE_GETCONNECTOR, &c) || c.connection != 1 || !c.count_modes)
            continue;
        struct drm_mode_modeinfo modes[64];
        uint32_t encoders[8];
        c.count_modes = c.count_modes > 64 ? 64 : c.count_modes;
        c.count_encoders = c.count_encoders > 8 ? 8 : c.count_encoders;
        c.count_props = 0;
        c.modes_ptr = (uintptr_t)modes;
        c.encoders_ptr = (uintptr_t)encoders;
        if (ioctl(card, DRM_IOCTL_MODE_GETCONNECTOR, &c))
            continue;
        int internal = c.connector_type == DRM_MODE_CONNECTOR_LVDS || c.connector_type == DRM_MODE_CONNECTOR_eDP;
        if (found && !internal)
            continue;
        best = c;
        mode = modes[0];
        for (uint32_t m = 0; m < c.count_modes; m++)
            if (modes[m].type & DRM_MODE_TYPE_PREFERRED) {
                mode = modes[m];
                break;
            }
        if (!best.encoder_id && c.count_encoders)
            best.encoder_id = encoders[0];
        found = 1;
        if (internal)
            break;
    }
    if (!found) {
        kprintf("drm: no display connected\n");
        return -1;
    }
    struct drm_mode_get_encoder enc = { .encoder_id = best.encoder_id };
    uint32_t crtc = 0;
    if (!ioctl(card, DRM_IOCTL_MODE_GETENCODER, &enc))
        crtc = enc.crtc_id;
    for (uint32_t i = 0; !crtc && i < res.count_crtcs; i++)
        if (enc.possible_crtcs & (1u << i))
            crtc = crtcs[i];
    if (!crtc)
        crtc = crtcs[0];

    struct drm_mode_create_dumb dumb = { .width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32 };
    if (ioctl(card, DRM_IOCTL_MODE_CREATE_DUMB, &dumb)) {
        kprintf("drm: no buffer for %ux%u\n", mode.hdisplay, mode.vdisplay);
        return -1;
    }
    struct drm_mode_fb_cmd fb = { .width = mode.hdisplay, .height = mode.vdisplay, .pitch = dumb.pitch,
                                  .bpp = 32, .depth = 24, .handle = dumb.handle };
    struct drm_mode_map_dumb map = { .handle = dumb.handle };
    if (ioctl(card, DRM_IOCTL_MODE_ADDFB, &fb) || ioctl(card, DRM_IOCTL_MODE_MAP_DUMB, &map))
        return -1;
    scan = mmap(0, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, card, (off_t)map.offset);
    if (scan == MAP_FAILED)
        return -1;
    memset(scan, 0, dumb.size);
    pitch = dumb.pitch;
    fb_id = fb.fb_id;
    struct drm_mode_crtc set = { .set_connectors_ptr = (uintptr_t)&best.connector_id, .count_connectors = 1,
                                 .crtc_id = crtc, .fb_id = fb.fb_id, .mode_valid = 1, .mode = mode };
    if (ioctl(card, DRM_IOCTL_MODE_SETCRTC, &set)) {
        kprintf("drm: couldn't show the mode %s\n", mode.name);
        return -1;
    }
    sw = *w = mode.hdisplay;
    sh = *h = mode.vdisplay;
    kprintf("drm: %ux%u on connector %u (%s), %u Hz\n", mode.hdisplay, mode.vdisplay, best.connector_id,
            best.connector_type == DRM_MODE_CONNECTOR_LVDS ? "the built-in panel" : "an output", mode.vrefresh);
    return 0;
}

/* What changed since the last flush: some drivers (QEMU's, and others with
 * a shadow copy) only show what they're told changed (DIRTYFB). */
#define CLIPS 16
static struct drm_clip_rect clips[CLIPS];
static int nclips;

static void d_present(const uint32_t *px, int stride, int x, int y, int w, int h)
{
    if (!scan || x < 0 || y < 0 || x + w > sw || y + h > sh)
        return;
    for (int r = 0; r < h; r++)
        memcpy(scan + (size_t)(y + r) * pitch + (size_t)x * 4, px + (y + r) * stride + x, (size_t)w * 4);
    struct drm_clip_rect c = { (uint16_t)x, (uint16_t)y, (uint16_t)(x + w), (uint16_t)(y + h) };
    if (nclips == CLIPS) {                  /* too many: one around them all */
        for (int i = 1; i < CLIPS; i++) {
            clips[0].x1 = clips[i].x1 < clips[0].x1 ? clips[i].x1 : clips[0].x1;
            clips[0].y1 = clips[i].y1 < clips[0].y1 ? clips[i].y1 : clips[0].y1;
            clips[0].x2 = clips[i].x2 > clips[0].x2 ? clips[i].x2 : clips[0].x2;
            clips[0].y2 = clips[i].y2 > clips[0].y2 ? clips[i].y2 : clips[0].y2;
        }
        nclips = 1;
    }
    clips[nclips++] = c;
}

static void flush(void)
{
    if (!nclips)
        return;
    struct drm_mode_fb_dirty_cmd d = { .fb_id = fb_id, .num_clips = (uint32_t)nclips,
                                       .clips_ptr = (uintptr_t)clips };
    ioctl(card, DRM_IOCTL_MODE_DIRTYFB, &d);    /* drivers that scan out directly ignore it */
    nclips = 0;
}

/* --- input ------------------------------------------------------------------------------------- */

#define DEVS 16
static struct dev {
    int fd;
    int abs;                        /* a tablet: place the pointer */
    struct input_absinfo ax, ay;
    char name[64];
} devs[DEVS];
static int px, py, dx, dy, moved;
static uint8_t buttons;

#define BIT(arr, n) ((arr)[(n) / (8 * sizeof(long))] >> ((n) % (8 * sizeof(long))) & 1)

static void add_device(const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return;
    unsigned long ev[1] = { 0 }, keys[KEY_MAX / (8 * sizeof(long)) + 1] = { 0 }, abs[1] = { 0 }, rel[1] = { 0 };
    ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel);
    int keyboard = BIT(ev, EV_KEY) && BIT(keys, KEY_A) && BIT(keys, KEY_ENTER);
    int mouse = BIT(ev, EV_REL) && BIT(rel, REL_X) && BIT(keys, BTN_LEFT);
    int tablet = BIT(ev, EV_ABS) && BIT(abs, ABS_X) && BIT(abs, ABS_Y) && BIT(keys, BTN_LEFT);
    if (!keyboard && !mouse && !tablet) {
        close(fd);
        return;
    }
    struct dev *d = 0;
    for (int i = 0; i < DEVS && !d; i++)
        if (!devs[i].fd)
            d = &devs[i];
    if (!d) {
        close(fd);
        return;
    }
    memset(d, 0, sizeof(*d));
    d->fd = fd;
    d->abs = tablet && !mouse;
    if (d->abs) {
        ioctl(fd, EVIOCGABS(ABS_X), &d->ax);
        ioctl(fd, EVIOCGABS(ABS_Y), &d->ay);
    }
    ioctl(fd, EVIOCGNAME(sizeof(d->name) - 1), d->name);
    ioctl(fd, EVIOCGRAB, 1);                /* ours: the text console behind doesn't get the keys */
    struct epoll_event e = { .events = EPOLLIN, .data.ptr = d };
    epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e);
    kprintf("input: %s (%s%s%s)\n", d->name, keyboard ? "keyboard" : "", keyboard && (mouse || tablet) ? ", " : "",
            mouse ? "pointer" : tablet ? "tablet" : "");
}

static void scan_devices(void)
{
    DIR *dir = opendir("/dev/input");
    struct dirent *e;
    while (dir && (e = readdir(dir))) {
        if (strncmp(e->d_name, "event", 5))
            continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        add_device(path);
    }
    if (dir)
        closedir(dir);
}

static int scale(int v, const struct input_absinfo *a, int size)
{
    int range = a->maximum - a->minimum;
    return range > 0 ? (int)((int64_t)(v - a->minimum) * (size - 1) / range) : 0;
}

static void read_device(struct dev *d)
{
    struct input_event evs[64];
    for (;;) {
        ssize_t n = read(d->fd, evs, sizeof(evs));
        if (n <= 0) {
            if (n == 0 || (n < 0 && errno == ENODEV))
                goto gone;
            return;
        }
        for (int i = 0; i < n / (int)sizeof(evs[0]); i++) {
            struct input_event *e = &evs[i];
            if (e->type == EV_KEY && e->code < BTN_MISC) {
                plat_key(plat_evdev_key(e->code), e->value != 0);
            } else if (e->type == EV_KEY && (e->code == BTN_LEFT || e->code == BTN_RIGHT || e->code == BTN_MIDDLE)) {
                uint8_t b = e->code == BTN_LEFT ? MOUSE_LEFT : e->code == BTN_RIGHT ? MOUSE_RIGHT : MOUSE_MIDDLE;
                buttons = e->value ? (uint8_t)(buttons | b) : (uint8_t)(buttons & ~b);
                moved = 1;
            } else if (e->type == EV_REL && e->code == REL_X) {
                dx += e->value, moved = 1;
            } else if (e->type == EV_REL && e->code == REL_Y) {
                dy += e->value, moved = 1;
            } else if (e->type == EV_ABS && d->abs && e->code == ABS_X) {
                px = scale(e->value, &d->ax, sw), moved = 1;
            } else if (e->type == EV_ABS && d->abs && e->code == ABS_Y) {
                py = scale(e->value, &d->ay, sh), moved = 1;
            } else if (e->type == EV_SYN && e->code == SYN_REPORT && moved) {
                px += dx, py += dy;
                dx = dy = moved = 0;
                px = px < 0 ? 0 : px >= sw ? sw - 1 : px;
                py = py < 0 ? 0 : py >= sh ? sh - 1 : py;
                plat_pointer(px, py, buttons);
            }
        }
    }
gone:
    kprintf("input: %s unplugged\n", d->name);
    epoll_ctl(ep, EPOLL_CTL_DEL, d->fd, 0);
    close(d->fd);
    d->fd = 0;
}

/* --- the backend ------------------------------------------------------------------------------- */

static int d_open(int *w, int *h)
{
    if (open_display(w, h))
        return -1;
    /* The text console behind: no cursor, no redrawing over us. */
    tty = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (tty >= 0)
        ioctl(tty, KDSETMODE, KD_GRAPHICS);
    ep = epoll_create1(EPOLL_CLOEXEC);
    ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino >= 0) {
        inotify_add_watch(ino, "/dev/input", IN_CREATE);
        struct epoll_event e = { .events = EPOLLIN, .data.ptr = 0 };
        epoll_ctl(ep, EPOLL_CTL_ADD, ino, &e);
    }
    px = *w / 2, py = *h / 2;               /* where the desktop's pointer starts */
    scan_devices();
    return 0;
}

static int d_fd(void)
{
    flush();                                /* the loop is about to sleep: show it all */
    return ep;
}

static int d_due(void)
{
    return -1;
}

static void d_pump(void)
{
    struct epoll_event evs[16];
    int n = epoll_wait(ep, evs, 16, 0);
    for (int i = 0; i < n; i++) {
        if (evs[i].data.ptr) {
            read_device(evs[i].data.ptr);
            continue;
        }
        /* Something new in /dev/input: a USB mouse or keyboard plugged in. */
        char buf[4096];
        ssize_t k = read(ino, buf, sizeof(buf));
        for (char *p = buf; k > 0 && p < buf + k;) {
            struct inotify_event *ie = (struct inotify_event *)p;
            if (ie->len && !strncmp(ie->name, "event", 5)) {
                char path[300];
                snprintf(path, sizeof(path), "/dev/input/%s", ie->name);
                usleep(50000);              /* let devtmpfs finish the node */
                add_device(path);
            }
            p += sizeof(*ie) + ie->len;
        }
    }
}

static void d_close(void)
{
    if (tty >= 0)
        ioctl(tty, KDSETMODE, KD_TEXT);
}

const struct backend backend_drm = { "drm", d_open, d_present, d_fd, d_due, d_pump, d_close };
