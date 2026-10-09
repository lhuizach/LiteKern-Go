/* LiteKern X — who needs the idle `hlt` woken often (see kernel/wake.h). */
#include "kernel/wake.h"
#include "kernel/driver.h"
#include "kernel/rtc.h"

static uint32_t wanted[WAKE_WHO], applied;

void wake_rate(enum wake_who who, uint32_t hz)
{
    if (who >= WAKE_WHO)
        return;
    wanted[who] = hz;
    uint32_t rate = 0;
    for (int i = 0; i < WAKE_WHO; i++)
        if (wanted[i] > rate)
            rate = wanted[i];
    if (rate == applied)
        return;
    device_t *rtc = device_find("rtc0");
    if (rtc && rtc->state == DEVICE_BOUND && dev_ioctl(rtc, RTC_SET_RATE, &rate) == 0)
        applied = rate;
}
