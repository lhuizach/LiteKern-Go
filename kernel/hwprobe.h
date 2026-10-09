/* LiteKern X — the hardware probe (stage (a) of the monitor and fan control
 * app, docs/MONITOR.md; 2026-10-03).
 *
 * Lists which sensors and controls this machine has and which of them
 * LiteKern X can reach: the CPU's thermal sensor and SpeedStep registers,
 * the firmware (SMBIOS, ACPI tables, devices, thermal zones, P-states), the
 * embedded controller and the fields the ACPI tables lay over its RAM, the
 * disk's SMART data, the battery, backlight and other devices. A summary
 * section gives a verdict for each feature the monitor plans to use.
 *
 * It only ever reads. The EC's RAM is read only when asked
 * (K86_PROBE_EC_RAM). */
#ifndef LKX_HWPROBE_H
#define LKX_HWPROBE_H

#include <stdint.h>
#include "kernel/kern86_abi.h"

/* Fill up to max items (out may be an app's buffer: the caller checked
 * it); returns how many there are. The first run also logs the summary. */
int hwprobe(struct k86_probe_item *out, int max, uint32_t flags);

#endif
