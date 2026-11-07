/* LiteKern Go: Settings' About Phone page. */
#ifndef SETTINGS_ABOUT_H
#define SETTINGS_ABOUT_H

#include "kernel/gfx.h"
#include "sdk/kern86.h"

/* wallpaper: the home screen's, for the picture of the phone. */
void about_open(struct gfx_surface *canvas, const char *wallpaper);
void about_draw(void);
void about_pointer(const struct k86_event *ev);
/* 1 if it used the key (renaming). */
int about_key(const struct key_event *k);
int about_editing(void);
/* The phone's name, for the Settings list. */
const char *about_phone_name(void);

#endif
