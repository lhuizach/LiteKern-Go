/* LiteKern X — the firmware's own description of the machine (SMBIOS/DMI):
 * BIOS vendor, version and date, and the system and board names. Read once
 * at boot (the hardware probe shows it; fan control will key its EC
 * register map to the BIOS version, since EC layouts can change between
 * versions). */
#ifndef LKX_SMBIOS_H
#define LKX_SMBIOS_H

#include <stdint.h>

struct smbios_info {
    int found;
    uint8_t major, minor;           /* SMBIOS version */
    uint32_t table_addr;
    uint16_t structures;
    char bios_vendor[48], bios_version[32], bios_date[16];
    char sys_vendor[48], sys_product[48], sys_version[32];
    char board_vendor[48], board_product[48];
};

/* Find the entry point (0xF0000-0xFFFFF), map the table, copy the strings.
 * At boot, while firmware memory can still be mapped. Logs one line. */
void smbios_init(void);

const struct smbios_info *smbios(void);

#endif
