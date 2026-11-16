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

/* Zoom (Gallery's pinch). Scales are screen pixels per picture pixel in 16.16 fixed point; (cx, cy) is the
 * picture point in the middle of the view. picture_zoom_get() says where the view is (s is the fitted scale if
 * it isn't zoomed); picture_zoom_set() moves it (kept inside the picture; about the fitted size means fitted
 * again); fast draws roughly, for while fingers move. The next picture_draw() shows it. */
int picture_zoomed(void);
void picture_zoom_reset(void);
void picture_zoom_get(struct gfx_rect r, int32_t *fit, int32_t *s, int32_t *cx, int32_t *cy);
void picture_zoom_set(struct gfx_rect r, int32_t s, int32_t cx, int32_t cy, int fast);

/* Make it the wallpaper: where is K86_WALLPAPER_DESKTOP and/or
 * K86_WALLPAPER_LOCK (the lock screen's). 0, or -1 with *why. */
int picture_wallpaper(int where, const char **why);

/* Let go of its memory. */
void picture_close(void);

/* LiteKern Go's Gallery: the open picture's middle square, scaled to
 * size x size, over black (0x00RRGGBB). 0, or -1 with none open. */
int picture_thumb(uint32_t *out, int size);

/* Which app's name its log lines start with ("files" unless set; 0: no "picture ..." line). */
extern const char *picture_app;

#endif
