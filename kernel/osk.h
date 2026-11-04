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

/* show: 0 hides it, 1 shows the letters, 2 the numbers (a PIN). */
void osk_show(int show);
int osk_shown(void);
int osk_mode(void);                     /* what osk_show() last asked for */
int osk_height(void);                   /* 0 while hidden */
/* Where it draws: the keys, and above them a key's preview while pressed. */
struct gfx_rect osk_bounds(void);
/* Draw into dst, which shows the screen from (ox, oy). */
void osk_draw(struct gfx_surface *dst, int ox, int oy);
/* The pointer (the finger), in screen coordinates: 1 if the keyboard took it. */
int osk_pointer(int x, int y, uint8_t buttons);
/* Backspace's repeat, and its slide in and out. */
void osk_tick(void);
/* 1 while it slides (the shell keeps drawing). */
int osk_moving(void);

/* What the shell provides (kernel/desktop.c): a key for the app in front. */
void shell_osk_key(const struct key_event *k);

#endif
