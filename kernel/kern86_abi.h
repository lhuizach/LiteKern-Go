/* LiteKern X — the KERN86 app ABI (Phase 2 §5): what ring 3 apps and the
 * kernel agree on. Included by both the kernel and the app SDK (sdk/), so
 * it holds only numbers and plain structs.
 *
 * Calls: `int 0x80`, EAX = number, arguments in EBX, ECX, EDX, ESI, EDI,
 * result in EAX (a negative errno on failure). The kernel checks every
 * pointer an app passes. v1's apps called kernel functions directly through
 * a jump table, in ring 0; these run in ring 3, and a crash only ends the app.
 *
 * The table grows only as apps need it. */
#ifndef LKX_KERN86_ABI_H
#define LKX_KERN86_ABI_H

#include <stdint.h>
#include "kernel/input.h"

/* --- calls ------------------------------------------------------------------- */
#define SYS_EXIT            0   /* ebx: code. Doesn't return */
#define SYS_DEBUG_WRITE     1   /* ebx: text, ecx: length. Logs "user: <text>" */
#define SYS_UPTIME_MS       2
#define SYS_WINDOW_OPEN     3   /* ebx: struct k86_window * out. Maps the canvas (the window's
                                 * own pixels: draw, then SYS_WINDOW_PRESENT) */
#define SYS_WINDOW_PRESENT  4   /* ebx, ecx, edx, esi: x, y, w, h of the canvas to show */
#define SYS_WINDOW_HEADER   5   /* ebx: const struct k86_header *: title and buttons */
#define SYS_WAIT_EVENT      6   /* ebx: struct k86_event * out. Sleeps until one comes */
#define SYS_FONT            7   /* ebx: uint8_t[K86_FONT_BYTES] out: the 8x16 font */
#define SYS_FS_VOLUMES      8   /* ebx: struct k86_volume * out, ecx: max. Returns the count */
#define SYS_FS_LIST         9   /* ebx: volume, ecx: path, edx: struct k86_dirent * out,
                                 * esi: max. Returns the count (all of them: may be > max) */
#define SYS_FS_CREATE       10  /* ebx: volume, ecx: folder path, edx: name, esi: 1 = folder */
#define SYS_FS_RENAME       11  /* ebx: volume, ecx: folder path, edx: old name, esi: new name */
#define SYS_FS_DELETE       12  /* ebx: volume, ecx: folder path, edx: name (folders: all of it) */
#define SYS_THEME_GET       13  /* ebx: struct theme * out (kernel/theme.h): the current colours */
#define SYS_APPEARANCE      14  /* ebx: struct k86_appearance * out: settings and choices */
#define SYS_APPEARANCE_SET  15  /* ebx: style (0 dark, 1 light, -1 keep), ecx: accent (-1 keep),
                                 * edx: wallpaper name (NULL keep, "" none). Every app window
                                 * gets K86_EVENT_THEME */
#define SYS_WALLPAPER_THUMB 16  /* ebx: name, ecx: uint32_t * out, edx: w, esi: h (<= 256 x 160) */
#define SYS_FS_READ         17  /* ebx: volume, ecx: folder path, edx: name, esi: buffer,
                                 * edi: length (<= 32 MiB). Reads from the start; returns the
                                 * bytes read */
#define SYS_SCREENSAVER_SET 18  /* ebx: seconds without input before the screen saver
                                 * starts (0 = never, at most 3600); or ecx = 1: show it
                                 * now (a preview; any input ends it) */
#define SYS_TIMER_SET       20  /* ebx: milliseconds between K86_EVENT_TIMER events, 0 = off.
                                 * The app is woken when one is due; it never wakes sooner than
                                 * the kernel gets a turn (at least once a second) */
#define SYS_EC_READ         21  /* ebx: uint8_t[256] out: the embedded controller's RAM, read
                                 * only. -ENODEV if there is none */
#define SYS_FS_WRITE        22  /* ebx: volume, ecx: folder path, edx: name, esi: buffer,
                                 * edi: length (<= 32 MiB). Replaces the file's contents,
                                 * creating it if it isn't there; the old contents stay if
                                 * it fails */
#define SYS_MEMORY          23  /* ebx: how many bytes the app's memory at K86_HEAP should be
                                 * (rounded up to pages; at most K86_HEAP_MAX). Grows with
                                 * zeroed pages or shrinks, freeing them. 0, -ENOMEM or -EINVAL */
#define SYS_WALLPAPER_IMAGE 24  /* ebx: const uint32_t * 0x00RRGGBB pixels, ecx: width, edx:
                                 * height (each 1..4096), esi: stride in pixels, edi: where
                                 * (K86_WALLPAPER_*; 0 = the desktop). Shown scaled to fill
                                 * the screen (the middle kept), as the wallpaper "Custom";
                                 * kept for the next start */
#define SYS_LOCK            25  /* lock the screen now (kernel/lockscreen.h) */
#define SYS_LOCK_SETTINGS   26  /* ebx: struct k86_lock *, ecx: 0 = read it, 1 = change to it
                                 * (saved). An unknown wallpaper means the desktop's */
#define SYS_USERS           27  /* ebx: struct k86_users * out: everyone, and who's logged in */
#define SYS_USER_CHANGE     28  /* ebx: const struct k86_user_change *. Checked by the kernel:
                                 * adding, removing and making admins needs an admin; changing
                                 * someone else needs an admin; and if whoever's logged in has
                                 * a password, `auth` must be it. 0, -EPERM, -EACCES (a wrong
                                 * auth), -EINVAL, -EEXIST, -ENOSPC, -EBUSY (the last admin, or
                                 * you), saved at once */
#define SYS_HOME            29  /* ebx: char[K86_PATH_MAX] out: the logged-in user's home folder
                                 * ("/Users/Ann"; "/" with no accounts). Returns its volume */
#define SYS_HW_PROBE        19  /* ebx: struct k86_probe_item * out, ecx: max, edx: flags
                                 * (K86_PROBE_*). Lists the sensors and controls the kernel
                                 * can see on this machine; only ever reads. Returns the
                                 * count (all of them: may be > max) */

/* --- memory -------------------------------------------------------------------- */
#define K86_APP_BASE        0x80000000u     /* where an app's image is loaded */
#define K86_CANVAS          0xa0000000u     /* where SYS_WINDOW_OPEN maps the canvas */
#define K86_HEAP            0x90000000u     /* SYS_MEMORY's memory starts here... */
#define K86_HEAP_MAX        (128u * 1024 * 1024)   /* ...and can be this big */
#define K86_FONT_BYTES      (256 * 16)
#define K86_PATH_MAX        256             /* "/Documents/Work", NUL included */
#define K86_NAME_MAX        64

/* --- the .lkx file: this header, then the image loaded at K86_APP_BASE ----------- */
#define LKX_MAGIC           0x41584b4cu     /* "LKXA" */
#define LKX_VERSION         1

struct lkx_header {
    uint32_t magic, version;
    uint32_t image_size;    /* bytes after this header */
    uint32_t bss_size;      /* zeroed bytes mapped after the image */
    uint32_t entry;         /* virtual address */
    uint32_t reserved[3];
};

/* --- structures ----------------------------------------------------------------- */
struct k86_window {
    int32_t w, h;           /* the content area, in pixels (K86_EVENT_RESIZE changes them) */
    uint32_t *canvas;       /* 0x00RRGGBB, `stride` pixels a row (room for the largest size) */
    int32_t stride;
};

#define K86_MAX_BUTTONS 6
struct k86_header {
    char title[48];
    int32_t nbuttons;
    struct {
        int32_t side;       /* 0 left, 1 right */
        int32_t icon;       /* 0 text, 1 back, 2 add, 3 up (kernel/wm.h's order) */
        int32_t id;
        char label[16];
        int32_t selected;   /* 1: shown as the current one (a tab: Settings' pages) */
    } buttons[K86_MAX_BUTTONS];
};

/* Event types: the same values as kernel/wm.h's. */
#define K86_EVENT_KEY       0
#define K86_EVENT_CLICK     1
#define K86_EVENT_HEADER    2   /* id: a header button */
#define K86_EVENT_CLOSE     3   /* close now: the next SYS_WAIT_EVENT ends the app */
#define K86_EVENT_POINTER   4   /* x, y in the canvas, buttons held, changed */
#define K86_EVENT_THEME     5   /* the style or accent changed: redraw (the SDK has already
                                 * fetched the new theme) */
#define K86_EVENT_RESIZE    6   /* x, y: the canvas's new width and height: lay out, redraw */
#define K86_EVENT_TIMER     7   /* SYS_TIMER_SET's interval is up */
#define K86_EVENT_SCROLL    8   /* x, y in the canvas (the pointer); id: notches, > 0 down
                                 * (the content moves up), < 0 up */

struct k86_event {
    int32_t type;
    int32_t x, y;
    uint8_t buttons, changed, pad[2];
    int32_t id;
    uint32_t time_ms;
    struct key_event key;
};

struct k86_volume {
    char name[40];
    int32_t supported, read_only;
    char why_not[24];
    uint32_t free_kib;
};

#define K86_MAX_ACCENTS 8
#define K86_MAX_WALLPAPERS 8
struct k86_appearance {
    int32_t style;          /* 0 dark, 1 light */
    int32_t accent;
    char wallpaper[32];     /* "" for the plain colour */
    int32_t naccents;
    struct {
        char name[12];
        uint32_t colour;
    } accents[K86_MAX_ACCENTS];
    int32_t nwallpapers;
    char wallpapers[K86_MAX_WALLPAPERS][32];
    int32_t screensaver_s;  /* seconds without input before it starts; 0 = never */
};

/* The hardware probe (SYS_HW_PROBE): one line of its report each. */
#define K86_PROBE_EC_RAM    1   /* flag: also read the embedded controller's 256 bytes */

#define K86_PS_SUMMARY      0   /* sections */
#define K86_PS_CPU          1
#define K86_PS_FIRMWARE     2
#define K86_PS_ACPI         3
#define K86_PS_EC           4
#define K86_PS_EC_FIELDS    5
#define K86_PS_DISK         6
#define K86_PS_POWER        7
#define K86_PS_DEVICES      8
#define K86_PROBE_SECTIONS  9

#define K86_PROBE_INFO      0   /* statuses: a plain fact */
#define K86_PROBE_OK        1   /* there, and read */
#define K86_PROBE_PARTIAL   2   /* there, but not usable yet (the note says why) */
#define K86_PROBE_MISSING   3   /* not on this machine, or LiteKern X can't reach it */
#define K86_PROBE_ERROR     4   /* there, but reading it failed or gave nonsense */

struct k86_probe_item {
    uint8_t section, status, pad[2];
    char name[28];
    char value[56];
    char note[80];
};

#define K86_WALLPAPER_DESKTOP 1  /* SYS_WALLPAPER_IMAGE: for the desktop */
#define K86_WALLPAPER_LOCK    2  /* ... for the lock screen (both: 3) */

struct k86_lock {
    char wallpaper[32];         /* "desktop", "none", a wallpaper's name, or "Custom" (its own
                                 * picture: SYS_WALLPAPER_IMAGE with K86_WALLPAPER_LOCK) */
    int32_t clock24;            /* 1: 14:05; 0: 2:05 PM */
    int32_t on_wake;            /* lock when the screen saver ends */
    int32_t has_picture;        /* read only: it has a picture of its own to show */
};

#define K86_USERS_MAX       8
#define K86_USER_NAME       21          /* 20 characters, NUL included */
#define K86_PASSWORD        64
struct k86_users {
    int32_t count;
    int32_t current;            /* the one logged in */
    struct {
        char name[K86_USER_NAME];
        uint8_t admin, has_password, pad[2];
        uint32_t colour;        /* the avatar's */
    } user[K86_USERS_MAX];
};

#define K86_USER_ADD        1   /* name, password ("": none), admin */
#define K86_USER_PASSWORD   2   /* user: index; password: the new one ("": none) */
#define K86_USER_RENAME     3   /* user, name */
#define K86_USER_ADMIN      4   /* user, admin: 1 or 0 */
#define K86_USER_REMOVE     5   /* user; admin: 1 = delete their home folder too */
struct k86_user_change {
    int32_t op;
    int32_t user;
    int32_t admin;
    char name[K86_USER_NAME];
    char password[K86_PASSWORD];
    char auth[K86_PASSWORD];    /* the logged-in user's own password, if they have one */
};

struct k86_dirent {
    char name[K86_NAME_MAX];
    uint32_t size;
    int32_t is_dir;
    uint16_t date, time;
};

/* --- networking (the Linux port's phase L5; kernel/net.h) ------------------------------------ *
 * The kernel version has no network hardware: SYS_NET_STATUS says so and the
 * others return -ENODEV. */
#define SYS_NET_STATUS      30  /* ebx: struct k86_net * out */
#define SYS_NET_SCAN        31  /* ebx: struct k86_wifi * out, ecx: max. Asks for a new scan and
                                 * returns what the last one found (all of them: may be > max) */
#define SYS_NET_JOIN        32  /* ebx: name (SSID), ecx: password ("": an open network).
                                 * Remembered, on the stick, for next time */
#define SYS_NET_FORGET      33  /* ebx: name: no longer remembered (and left, if joined) */

/* --- the machine (the Linux port's phase L4; kernel/machine.h) ------------------------------ *
 * The kernel version has no battery or brightness driver yet and keeps the
 * EeePC's clock as it is: SYS_MACHINE says so and the others return -ENODEV. */
#define SYS_MACHINE         34  /* ebx: struct k86_machine * out */
#define SYS_BRIGHTNESS_SET  35  /* ebx: the screen's brightness, 5-100 (%). Saved */
#define SYS_TIMEZONE_SET    36  /* ebx: the time zone, an index in kernel/timezones.h. Saved */
#define SYS_TOUCHPAD_SET    37  /* ebx: the touchpad's speed, 0-100 (50: the usual). Saved */
/* LiteKern Go: the phone's Back key (UI-PLAN.md). ebx: 1 = the app has something
 * for Back to close (a dialog, a page under its first, a folder's parent): Back
 * then comes to it as an Esc key press. 0 (how apps start): Back closes the app. */
#define SYS_WINDOW_BACK     38
/* LiteKern Go: ebx: 1 while the app has a text field open: the on-screen keyboard
 * comes up (kernel/osk.h) and the window shrinks above it (K86_EVENT_RESIZE). */
#define SYS_WINDOW_KEYBOARD 39

#define K86_BATTERY_DISCHARGING 0
#define K86_BATTERY_CHARGING    1
#define K86_BATTERY_FULL        2       /* on the charger, not charging */
struct k86_machine {
    int32_t battery;            /* 0-100 (%); -1: no battery */
    int32_t charging;           /* K86_BATTERY_* */
    int32_t minutes;            /* until empty (discharging) or full (charging); -1: unknown */
    int32_t brightness;         /* 0-100 (%); -1: not adjustable here */
    int32_t timezone;           /* an index in kernel/timezones.h; -1: no time zones here */
    int32_t touchpad;           /* its speed, 0-100; -1: no touchpad (of ours) */
};

#define K86_SSID_MAX        33          /* 32 bytes and a NUL */
#define K86_WIFI_MAX        32
#define K86_NET_NONE        0   /* states: no such hardware */
#define K86_NET_OFF         1   /* Wi-Fi switched off (the radio switch, Fn+F2) / no cable */
#define K86_NET_DOWN        2   /* there, not connected */
#define K86_NET_CONNECTING  3   /* joining, or asking for an address */
#define K86_NET_UP          4   /* connected, with an address */
#define K86_NET_WRONG_KEY   5   /* Wi-Fi: the password was refused */
struct k86_net {
    int32_t wired;              /* K86_NET_*: the Ethernet port */
    int32_t wifi;               /* K86_NET_*: Wi-Fi */
    char ssid[K86_SSID_MAX];    /* the Wi-Fi network joined, or being joined */
    uint8_t signal;             /* its strength, 0-100 */
    uint8_t pad[2];
    char address[16];           /* "192.168.1.23": the address in use ("" none) */
};

struct k86_wifi {
    char ssid[K86_SSID_MAX];
    uint8_t signal;             /* 0-100 */
    uint8_t secure;             /* needs a password */
    uint8_t known;              /* remembered */
    uint8_t connected;
    uint8_t pad[3];
};

#endif
