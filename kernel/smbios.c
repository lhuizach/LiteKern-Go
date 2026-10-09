/* LiteKern X — SMBIOS (kernel/smbios.h). DMTF DSP0134: a 32-bit entry
 * point ("_SM_", or the older bare "_DMI_") points at a table of
 * structures, each a formatted part followed by its strings. */
#include "kernel/smbios.h"
#include "kernel/printk.h"
#include "kernel/string.h"
#include "kernel/vmm.h"

#define TYPE_BIOS   0
#define TYPE_SYSTEM 1
#define TYPE_BOARD  2
#define TYPE_END    127

static struct smbios_info info;

static int sum_ok(const uint8_t *p, uint32_t len)
{
    uint8_t s = 0;
    for (uint32_t i = 0; i < len; i++)
        s = (uint8_t)(s + p[i]);
    return s == 0;
}

/* String number `idx` (1-based) of the structure at s, printable and
 * trimmed, into out. */
static void string(const uint8_t *s, const uint8_t *end, uint8_t idx, char *out, int max)
{
    out[0] = '\0';
    if (!idx)
        return;
    const uint8_t *p = s + s[1];
    for (uint8_t i = 1; i < idx && p < end && *p; i++)
        while (p < end && *p++)
            ;
    int o = 0;
    for (; p < end && *p && o < max - 1; p++)
        out[o++] = *p >= 0x20 && *p < 0x7f ? (char)*p : '?';
    while (o > 0 && out[o - 1] == ' ')
        o--;
    out[o] = '\0';
}

static void parse(const uint8_t *p, const uint8_t *end, uint16_t count)
{
    for (uint16_t n = 0; n < count && p + 4 <= end; n++) {
        uint8_t type = p[0], len = p[1];
        if (len < 4 || p + len > end || type == TYPE_END)
            return;
        if (type == TYPE_BIOS && len >= 9) {
            string(p, end, p[4], info.bios_vendor, sizeof(info.bios_vendor));
            string(p, end, p[5], info.bios_version, sizeof(info.bios_version));
            string(p, end, p[8], info.bios_date, sizeof(info.bios_date));
        } else if (type == TYPE_SYSTEM && len >= 7) {
            string(p, end, p[4], info.sys_vendor, sizeof(info.sys_vendor));
            string(p, end, p[5], info.sys_product, sizeof(info.sys_product));
            string(p, end, p[6], info.sys_version, sizeof(info.sys_version));
        } else if (type == TYPE_BOARD && len >= 6) {
            string(p, end, p[4], info.board_vendor, sizeof(info.board_vendor));
            string(p, end, p[5], info.board_product, sizeof(info.board_product));
        }
        /* The next structure starts after the strings' double NUL. */
        const uint8_t *q = p + len;
        while (q + 1 < end && (q[0] || q[1]))
            q++;
        p = q + 2;
    }
}

void smbios_init(void)
{
    const uint8_t *dmi = 0;
    for (uint32_t a = 0xf0000; a < 0x100000 && !dmi; a += 16) {
        const uint8_t *e = (const uint8_t *)a;
        if (!memcmp(e, "_SM_", 4) && e[5] >= 0x1f && e[5] <= 0x40 && sum_ok(e, e[5]) &&
            !memcmp(e + 0x10, "_DMI_", 5)) {
            info.major = e[6];
            info.minor = e[7];
            dmi = e + 0x10;
        } else if (!memcmp(e, "_DMI_", 5) && sum_ok(e, 15)) {
            info.major = e[14] >> 4;
            info.minor = e[14] & 0x0f;
            dmi = e;
        }
    }
    if (!dmi || !sum_ok(dmi, 15)) {
        kprintf("smbios: none\n");
        return;
    }
    uint16_t len = (uint16_t)(dmi[6] | dmi[7] << 8);
    memcpy(&info.table_addr, dmi + 8, 4);
    info.structures = (uint16_t)(dmi[12] | dmi[13] << 8);
    /* Page 0 is never mapped (null pointers fault): a table there is bogus. */
    if (!len || info.table_addr < 0x1000 || vmm_map_firmware(info.table_addr, len)) {
        kprintf("smbios: %u.%u, table at 0x%08x can't be mapped\n", info.major, info.minor,
                info.table_addr);
        return;
    }
    const uint8_t *t = (const uint8_t *)info.table_addr;
    parse(t, t + len, info.structures);
    info.found = 1;
    kprintf("smbios: %u.%u; %s %s (BIOS %s %s, %s)\n", info.major, info.minor, info.sys_vendor,
            info.sys_product, info.bios_vendor, info.bios_version, info.bios_date);
}

const struct smbios_info *smbios(void)
{
    return &info;
}
