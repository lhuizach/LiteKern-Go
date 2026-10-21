/* LiteKern X — the machine in the kernel version (kernel/machine.h): no
 * battery or brightness driver yet, and the EeePC's clock is shown as it is
 * (it keeps local time). The Linux build uses platform/machine.c instead. */
#include "kernel/machine.h"
#include "kernel/errno.h"

void machine_init(void)
{
}

void machine_status(struct k86_machine *out)
{
    *out = (struct k86_machine){ .battery = -1, .minutes = -1, .brightness = -1, .timezone = -1 };
}

int machine_set_brightness(int percent)
{
    (void)percent;
    return -ENODEV;
}

int machine_set_timezone(int zone)
{
    (void)zone;
    return -ENODEV;
}
