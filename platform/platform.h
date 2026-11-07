/* LiteKern X on Linux — the platform layer (LiteKernX-Roadmap/06, phase L1).
 *
 * The desktop is today's kernel/ code, unchanged where possible. What it got
 * from the hardware it now gets from this folder:
 *
 *   devices.c   "fb0", "kbd0", "mouse0", "rtc0" and "boot0" for the kernel's
 *               driver layer (kernel/driver.h), backed by a backend below and
 *               by a disk image file
 *   pmm.c       pages from mmap
 *   timing.c    the clock, from CLOCK_MONOTONIC
 *   power.c     Restart and Shut Down (the PC build just exits)
 *   lkx.c       apps, each its own Linux process (kip.h, libk86.c: phase L2)
 *   hwprobe.c   the Hardware Probe's facts, from /proc and /sys
 *   main.c      start-up and the event loop (kernel/main.c's idle loop)
 *
 * A backend shows the screen and brings input:
 *   headless    no window: scripted input (the tests' key language), PNG
 *               screenshots
 *   x11         a window on the PC (WSL's own X server), spoken to directly:
 *               no library needed
 *   drm         the real screen and input (Linux DRM/KMS and evdev): the
 *               EeePC, and the Linux image in QEMU (phase L3) */
#ifndef LKX_PLATFORM_H
#define LKX_PLATFORM_H

#include <stdint.h>
#include "kernel/screen.h"

struct backend {
    const char *name;
    /* Open the display: w x h as asked, or the size it has (drm: the panel's),
     * written back. 0, or -1 with the reason logged. */
    int (*open)(int *w, int *h);
    /* Show a rectangle of the framebuffer (0xAARRGGBB pixels, `stride` per row). */
    void (*present)(const uint32_t *px, int stride, int x, int y, int w, int h);
    /* Before the loop sleeps: send what's queued; the fd to wake for (input),
     * or -1. */
    int (*fd)(void);
    /* ms until it wants a turn without input (a script's next step); -1: never. */
    int (*due_ms)(void);
    /* Take what's come (input, or a due step): to plat_key() and plat_pointer(). */
    void (*pump)(void);
    void (*close)(void);
    /* The display's own cursor, if it has one (kernel/screen.h); NULL: none. */
    screen_cursor_fn cursor;
    /* The panel off (1) or on again (0), the picture kept; NULL: it can't
     * (the VM, the tests: the screen is never "off" there). */
    void (*blank)(int off);
};

extern const struct backend backend_headless, backend_x11, backend_drm;

/* --- what backends call ---------------------------------------------------------------------- */

/* The screen off or on (the Power key, kernel/machine.h). While it is off,
 * only the Power key and nothing from the touchscreen reaches the shell. */
void plat_screen_power(int on);
int plat_screen_on(void);
/* A key went down (1) or up (0); `key` is a KEY_* code (kernel/input.h). */
void plat_key(uint16_t key, int pressed);
/* Where the pointer is now, in screen pixels, and the MOUSE_* buttons held. */
void plat_pointer(int x, int y, uint8_t buttons);
/* The wheel turned: notches, > 0 down (towards you: the page moves up). */
void plat_wheel(int notches);
/* The touchpad (drm.c): whether there is one, and its speed, 0-100 (50:
 * the usual), set by platform/machine.c from Settings. */
extern int plat_touchpad_present, plat_touchpad_speed;
/* Linux evdev key code (X11's key code - 8) to KEY_*; 0 if it has none. */
uint16_t plat_evdev_key(unsigned code);
/* The whole framebuffer, for screenshots. */
const uint32_t *plat_framebuffer(int *w, int *h);
/* Write the screen as a PNG. 0, or -1. */
int plat_screenshot(const char *path);
/* 1 once the desktop is up (scripts start then). */
int plat_ready(void);
/* Sleep up to `ms` (0: just look), waking for input, the apps, or the
 * backend's next turn; then hand over the input. */
void plat_wait(int ms);
/* Leave: tidy up and exit with `code`. */
void plat_exit(int code) __attribute__((noreturn));

/* --- set up by main.c ------------------------------------------------------------------------ */

/* Register the devices; `disk`: the disk image (a LiteKern X .img), or NULL;
 * `layout`: the keyboard's, or NULL for the build's. */
void plat_devices_init(const struct backend *b, int w, int h, const char *disk, const char *layout);
/* The memory file behind an allocation that starts at `start` (pmm_alloc*),
 * or -1: a window's pixels, shared with its app. */
int plat_memory_fd(uintptr_t start);

/* --- apps (platform/lkx.c), for the loop's waiting ------------------------------------------- */
struct pollfd;
/* Their sockets, to wake for. Returns how many. */
int lkx_fds(struct pollfd *out, int max);
/* ms until an app's timer (or the watchdog) wants a turn; -1: none. */
int lkx_next_ms(void);

/* Networking (platform/net.c): started once the disk is read; looked after
 * from the loop (it rate-limits itself). */
void net_init(void);
void net_poll(void);

/* Linux's own messages into the Log (platform/kmsg.c): its warnings from
 * starting, then its news as it comes. */
void kmsg_init(void);
void kmsg_poll(void);

/* How often something asked to be woken (RTC_SET_RATE), in Hz; 0: once a second. */
uint32_t plat_wake_hz(void);

#endif
