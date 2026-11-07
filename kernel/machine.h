/* LiteKern X — the machine: the battery, the screen's brightness and the time
 * zone (the Linux port's phase L4). The top bar's battery icon and Settings'
 * System page (kernel/sys_app.c: SYS_MACHINE, SYS_BRIGHTNESS_SET,
 * SYS_TIMEZONE_SET) ask here.
 *
 * The Linux build has them (platform/machine.c: Linux's battery and backlight
 * drivers, the TZ rule); the kernel version has none yet (kernel/machine.c),
 * so the icon never shows and the System page says what's missing. */
#ifndef LKX_MACHINE_H
#define LKX_MACHINE_H

#include "kernel/kern86_abi.h"

/* After settings_load(): the saved brightness and time zone, applied. */
void machine_init(void);
void machine_status(struct k86_machine *out);
/* 0 or -errno (-EINVAL out of range, -ENODEV none). Both are saved. */
int machine_set_brightness(int percent);
/* LiteKern Go: a short buzz on the phone's vibrator; nothing on a machine without one. */
void machine_haptic(int ms);
int machine_set_timezone(int zone);
int machine_set_touchpad(int speed);
/* The screen (the phone's Power key): off keeps everything as it is, on shows
 * it again. Where the screen can't be turned off these only remember. */
void machine_screen(int on);
int machine_screen_on(void);

#endif
