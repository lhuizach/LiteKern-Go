/* LiteKern X — Settings: the System page (system.c): battery, brightness, time zone. */
#ifndef SETTINGS_SYSTEM_H
#define SETTINGS_SYSTEM_H

#include "kernel/gfx.h"
#include "sdk/kern86.h"

void system_page_open(struct gfx_surface *canvas);   /* read the machine, lay out, draw */
void system_page_draw(void);
void system_page_layout(void);                      /* the window was resized */
void system_page_tick(void);                        /* every 2 s while it shows: the battery */
void system_page_pointer(const struct k86_event *ev);
void system_page_scroll(const struct k86_event *ev);
void system_page_key(const struct key_event *k);

#endif
