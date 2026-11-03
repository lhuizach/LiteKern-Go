/* LiteKern X — networking (the Linux port's phase L5): the desktop's top-bar
 * icon and the network calls (kernel/sys_app.c: SYS_NET_*) ask here.
 *
 * The Linux build has it (platform/net.c: Ethernet and Wi-Fi through Linux's
 * drivers, wpa_supplicant and DHCP); the kernel version has no network
 * hardware (kernel/net.c), so the icon never shows and the calls say no. */
#ifndef LKX_NET_H
#define LKX_NET_H

#include "kernel/kern86_abi.h"

void net_status(struct k86_net *out);
/* What the last scan found (up to max; the count of all), and a new one asked for. */
int net_scan(struct k86_wifi *out, int max);
/* Join a Wi-Fi network (password "" for an open one), remembering it. 0 or -errno. */
int net_join(const char *ssid, const char *password);
int net_forget(const char *ssid);
/* LiteKern Go's quick settings: Wi-Fi's radio on or off (rfkill). 0 or -errno. */
int net_wifi_set(int on);

#endif
