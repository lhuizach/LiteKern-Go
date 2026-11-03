/* LiteKern X — networking in the kernel version (kernel/net.h): there is
 * none (no network drivers: LiteKernX-Roadmap/05 §4). The Linux build uses
 * platform/net.c instead of this file. */
#include "kernel/net.h"
#include "kernel/errno.h"
#include "kernel/string.h"

void net_status(struct k86_net *out)
{
    memset(out, 0, sizeof(*out));           /* K86_NET_NONE: no Ethernet, no Wi-Fi */
}

int net_scan(struct k86_wifi *out, int max)
{
    (void)out, (void)max;
    return -ENODEV;
}

int net_join(const char *ssid, const char *password)
{
    (void)ssid, (void)password;
    return -ENODEV;
}

int net_forget(const char *ssid)
{
    (void)ssid;
    return -ENODEV;
}

int net_wifi_set(int on)
{
    (void)on;
    return -ENODEV;
}
