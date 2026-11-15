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
#include "kernel/gfx.h"
#include "kernel/input.h"
#include "kernel/rtc.h"

/* Its choices. wallpaper: "desktop" (the desktop's own, the default),
 * "none" (the plain colour), a preset's name, or LOCK_PICTURE (a picture
 * of its own, lockscreen_set_picture). */
#define LOCK_PICTURE "Custom"
struct lock_settings {
    char wallpaper[32];
    int clock24;                /* 1: 14:05; 0: 2:05 PM */
    int on_wake;                /* lock when the screen saver ends */
    int clock_font;             /* K86_CLOCK_FONTS: Bold, Light, Thin, Condensed, Mono, Serif, Slim */
    int clock_style;            /* 0 one line, 1 stacked, 2 outline, 3 glass */
    int clock_opacity;          /* 20-100, % */
    int clock_stretch;          /* 100-250, % of its height */
    int clock_x, clock_y;       /* its middle, % of the screen; clock_y -1: where it goes by itself */
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
/* The lock screen as it'd look with these, shrunk to w x h (0x00RRGGBB). */
int lockscreen_preview(const struct lock_settings *s, uint32_t *out, int w, int h);

/* A picture of its own (0x00RRGGBB, w x h, rows `stride` apart), scaled to
 * fill the screen; its wallpaper becomes LOCK_PICTURE. 0 or -ENOMEM. */
int lockscreen_set_picture(const uint32_t *px, int w, int h, int stride);
/* The same from a saved .lkxw file (kernel/settings.h, at boot). */
int lockscreen_load_picture(const void *data, uint32_t size);
/* 1 if it has a picture of its own. */
int lockscreen_has_picture(void);
/* The picture packed for saving, as wallpaper_pack(). -ENOENT if none. */
int lockscreen_pack_picture(uintptr_t *phys, uint32_t *bytes, uint32_t *len);

/* The home screen's clock: the same clock, with choices of its own (saved in settings.txt).
 * x, y: its middle, in tenths of a percent of the screen; y -1: where it goes by itself.
 * size, opacity, stretch: %. */
struct home_clock {
    int show, font, style, opacity, stretch, size, x, y, date;   /* date: the day and month under the digits */
};
const struct home_clock *lockscreen_home_clock(void);
void lockscreen_set_home_clock(const struct home_clock *h);     /* clamps; doesn't save */
/* The wallpaper changed (for the glass style). */
void lockscreen_home_changed(void);
/* Draws it into s (0: only measure): something at screen (x, y) goes to s at (x - ox, y - oy).
 * mask: the digits, box: the digits and the date, both in screen coordinates. */
void lockscreen_home_clock_draw(struct gfx_surface *s, int ox, int oy, const struct rtc_time *t,
                                struct gfx_rect *mask, struct gfx_rect *box);

/* The big bold clock's face (the home screen uses it too). */
struct text_face;
const struct text_face *lockscreen_clock_face(void);

#endif
