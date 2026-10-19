/* LiteKern X — keyboard layouts: key positions (scancode set 1) to
 * characters. Shared by the PS/2 driver (drivers/kbd.c) and the Linux
 * platform's keyboard (platform/), which both see keys by position.
 *
 * KEY_* codes (kernel/input.h) are set 1 make codes, | KEY_EXTENDED for the
 * E0-prefixed keys; Linux's evdev key codes are the same numbers for the main
 * block, so the Linux side translates only the extended keys. */
#ifndef LKX_KEYMAP_H
#define LKX_KEYMAP_H

#include <stdint.h>
#include "kernel/input.h"

/* "azerty" (French) or "qwerty" (US); anything else: qwerty. */
void keymap_select(const char *name);
const char *keymap_name(void);

/* A key went down (pressed = 1, or auto-repeat) or up: the event, with the
 * modifiers it leaves in effect and the character it types. */
struct key_event keymap_event(uint16_t key, int pressed);

/* Feed set 1 bytes in the order they arrive: 1 with *out filled when they
 * complete a key event, else 0 (a prefix, an ACK, the fake shifts around
 * PrtSc, the rest of Pause). */
int keymap_scancode(uint8_t sc, struct key_event *out);

#endif
