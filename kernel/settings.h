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
 * Saved every time one changes. Without a writable boot disk nothing is
 * kept, and the build's defaults (make STYLE= ACCENT= ...) are used. A
 * damaged or hand-edited file can't do harm: unknown lines and values are
 * ignored. */
#ifndef LKX_SETTINGS_H
#define LKX_SETTINGS_H

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

/* The lock screen's own picture: read (at boot, from settings_load) and written. */
int settings_load_lock_picture(void);
void settings_save_lock_picture(void);

#endif
