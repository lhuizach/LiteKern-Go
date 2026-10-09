/* LiteKern X — Files: the picture viewer (picture.c), next to the text one.
 *
 * A picture is read and decoded into memory from SYS_MEMORY (apps/files/
 * image.c does the decoding), shown fitted into the window, and can be
 * made the wallpaper. */
#ifndef FILES_PICTURE_H
#define FILES_PICTURE_H

#include "kernel/gfx.h"
#include "sdk/kern86.h"

/* Is it named like a picture (.png, .jpg, .jpeg, .bmp)? */
int picture_named(const char *name);

/* Read and decode it. 0, or -1 with *why for the user. */
int picture_open(int vol, const char *dir, const struct k86_dirent *e, const char **why);

/* Draw it fitted into r (on canvas c), with its size and kind underneath,
 * and present that. */
void picture_draw(struct gfx_surface *c, struct gfx_rect r);

/* Make it the wallpaper: where is K86_WALLPAPER_DESKTOP and/or
 * K86_WALLPAPER_LOCK (the lock screen's). 0, or -1 with *why. */
int picture_wallpaper(int where, const char **why);

/* Let go of its memory. */
void picture_close(void);

#endif
