/* LiteKern X — Settings: the Users page (users.c). */
#ifndef SETTINGS_USERS_H
#define SETTINGS_USERS_H

#include "kernel/gfx.h"
#include "sdk/kern86.h"

void users_page_open(struct gfx_surface *canvas);   /* read the users, lay out, draw */
void users_page_draw(void);
void users_page_layout(void);                       /* the window was resized */
void users_page_pointer(const struct k86_event *ev);
void users_page_key(const struct key_event *k);
int users_page_busy(void);               /* a dialog open: Back cancels it */

#endif
