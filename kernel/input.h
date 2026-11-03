/* LiteKern X — input events, as read() from input devices (kbd0; the
 * touchpad later). Reads return whole events: `len` must be a multiple of
 * the event size (-EINVAL otherwise), and 0 means nothing is waiting —
 * reads never block. */
#ifndef LKX_INPUT_H
#define LKX_INPUT_H

#include <stdint.h>

/* Key codes: the scancode-set-1 make code of the key, plus KEY_EXTENDED for
 * keys sent with an E0 prefix. US layout. */
#define KEY_EXTENDED    0x100
#define KEY_ESC         0x001
#define KEY_BACKSPACE   0x00e
#define KEY_TAB         0x00f
#define KEY_ENTER       0x01c
#define KEY_LCTRL       0x01d
#define KEY_LSHIFT      0x02a
#define KEY_RSHIFT      0x036
#define KEY_LALT        0x038
#define KEY_SPACE       0x039
#define KEY_CAPSLOCK    0x03a
#define KEY_F1          0x03b   /* F1-F10 are 0x03b-0x044 */
#define KEY_F11         0x057
#define KEY_F12         0x058
#define KEY_RCTRL       (KEY_EXTENDED | 0x01d)
#define KEY_RALT        (KEY_EXTENDED | 0x038)
#define KEY_HOME        (KEY_EXTENDED | 0x047)
#define KEY_UP          (KEY_EXTENDED | 0x048)
#define KEY_PAGEUP      (KEY_EXTENDED | 0x049)
#define KEY_LEFT        (KEY_EXTENDED | 0x04b)
#define KEY_RIGHT       (KEY_EXTENDED | 0x04d)
#define KEY_END         (KEY_EXTENDED | 0x04f)
#define KEY_DOWN        (KEY_EXTENDED | 0x050)
#define KEY_PAGEDOWN    (KEY_EXTENDED | 0x051)
#define KEY_INSERT      (KEY_EXTENDED | 0x052)
#define KEY_DELETE      (KEY_EXTENDED | 0x053)
#define KEY_LSUPER      (KEY_EXTENDED | 0x05b)  /* the Windows keys */
#define KEY_RSUPER      (KEY_EXTENDED | 0x05c)

/* LiteKern Go: the phone's own keys, which no PC keyboard has (UI-PLAN.md).
 * Back goes to the app as KEY_ESC when it asked for it (SYS_WINDOW_BACK), which
 * apps already treat as "close this"; otherwise it closes the app. In the VM,
 * PC keys stand in (platform/devices.c, plat_evdev_key()). */
#define KEY_PHONE           0x200
#define KEY_PHONE_HOME      (KEY_PHONE | 1)
#define KEY_PHONE_RECENTS   (KEY_PHONE | 2)
#define KEY_PHONE_POWER     (KEY_PHONE | 3)
#define KEY_VOLUME_UP       (KEY_PHONE | 4)
#define KEY_VOLUME_DOWN     (KEY_PHONE | 5)
#define KEY_PHONE_BACK      (KEY_PHONE | 6)  /* to the app as KEY_ESC, or it closes it */

/* kbd0 ioctl */
#define KBD_GET_DROPPED 1       /* arg: uint32_t *, events lost to a full queue */

#define MOD_SHIFT       0x01
#define MOD_CTRL        0x02
#define MOD_ALT         0x04
#define MOD_CAPSLOCK    0x08

/* mouse0 (the touchpad): one event per PS/2 packet. Screen convention:
 * dx > 0 is right, dy > 0 is down. */
#define MOUSE_GET_DROPPED 1     /* ioctl arg: uint32_t *, events lost to a full queue */
#define MOUSE_LEFT      0x01
#define MOUSE_RIGHT     0x02
#define MOUSE_MIDDLE    0x04

struct mouse_event {
    int16_t dx, dy;
    uint8_t buttons;    /* MOUSE_* held after this packet */
    int8_t wheel;       /* scroll notches: > 0 down (the page moves up), < 0 up. The
                         * Linux build: a mouse's wheel, two fingers on the touchpad */
    uint8_t reserved[2];
};

struct key_event {
    uint16_t key;       /* KEY_* / scancode, see above */
    uint8_t pressed;    /* 1 = press (or auto-repeat), 0 = release */
    uint8_t mods;       /* MOD_* in effect for this event */
    uint8_t ascii;      /* the character typed, 0 if none (ctrl+letter = 1..26) */
    uint8_t reserved[3];
};

#endif
