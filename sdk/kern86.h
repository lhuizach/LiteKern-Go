/* LiteKern X — kern86.h, the app side of KERN86 (Phase 2 §5).
 *
 * Include this in an app (apps/<name>/). Every call is a thin wrapper over
 * the kernel's `int 0x80` gate (numbers and structures: kernel/kern86_abi.h),
 * where v1's apps called kernel functions directly. Apps also get the
 * kernel's drawing and widget code (kernel/gfx.h, kernel/widget.h), built
 * into them: they draw into their canvas and show it with k86_present().
 *
 * An app's life: k86_window_open(), draw, k86_present(), then loop on
 * k86_wait_event() until K86_EVENT_CLOSE (returning from main, or
 * k86_exit(), ends it; so does the next k86_wait_event() after a close). */
#ifndef LKX_SDK_KERN86_H
#define LKX_SDK_KERN86_H

#include <stdint.h>
#include "kernel/kern86_abi.h"

#ifdef LKX_LINUX
/* On Linux (the port's phase L2) an app is a Linux process: its calls go to
 * lkx-shell over a socket (platform/libk86.c), not through int 0x80, which
 * on Linux is Linux's own system call gate. */
int k86_linux_call(uint32_t nr, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);

static inline int k86_call(uint32_t nr, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    return k86_linux_call(nr, a, b, c, d, 0);
}

static inline int k86_call5(uint32_t nr, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e)
{
    return k86_linux_call(nr, a, b, c, d, e);
}
#else
static inline int k86_call(uint32_t nr, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "b"(a), "c"(b), "d"(c), "S"(d)
                     : "memory");
    return ret;
}

static inline int k86_call5(uint32_t nr, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e)
{
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                     : "memory");
    return ret;
}
#endif

static inline __attribute__((noreturn)) void k86_exit(int code)
{
    k86_call(SYS_EXIT, (uintptr_t)code, 0, 0, 0);
    __builtin_unreachable();
}

/* Log a line ("user: <text>", in the Log app and on serial). */
int k86_log(const char *text);
/* printf-lite into the log: %s %d %u %x only. */
void k86_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static inline uint32_t k86_uptime_ms(void)
{
    return (uint32_t)k86_call(SYS_UPTIME_MS, 0, 0, 0, 0);
}

/* The window: its size and canvas. Also loads the font for kernel/gfx.h. */
int k86_window_open(struct k86_window *w);

static inline int k86_present(int x, int y, int w, int h)
{
    return k86_call(SYS_WINDOW_PRESENT, (uintptr_t)x, (uintptr_t)y, (uintptr_t)w, (uintptr_t)h);
}

static inline int k86_header(const struct k86_header *h)
{
    return k86_call(SYS_WINDOW_HEADER, (uintptr_t)h, 0, 0, 0);
}

/* Sleeps until an event comes. On K86_EVENT_THEME the app's copy of the
 * theme (kernel/theme.h's theme_get()) is already updated: just redraw. */
int k86_wait_event(struct k86_event *ev);

/* Appearance (the Settings app). */
static inline int k86_appearance(struct k86_appearance *out)
{
    return k86_call(SYS_APPEARANCE, (uintptr_t)out, 0, 0, 0);
}

static inline int k86_appearance_set(int style, int accent, const char *wallpaper)
{
    return k86_call(SYS_APPEARANCE_SET, (uintptr_t)style, (uintptr_t)accent, (uintptr_t)wallpaper, 0);
}

/* Seconds without input before the screen saver starts; 0 turns it off. */
static inline int k86_screensaver_set(int seconds)
{
    return k86_call(SYS_SCREENSAVER_SET, (uintptr_t)seconds, 0, 0, 0);
}

/* Show the screen saver now (any input ends it). */
static inline int k86_screensaver_preview(void)
{
    return k86_call(SYS_SCREENSAVER_SET, 0, 1, 0, 0);
}

static inline int k86_wallpaper_thumb(const char *name, uint32_t *out, int w, int h)
{
    return k86_call(SYS_WALLPAPER_THUMB, (uintptr_t)name, (uintptr_t)out, (uintptr_t)w, (uintptr_t)h);
}

/* A K86_EVENT_TIMER every ms milliseconds (0: off). It's only checked when the
 * kernel gets a turn: at least once a second, at once while any app is busy. */
static inline int k86_timer_set(uint32_t ms)
{
    return k86_call(SYS_TIMER_SET, ms, 0, 0, 0);
}

/* The embedded controller's 256 bytes into buf. 0, or a negative errno. */
static inline int k86_ec_read(uint8_t *buf)
{
    return k86_call(SYS_EC_READ, (uintptr_t)buf, 0, 0, 0);
}

/* The hardware probe: up to max report lines into out; returns how many
 * there are. flags: K86_PROBE_EC_RAM to read the EC's RAM too. Reads only. */
static inline int k86_hw_probe(struct k86_probe_item *out, int max, uint32_t flags)
{
    return k86_call(SYS_HW_PROBE, (uintptr_t)out, (uintptr_t)max, flags, 0);
}

/* Files. Paths start with '/', inside a volume (index from k86_volumes). */
static inline int k86_volumes(struct k86_volume *out, int max)
{
    return k86_call(SYS_FS_VOLUMES, (uintptr_t)out, (uintptr_t)max, 0, 0);
}

static inline int k86_list(int vol, const char *path, struct k86_dirent *out, int max)
{
    return k86_call(SYS_FS_LIST, (uintptr_t)vol, (uintptr_t)path, (uintptr_t)out, (uintptr_t)max);
}

static inline int k86_create(int vol, const char *dir, const char *name, int is_dir)
{
    return k86_call(SYS_FS_CREATE, (uintptr_t)vol, (uintptr_t)dir, (uintptr_t)name, (uintptr_t)is_dir);
}

static inline int k86_rename(int vol, const char *dir, const char *from, const char *to)
{
    return k86_call(SYS_FS_RENAME, (uintptr_t)vol, (uintptr_t)dir, (uintptr_t)from, (uintptr_t)to);
}

/* Read a file from its start into buf (at most len bytes); the bytes read. */
static inline int k86_read(int vol, const char *dir, const char *name, void *buf, uint32_t len)
{
    return k86_call5(SYS_FS_READ, (uintptr_t)vol, (uintptr_t)dir, (uintptr_t)name, (uintptr_t)buf, len);
}

/* Memory beyond the 1 MiB image, for big jobs: make the block at K86_HEAP
 * `bytes` long (up to K86_HEAP_MAX). Growing adds zeroed pages, shrinking
 * frees them; the app's own pointer is (void *)K86_HEAP. 0, or -ENOMEM. */
static inline int k86_memory(uint32_t bytes)
{
    return k86_call(SYS_MEMORY, bytes, 0, 0, 0);
}

/* Show a picture (0x00RRGGBB, w x h, rows `stride` pixels apart) as the
 * wallpaper, scaled to fill the screen: where is K86_WALLPAPER_DESKTOP,
 * K86_WALLPAPER_LOCK, or both. */
static inline int k86_wallpaper_image(const uint32_t *px, int w, int h, int stride, int where)
{
    return k86_call5(SYS_WALLPAPER_IMAGE, (uintptr_t)px, (uintptr_t)w, (uintptr_t)h, (uintptr_t)stride,
                     (uint32_t)where);
}

/* The logged-in user's home folder into path (K86_PATH_MAX): "/Users/Ann",
 * or "/" with no accounts. Returns its volume (for k86_list etc.), or a
 * negative errno. */
static inline int k86_home(char *path)
{
    return k86_call(SYS_HOME, (uintptr_t)path, 0, 0, 0);
}

/* The users (kernel/users.h): list them; change them (checked and saved by the kernel). */
static inline int k86_users(struct k86_users *out)
{
    return k86_call(SYS_USERS, (uintptr_t)out, 0, 0, 0);
}

static inline int k86_user_change(const struct k86_user_change *c)
{
    return k86_call(SYS_USER_CHANGE, (uintptr_t)c, 0, 0, 0);
}

/* The lock screen: lock now; read or change its choices (saved). */
static inline int k86_lock(void)
{
    return k86_call(SYS_LOCK, 0, 0, 0, 0);
}

static inline int k86_lock_settings(struct k86_lock *l, int set)
{
    return k86_call(SYS_LOCK_SETTINGS, (uintptr_t)l, (uintptr_t)set, 0, 0);
}

static inline int k86_lock_preview(const struct k86_lock *l, uint32_t *out, int w, int h)
{
    return k86_call(SYS_LOCK_PREVIEW, (uintptr_t)l, (uintptr_t)out, (uintptr_t)w, (uintptr_t)h);
}

/* Replace a file's contents with len bytes (creating it if needed). */
static inline int k86_write(int vol, const char *dir, const char *name, const void *buf, uint32_t len)
{
    return k86_call5(SYS_FS_WRITE, (uintptr_t)vol, (uintptr_t)dir, (uintptr_t)name, (uintptr_t)buf, len);
}

static inline int k86_delete(int vol, const char *dir, const char *name)
{
    return k86_call(SYS_FS_DELETE, (uintptr_t)vol, (uintptr_t)dir, (uintptr_t)name, 0);
}

/* Networking (the Linux port; the kernel version has none: K86_NET_NONE, -ENODEV). */
static inline int k86_net_status(struct k86_net *out)
{
    return k86_call(SYS_NET_STATUS, (uintptr_t)out, 0, 0, 0);
}

/* What the last Wi-Fi scan found (and a new one asked for): the count of all. */
static inline int k86_net_scan(struct k86_wifi *out, int max)
{
    return k86_call(SYS_NET_SCAN, (uintptr_t)out, (uintptr_t)max, 0, 0);
}

static inline int k86_net_join(const char *ssid, const char *password)
{
    return k86_call(SYS_NET_JOIN, (uintptr_t)ssid, (uintptr_t)password, 0, 0);
}

static inline int k86_net_forget(const char *ssid)
{
    return k86_call(SYS_NET_FORGET, (uintptr_t)ssid, 0, 0, 0);
}

/* The machine (the Linux port; the kernel version: no battery, -1s, -ENODEV). */
static inline int k86_machine(struct k86_machine *out)
{
    return k86_call(SYS_MACHINE, (uintptr_t)out, 0, 0, 0);
}

static inline int k86_brightness_set(int percent)
{
    return k86_call(SYS_BRIGHTNESS_SET, (uintptr_t)percent, 0, 0, 0);
}

/* LiteKern Go's Back key: wanted = 1 while the app has something for Back to
 * close (it then gets Back as an Esc press), 0 when Back should close the app.
 * Only calls the shell when it changes, so it can be called before each wait. */
static inline int k86_back_wanted(int wanted)
{
    static int was = 0;                     /* apps start with Back closing them */
    if (!!wanted == was)
        return 0;
    was = !!wanted;
    return k86_call(SYS_WINDOW_BACK, (uintptr_t)was, 0, 0, 0);
}

/* LiteKern Go: bare = 1 takes the window's header bar away (a full-screen game): the canvas
 * grows, so wait for K86_EVENT_RESIZE (or re-read the size) and lay out again. 0 brings it
 * back. Call it right after k86_window_open. */
static inline int k86_window_bare(int bare)
{
    return k86_call(SYS_WINDOW_BARE, (uintptr_t)bare, 0, 0, 0);
}

/* LiteKern Go's on-screen keyboard: wanted = 1 (K86_KEYBOARD_TEXT) while a text
 * field is open, K86_KEYBOARD_NUMBERS for a PIN. Only calls the shell when it
 * changes, so it can be called before each wait. */
static inline int k86_keyboard_wanted(int wanted)
{
    static int was = 0;
    wanted = wanted == K86_KEYBOARD_NUMBERS ? K86_KEYBOARD_NUMBERS : !!wanted;
    if (wanted == was)
        return 0;
    was = wanted;
    return k86_call(SYS_WINDOW_KEYBOARD, (uintptr_t)was, 0, 0, 0);
}

/* LiteKern Go's Task Manager: the running apps (SYS_TASKS), ending one. */
static inline int k86_tasks(struct k86_task *out, int max, struct k86_meminfo *mem)
{
    return k86_call(SYS_TASKS, (uintptr_t)out, (uintptr_t)max, (uintptr_t)mem, 0);
}

static inline int k86_task_stop(int32_t id)
{
    return k86_call(SYS_TASK_STOP, (uintptr_t)id, 0, 0, 0);
}

static inline int k86_task_icon(int32_t id, uint32_t *out)
{
    return k86_call(SYS_TASK_ICON, (uintptr_t)id, (uintptr_t)out, 0, 0);
}

/* LiteKern Go: the local time (SYS_TIME). */
static inline int k86_time(struct k86_time *out)
{
    return k86_call(SYS_TIME, (uintptr_t)out, 0, 0, 0);
}

/* LiteKern Go: a notification (SYS_NOTIFY). */
static inline int k86_notify(const char *title, const char *body)
{
    return k86_call(SYS_NOTIFY, (uintptr_t)title, (uintptr_t)body, 0, 0);
}

/* LiteKern Go: open another app, by its name. */
static inline int k86_app_open(const char *name)
{
    return k86_call(SYS_APP_OPEN, (uintptr_t)name, 0, 0, 0);
}

/* speed: 0-100, 50 the usual. */
static inline int k86_touchpad_set(int speed)
{
    return k86_call(SYS_TOUCHPAD_SET, (uintptr_t)speed, 0, 0, 0);
}

/* zone: an index in kernel/timezones.h. */
static inline int k86_timezone_set(int zone)
{
    return k86_call(SYS_TIMEZONE_SET, (uintptr_t)zone, 0, 0, 0);
}

#endif
