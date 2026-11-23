/* LiteKern Go: the first-run setup (docs/SETUP-PLAN.md). A "hello" in several languages over the
 * wallpaper, blurred once; then the look (dark or light, accent); then "You're all set". Drawn by the
 * shell over a covered desktop, like the lock screen. No PIN by default (the phone has no fingerprint
 * sensor, and the owner chose no lock). */
#ifndef LKX_SETUP_H
#define LKX_SETUP_H

#include <stdint.h>

void setup_start(void);
int setup_active(void);
void setup_tick(void);                                  /* every pass of the input loop: the animation */
void setup_pointer(int x, int y, uint8_t buttons);
void setup_key(uint16_t key, int pressed);

/* The platform's: the setup has finished (the Linux port removes its trigger file). */
void plat_setup_finished(void);

#endif
