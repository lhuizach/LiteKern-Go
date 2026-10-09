/* LiteKern X — the hardware probe (kernel/hwprobe.h).
 *
 * Each section gathers facts into `f`, adding its own report lines as it
 * goes; the summary is worked out from the facts at the end. Report lines
 * are written straight into the caller's array; past `max` they're only
 * counted. */
#include "kernel/hwprobe.h"
#include "kernel/acpi.h"
#include "kernel/aml.h"
#include "kernel/block.h"
#include "kernel/cpu.h"
#include "kernel/driver.h"
#include "kernel/ec.h"
#include "kernel/errno.h"
#include "kernel/io.h"
#include "kernel/pci.h"
#include "kernel/pmm.h"
#include "kernel/printk.h"
#include "kernel/smbios.h"
#include "kernel/string.h"
#include "kernel/timing.h"

/* MSRs (Intel SDM vol. 4: architectural, and the Atom 06_1CH table). */
#define MSR_FSB_FREQ            0x0cd
#define MSR_PERF_STATUS         0x198
#define MSR_PERF_CTL            0x199
#define MSR_CLOCK_MODULATION    0x19a
#define MSR_THERM_STATUS        0x19c
#define MSR_MISC_ENABLE         0x1a0
#define MSR_TEMPERATURE_TARGET  0x1a2

/* FADT fields (offsets from the ACPI spec). */
#define FADT_PM_TMR_BLK  76
#define FADT_P_LVL2_LAT  96
#define FADT_P_LVL3_LAT  98
#define FADT_DUTY_OFFSET 104
#define FADT_DUTY_WIDTH  105

#define MAX_SCOPES  256
#define MAX_REGIONS 96
#define MAX_FIELDS  320

static struct k86_probe_item *items, spare;
static int nitems, maxitems;

/* What the sections found, for the summary. */
static struct {
    /* CPU */
    int model, stepping, est, est_on, perf_ok, dts, dts_ok, temp_c, tjmax, tjmax_assumed;
    int therm_ok, prochot_now, prochot_log, clockmod_on, clockmod_pct, mhz, ht;
    /* firmware */
    int sci, nzones, npstates;
    char battery[40], ac[40], lid[40], asus[40];
    /* EC */
    int ec, ec_read, ec_fields;
    char ec_declared[40];
    char fan_fields[48];
    /* disk */
    int disk, smart, disk_temp;     /* smart: 0 none, 1 off, 2 no temperature, 3 temperature */
    /* other */
    int gma, lbb, nets, wifi;
} f;

static uint8_t ec_ram[256];
static struct aml_scope scopes[MAX_SCOPES];
static struct aml_region regions[MAX_REGIONS];
static struct aml_field fields[MAX_FIELDS];
static int logged;

static struct k86_probe_item *add(int section, int status, const char *name)
{
    struct k86_probe_item *it = nitems < maxitems ? &items[nitems] : &spare;
    nitems++;
    memset(it, 0, sizeof(*it));
    it->section = (uint8_t)section;
    it->status = (uint8_t)status;
    ksnprintf(it->name, sizeof(it->name), "%s", name);
    return it;
}

#define VALUE(it, ...) ksnprintf((it)->value, sizeof((it)->value), __VA_ARGS__)
#define NOTE(it, ...)  ksnprintf((it)->note, sizeof((it)->note), __VA_ARGS__)

static void copy(char *dst, const char *src, int max)
{
    int i = 0;
    for (; src[i] && i < max - 1; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* "4 KB", "24 MB": sizes for people. */
static void size_text(char *out, int max, uint32_t bytes)
{
    if (bytes < 10 * 1024)
        ksnprintf(out, (uint32_t)max, "%u bytes", bytes);
    else
        ksnprintf(out, (uint32_t)max, "%u KB", (bytes + 512) / 1024);
}

/* --- CPU ------------------------------------------------------------------------ */

static void probe_cpu(void)
{
    uint32_t a, b, c, d, max_leaf, max_ext;
    char vendor[13], brand[49];
    cpuid(0, &max_leaf, &b, &c, &d);
    memcpy(vendor, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = '\0';
    cpuid(0x80000000u, &max_ext, &b, &c, &d);
    brand[0] = '\0';
    if (max_ext >= 0x80000004u) {
        uint32_t *w = (uint32_t *)brand;
        for (uint32_t leaf = 0; leaf < 3; leaf++)
            cpuid(0x80000002u + leaf, &w[leaf * 4], &w[leaf * 4 + 1], &w[leaf * 4 + 2], &w[leaf * 4 + 3]);
        brand[48] = '\0';
    }
    const char *name = brand;
    while (*name == ' ')
        name++;
    cpuid(1, &a, &b, &c, &d);
    int family = (int)(a >> 8 & 0xf);
    f.model = (int)(a >> 4 & 0xf) | (family == 6 || family == 15 ? (int)(a >> 12 & 0xf0) : 0);
    f.stepping = (int)(a & 0xf);
    struct k86_probe_item *it = add(K86_PS_CPU, K86_PROBE_INFO, "Processor");
    VALUE(it, "%s", *name ? name : vendor);
    NOTE(it, "%s, family %u, model 0x%x, stepping %u", vendor, family, f.model, f.stepping);

    f.ht = (d >> 28 & 1) ? (int)(b >> 16 & 0xff) : 1;
    it = add(K86_PS_CPU, K86_PROBE_INFO, "Logical processors");
    VALUE(it, "%u%s", f.ht, f.ht > 1 ? " (Hyper-Threading)" : "");
    NOTE(it, "LiteKern X uses one (SMP and HT are non-goals); CPU %% is of that one");

    uint32_t e1d = 0, e1b, e1c, e1a;
    if (max_ext >= 0x80000001u)
        cpuid(0x80000001u, &e1a, &e1b, &e1c, &e1d);
    it = add(K86_PS_CPU, K86_PROBE_INFO, "Instruction sets");
    VALUE(it, "%s%s%s%s", d >> 26 & 1 ? "SSE2 " : "", c & 1 ? "SSE3 " : "", c >> 9 & 1 ? "SSSE3 " : "",
          e1d >> 29 & 1 ? "64-bit" : "32-bit only");
    NOTE(it, "LiteKern X builds for i686 and uses none of the SSE units");

    /* SpeedStep: CPUID says the CPU has it; IA32_MISC_ENABLE bit 16 says
     * the firmware left it on. */
    uint64_t misc = 0, perf = 0, ctl = 0;
    int misc_ok = !rdmsr_safe(MSR_MISC_ENABLE, &misc);
    f.est = c >> 7 & 1;
    f.est_on = f.est && misc_ok && (misc >> 16 & 1);
    it = add(K86_PS_CPU, f.est_on ? K86_PROBE_OK : f.est ? K86_PROBE_PARTIAL : K86_PROBE_MISSING,
             "SpeedStep (EIST)");
    VALUE(it, "%s", f.est_on ? "on" : f.est ? "present, switched off by the firmware" : "not reported");
    NOTE(it, "%s", f.est ? "frequency states are set through IA32_PERF_CTL (0x199)"
                         : "CPUID.1 ECX bit 7 is clear (a VM, or a CPU without it)");

    /* The clock: IA32_PERF_STATUS has the current ratio (bits 12:8, bit 14
     * adds a half) and, on Core 2 and Atom, the highest (44:40, 46). The
     * TSC runs at the highest, so now = TSC rate x current / highest. */
    f.perf_ok = f.est && !rdmsr_safe(MSR_PERF_STATUS, &perf) && !rdmsr_safe(MSR_PERF_CTL, &ctl);
    uint32_t cur2 = (uint32_t)(perf >> 8 & 0x1f) * 2 + (uint32_t)(perf >> 14 & 1);
    uint32_t max2 = (uint32_t)(perf >> 40 & 0x1f) * 2 + (uint32_t)(perf >> 46 & 1);
    uint64_t fsb = 0;
    int fsb_ok = !rdmsr_safe(MSR_FSB_FREQ, &fsb);
    static const uint16_t fsb_mhz[8] = { 267, 133, 200, 167, 333, 100, 0, 0 };
    uint32_t bus = fsb_ok ? fsb_mhz[fsb & 7] : 0;
    it = add(K86_PS_CPU, f.perf_ok && cur2 ? K86_PROBE_OK : K86_PROBE_MISSING, "Clock now");
    if (f.perf_ok && cur2 && max2 >= cur2) {
        f.mhz = (int)(tsc_mhz() * cur2 / max2);
        VALUE(it, "%u MHz (ratio %u%s of %u%s)", f.mhz, cur2 / 2, cur2 & 1 ? ".5" : "", max2 / 2,
              max2 & 1 ? ".5" : "");
        NOTE(it, "TSC %u MHz; bus %u MHz (MSR 0xcd); VID 0x%02x", tsc_mhz(), bus,
             (uint32_t)(perf & 0xff));
    } else if (f.perf_ok && cur2 && bus) {
        f.mhz = (int)(bus * cur2 / 2);
        VALUE(it, "%u MHz (ratio %u%s x %u MHz bus)", f.mhz, cur2 / 2, cur2 & 1 ? ".5" : "", bus);
        NOTE(it, "highest ratio not reported; TSC %u MHz", tsc_mhz());
    } else {
        VALUE(it, "TSC %u MHz", tsc_mhz());
        NOTE(it, "%s", f.est ? "IA32_PERF_STATUS gave no ratio" : "no SpeedStep: the TSC rate is all we know");
    }

    /* The digital thermal sensor: IA32_THERM_STATUS bits 22:16 are degrees
     * below TjMax, valid when bit 31 is set. */
    uint32_t l6a = 0, l6b, l6c, l6d;
    if (max_leaf >= 6)
        cpuid(6, &l6a, &l6b, &l6c, &l6d);
    uint64_t therm = 0, target = 0;
    f.dts = (l6a & 1) || (d >> 22 & 1);
    f.therm_ok = f.dts && !rdmsr_safe(MSR_THERM_STATUS, &therm);
    int target_ok = !rdmsr_safe(MSR_TEMPERATURE_TARGET, &target);
    int tj = target_ok ? (int)(target >> 16 & 0xff) : 0;
    f.tjmax_assumed = tj < 70 || tj > 110;
    /* Atom 06_1CH: 90 C, except stepping 10 (N4xx/D4xx/D5xx): 100 C, as in
     * Intel's datasheets and Linux's coretemp. Otherwise 100 C. */
    f.tjmax = !f.tjmax_assumed ? tj : f.model == 0x1c && f.stepping != 10 ? 90 : 100;
    int readout = (int)(therm >> 16 & 0x7f);
    f.dts_ok = f.therm_ok && (therm >> 31 & 1);
    f.temp_c = f.tjmax - readout;
    int sane = f.temp_c > 5 && f.temp_c < 125;
    it = add(K86_PS_CPU, f.dts_ok ? (sane ? K86_PROBE_OK : K86_PROBE_ERROR)
                         : f.dts ? K86_PROBE_ERROR : K86_PROBE_MISSING, "Thermal sensor (DTS)");
    if (f.dts_ok) {
        VALUE(it, "%d C%s", f.temp_c, sane ? "" : " (impossible: ignored)");
        NOTE(it, "TjMax %u C %s, minus %u", f.tjmax,
             f.tjmax_assumed ? "(assumed: no MSR 0x1a2)" : "(MSR 0x1a2)", readout);
    } else {
        VALUE(it, "%s", f.dts ? "no valid reading" : "not reported");
        NOTE(it, "%s", f.dts ? "IA32_THERM_STATUS bit 31 is clear (a VM gives none)"
                             : "CPUID has neither the DTS nor the ACPI thermal bit");
    }
    if (!sane)
        f.dts_ok = 0;

    /* Throttling: PROCHOT (bit 0 now, bit 1 since last cleared), TM1/TM2
     * (IA32_MISC_ENABLE bits 3 and 13), and on-demand clock modulation. */
    uint64_t mod = 0;
    int mod_ok = !rdmsr_safe(MSR_CLOCK_MODULATION, &mod);
    f.prochot_now = (int)(therm & 1);
    f.prochot_log = (int)(therm >> 1 & 1);
    f.clockmod_on = mod_ok && (mod >> 4 & 1);
    f.clockmod_pct = (int)((mod >> 1 & 7) * 125 / 10);
    it = add(K86_PS_CPU, f.therm_ok ? K86_PROBE_OK : K86_PROBE_MISSING, "Thermal throttling");
    if (f.therm_ok) {
        VALUE(it, "%s now; %s since boot", f.prochot_now ? "hot" : "not hot",
              f.prochot_log ? "was hot" : "never hot");
        NOTE(it, "TM1 %s, TM2 %s (the CPU slows itself at TjMax)", misc >> 3 & 1 ? "on" : "off",
             misc >> 13 & 1 ? "on" : "off");
    } else {
        VALUE(it, "unknown");
        NOTE(it, "needs IA32_THERM_STATUS");
    }
    it = add(K86_PS_CPU, mod_ok ? K86_PROBE_INFO : K86_PROBE_MISSING, "Clock modulation");
    VALUE(it, "%s", !mod_ok ? "not readable" : f.clockmod_on ? "on" : "off (full speed)");
    if (f.clockmod_on)
        NOTE(it, "duty %u%% of the time (IA32_CLOCK_MODULATION)", f.clockmod_pct);

    /* ACPI's own throttling and idle states (the FADT). */
    const struct acpi_sdt *fadt = acpi_find("FACP");
    if (fadt && fadt->length > FADT_DUTY_WIDTH) {
        const uint8_t *t = (const uint8_t *)fadt;
        it = add(K86_PS_CPU, K86_PROBE_INFO, "ACPI throttling (FADT)");
        if (t[FADT_DUTY_WIDTH])
            VALUE(it, "duty width %u bits at bit %u", t[FADT_DUTY_WIDTH], t[FADT_DUTY_OFFSET]);
        else
            VALUE(it, "not offered (duty width 0)");
        NOTE(it, "C2 latency %u us, C3 %u us (over 100 / 1000: not supported)",
             t[FADT_P_LVL2_LAT] | t[FADT_P_LVL2_LAT + 1] << 8,
             t[FADT_P_LVL3_LAT] | t[FADT_P_LVL3_LAT + 1] << 8);
    }

    it = add(K86_PS_CPU, K86_PROBE_INFO, "Raw MSRs");
    VALUE(it, "198=%08x 19c=%08x", (uint32_t)perf, (uint32_t)therm);
    NOTE(it, "199=%04x 1a0=%08x cd=%s 1a2=%s", (uint32_t)ctl & 0xffff, (uint32_t)misc,
         fsb_ok ? "ok" : "fault", target_ok ? "ok" : "fault");
}

/* --- firmware: SMBIOS, ACPI tables ------------------------------------------- */

static void probe_firmware(void)
{
    const struct smbios_info *s = smbios();
    struct k86_probe_item *it;
    if (s->found) {
        it = add(K86_PS_FIRMWARE, K86_PROBE_INFO, "System");
        VALUE(it, "%s %s", s->sys_vendor, s->sys_product);
        NOTE(it, "version %s", s->sys_version[0] ? s->sys_version : "(none)");
        it = add(K86_PS_FIRMWARE, K86_PROBE_INFO, "BIOS");
        VALUE(it, "%s %s", s->bios_vendor, s->bios_version);
        NOTE(it, "dated %s. EC register maps are checked against this", s->bios_date);
        it = add(K86_PS_FIRMWARE, K86_PROBE_INFO, "Board");
        VALUE(it, "%s %s", s->board_vendor, s->board_product);
        it = add(K86_PS_FIRMWARE, K86_PROBE_INFO, "SMBIOS");
        VALUE(it, "%u.%u, %u structures", s->major, s->minor, s->structures);
        NOTE(it, "table at 0x%08x", s->table_addr);
    } else {
        it = add(K86_PS_FIRMWARE, K86_PROBE_MISSING, "SMBIOS");
        VALUE(it, "not found");
        NOTE(it, "no _SM_ or _DMI_ entry point in 0xF0000-0xFFFFF");
    }

    int rev;
    const char *oem = acpi_oem(&rev);
    f.sci = acpi_sci_enabled();
    it = add(K86_PS_FIRMWARE, acpi_table(0) ? K86_PROBE_INFO : K86_PROBE_MISSING, "ACPI");
    if (acpi_table(0))
        VALUE(it, "OEM %s, RSDP revision %u", oem, rev);
    else
        VALUE(it, "no tables found");
    it = add(K86_PS_FIRMWARE, K86_PROBE_INFO, "ACPI mode");
    VALUE(it, "%s", f.sci < 0 ? "no ACPI" : f.sci ? "on (the OS handles events)" : "off (legacy)");
    if (f.sci == 0)
        NOTE(it, "the firmware (SMM) still runs thermal events, the lid and Fn keys");

    for (int i = 0; acpi_table(i); i++) {
        const struct acpi_sdt *t = acpi_table(i);
        char sig[5], oem_t[9], size[16];
        memcpy(sig, t->sig, 4);
        sig[4] = '\0';
        memcpy(oem_t, t->oem_table, 8);
        oem_t[8] = '\0';
        for (int k = 7; k >= 0 && oem_t[k] == ' '; k--)
            oem_t[k] = '\0';
        size_text(size, sizeof(size), t->length);
        it = add(K86_PS_ACPI, K86_PROBE_INFO, sig);
        VALUE(it, "%s, revision %u", oem_t, t->revision);
        NOTE(it, "%s at 0x%08x", size, (uint32_t)t);
    }
}

/* What a _HID means, for the ones that matter here. */
static const char *hid_meaning(const char *hid)
{
    static const struct { const char *hid, *what; } known[] = {
        { "PNP0C09", "embedded controller" }, { "PNP0C0A", "battery" },
        { "ACPI0003", "AC adapter" },          { "PNP0C0D", "lid" },
        { "PNP0C0C", "power button" },         { "PNP0C0E", "sleep button" },
        { "ASUS010", "ASUS Eee PC hotkeys and radios" },
        { "ATK0100", "ASUS hotkeys (ATK)" },   { "PNP0C14", "WMI" },
        { "PNP0A03", "PCI root bridge" },      { "PNP0A08", "PCI Express root" },
        { "PNP0B00", "real-time clock" },      { "PNP0100", "timer (PIT)" },
        { "PNP0103", "HPET" },                 { "PNP0303", "PS/2 keyboard" },
        { "PNP0F13", "PS/2 mouse (touchpad)" }, { "PNP0F03", "PS/2 mouse (touchpad)" },
        { "PNP0800", "PC speaker" },           { "PNP0000", "interrupt controller" },
        { "PNP0200", "DMA controller" },       { "PNP0C04", "maths coprocessor" },
        { "PNP0C02", "motherboard resources" }, { "PNP0C01", "system board" },
        { "PNP0C0F", "PCI interrupt link" },   { "PNP0700", "floppy controller" },
        { "PNP0501", "serial port" },          { "PNP0400", "printer port" },
        { "ELAN0000", "ELAN touchpad" },       { "SYN1B7F", "Synaptics touchpad" },
    };
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (!strcmp(hid, known[i].hid))
            return known[i].what;
    return "";
}

/* A thermal zone's constant trip points (deci-Kelvin), as "_CRT 95 C". */
static void trip(char *out, int max, const struct aml_scope *z, const char *seg)
{
    uint32_t v;
    if (aml_const(z->start, z->end, seg, &v) || v < 2732 || v > 2732 + 1500)
        ksnprintf(out, (uint32_t)max, "%c%c%c%c ?", seg[0], seg[1], seg[2], seg[3]);
    else
        ksnprintf(out, (uint32_t)max, "%c%c%c%c %u C", seg[0], seg[1], seg[2], seg[3], (v - 2732) / 10);
}

static int is_aml(const struct acpi_sdt *t)
{
    return !memcmp(t->sig, "DSDT", 4) || !memcmp(t->sig, "SSDT", 4);
}

/* One table's devices, thermal zones and processors. */
static int aml_objects(const uint8_t *p, const uint8_t *end)
{
    struct k86_probe_item *it;
    char path[64];
    int n = aml_scopes(p, end, scopes, MAX_SCOPES), ndev = 0;
    if (n > MAX_SCOPES) {
        it = add(K86_PS_ACPI, K86_PROBE_INFO, "Scopes");
        VALUE(it, "%u declared in one table; the first %u listed", n, MAX_SCOPES);
        n = MAX_SCOPES;
    }
    /* Devices with an _HID, and the ones the monitor needs. */
    for (int i = 0; i < n; i++) {
        if (scopes[i].kind != AML_DEVICE || !scopes[i].hid[0])
            continue;
        ndev++;
        aml_path(scopes, n, i, path, sizeof(path));
        it = add(K86_PS_ACPI, K86_PROBE_INFO, scopes[i].hid);
        VALUE(it, "%s", path);
        NOTE(it, "%s", hid_meaning(scopes[i].hid));
        const char *h = scopes[i].hid;
        char *slot = !strcmp(h, "PNP0C0A") ? f.battery : !strcmp(h, "ACPI0003") ? f.ac
                   : !strcmp(h, "PNP0C0D") ? f.lid : !strcmp(h, "PNP0C09") ? f.ec_declared
                   : !strcmp(h, "ASUS010") || !strcmp(h, "ATK0100") ? f.asus : 0;
        if (slot && !slot[0])
            copy(slot, path, 40);
    }
    /* Thermal zones: their temperature (_TMP) is always a method, so only
     * the constant trip points can be read without running AML. */
    for (int i = 0; i < n; i++) {
        if (scopes[i].kind != AML_THERMAL)
            continue;
        f.nzones++;
        aml_path(scopes, n, i, path, sizeof(path));
        char crt[16], psv[16], hot[16];
        trip(crt, sizeof(crt), &scopes[i], "_CRT");
        trip(psv, sizeof(psv), &scopes[i], "_PSV");
        trip(hot, sizeof(hot), &scopes[i], "_HOT");
        it = add(K86_PS_ACPI, K86_PROBE_PARTIAL, "Thermal zone");
        VALUE(it, "%s", path);
        NOTE(it, "%s, %s, %s; its _TMP is a method: read through the EC", crt, psv, hot);
    }
    for (int i = 0; i < n; i++) {
        if (scopes[i].kind != AML_PROCESSOR)
            continue;
        const uint8_t *b = scopes[i].start;
        aml_path(scopes, n, i, path, sizeof(path));
        it = add(K86_PS_ACPI, K86_PROBE_INFO, "Processor");
        VALUE(it, "%s", path);
        if (b + 6 <= scopes[i].end)
            NOTE(it, "ID %u, P_BLK 0x%x (%u bytes: throttling and C-states)", b[0],
                 b[1] | b[2] << 8 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 24, b[5]);
    }
    return ndev;
}

/* The DSDT and every SSDT: devices, zones, P-states, regions. */
static void probe_aml(void)
{
    struct k86_probe_item *it;
    int ntables = 0, ndev = 0;
    struct aml_pstate ps[8];
    char psname[5];
    for (int ti = 0; acpi_table(ti); ti++) {
        const struct acpi_sdt *t = acpi_table(ti);
        if (!is_aml(t))
            continue;
        ntables++;
        const uint8_t *p, *end;
        acpi_body(t, &p, &end);
        ndev += aml_objects(p, end);
        if (!f.npstates && (f.npstates = aml_pstates(p, end, psname, ps, 8)))
            for (int i = 0; i < f.npstates && i < 8; i++) {
                it = add(K86_PS_CPU, K86_PROBE_OK, "P-state");
                VALUE(it, "%u MHz, %u mW", ps[i].mhz, ps[i].mw);
                NOTE(it, "%s[%u] in the %c%c%c%c: control 0x%x, status 0x%x", psname, i, t->sig[0],
                     t->sig[1], t->sig[2], t->sig[3], ps[i].control, ps[i].status);
            }
        /* Operation regions: where the firmware's fields live. */
        int nr = aml_regions(p, end, regions, MAX_REGIONS);
        for (int i = 0; i < nr && i < MAX_REGIONS; i++) {
            const struct aml_region *r = &regions[i];
            it = add(K86_PS_ACPI, K86_PROBE_INFO, "Region");
            VALUE(it, "%s, %s", r->name, aml_space_name(r->space));
            if (r->dynamic)
                NOTE(it, "address computed at run time");
            else
                NOTE(it, "0x%x-0x%x (%u bytes)", r->offset, r->offset + (r->len ? r->len - 1 : 0), r->len);
        }
    }
    if (!ntables) {
        it = add(K86_PS_ACPI, K86_PROBE_MISSING, "DSDT");
        VALUE(it, "none: no devices, regions or fields to list");
    } else if (!ndev) {
        it = add(K86_PS_ACPI, K86_PROBE_INFO, "Devices");
        VALUE(it, "none with an _HID found");
    }
    if (!f.npstates) {
        it = add(K86_PS_CPU, K86_PROBE_PARTIAL, "P-states");
        VALUE(it, "no constant table in the static ACPI tables");
        NOTE(it, "Intel firmware often loads them at run time; the MSRs still work");
    }
}

/* --- the embedded controller ------------------------------------------------------- */

/* The fields the tables lay over EC RAM (EmbeddedControl regions), into
 * fields[], their bits counted from EC address 0. */
static int ec_fields(void)
{
    struct k86_probe_item *it;
    int nf = 0;
    for (int ti = 0; acpi_table(ti); ti++) {
        const struct acpi_sdt *t = acpi_table(ti);
        if (!is_aml(t))
            continue;
        const uint8_t *p, *end;
        acpi_body(t, &p, &end);
        int nr = aml_regions(p, end, regions, MAX_REGIONS);
        for (int i = 0; i < nr && i < MAX_REGIONS; i++) {
            if (regions[i].space != 3)
                continue;
            it = add(K86_PS_EC, K86_PROBE_INFO, "EC region");
            VALUE(it, "%s", regions[i].name);
            if (regions[i].dynamic)
                NOTE(it, "computed at run time");
            else
                NOTE(it, "0x%x-0x%x", regions[i].offset, regions[i].offset + regions[i].len - 1);
            int room = nf < MAX_FIELDS ? MAX_FIELDS - nf : 0;
            int got = aml_fields(p, end, regions[i].name, fields + (MAX_FIELDS - room), room);
            for (int k = nf; k < nf + got && k < MAX_FIELDS; k++)
                fields[k].bit += regions[i].dynamic ? 0 : regions[i].offset * 8;
            nf += got;
        }
    }
    return nf;
}

static void probe_ec(uint32_t flags)
{
    struct k86_probe_item *it;
    device_t *dev = device_find("ec0");
    struct ec_info info;
    f.ec = dev && dev->state == DEVICE_BOUND && !dev_ioctl(dev, EC_GET_INFO, &info);
    it = add(K86_PS_EC, f.ec ? K86_PROBE_OK : f.ec_declared[0] ? K86_PROBE_ERROR : K86_PROBE_MISSING,
             "Embedded controller");
    if (f.ec) {
        static const char *const from[] = { "from the ECDT", "from its _CRS", "assumed: the standard ones" };
        VALUE(it, "%s", info.path);
        NOTE(it, "ports 0x%x (data), 0x%x (command), %s", info.data_port, info.cmd_port,
             from[info.ports_from]);
        uint8_t st = inb(info.cmd_port);
        it = add(K86_PS_EC, K86_PROBE_INFO, "Status register");
        VALUE(it, "0x%02x", st);
        NOTE(it, "%s%s%s%s", st & 1 ? "output full, " : "", st & 2 ? "input full, " : "",
             st & 0x10 ? "burst, " : "", st & 0x20 ? "event pending (handled by the firmware)" : "idle");
        it = add(K86_PS_EC, K86_PROBE_INFO, "Writes");
        VALUE(it, "none: this driver can only read");
        NOTE(it, "fan control writes come later, behind a verified map and a warning");
    } else if (f.ec_declared[0]) {
        VALUE(it, "%s", f.ec_declared);
        NOTE(it, "declared, but nothing answers at its ports (a VM has no EC)");
    } else {
        VALUE(it, "not found");
        NOTE(it, "no PNP0C09 device or ECDT in the ACPI tables");
    }

    int nf = ec_fields();
    f.ec_fields = nf;
    if (f.ec || f.ec_declared[0]) {
        it = add(K86_PS_EC, nf ? K86_PROBE_OK : K86_PROBE_PARTIAL, "Named fields");
        VALUE(it, "%u", nf);
        NOTE(it, "%s", nf ? "listed under EC fields: names the firmware gives EC bytes"
                          : "the tables name no EC bytes: the map must come from reading it");
    }

    if (f.ec && (flags & K86_PROBE_EC_RAM)) {
        int err = dev_ioctl(dev, EC_READ_ALL, ec_ram);
        f.ec_read = !err;
        if (err) {
            it = add(K86_PS_EC, K86_PROBE_ERROR, "EC RAM");
            VALUE(it, "reading failed (%d)", err);
            NOTE(it, "the EC didn't answer in time; the firmware may be busy with it");
        } else {
            for (int row = 0; row < 16; row++) {
                char name[8];
                ksnprintf(name, sizeof(name), "0x%02x", row * 16);
                it = add(K86_PS_EC, K86_PROBE_INFO, name);
                const uint8_t *b = &ec_ram[row * 16];
                VALUE(it, "%02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x",
                      b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12],
                      b[13], b[14], b[15]);
            }
        }
    } else if (f.ec) {
        it = add(K86_PS_EC, K86_PROBE_INFO, "EC RAM");
        VALUE(it, "not read");
        NOTE(it, "Read EC RAM in the header bar reads all 256 bytes (no writes)");
    }

    /* Each field, with its value now if the RAM was read. Names that look
     * like the fan's go into the summary. */
    int fan = 0;
    f.fan_fields[0] = '\0';
    for (int k = 0; k < nf && k < MAX_FIELDS; k++) {
        const struct aml_field *fl = &fields[k];
        uint32_t byte = fl->bit / 8, bit = fl->bit % 8;
        it = add(K86_PS_EC_FIELDS, K86_PROBE_INFO, fl->name);
        if (!bit && !(fl->width % 8))
            VALUE(it, "0x%02x, %u byte%s", byte, fl->width / 8, fl->width == 8 ? "" : "s");
        else
            VALUE(it, "0x%02x bit %u, %u bit%s", byte, bit, fl->width, fl->width == 1 ? "" : "s");
        if (f.ec_read && byte < 256 && bit + fl->width <= 16) {
            uint32_t raw = ec_ram[byte] | (byte + 1 < 256 ? ec_ram[byte + 1] << 8 : 0);
            uint32_t v = raw >> bit & ((1u << fl->width) - 1);
            NOTE(it, "%s; now 0x%x (%u)", fl->region, v, v);
        } else {
            NOTE(it, "%s", fl->region);
        }
        const char *nm = fl->name;
        int fanlike = !memcmp(nm, "FAN", 3) || !memcmp(nm + 1, "FAN", 3) || !memcmp(nm, "RPM", 3) ||
                      !memcmp(nm + 1, "RPM", 3) || !memcmp(nm, "PWM", 3) || !memcmp(nm, "FS", 2);
        if (fanlike && fan < 6) {
            int o = (int)strlen(f.fan_fields);
            ksnprintf(f.fan_fields + o, sizeof(f.fan_fields) - (uint32_t)o, "%s%s", fan ? " " : "", nm);
            fan++;
        }
    }
}

/* --- disk ----------------------------------------------------------------------------- */

static const char *smart_name(uint8_t id)
{
    switch (id) {
    case 1:    return "read error rate";
    case 5:    return "reallocated sectors";
    case 9:    return "power-on hours";
    case 12:   return "power cycles";
    case 190:  return "airflow temperature";
    case 191:  return "G-sense errors";
    case 193:  return "load cycles";
    case 194:  return "temperature";
    case 197:  return "pending sectors";
    case 198:  return "uncorrectable sectors";
    case 199:  return "CRC errors";
    default:   return "";
    }
}

static void probe_disk(void)
{
    struct k86_probe_item *it;
    struct blk_info bi;
    device_t *boot = device_find("boot0");
    if (boot && !dev_ioctl(boot, BLK_GET_INFO, &bi)) {
        it = add(K86_PS_DISK, K86_PROBE_INFO, "Boot disk");
        VALUE(it, "%s, %u MiB", bi.model, bi.sectors / 2048);
        NOTE(it, "reached through the BIOS: no SMART there");
    }
    device_t *ata = device_find("ata0");
    f.disk = ata && ata->state == DEVICE_BOUND && !dev_ioctl(ata, BLK_GET_INFO, &bi);
    it = add(K86_PS_DISK, f.disk ? K86_PROBE_OK : K86_PROBE_MISSING, "Internal disk");
    if (!f.disk) {
        VALUE(it, "none found");
        NOTE(it, "no ATA disk besides the one we booted from");
        return;
    }
    VALUE(it, "%s, %u MiB", bi.model, bi.sectors / 2048);
    NOTE(it, "ATA (IDE mode), read-only");

    static struct blk_smart s;
    int err = dev_ioctl(ata, BLK_SMART, &s);
    it = add(K86_PS_DISK, !err ? K86_PROBE_OK : s.supported ? K86_PROBE_PARTIAL : K86_PROBE_MISSING, "SMART");
    VALUE(it, "%s", !err ? "on, read" : !s.supported ? "not supported" : !s.enabled ? "switched off"
                                                     : "reading failed");
    if (err && s.supported && !s.enabled)
        NOTE(it, "left off: switching it on would change the drive's settings");
    f.smart = err ? (s.supported ? 1 : 0) : 2;
    if (err)
        return;
    uint8_t sum = 0;
    for (int i = 0; i < BLK_SECTOR; i++)
        sum = (uint8_t)(sum + s.data[i]);
    if (sum)
        NOTE(it, "checksum doesn't add up: values may be wrong");
    for (int a = 0; a < 30; a++) {
        const uint8_t *e = &s.data[2 + a * 12];
        if (!e[0])
            continue;
        uint32_t raw = e[5] | e[6] << 8 | (uint32_t)e[7] << 16 | (uint32_t)e[8] << 24;
        if ((e[0] == 194 || (e[0] == 190 && f.smart < 3)) && e[5] > 0 && e[5] < 90) {
            f.smart = 3;
            f.disk_temp = e[5];
        }
        const char *nm = smart_name(e[0]);
        if (!*nm)
            continue;
        char name[28];
        ksnprintf(name, sizeof(name), "SMART %u", e[0]);
        it = add(K86_PS_DISK, K86_PROBE_INFO, name);
        if (e[0] == 194 || e[0] == 190)
            VALUE(it, "%u C", e[5]);
        else
            VALUE(it, "%u", raw);
        NOTE(it, "%s (normalised %u, worst %u)", nm, e[3], e[4]);
    }
}

/* --- power and other devices --------------------------------------------------------------- */

static const char *pci_known(uint16_t ven, uint16_t dev)
{
    static const struct { uint16_t ven, dev; const char *name; } known[] = {
        { 0x8086, 0x27ac, "945GSE memory controller" }, { 0x8086, 0x27ae, "GMA 950 graphics" },
        { 0x8086, 0x27a6, "GMA 950 (second head)" },    { 0x8086, 0x27a2, "GMA 950 graphics" },
        { 0x8086, 0x27b9, "ICH7-M LPC bridge" },         { 0x8086, 0x27bc, "NM10 LPC bridge" },
        { 0x8086, 0x27c4, "ICH7-M SATA (IDE mode)" },    { 0x8086, 0x27c8, "USB 1.1 (UHCI)" },
        { 0x8086, 0x27c9, "USB 1.1 (UHCI)" },            { 0x8086, 0x27ca, "USB 1.1 (UHCI)" },
        { 0x8086, 0x27cb, "USB 1.1 (UHCI)" },            { 0x8086, 0x27cc, "USB 2.0 (EHCI)" },
        { 0x8086, 0x27d8, "HD Audio" },                  { 0x8086, 0x27d0, "PCIe port 1" },
        { 0x8086, 0x27d2, "PCIe port 2" },               { 0x8086, 0x27d4, "PCIe port 3" },
        { 0x8086, 0x27d6, "PCIe port 4" },               { 0x8086, 0x2448, "PCI bridge (DMI)" },
        { 0x8086, 0x27da, "SMBus" },                     { 0x1969, 0x1026, "Atheros AR8121 Ethernet" },
        { 0x1969, 0x1062, "Atheros AR8132 Ethernet" },   { 0x1969, 0x2062, "Atheros AR8152 Ethernet" },
        { 0x168c, 0x002a, "Atheros AR928X Wi-Fi" },      { 0x168c, 0x002b, "Atheros AR9285 Wi-Fi" },
        { 0x168c, 0x001c, "Atheros AR242x Wi-Fi" },      { 0x10ec, 0x8192, "Realtek RTL8192 Wi-Fi" },
    };
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (known[i].ven == ven && known[i].dev == dev)
            return known[i].name;
    return 0;
}

static void probe_devices(void)
{
    struct k86_probe_item *it;
    for (uint32_t i = 0; i < pci_device_count; i++) {
        const struct pci_device *d = &pci_devices[i];
        char name[12];
        ksnprintf(name, sizeof(name), "%02x:%02x.%u", d->bus, d->dev, d->func);
        const char *known = pci_known(d->vendor_id, d->device_id);
        const char *cls = pci_class_name(d->class_code, d->subclass);
        int net = d->class_code == 0x02, wifi = net && d->subclass == 0x80;
        f.nets += net && !wifi;
        f.wifi += wifi;
        const char *driver = net ? "no driver (networking: Phase 5)"
                           : d->class_code == 0x04 ? "no driver (audio: Phase 5)"
                           : d->class_code == 0x0c && d->subclass == 0x03 ? "no driver (USB: Phase 5)"
                           : d->class_code == 0x01 ? "ata-pio (read-only)"
                           : d->class_code == 0x03 ? "VBE framebuffer (no native driver)" : "";
        it = add(K86_PS_DEVICES, net ? K86_PROBE_MISSING : K86_PROBE_INFO, name);
        VALUE(it, "%04x:%04x %s", d->vendor_id, d->device_id, known ? known : cls);
        NOTE(it, "%s%s%s", known ? cls : "", known && *driver ? "; " : "", driver);
        if (d->vendor_id == 0x8086 && d->class_code == 0x03 && d->dev == 2 && d->func == 0) {
            f.gma = 1;
            f.lbb = (int)(pci_read32(d->bus, d->dev, d->func, 0xf4) & 0xff);
        }
    }
    it = add(K86_PS_DEVICES, K86_PROBE_MISSING, "USB devices");
    VALUE(it, "not visible");
    NOTE(it, "webcam, Bluetooth and the card reader sit on USB: no USB stack yet");
}

static void probe_power(void)
{
    struct k86_probe_item *it;
    it = add(K86_PS_POWER, f.battery[0] ? K86_PROBE_PARTIAL : K86_PROBE_MISSING, "Battery");
    VALUE(it, "%s", f.battery[0] ? f.battery : "no PNP0C0A device declared");
    if (f.battery[0])
        NOTE(it, "its _BIF/_BST are AML methods: read through the EC fields instead");
    it = add(K86_PS_POWER, f.ac[0] ? K86_PROBE_PARTIAL : K86_PROBE_MISSING, "AC adapter");
    VALUE(it, "%s", f.ac[0] ? f.ac : "no ACPI0003 device declared");
    if (f.ac[0])
        NOTE(it, "plugged in or not: an EC bit, once the map is known");
    it = add(K86_PS_POWER, f.lid[0] ? K86_PROBE_INFO : K86_PROBE_MISSING, "Lid");
    VALUE(it, "%s", f.lid[0] ? f.lid : "none declared");
    it = add(K86_PS_POWER, f.asus[0] ? K86_PROBE_PARTIAL : K86_PROBE_MISSING, "ASUS controls");
    VALUE(it, "%s", f.asus[0] ? f.asus : "no ASUS010/ATK0100 device");
    NOTE(it, "%s", f.asus[0] ? "radios, camera, card reader: ASUS methods (AML) or EC bits"
                             : "the Wi-Fi/camera/card reader switches live there on Eee PCs");
    it = add(K86_PS_POWER, f.gma ? K86_PROBE_PARTIAL : K86_PROBE_MISSING, "Backlight");
    if (f.gma) {
        VALUE(it, "LBB register 0x%02x", f.lbb);
        NOTE(it, "the GMA's legacy brightness (PCI config 0xF4, 0-255); changing it: later");
    } else {
        VALUE(it, "no Intel GMA graphics");
        NOTE(it, "the EeePC's brightness register is on its GMA 950");
    }
    uint32_t total = pmm_total_frames() / 256, free = pmm_free_frames() / 256;
    it = add(K86_PS_POWER, K86_PROBE_INFO, "Memory");
    VALUE(it, "%u MiB managed, %u MiB free", total, free);
    NOTE(it, "no swap: LiteKern X doesn't page to disk");
}

/* --- the summary ----------------------------------------------------------------------------- */

static void summary(void)
{
    struct k86_probe_item *it;
#define ROW(st, nm) (it = add(K86_PS_SUMMARY, (st), (nm)))

    ROW(f.dts_ok ? K86_PROBE_OK : f.dts ? K86_PROBE_ERROR : K86_PROBE_MISSING, "CPU temperature");
    if (f.dts_ok) {
        VALUE(it, "%d C", f.temp_c);
        NOTE(it, "the CPU's own sensor; TjMax %u C%s", f.tjmax, f.tjmax_assumed ? " (assumed)" : "");
    } else {
        VALUE(it, "%s", f.dts ? "sensor gives no reading" : "no sensor reported");
        NOTE(it, "fan control stays off without it");
    }

    ROW(f.perf_ok ? K86_PROBE_OK : K86_PROBE_MISSING, "Clock and SpeedStep");
    if (f.perf_ok) {
        VALUE(it, "%u MHz now%s", f.mhz, f.est_on ? "" : " (SpeedStep off)");
        NOTE(it, "%u P-state%s known; switching: power management", f.npstates, f.npstates == 1 ? "" : "s");
    } else {
        VALUE(it, "TSC %u MHz only", tsc_mhz());
        NOTE(it, "no SpeedStep reported: the clock can't be read or set");
    }

    ROW(f.therm_ok ? K86_PROBE_OK : K86_PROBE_MISSING, "Throttling detection");
    VALUE(it, "%s", f.therm_ok ? (f.prochot_now ? "throttling now" : "not throttling") : "not available");
    NOTE(it, "%s", f.therm_ok ? "PROCHOT status, plus the clock against its highest" : "needs IA32_THERM_STATUS");

    ROW(f.ec ? K86_PROBE_PARTIAL : K86_PROBE_MISSING, "Fan speed");
    if (!f.ec) {
        VALUE(it, "no embedded controller");
        NOTE(it, "the fan is run by the EC: without one, nothing can be read");
    } else if (f.fan_fields[0]) {
        VALUE(it, "fields named like it: %s", f.fan_fields);
        NOTE(it, "to confirm: read EC RAM idle, then under load, and compare");
    } else {
        VALUE(it, "EC found; which bytes hold it isn't known");
        NOTE(it, "to find out: read EC RAM idle, then under load, and compare");
    }

    ROW(f.ec ? K86_PROBE_PARTIAL : K86_PROBE_MISSING, "Fan control");
    VALUE(it, "%s", f.ec ? "locked: monitoring only" : "not possible");
    NOTE(it, "%s", f.ec ? "writes need a register map verified for this BIOS, then your OK"
                        : "needs the embedded controller");

    ROW(f.nzones ? K86_PROBE_PARTIAL : K86_PROBE_MISSING, "Other temperatures");
    VALUE(it, "%u ACPI thermal zone%s", f.nzones, f.nzones == 1 ? "" : "s");
    NOTE(it, "%s", f.nzones ? "readable once their EC bytes are known" : "the firmware declares none");

    ROW(f.smart == 3 ? K86_PROBE_OK : f.disk ? K86_PROBE_PARTIAL : K86_PROBE_MISSING, "Drive temperature");
    if (f.smart == 3)
        VALUE(it, "%u C (SMART)", f.disk_temp);
    else
        VALUE(it, "%s", !f.disk ? "no internal disk" : f.smart == 2 ? "SMART has no temperature"
                      : f.smart == 1 ? "SMART is off" : "no SMART");

    ROW(f.battery[0] && f.ec ? K86_PROBE_PARTIAL : K86_PROBE_MISSING, "Battery");
    VALUE(it, "%s", f.battery[0] ? (f.ec ? "declared; values from EC fields next" : "declared, no EC")
                                 : "none declared");
    NOTE(it, "charge, design vs full capacity, cycles, rate, time left");

    ROW(f.ac[0] && f.ec ? K86_PROBE_PARTIAL : K86_PROBE_MISSING, "AC or battery");
    VALUE(it, "%s", f.ac[0] ? "adapter declared" : "no adapter declared");
    NOTE(it, "switches profiles when it changes");

    ROW(K86_PROBE_OK, "CPU usage");
    VALUE(it, "kernel idle-time accounting");
    NOTE(it, "time halted vs busy; added with monitoring");

    ROW(K86_PROBE_OK, "Memory usage");
    VALUE(it, "%u MiB free of %u", pmm_free_frames() / 256, pmm_total_frames() / 256);

    ROW(K86_PROBE_MISSING, "Swap");
    VALUE(it, "none");
    NOTE(it, "LiteKern X doesn't swap (docs/NON-GOALS.md)");

    ROW(K86_PROBE_OK, "Disk throughput");
    VALUE(it, "kernel sector counts");
    NOTE(it, "every block read and write, counted per disk");

    ROW(K86_PROBE_MISSING, "Network throughput");
    VALUE(it, "%u Ethernet, %u Wi-Fi: no drivers", f.nets, f.wifi);
    NOTE(it, "networking is Phase 5");

    ROW(K86_PROBE_OK, "Per-app CPU and memory");
    VALUE(it, "kernel accounting per app");
    NOTE(it, "apps are the only processes LiteKern X has");

    ROW(f.gma ? K86_PROBE_PARTIAL : K86_PROBE_MISSING, "Backlight");
    VALUE(it, "%s", f.gma ? "GMA register readable" : "no Intel GMA");

    ROW(f.asus[0] ? K86_PROBE_PARTIAL : K86_PROBE_MISSING, "Radio and device switches");
    VALUE(it, "%s", f.asus[0] ? "ASUS device found" : "none found");
    NOTE(it, "Wi-Fi, Bluetooth, webcam, card reader");

    ROW(K86_PROBE_OK, "Alert beep");
    VALUE(it, "PC speaker");
    NOTE(it, "PIT channel 2 and port 0x61");
#undef ROW
}

int hwprobe(struct k86_probe_item *out, int max, uint32_t flags)
{
    uint32_t t0 = uptime_ms();
    items = out;
    maxitems = max;
    nitems = 0;
    memset(&f, 0, sizeof(f));
    probe_cpu();
    probe_firmware();
    probe_aml();
    probe_ec(flags);
    probe_disk();
    probe_devices();
    probe_power();
    int first = nitems;
    summary();
    static const char *const word[] = { "info", "ok", "partial", "missing", "error" };
    if (!logged) {
        /* The summary to the log (the Log app shows it on the EeePC); every
         * detail to serial only, for the tests and a serial console. */
        static const char *const sec[] = { "summary", "cpu", "firmware", "acpi", "ec", "ec-field",
                                           "disk", "power", "devices" };
        logged = 1;
        for (int i = first; i < nitems && i < max; i++)
            kprintf("probe: %s: %s [%s]\n", out[i].name, out[i].value, word[out[i].status]);
        for (int i = 0; i < first && i < max; i++)
            kdebugf("probe: %s: %s: %s [%s] %s\n", sec[out[i].section], out[i].name, out[i].value,
                    word[out[i].status], out[i].note);
    }
    kprintf("probe: %u items in %u ms%s\n", nitems, uptime_ms() - t0,
            !(flags & K86_PROBE_EC_RAM) ? "" : f.ec_read ? "; EC RAM read" : "; EC RAM not read");
    return nitems;
}
