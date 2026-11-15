/* LiteKern Go: the cover screen. A full-screen fake of a failing boot: a memory test that finds errors, then a
 * kernel panic dump that scrolls, over and over. It's only a picture (nothing is wrong): the shell shows it
 * over everything when asked from the control centre, ignores every key and touch, and a held Power key
 * ends it like a restart. See kernel/desktop.c. */
#ifndef LKX_PANICSCREEN_H
#define LKX_PANICSCREEN_H

#include <stdint.h>
#include "kernel/gfx.h"

/* Starts the picture from its first line. */
void panicscreen_start(uint32_t now_ms);

/* Draws what it shows at time now_ms into s, which shows the screen from (ox, oy). */
void panicscreen_draw(struct gfx_surface *s, int ox, int oy, uint32_t now_ms);

/* A black screen with a line of text: what shows for a moment as it "restarts". */
void panicscreen_draw_restart(struct gfx_surface *s, int ox, int oy);

#endif
