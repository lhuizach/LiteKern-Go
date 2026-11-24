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
/* LiteKern Go: the loudspeaker's volume, 0-100. set: apps see it at once (sdk/audio.h);
 * commit: kept for the next start (call it when the changing stops). */
int machine_volume(void);
void machine_set_volume(int percent);
void machine_volume_commit(void);

/* Airplane mode (LiteKern Go): kept in machine.txt; the shell turns Wi-Fi off and on with it. */
int machine_airplane(void);
void machine_set_airplane(int on);

/* The shell's own sounds (platform/uisound.c): a small sound for a tap, lock, unlock, screenshot, volume change and so on.
 * Silent while the volume is 0 or machine_ui_mute(1) (do not disturb) is set. */
enum { UI_TAP, UI_BACK, UI_LOCK, UI_UNLOCK, UI_SHUTTER, UI_BOOT, UI_NOTIFY, UI_VOLUME, UI_ON, UI_OFF };
void machine_ui_sound(int id);
void machine_ui_mute(int muted);
/* LiteKern Go: a short buzz on the phone's vibrator; nothing on a machine without one. */
void machine_haptic(int ms);
int machine_set_timezone(int zone);
int machine_set_touchpad(int speed);
/* LiteKern Go's Music widget: what the Music app is playing (it shares it in /dev/shm/lkx-music).
 * 0: it isn't running, 1: playing, 2: paused or stopped; the track's name goes in title. A command
 * ("toggle", "next", "prev") is left for the app to take. */
int machine_music(char *title, int max);
void machine_music_command(const char *cmd);
/* LiteKern Go's Weather widget: the numbers the Weather helper left in /dev/shm/lkx-weather. Temperatures
 * are tenths of a degree, code is a WMO weather code. 0: no numbers yet. machine_weather_refresh() starts
 * the helper (`weather --fetch`) when the numbers are old; call it now and then. */
struct weather_now {
    char city[32];
    int temp, code, day, hi, lo;
};
int machine_weather(struct weather_now *out);
void machine_weather_refresh(void);
/* The screen (the phone's Power key): off keeps everything as it is, on shows
 * it again. Where the screen can't be turned off these only remember. */
void machine_screen(int on);
int machine_screen_on(void);

#endif
