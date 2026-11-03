/* LiteKern Go — Settings: the lock screen's PIN (pin.c). */
#ifndef SETTINGS_PIN_H
#define SETTINGS_PIN_H

#include "kernel/gfx.h"
#include "sdk/kern86.h"

enum { PIN_SET, PIN_CHANGE, PIN_REMOVE };

/* Start: its dialogs over the page (draw redraws the page, then calls
 * pin_draw()). done gets what to say ("" if cancelled), and if it's bad. */
void pin_start(struct gfx_surface *canvas, int what, int has_pin, int user, void (*done)(const char *, int),
               void (*draw)(void));
int pin_active(void);
void pin_draw(void);
void pin_layout(void);                  /* the window was resized (the keyboard came up) */
void pin_cancel(void);
void pin_pointer(const struct k86_event *ev);
void pin_key(const struct key_event *k);

#endif
