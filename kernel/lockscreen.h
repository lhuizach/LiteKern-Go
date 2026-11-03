/* LiteKern X — the lock screen (Phase 5 §5, stage 1).
 *
 * A big clock and the date over a dimmed wallpaper of its own, with the
 * power menu (Restart, Shut Down) at the top right. It's drawn by the
 * kernel's shell, like the screen saver: while it shows, apps keep running
 * but get no input, and their windows aren't drawn (the desktop is covered,
 * kernel/desktop.h). A click or a key unlocks it, or, if the user has a
 * password (kernel/users.h), shows the password prompt. Logging in (at
 * boot, when there's more than one user or a password) it starts with
 * everyone to choose from instead of the clock.
 *
 * It locks from the power menu's Lock, Super+L, SYS_LOCK, or when the
 * screen saver ends if lock-on-wake is set. Its choices are kept with the
 * other settings (kernel/settings.h).
 *
 * LiteKern Go (UI-PLAN.md): a swipe up unlocks, or with a password shows
 * the PIN pad (a password is the PIN). Notifications show as cards under
 * the clock, titles only. The picker and the power button are hidden: one
 * user, and holding Power opens the power menu (Restart, Power off). */
#ifndef LKX_LOCKSCREEN_H
#define LKX_LOCKSCREEN_H

#include <stdint.h>
#include "kernel/input.h"

/* Its choices. wallpaper: "desktop" (the desktop's own, the default),
 * "none" (the plain colour), a preset's name, or LOCK_PICTURE (a picture
 * of its own, lockscreen_set_picture). */
#define LOCK_PICTURE "Custom"
struct lock_settings {
    char wallpaper[32];
    int clock24;                /* 1: 14:05; 0: 2:05 PM */
    int on_wake;                /* lock when the screen saver ends */
};

void lockscreen_lock(void);
/* Someone has to log in (at boot): the users to choose from, or the one's
 * password prompt. Unlocking then logs them in (users_set_current()). */
void lockscreen_login(void);
int lockscreen_active(void);

/* The input loop, while it's active: every key and pointer update. */
void lockscreen_key(const struct key_event *k);
void lockscreen_pointer(int x, int y, uint8_t buttons);

/* Every pass of the input loop: the clock (redrawn when the minute changes). */
void lockscreen_tick(void);

/* The screen saver ended over it: draw it again. */
void lockscreen_redraw(void);

const struct lock_settings *lockscreen_settings(void);
/* Change them (Settings, or settings.txt at boot). A wallpaper it can't
 * draw shows the desktop's instead. Doesn't save: the caller does. */
void lockscreen_configure(const struct lock_settings *s);

/* A picture of its own (0x00RRGGBB, w x h, rows `stride` apart), scaled to
 * fill the screen; its wallpaper becomes LOCK_PICTURE. 0 or -ENOMEM. */
int lockscreen_set_picture(const uint32_t *px, int w, int h, int stride);
/* The same from a saved .lkxw file (kernel/settings.h, at boot). */
int lockscreen_load_picture(const void *data, uint32_t size);
/* 1 if it has a picture of its own. */
int lockscreen_has_picture(void);
/* The picture packed for saving, as wallpaper_pack(). -ENOENT if none. */
int lockscreen_pack_picture(uintptr_t *phys, uint32_t *bytes, uint32_t *len);

#endif
