/* LiteKern Go: Settings' lock screen clock editor. */
#ifndef SETTINGS_CLOCKEDIT_H
#define SETTINGS_CLOCKEDIT_H

#include "kernel/gfx.h"
#include "sdk/kern86.h"

void clockedit_open(struct gfx_surface *canvas, struct k86_lock *lock);
int clockedit_active(void);
/* An event for it. 1 when it's closed. */
int clockedit_event(const struct k86_event *ev);

#endif
