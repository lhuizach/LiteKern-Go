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
 *   lkx.c       apps: listed, not run yet (phase L2)
 *   main.c      start-up and the event loop (kernel/main.c's idle loop)
 *
 * A backend shows the screen and brings input:
 *   headless    no window: scripted input (the tests' key language), PNG
 *               screenshots
 *   x11         a window on the PC (WSL's own X server), spoken to directly:
 *               no library needed
 * The EeePC's (Linux DRM/KMS and evdev) comes in phase L3. */
#ifndef LKX_PLATFORM_H
#define LKX_PLATFORM_H

#include <stdint.h>

struct backend {
    const char *name;
    /* Open a w x h display. 0, or -1 with the reason logged. */
    int (*open)(int w, int h);
    /* Show a rectangle of the framebuffer (0xAARRGGBB pixels, `stride` per row). */
    void (*present)(const uint32_t *px, int stride, int x, int y, int w, int h);
    /* Wait up to `ms` (0: just check) for input, handing it to plat_key() and
     * plat_pointer(). 1 if anything came. */
    int (*wait)(int ms);
    void (*close)(void);
};

extern const struct backend backend_headless, backend_x11;

/* --- what backends call ---------------------------------------------------------------------- */

/* A key went down (1) or up (0); `key` is a KEY_* code (kernel/input.h). */
void plat_key(uint16_t key, int pressed);
/* Where the pointer is now, in screen pixels, and the MOUSE_* buttons held. */
void plat_pointer(int x, int y, uint8_t buttons);
/* Linux evdev key code (X11's key code - 8) to KEY_*; 0 if it has none. */
uint16_t plat_evdev_key(unsigned code);
/* The whole framebuffer, for screenshots. */
const uint32_t *plat_framebuffer(int *w, int *h);
/* Write the screen as a PNG. 0, or -1. */
int plat_screenshot(const char *path);
/* 1 once the desktop is up (scripts start then). */
int plat_ready(void);
/* Leave: tidy up and exit with `code`. */
void plat_exit(int code) __attribute__((noreturn));

/* --- set up by main.c ------------------------------------------------------------------------ */

/* Register the devices; `disk`: the disk image (a LiteKern X .img), or NULL;
 * `layout`: the keyboard's, or NULL for the build's. */
void plat_devices_init(const struct backend *b, int w, int h, const char *disk, const char *layout);
/* How often something asked to be woken (RTC_SET_RATE), in Hz; 0: once a second. */
uint32_t plat_wake_hz(void);

#endif
