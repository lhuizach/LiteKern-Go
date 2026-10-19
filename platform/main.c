/* LiteKern X on Linux — lkx-shell: start-up and the event loop. The kernel's
 * kmain() and idle loop (kernel/main.c), minus everything Linux now does:
 * the same desktop code, the same log lines, the same order.
 *
 *   lkx-shell [--backend x11|headless|drm] [--disk IMAGE] [--size WxH]
 *             [--keys "STEPS"] [--shot FILE] [--for SECONDS] [--keyboard azerty|qwerty]
 *
 *   --backend   x11 (a window; the default when $DISPLAY is set), headless, or
 *               drm (the real screen: the EeePC, linux/overlay/usr/bin/lkx-start)
 *   --disk      the disk image: its ramdisk (apps, wallpapers) and FAT32
 *               partition (settings, users, files). Default: build/litekernx.img
 *   --size      the screen: 1024x600 like the EeePC (default), 1024x768 like the VMs
 *   --keys      headless: a script in the tests' key language (platform/headless.c)
 *   --shot      headless: a PNG once the script has run, then exit
 *   --for       give up after this many seconds (tests)
 *   --keyboard  the layout, instead of the build's (the tests type US-style) */
#include "platform/platform.h"
#include "kernel/apps.h"
#include "kernel/console.h"
#include "kernel/cursor.h"
#include "kernel/desktop.h"
#include "kernel/driver.h"
#include "kernel/input.h"
#include "kernel/lkx.h"
#include "kernel/lockscreen.h"
#include "kernel/printk.h"
#include "kernel/screen.h"
#include "kernel/screensaver.h"
#include "kernel/serial.h"
#include "kernel/settings.h"
#include "kernel/splash.h"
#include "kernel/status.h"
#include "kernel/storage.h"
#include "kernel/theme.h"
#include "kernel/timing.h"
#include "kernel/users.h"
#include "kernel/wm.h"
#include "defaults.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const uint8_t vga_font_8x16[4096];      /* build-linux/gen/vgafont.c */
#ifdef LKX_SELFTEST_GFX
void selftest_gfx_run(void);
#endif
#ifdef LKX_SELFTEST_WM
void selftest_wm_run(void);
#endif
#ifdef LKX_SELFTEST_WIDGETS
void selftest_widgets_run(void);
#endif
void headless_script(const char *s);

static const struct backend *be;
static int ready;
static uint32_t give_up_ms;

int plat_ready(void)
{
    return ready;
}

/* Sleep until input, an app's call, or someone's next turn (apps' timers,
 * the backend's script, the clock or the screen saver: `ms`). */
void plat_wait(int ms)
{
    struct pollfd fds[1 + 32];
    int n = 0, bfd = be->fd();
    if (bfd >= 0)
        fds[n++] = (struct pollfd){ bfd, POLLIN, 0 };
    n += lkx_fds(fds + n, 32);
    int due = be->due_ms(), apps = lkx_next_ms();
    if (due >= 0 && due < ms)
        ms = due;
    if (apps >= 0 && apps < ms)
        ms = apps;
    poll(fds, (nfds_t)n, ms < 0 ? 0 : ms);
    be->pump();
}

void plat_exit(int code)
{
    if (be)
        be->close();
    fflush(stdout);
    exit(code);
}

/* --- the kernel's idle loop (kernel/main.c), waiting on the backend instead of hlt ------------- */

#define MOUSE_LOG_MS 100    /* movement is logged at most this often */

static void log_pointer(int x, int y, uint8_t buttons, int on_screen)
{
    void (*log)(const char *, ...) = on_screen ? kprintf : kdebugf;
    log("mouse: (%d, %d) buttons %c%c%c\n", x, y, buttons & MOUSE_LEFT ? 'L' : '-',
        buttons & MOUSE_MIDDLE ? 'M' : '-', buttons & MOUSE_RIGHT ? 'R' : '-');
}

static struct {
    device_t *kbd, *mouse;
    int w, h, x, y, moved;
    uint8_t buttons;
    uint32_t last_log;
} in;

static void locked_input(const struct key_event *keys, int nk, const struct mouse_event *moves, int nm)
{
    for (int i = 0; i < nk; i++) {
        log_key(&keys[i]);
        lockscreen_key(&keys[i]);
    }
    for (int i = 0; i < nm && lockscreen_active(); i++) {
        in.x += moves[i].dx;
        in.y += moves[i].dy;
        in.x = in.x < 0 ? 0 : in.x >= in.w ? in.w - 1 : in.x;
        in.y = in.y < 0 ? 0 : in.y >= in.h ? in.h - 1 : in.y;
        if (i + 1 < nm && moves[i + 1].buttons == moves[i].buttons && moves[i].buttons == in.buttons)
            continue;
        cursor_move_to(in.x, in.y);
        if (moves[i].buttons != in.buttons) {
            in.buttons = moves[i].buttons;
            log_pointer(in.x, in.y, in.buttons, 1);
        }
        lockscreen_pointer(in.x, in.y, moves[i].buttons);
    }
}

/* How long the loop may sleep: until the next second (the clock), sooner
 * when something asked to be woken more often (the screen saver). */
static int sleep_ms(int busy)
{
    if (busy || (desktop_busy() && !screensaver_active()))
        return 1;
    uint32_t hz = plat_wake_hz();
    if (hz)
        return (int)(1000 / hz);
    return 1000 - (int)(uptime_ms() % 1000);
}

void idle_step(int busy)
{
    struct key_event keys[8];
    struct mouse_event moves[16];
    desktop_tick();
    lockscreen_tick();
    screensaver_poll();
    if (give_up_ms && uptime_ms() > give_up_ms) {
        kprintf("lkx-shell: --for ran out\n");
        plat_exit(1);
    }
    int nk = dev_read(in.kbd, keys, sizeof(keys));
    int nm = dev_read(in.mouse, moves, sizeof(moves));
    if (nk <= 0 && nm <= 0 && !in.moved) {
        plat_wait(sleep_ms(busy));
        return;
    }
    if ((nk > 0 || nm > 0) && screensaver_input())
        return;
    if (lockscreen_active()) {
        locked_input(keys, nk > 0 ? nk / (int)sizeof(keys[0]) : 0, moves, nm > 0 ? nm / (int)sizeof(moves[0]) : 0);
        return;
    }
    for (int i = 0; i < nk / (int)sizeof(keys[0]); i++) {
        if (desktop_input_key(&keys[i]))
            continue;
        if (wm_focused())
            wm_input_key(&keys[i]);
        else
            log_key(&keys[i]);
        lkx_schedule();
        desktop_handle_events();
    }
    if (nm > 0) {
        int x = in.x, y = in.y;
        for (int i = 0; i < nm / (int)sizeof(moves[0]); i++) {
            x += moves[i].dx;
            y += moves[i].dy;
            x = x < 0 ? 0 : x >= in.w ? in.w - 1 : x;
            y = y < 0 ? 0 : y >= in.h ? in.h - 1 : y;
        }
        cursor_move_to(x, y);
    }
    int npkt = nm / (int)sizeof(moves[0]);
    for (int i = 0; i < npkt; i++) {
        in.x += moves[i].dx;
        in.y += moves[i].dy;
        in.x = in.x < 0 ? 0 : in.x >= in.w ? in.w - 1 : in.x;
        in.y = in.y < 0 ? 0 : in.y >= in.h ? in.h - 1 : in.y;
        if (i + 1 < npkt && moves[i + 1].buttons == moves[i].buttons && moves[i].buttons == in.buttons) {
            in.moved |= moves[i].dx || moves[i].dy;
            continue;
        }
        desktop_input_mouse(in.x, in.y, moves[i].buttons);
        in.moved |= moves[i].dx || moves[i].dy;
        if (moves[i].buttons != in.buttons) {
            in.buttons = moves[i].buttons;
            log_pointer(in.x, in.y, in.buttons, 1);
            in.moved = 0;
            in.last_log = uptime_ms();
            lkx_schedule();
            desktop_handle_events();
        }
    }
    desktop_handle_events();
    shell_flush();
    if (in.moved && uptime_ms() - in.last_log >= MOUSE_LOG_MS) {
        log_pointer(in.x, in.y, in.buttons, 0);
        in.moved = 0;
        in.last_log = uptime_ms();
    } else if (in.moved) {
        plat_wait(1);
    }
}

/* --- start-up ---------------------------------------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr, "usage: lkx-shell [--backend x11|headless|drm] [--disk IMAGE] [--size WxH]\n"
                    "                 [--keys \"STEPS\"] [--shot FILE] [--for SECONDS]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *backend = getenv("DISPLAY") ? "x11" : "headless";
    const char *disk = "build/litekernx.img", *keys = 0, *shot = 0, *layout = 0;
    int w = 1024, h = 600;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : 0;
        if (!v)
            usage();
        if (!strcmp(a, "--backend"))
            backend = v;
        else if (!strcmp(a, "--disk"))
            disk = v;
        else if (!strcmp(a, "--size")) {
            if (sscanf(v, "%dx%d", &w, &h) != 2 || w < 640 || h < 480 || w > 4096 || h > 4096)
                usage();
        } else if (!strcmp(a, "--keys"))
            keys = v;
        else if (!strcmp(a, "--shot"))
            shot = v;
        else if (!strcmp(a, "--keyboard"))
            layout = v;
        else if (!strcmp(a, "--for"))
            give_up_ms = (uint32_t)(atof(v) * 1000);
        else
            usage();
        i++;
    }
    be = !strcmp(backend, "x11") ? &backend_x11 : !strcmp(backend, "headless") ? &backend_headless
       : !strcmp(backend, "drm") ? &backend_drm : 0;
    if (!be)
        usage();
    if (!strcmp(be->name, "headless")) {
        char *script = malloc((keys ? strlen(keys) : 0) + (shot ? strlen(shot) : 0) + 64);
        sprintf(script, "%s%s%s%s", keys ? keys : "", shot ? " sleep:0.5 shot:" : "",
                shot ? shot : "", shot ? " quit" : "");
        headless_script(script);
        free(script);
    }

    serial_init();
    kprintf("\nLiteKern X (on Linux)\n");
    timing_init(0);
    if (be->open(&w, &h)) {
        fprintf(stderr, "lkx-shell: the %s backend couldn't start\n", be->name);
        return 1;
    }
    plat_devices_init(be, w, h, disk, layout);
    device_report();

    /* The same first frame as the kernel's (kernel/main.c). */
    if (screen_init(device_find("fb0")) < 0 || console_init((uint32_t)vga_font_8x16) < 0)
        kprintf("console: unavailable\n");
    status_show(STATUS_READY);
    theme_set(theme_find_style(DEFAULT_STYLE), theme_find_accent(DEFAULT_ACCENT));
    splash_start();
    if (screen_ready() && !splash_active() && cursor_init() < 0)
        kprintf("cursor: no \"arrow\" cursor built in\n");
    boot_report(0);
    splash_progress(30);
    /* The kernel's UI self-tests, where kernel/main.c runs them (platform/Makefile SELFTEST=). */
#ifdef LKX_SELFTEST_GFX
    selftest_gfx_run();
#endif
#ifdef LKX_SELFTEST_WM
    selftest_wm_run();
#endif
#ifdef LKX_SELFTEST_WIDGETS
    selftest_widgets_run();
#endif

    storage_init();
    users_load();
    settings_load();
    splash_progress(45);

    in.kbd = device_find("kbd0");
    in.mouse = device_find("mouse0");
    in.w = w;
    in.h = h;
    in.x = w / 2;
    in.y = h / 2;
    kprintf("kbd: ready; key presses are logged (PgUp/PgDn/Home/End scroll the log)\n");
    kprintf("mouse: ready; pointer starts at the centre, clicks logged\n");
    log_pointer(in.x, in.y, in.buttons, 1);
#ifndef LKX_SELFTEST_GFX
    desktop_start();
#endif
    ready = 1;
    for (;;)
        idle_step(lkx_schedule());
}
