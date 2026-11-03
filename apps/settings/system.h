/* LiteKern Go — Settings: the Battery and Date and time pages (system.c). */
#ifndef SETTINGS_SYSTEM_H
#define SETTINGS_SYSTEM_H

#include "kernel/gfx.h"
#include "sdk/kern86.h"

#define SYSTEM_BATTERY 0
#define SYSTEM_TIME 1

void system_page_open(struct gfx_surface *canvas, int page);    /* read the machine, lay out, draw */
void system_page_draw(void);
void system_page_layout(void);                      /* the window was resized */
void system_page_tick(void);                        /* every 2 s while it shows: the battery */
void system_page_pointer(const struct k86_event *ev);
void system_page_scroll(const struct k86_event *ev);
void system_page_key(const struct key_event *k);
/* "72%, on battery" and "Empty in 3 h 10 min" (b empty if unknown): also the page list's. */
void system_battery_lines(const struct k86_machine *m, char *a, char *b, int size);

#endif
