/* LiteKern X — who needs the idle `hlt` woken often.
 *
 * The kernel has no timer besides rtc0: its periodic interrupt (RTC_SET_RATE)
 * is what wakes the idle loop for each frame of an animation. More than one
 * thing can want it (the screen saver, apps with a fast K86_EVENT_TIMER), so
 * each asks here and rtc0 runs at the fastest rate any of them wants. */
#ifndef LKX_WAKE_H
#define LKX_WAKE_H

#include <stdint.h>

enum wake_who { WAKE_SCREENSAVER, WAKE_APPS, WAKE_WHO };

/* hz: a power of two, 2..8192, or 0 = not needed any more. */
void wake_rate(enum wake_who who, uint32_t hz);

#endif
