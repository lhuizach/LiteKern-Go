/* LiteKern X — the boot splash (Phase 3 §5): the logo while the system
 * starts, instead of the scrolling log. LiteKern Go: the G logo and the
 * name on black, fading in; shown at least 1.5 s, then it cross-fades into
 * the first screen (the lock screen or the home screen).
 *
 * `make SPLASH=logo` (the default) or `make SPLASH=log` (the scrolling
 * boot log, as before). Behind the splash the log keeps being recorded (the
 * Log app shows it), and a panic always shows it, on red. */
#ifndef LKX_SPLASH_H
#define LKX_SPLASH_H

#include <stdint.h>

/* Show it (after the screen and the console are up), if it's the chosen one. */
void splash_start(void);

/* Move the bar: 0..100. Does nothing without a splash. */
void splash_progress(int percent);

/* 1 while the splash is up. */
int splash_active(void);

/* The desktop takes over from here. */
void splash_end(void);

/* The cross-fade: begin() (1 if there's a splash to fade from) holds the
 * screen, the first screen is drawn, then end() fades it in. */
int splash_fade_begin(void);
void splash_fade_end(void);

/* No fades and no minimum time: the logo, then the first screen at once
 * (lkx-shell's headless backend, so the tests don't wait on it). */
void splash_set_brief(int brief);

/* Draw the splash as it is `t_ms` after it began (the tests and previews: platform/main.c's
 * LKX_SPLASH_PREVIEW). */
void splash_preview(uint32_t t_ms);

#endif
