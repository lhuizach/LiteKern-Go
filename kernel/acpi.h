/* LiteKern X — just enough ACPI to switch the machine off (Phase 5 §1,
 * brought forward on the user's request, 2026-10-02), and to let the
 * hardware probe read the firmware's tables (kernel/hwprobe.h, 2026-10-03).
 *
 * At boot it finds the firmware's tables (RSDP -> RSDT -> FADT -> DSDT) and
 * reads two things: the PM1 control registers (FADT) and the sleep-type
 * values of the _S5 ("soft off") object in the DSDT. That's a tiny AML
 * reader for one object, not an interpreter: _S5 is a plain package of
 * numbers on every machine we've seen (QEMU, VirtualBox, Intel ICH7 boards
 * like the EeePC's). Power-off then writes SLP_TYPx | SLP_EN to PM1a (and
 * PM1b).
 *
 * Every table the RSDT lists (and the DSDT) is checked and mapped at boot,
 * when mapping firmware memory is still allowed, and kept for
 * acpi_table(). Nothing here runs AML methods. */
#ifndef LKX_ACPI_H
#define LKX_ACPI_H

#include <stdint.h>

/* The header every ACPI table starts with. */
struct __attribute__((packed)) acpi_sdt {
    char sig[4];
    uint32_t length;            /* header included */
    uint8_t revision, checksum;
    char oem[6], oem_table[8];
    uint32_t oem_rev, creator, creator_rev;
};

/* Find the tables. Logs what it found ("acpi: ..."). 0 or -ENODEV. */
int acpi_init(void);

/* 1 if acpi_init found everything power-off needs. */
int acpi_can_power_off(void);

/* Enter S5 (soft off). Returns only if the machine is still on after it. */
void acpi_power_off(void);

/* The i-th table found (the DSDT first, then the RSDT's entries in order,
 * the FADT among them), checksum-checked; NULL past the end. */
const struct acpi_sdt *acpi_table(int i);

/* The first table with this signature ("SSDT", "ECDT"...), or NULL. */
const struct acpi_sdt *acpi_find(const char *sig);

/* The RSDP's OEM ID and revision ("" and 0 if there's no ACPI). */
const char *acpi_oem(int *revision);

/* 1 if the firmware has handed the machine to the OS (PM1a_CNT's SCI_EN):
 * ACPI mode. 0: legacy mode, where the firmware (SMM) still handles
 * thermal events, the lid and the Fn keys itself. -1: no ACPI. */
int acpi_sci_enabled(void);

#endif
