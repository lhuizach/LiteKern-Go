/* LiteKern X — settings that last across restarts.
 *
 * What Settings chooses (style, accent, background, screen saver) and a
 * picture made the wallpaper in Files are kept on the boot disk's FAT32
 * partition, in its LiteKern folder:
 *
 *   /LiteKern/settings.txt      one setting a line, written by the system:
 *                                 style light | dark
 *                                 accent teal (a name from theme_accent())
 *                                 wallpaper crossing | Custom | none
 *                                 screensaver 300 (seconds; 0 = never)
 *                                 lock-wallpaper desktop | none | <preset> | Custom
 *                                 clock 24 | 12 (the lock screen's)
 *                                 lock-on-wake no | yes (lock when the screen saver ends)
 *   /LiteKern/wallpaper.lkxw    the custom wallpaper as shown (the screen's
 *                               size), packed as the build packs the others:
 *                               PNG-style filters, deflated (kernel/deflate.h),
 *                               checksummed. About 1 MB for a photo, where
 *                               the raw pixels would be 2.4: the BIOS reads
 *                               it at every start, and that's slow
 *   /LiteKern/lock.lkxw         the lock screen's own picture, the same way
 *
 * With accounts (more than one user, or a password: kernel/users.h) each
 * user has their own, in /LiteKern/users/<name>/ (the same three files),
 * read when they log in; the login screen looks as whoever logged in last
 * (/LiteKern/last-user.txt) left it. Their files go in a home folder,
 * /Users/<name>/ (Documents, Pictures), made when they first log in.
 *
 * Saved every time one changes. Without a writable boot disk nothing is
 * kept, and the build's defaults (make STYLE= ACCENT= ...) are used. A
 * damaged or hand-edited file can't do harm: unknown lines and values are
 * ignored. */
#ifndef LKX_SETTINGS_H
#define LKX_SETTINGS_H

#include <stdint.h>

/* After storage_init(): read settings.txt and apply the style, accent and
 * screen saver. Logs what it found. */
void settings_load(void);

/* The wallpaper to start with: the saved one, or `fallback` (the build's).
 * WALLPAPER_CUSTOM means settings_load_wallpaper() has the picture. */
const char *settings_wallpaper(const char *fallback);

/* Show the saved custom wallpaper. 0, or a negative error. */
int settings_load_wallpaper(void);

/* Write settings.txt now (after a change). */
void settings_save(void);

/* Write the custom wallpaper (after Files' Set Wallpaper), then settings.txt. */
void settings_save_wallpaper(void);

/* Users (kernel/users.c tells): someone logged in (their settings and
 * home folder), was renamed (their folders follow), was removed (their
 * settings go; their home folder too if delete_files). */
void settings_user_changed(const char *name);
void settings_user_renamed(const char *old_name, const char *new_name);
void settings_user_removed(const char *name, int delete_files);

/* The logged-in user's home folder ("/Users/Ann"), or "/" with one user
 * and no password. 0. */
int settings_home(char *out, int size);
/* The boot disk's index among the volumes (kernel/storage.h), or -ENODEV. */
int settings_boot_volume(void);

/* 1 if whoever's logged in mustn't see a path: /LiteKern, or someone
 * else's home folder, unless they're an admin (or there are no accounts). */
int settings_private(const char *path);

/* Other files in the LiteKern folder (kernel/users.h's users.txt): write
 * one (replacing it), or read one into buf (its size, -ENOENT, -EINVAL if
 * bigger than len, -ENODEV without a writable boot disk). */
int settings_write_file(const char *name, const void *data, uint32_t len);
/* LiteKern Go: a screenshot (a PNG file) into Pictures/Screenshots, in the
 * home folder ("/" without accounts), made if missing. 0 or an error. */
int settings_save_screenshot(const char *name, const void *data, uint32_t len);
int settings_read_file(const char *name, void *buf, uint32_t len);

/* The lock screen's own picture: read (at boot, from settings_load) and written. */
int settings_load_lock_picture(void);
void settings_save_lock_picture(void);

#endif
