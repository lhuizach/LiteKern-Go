/* LiteKern X — the ACPI embedded controller (EC), legacy device "ec0"
 * (drivers/ec.c; the hardware probe, 2026-10-03). On a laptop the EC runs
 * the fan, the battery gauge and the keyboard's Fn keys; its 256 bytes of
 * RAM hold their readings.
 *
 * READ-ONLY for now: the hardware probe only lists what's there. Writes
 * (fan control) come later, through a register map checked against the
 * BIOS version and only after the user confirms (docs/MONITOR.md).
 *
 *   EC_GET_INFO  struct ec_info *
 *   EC_READ      struct ec_io *: the byte at `addr` (the ACPI "read" command, 0x80)
 *   EC_READ_ALL  uint8_t[256]: every byte, 0x00 to 0xff */
#ifndef LKX_EC_H
#define LKX_EC_H

#include <stdint.h>

#define EC_GET_INFO 0x200
#define EC_READ     0x201
#define EC_READ_ALL 0x202

enum ec_ports_from { EC_PORTS_ECDT, EC_PORTS_CRS, EC_PORTS_DEFAULT };

struct ec_info {
    uint16_t data_port, cmd_port;
    uint8_t ports_from;         /* enum ec_ports_from */
    uint8_t status;             /* the status register when it was found */
    char path[40];              /* its ACPI device, e.g. "\_SB.PCI0.SBRG.EC0" */
};

struct ec_io {
    uint8_t addr;
    uint8_t value;
};

#endif
