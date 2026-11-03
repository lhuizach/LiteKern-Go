/* LiteKern Go — the on-screen keyboard (UI-PLAN.md), part of the shell.
 *
 * It comes up at the bottom of the screen while the app in front has a text
 * field open (SYS_WINDOW_KEYBOARD) and the app's window shrinks above it.
 * Taps on its keys become ordinary key events for the app (the build's
 * keyboard layout gives them their characters), so apps need nothing else.
 * Letters, then numbers and symbols (123); Shift once for one capital, twice
 * for caps lock; Backspace repeats while held. */
#ifndef LKX_OSK_H
#define LKX_OSK_H

#include <stdint.h>
#include "kernel/gfx.h"
#include "kernel/input.h"

void osk_show(int show);
int osk_shown(void);
int osk_height(void);                   /* 0 while hidden */
/* Where it draws: the keys, and above them a key's preview while pressed. */
struct gfx_rect osk_bounds(void);
/* Draw into dst, which shows the screen from (ox, oy). */
void osk_draw(struct gfx_surface *dst, int ox, int oy);
/* The pointer (the finger), in screen coordinates: 1 if the keyboard took it. */
int osk_pointer(int x, int y, uint8_t buttons);
/* Backspace's repeat. */
void osk_tick(void);

/* What the shell provides (kernel/desktop.c): a key for the app in front. */
void shell_osk_key(const struct key_event *k);

#endif
