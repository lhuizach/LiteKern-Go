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
 * (AZERTY) is LiteKern X's own. Pointers: relative ones (USB mice; the
 * touchpad when started as a plain PS/2 mouse, psmouse.proto=exps) move the
 * pointer by what they send, their wheel scrolls; absolute ones (tablets)
 * place it. A touchpad (Linux's own driver: the EeePC's Elantech) is ours to
 * make sense of: one finger moves the pointer (faster when it moves fast), a
 * tap clicks (two fingers: right click, three: middle), two fingers moving
 * scroll, as the page follows them. */
#include "platform/platform.h"
#include "kernel/input.h"
#include "kernel/printk.h"
#include "kernel/timing.h"

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
static uint32_t pitch, fb_id, crtc_id;
static int sw, sh;

/* --- the screen -------------------------------------------------------------------------------- */

/* The size Linux was asked for (video=WxH or video=NAME:WxH on its command
 * line: LiteKern Go's VM asks for the phone's 720x1280). 0 if none. Linux
 * adds that mode to the output's list but may still prefer another. */
static int wanted_mode(int *w, int *h)
{
    char line[1024];
    FILE *f = fopen("/proc/cmdline", "r");
    if (!f)
        return 0;
    int ok = 0;
    if (fgets(line, sizeof(line), f)) {
        for (char *t = strtok(line, " \n"); t && !ok; t = strtok(0, " \n")) {
            if (strncmp(t, "video=", 6))
                continue;
            char *s = strchr(t + 6, ':');
            ok = sscanf(s ? s + 1 : t + 6, "%dx%d", w, h) == 2 && *w > 0 && *h > 0;
        }
    }
    fclose(f);
    return ok;
}

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

    /* A connected output: the built-in panel (LVDS, eDP) first. Its mode:
     * the one video= asked for when it has it, else its preferred one. */
    struct drm_mode_get_connector best = { 0 };
    struct drm_mode_modeinfo mode = { 0 };
    int found = 0, want_w = 0, want_h = 0;
    int want = wanted_mode(&want_w, &want_h);
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
        for (uint32_t m = 0; want && m < c.count_modes; m++)
            if (modes[m].hdisplay == want_w && modes[m].vdisplay == want_h) {
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
    crtc_id = crtc;
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

/* --- the hardware cursor (kernel/screen.h): the GPU draws the pointer -------------------------
 * A 64x64 buffer given to the CRTC (CURSOR2: with its click point, which
 * virtual GPUs need to show it as the PC's own pointer). Moving it is one
 * call and redraws nothing. A driver without one (QEMU's bochs) says no on
 * the first try, and the pointer is drawn in software from then on. */
#define CURSOR_SIZE 64
static int cursor_state;            /* 0 untried, 1 works, -1 none */
static uint32_t cursor_handle, *cursor_px;
static const uint32_t *cursor_img;  /* what's in it */

static int d_cursor(const uint32_t *argb, int w, int h, int hot_x, int hot_y, int x, int y)
{
    if (cursor_state < 0)
        return -1;
    if (card < 0 || !crtc_id) {
        kprintf("drm: no display for a hardware cursor yet\n");
        return -1;
    }
    if (!argb) {
        struct drm_mode_cursor c = { .flags = DRM_MODE_CURSOR_BO, .crtc_id = crtc_id, .handle = 0 };
        ioctl(card, DRM_IOCTL_MODE_CURSOR, &c);
        cursor_img = 0;
        return cursor_state > 0 ? 0 : -1;
    }
    if (w > CURSOR_SIZE || h > CURSOR_SIZE) {
        kprintf("drm: a %dx%d pointer is too big for the hardware cursor: drawn\n", w, h);
        return -1;
    }
    if (!cursor_handle) {
        struct drm_mode_create_dumb dumb = { .width = CURSOR_SIZE, .height = CURSOR_SIZE, .bpp = 32 };
        struct drm_mode_map_dumb map;
        if (ioctl(card, DRM_IOCTL_MODE_CREATE_DUMB, &dumb))
            goto none;
        map = (struct drm_mode_map_dumb){ .handle = dumb.handle };
        if (ioctl(card, DRM_IOCTL_MODE_MAP_DUMB, &map))
            goto none;
        cursor_px = mmap(0, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, card, (off_t)map.offset);
        if (cursor_px == MAP_FAILED)
            goto none;
        cursor_handle = dumb.handle;
    }
    if (argb != cursor_img) {               /* a new shape: into the buffer, then shown */
        memset(cursor_px, 0, CURSOR_SIZE * CURSOR_SIZE * 4);
        for (int r = 0; r < h; r++)
            memcpy(cursor_px + r * CURSOR_SIZE, argb + r * w, (size_t)w * 4);
        struct drm_mode_cursor2 c = { .flags = DRM_MODE_CURSOR_BO, .crtc_id = crtc_id,
                                      .width = CURSOR_SIZE, .height = CURSOR_SIZE,
                                      .handle = cursor_handle, .hot_x = hot_x, .hot_y = hot_y };
        if (ioctl(card, DRM_IOCTL_MODE_CURSOR2, &c)) {
            struct drm_mode_cursor old = { .flags = DRM_MODE_CURSOR_BO, .crtc_id = crtc_id,
                                           .width = CURSOR_SIZE, .height = CURSOR_SIZE, .handle = cursor_handle };
            if (ioctl(card, DRM_IOCTL_MODE_CURSOR, &old))
                goto none;
        }
        cursor_img = argb;
    }
    struct drm_mode_cursor m = { .flags = DRM_MODE_CURSOR_MOVE, .crtc_id = crtc_id,
                                 .x = x - hot_x, .y = y - hot_y };
    if (ioctl(card, DRM_IOCTL_MODE_CURSOR, &m))
        goto none;
    if (!cursor_state)
        kprintf("drm: the pointer is the display's own (a hardware cursor)\n");
    cursor_state = 1;
    return 0;
none:
    if (!cursor_state)
        kprintf("drm: no hardware cursor: the pointer is drawn\n");
    cursor_state = -1;
    return -1;
}

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
    int pad;                        /* a touchpad: moves, taps and scrolls (below) */
    struct input_absinfo ax, ay;
    /* The touchpad: what this report says, and the touch so far. */
    int x, y, fingers, touching;
    int was_touching, last_x, last_y, last_fingers, most_fingers, travel;
    uint32_t touch_ms;
    int64_t fx, fy, fs;             /* moves still to come: 1/256 px, 1/256 notch */
    char name[64];
    int num;                        /* its eventN */
} devs[DEVS];
static int px, py, dx, dy, moved;
static uint8_t seen[256];           /* /dev/input/eventN looked at (scan_devices) */
static uint32_t next_scan;
static uint8_t buttons;
int plat_touchpad_present, plat_touchpad_speed = 50;

#define BIT(arr, n) ((arr)[(n) / (8 * sizeof(long))] >> ((n) % (8 * sizeof(long))) & 1)

static void add_device(const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return;
    unsigned long ev[8] = { 0 }, keys[KEY_MAX / (8 * sizeof(long)) + 1] = { 0 }, abs[8] = { 0 }, rel[8] = { 0 };
    ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel);
    char name[64] = "";
    ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
    int keyboard = BIT(ev, EV_KEY) && BIT(keys, KEY_A) && BIT(keys, KEY_ENTER);
    int mouse = BIT(ev, EV_REL) && BIT(rel, REL_X) && BIT(keys, BTN_LEFT);
    int tablet = BIT(ev, EV_ABS) && BIT(abs, ABS_X) && BIT(abs, ABS_Y) &&
                 (BIT(keys, BTN_LEFT) || BIT(keys, BTN_TOUCH));
    int touchpad = tablet && BIT(keys, BTN_TOOL_FINGER);
    if (!keyboard && !mouse && !tablet) {
        kprintf("input: %s left alone (not a keyboard or pointer)\n", *name ? name : path);
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
    d->pad = touchpad;
    plat_touchpad_present |= touchpad;
    d->abs = tablet && !mouse && !touchpad;
    if (d->abs || d->pad) {
        ioctl(fd, EVIOCGABS(ABS_X), &d->ax);
        ioctl(fd, EVIOCGABS(ABS_Y), &d->ay);
    }
    memcpy(d->name, name, sizeof(d->name));
    d->num = atoi(strrchr(path, 't') + 1);
    ioctl(fd, EVIOCGRAB, 1);                /* ours: the text console behind doesn't get the keys */
    struct epoll_event e = { .events = EPOLLIN, .data.ptr = d };
    epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e);
    kprintf("input: %s (%s%s%s)\n", d->name, keyboard ? "keyboard" : "", keyboard && (mouse || tablet) ? ", " : "",
            mouse ? "pointer" : touchpad ? "touchpad pointer" : tablet ? "tablet" : "");
    if (touchpad)
        kprintf("input: touchpad %d-%d x %d-%d\n", d->ax.minimum, d->ax.maximum, d->ay.minimum, d->ay.maximum);
}

/* Every eventN not looked at yet (seen[]: once each, kept or not). Called at
 * start, for inotify's news, and every 2 s in case a node's creation was
 * missed (a USB device can take seconds to appear). A device unplugged and
 * plugged back gets a new N. */

static void scan_devices(void)
{
    DIR *dir = opendir("/dev/input");
    struct dirent *e;
    while (dir && (e = readdir(dir))) {
        if (strncmp(e->d_name, "event", 5))
            continue;
        int n = atoi(e->d_name + 5);
        if (n < 0 || n > 255 || seen[n])
            continue;
        seen[n] = 1;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        add_device(path);
    }
    if (dir)
        closedir(dir);
    next_scan = uptime_ms() + 2000;
}

static int scale(int v, const struct input_absinfo *a, int size)
{
    int range = a->maximum - a->minimum;
    return range > 0 ? (int)((int64_t)(v - a->minimum) * (size - 1) / range) : 0;
}

/* --- the touchpad: sizes are fractions of the pad's width, so any pad feels the same */
#define TAP_MS        180       /* a touch this short that hardly moved is a tap */
#define TAP_TRAVEL    40        /* ...moving at most 1/40 of the width */
#define SCROLL_NOTCH  25        /* two fingers moving 1/25 of the width: a notch */

static void pad_click(uint8_t b)
{
    plat_pointer(px, py, buttons | b);
    plat_pointer(px, py, buttons);
}

static void pad_report(struct dev *d)
{
    int range = d->ax.maximum - d->ax.minimum;
    if (range <= 0)
        range = 1000;
    if (d->touching && !d->was_touching) {          /* a finger comes down */
        d->touch_ms = uptime_ms();
        d->travel = 0;
        d->most_fingers = d->fingers;
        d->fx = d->fy = d->fs = 0;
    } else if (d->touching) {
        int mx = d->x - d->last_x, my = d->y - d->last_y;
        if (d->fingers != d->last_fingers)
            mx = my = 0;                            /* fingers came or went: the spot jumps */
        d->travel += abs(mx) + abs(my);
        if (d->fingers > d->most_fingers)
            d->most_fingers = d->fingers;
        if (d->fingers >= 2) {
            d->fs -= (int64_t)my * 256 * SCROLL_NOTCH / range;  /* the page follows: up scrolls down */
            int notches = (int)(d->fs / 256);
            if (notches) {
                d->fs -= (int64_t)notches * 256;
                plat_wheel(notches);
            }
        } else if (mx || my) {
            /* Across the whole pad, slowly: half the screen at the usual
             * speed (50), from a quarter of that (0) to four times (100);
             * moving fast, up to 1.8 times further. */
            int speed = abs(mx) + abs(my), slow = range / 250, fast = range / 50;
            int gain = 256 + (speed > slow ? (speed - slow) * 205 / (fast > slow ? fast - slow : 1) : 0);
            if (gain > 461)
                gain = 461;
            int mult = 256, q = plat_touchpad_speed - 50;  /* doubles every 25: 64 (0) to 1024 (100) */
            for (; q >= 25; q -= 25)
                mult *= 2;
            for (; q < 0; q += 25)
                mult /= 2;
            mult = mult * (25 + q) / 25;
            int64_t per = (int64_t)sw * 128 * mult / 256 / range;   /* 1/256 px a unit */
            d->fx += mx * per * gain / 256;
            d->fy += my * per * gain / 256;
            int ix = (int)(d->fx / 256), iy = (int)(d->fy / 256);
            d->fx -= (int64_t)ix * 256;
            d->fy -= (int64_t)iy * 256;
            if (ix || iy) {
                px += ix, py += iy;
                px = px < 0 ? 0 : px >= sw ? sw - 1 : px;
                py = py < 0 ? 0 : py >= sh ? sh - 1 : py;
                plat_pointer(px, py, buttons);
            }
        }
    } else if (d->was_touching) {                   /* lifted: was it a tap? */
        if (uptime_ms() - d->touch_ms <= TAP_MS && d->travel * TAP_TRAVEL <= range)
            pad_click(d->most_fingers >= 3 ? MOUSE_MIDDLE : d->most_fingers == 2 ? MOUSE_RIGHT : MOUSE_LEFT);
    }
    d->was_touching = d->touching;
    d->last_x = d->x, d->last_y = d->y, d->last_fingers = d->fingers;
}

/* BTN_TOOL_*: how many fingers are down. */
static int tool_fingers(int code)
{
    switch (code) {
    case BTN_TOOL_FINGER:    return 1;
    case BTN_TOOL_DOUBLETAP: return 2;
    case BTN_TOOL_TRIPLETAP: return 3;
    case BTN_TOOL_QUADTAP:   return 4;
    case BTN_TOOL_QUINTTAP:  return 5;
    default:                 return 0;
    }
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
            if (d->pad && e->type == EV_KEY && tool_fingers(e->code)) {
                if (e->value)
                    d->fingers = tool_fingers(e->code);
                else if (d->fingers == tool_fingers(e->code))
                    d->fingers = 0;
            } else if (d->pad && e->type == EV_KEY && e->code == BTN_TOUCH) {
                d->touching = e->value != 0;
            } else if (d->pad && e->type == EV_ABS && (e->code == ABS_X || e->code == ABS_Y)) {
                *(e->code == ABS_X ? &d->x : &d->y) = e->value;
            } else if (d->pad && e->type == EV_SYN && e->code == SYN_REPORT) {
                if (!d->fingers && d->touching)
                    d->fingers = 1;
                pad_report(d);
                if (moved) {                    /* its buttons changed */
                    moved = 0;
                    plat_pointer(px, py, buttons);
                }
            } else if (e->type == EV_REL && e->code == REL_WHEEL) {
                plat_wheel(-e->value);          /* evdev: away from you is +1 */
            } else if (e->type == EV_KEY && e->code < BTN_MISC) {
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
    seen[d->num & 255] = 0;             /* plugged back in, it may get the same N */
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
    int32_t left = (int32_t)(next_scan - uptime_ms());
    return left < 0 ? 0 : left;
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
        while (read(ino, buf, sizeof(buf)) > 0)
            ;
        usleep(50000);                      /* let devtmpfs finish the node */
        scan_devices();
    }
    if (d_due() == 0)
        scan_devices();
}

static void d_close(void)
{
    if (tty >= 0)
        ioctl(tty, KDSETMODE, KD_TEXT);
}

const struct backend backend_drm = { "drm", d_open, d_present, d_fd, d_due, d_pump, d_close, d_cursor };
