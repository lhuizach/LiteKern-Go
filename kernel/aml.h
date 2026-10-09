/* LiteKern X — reading ACPI's AML byte code without running it (the
 * hardware probe, kernel/hwprobe.h; 2026-10-03).
 *
 * Not an interpreter: these find declarations in a table's byte code by
 * their opcode patterns and check each match (lengths inside the table,
 * valid names), the way kernel/acpi.c finds _S5. That's enough to list what
 * the firmware declares: devices and their _HIDs, operation regions, the
 * fields laid over the embedded controller's RAM, thermal zones, constant
 * P-state tables. Anything computed by a method stays unknown, and is
 * reported as such rather than guessed.
 *
 * All functions take [p, end): a table's body (acpi_body()). */
#ifndef LKX_AML_H
#define LKX_AML_H

#include <stdint.h>
#include "kernel/acpi.h"

/* A table's AML: after its header. */
void acpi_body(const struct acpi_sdt *t, const uint8_t **p, const uint8_t **end);

/* One integer constant at *p (ZeroOp, OneOp, OnesOp, Byte/Word/DWord/QWord
 * prefix; a QWord's low half). Advances *p. 0 or -1. */
int aml_int(const uint8_t **p, const uint8_t *end, uint32_t *v);

/* Devices, thermal zones, processors and root scopes: the objects that
 * contain others. */
enum aml_kind { AML_SCOPE, AML_DEVICE, AML_THERMAL, AML_PROCESSOR };

struct aml_scope {
    uint8_t kind;
    char name[5];               /* its last name segment, as in the table ("EC0_") */
    char path[40];              /* the name as written, padding trimmed ("\_SB.PCI0") */
    char hid[12];               /* Device: its _HID ("PNP0C09", or the string), or "" */
    const uint8_t *start, *end; /* everything inside it */
    int parent;                 /* the scope it's declared in (index), or -1 */
};

/* Find them all, parents and _HIDs included. Returns how many there are
 * (may be more than max: only max are filled in). */
int aml_scopes(const uint8_t *p, const uint8_t *end, struct aml_scope *out, int max);

/* The full path of scopes[i], following parents ("\_SB.PCI0.SBRG.EC0"). */
void aml_path(const struct aml_scope *scopes, int n, int i, char *out, int max);

/* OperationRegion(name, space, offset, length). */
struct aml_region {
    char name[5];
    uint8_t space;              /* 0 SystemMemory, 1 SystemIO, 2 PCI config, 3 EmbeddedControl... */
    uint8_t dynamic;            /* offset or length computed at run time: unknown */
    uint32_t offset, len;
};
int aml_regions(const uint8_t *p, const uint8_t *end, struct aml_region *out, int max);
const char *aml_space_name(uint8_t space);

/* One named field: `width` bits at bit `bit` of its region (Field), or of
 * the data register's window (IndexField: `region` is then the index
 * register's name and `indexed` is set). */
struct aml_field {
    char name[5];
    char region[5];
    uint8_t indexed;
    uint32_t bit, width;
};
/* The fields of every Field() over a region called `region` (NULL: every
 * Field and IndexField). Returns the count found (filled up to max). */
int aml_fields(const uint8_t *p, const uint8_t *end, const char *region, struct aml_field *out,
               int max);

/* A constant in [p, end): Name(seg, n) or Method(seg) { Return(n) }. 0 or -1. */
int aml_const(const uint8_t *p, const uint8_t *end, const char *seg, uint32_t *v);

/* The first P-state table: a Name holding a constant Package of
 * Package(6) { MHz, mW, latency, bus master latency, control, status }.
 * Its name goes in name[5]. Returns the number of states (filled up to max). */
struct aml_pstate {
    uint32_t mhz, mw, control, status;
};
int aml_pstates(const uint8_t *p, const uint8_t *end, char *name, struct aml_pstate *out, int max);

/* The I/O ports of a constant Name(_CRS, ResourceTemplate() { IO(...) ... })
 * in [p, end), in order. Returns how many were found (filled up to max). */
int aml_crs_io(const uint8_t *p, const uint8_t *end, uint16_t *ports, int max);

#endif
