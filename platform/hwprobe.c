/* LiteKern X on Linux — the hardware probe (kernel/hwprobe.h), from what Linux
 * shows in /proc and /sys instead of the hardware itself: the same sections
 * and kinds of items, so the Hardware Probe app shows them as it does on the
 * kernel version. Only ever reads. */
#include "kernel/hwprobe.h"

/* Long names are cut to fit the items' fields, as the kernel's probe does. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

static struct k86_probe_item *items, spare;
static int nitems, maxitems;

static struct k86_probe_item *add(int section, int status, const char *name)
{
    struct k86_probe_item *it = nitems < maxitems ? &items[nitems] : &spare;
    nitems++;
    memset(it, 0, sizeof(*it));
    it->section = (uint8_t)section;
    it->status = (uint8_t)status;
    snprintf(it->name, sizeof(it->name), "%s", name);
    return it;
}

#define VALUE(it, ...) snprintf((it)->value, sizeof((it)->value), __VA_ARGS__)
#define NOTE(it, ...)  snprintf((it)->note, sizeof((it)->note), __VA_ARGS__)

/* The first line of a small file, without its newline; "" if unreadable. */
static const char *line(const char *fmt, ...)
{
    static char buf[4][256];
    static int k;
    char path[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(path, sizeof(path), fmt, ap);
    va_end(ap);
    char *b = buf[k++ % 4];
    b[0] = '\0';
    FILE *f = fopen(path, "r");
    if (f) {
        if (!fgets(b, 256, f))
            b[0] = '\0';
        fclose(f);
    }
    b[strcspn(b, "\n")] = '\0';
    return b;
}

/* A "key : value" line from /proc/cpuinfo (the first CPU's). */
static int cpuinfo(const char *key, char *out, int max)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    char l[1024];
    int found = 0;
    size_t n = strlen(key);
    while (f && !found && fgets(l, sizeof(l), f)) {
        if (strncmp(l, key, n) || (l[n] != ' ' && l[n] != '\t' && l[n] != ':'))
            continue;
        const char *v = strchr(l, ':');
        if (!v)
            continue;
        v++;
        while (*v == ' ')
            v++;
        snprintf(out, (size_t)max, "%s", v);
        out[strcspn(out, "\n")] = '\0';
        found = 1;
    }
    if (f)
        fclose(f);
    return found;
}

static void probe_summary(void)
{
    struct utsname u;
    struct sysinfo si;
    char model[128];
    struct k86_probe_item *it = add(K86_PS_SUMMARY, K86_PROBE_INFO, "Machine");
    const char *vendor = line("/sys/class/dmi/id/sys_vendor"), *product = line("/sys/class/dmi/id/product_name");
    VALUE(it, "%s %s", *vendor ? vendor : "unknown", product);
    it = add(K86_PS_SUMMARY, K86_PROBE_INFO, "Running on");
    if (!uname(&u))
        VALUE(it, "Linux %s (%s)", u.release, u.machine);
    NOTE(it, "LiteKern X's desktop, on the Linux kernel");
    if (cpuinfo("model name", model, sizeof(model))) {
        it = add(K86_PS_SUMMARY, K86_PROBE_INFO, "Processor");
        VALUE(it, "%s", model);
    }
    if (!sysinfo(&si)) {
        it = add(K86_PS_SUMMARY, K86_PROBE_INFO, "Memory");
        VALUE(it, "%u MB, %u MB free", (unsigned)((uint64_t)si.totalram * si.mem_unit >> 20),
              (unsigned)((uint64_t)si.freeram * si.mem_unit >> 20));
    }
}

static void probe_cpu(void)
{
    char v[256];
    struct k86_probe_item *it;
    if (cpuinfo("model name", v, sizeof(v))) {
        it = add(K86_PS_CPU, K86_PROBE_OK, "Model");
        VALUE(it, "%s", v);
    }
    it = add(K86_PS_CPU, K86_PROBE_INFO, "Cores Linux uses");
    VALUE(it, "%ld", sysconf(_SC_NPROCESSORS_ONLN));
    if (cpuinfo("cpu MHz", v, sizeof(v))) {
        it = add(K86_PS_CPU, K86_PROBE_OK, "Clock now");
        VALUE(it, "%d MHz", atoi(v));
    }
    if (cpuinfo("flags", v, sizeof(v))) {
        it = add(K86_PS_CPU, K86_PROBE_INFO, "64-bit");
        VALUE(it, "%s", strstr(v, " lm ") ? "yes" : "no (32-bit only)");
        it = add(K86_PS_CPU, K86_PROBE_INFO, "SpeedStep");
        VALUE(it, "%s", strstr(v, " est ") ? "yes" : "no");
    }
    /* Temperatures: Linux's thermal zones (on the EeePC, the CPU's). */
    DIR *d = opendir("/sys/class/thermal");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "thermal_zone", 12))
            continue;
        const char *t = line("/sys/class/thermal/%s/temp", e->d_name);
        if (!*t)
            continue;
        it = add(K86_PS_CPU, K86_PROBE_OK, "Temperature");
        VALUE(it, "%d C", atoi(t) / 1000);
        NOTE(it, "%s (%s)", line("/sys/class/thermal/%s/type", e->d_name), e->d_name);
    }
    if (d)
        closedir(d);
}

static void probe_firmware(void)
{
    static const char *const fields[][2] = {
        { "BIOS vendor", "bios_vendor" }, { "BIOS version", "bios_version" },
        { "BIOS date", "bios_date" }, { "Board", "board_name" }, { "Board maker", "board_vendor" },
    };
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        const char *v = line("/sys/class/dmi/id/%s", fields[i][1]);
        struct k86_probe_item *it = add(K86_PS_FIRMWARE, *v ? K86_PROBE_OK : K86_PROBE_MISSING, fields[i][0]);
        VALUE(it, "%s", *v ? v : "not shown");
    }
}

static void probe_acpi(void)
{
    DIR *d = opendir("/sys/firmware/acpi/tables");
    int n = 0;
    char names[56] = "";
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "data") || !strcmp(e->d_name, "dynamic"))
            continue;
        if (n++ < 8 && strlen(names) + strlen(e->d_name) + 2 < sizeof(names))
            strcat(strcat(names, *names ? " " : ""), e->d_name);
    }
    if (d)
        closedir(d);
    struct k86_probe_item *it = add(K86_PS_ACPI, n ? K86_PROBE_OK : K86_PROBE_MISSING, "Tables");
    if (n)
        VALUE(it, "%d: %s%s", n, names, n > 8 ? " ..." : "");
    else
        VALUE(it, "none shown");
    NOTE(it, "Linux reads ACPI itself (the kernel version's own reader isn't needed)");
}

static void probe_ec(void)
{
    struct k86_probe_item *it = add(K86_PS_EC, K86_PROBE_PARTIAL, "Embedded controller");
    VALUE(it, "Linux's driver has it");
    NOTE(it, "Its fan and temperature fields: phase L8 (the monitor, on Linux's hwmon)");
}

static void probe_disk(void)
{
    DIR *d = opendir("/sys/block");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' || !strncmp(e->d_name, "loop", 4) || !strncmp(e->d_name, "ram", 3) ||
            !strncmp(e->d_name, "zram", 4))
            continue;
        uint64_t sectors = strtoull(line("/sys/block/%s/size", e->d_name), 0, 10);
        struct k86_probe_item *it = add(K86_PS_DISK, K86_PROBE_OK, e->d_name);
        const char *model = line("/sys/block/%s/device/model", e->d_name);
        VALUE(it, "%llu MB%s%s", (unsigned long long)(sectors / 2048), *model ? ", " : "", model);
        NOTE(it, "%s", atoi(line("/sys/block/%s/removable", e->d_name)) ? "removable" : "fixed");
    }
    if (d)
        closedir(d);
}

static void probe_power(void)
{
    DIR *d = opendir("/sys/class/power_supply");
    struct dirent *e;
    int n = 0;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        n++;
        const char *type = line("/sys/class/power_supply/%s/type", e->d_name);
        struct k86_probe_item *it = add(K86_PS_POWER, K86_PROBE_OK, e->d_name);
        if (!strcmp(type, "Battery"))
            VALUE(it, "battery: %s%%, %s", line("/sys/class/power_supply/%s/capacity", e->d_name),
                  line("/sys/class/power_supply/%s/status", e->d_name));
        else
            VALUE(it, "%s: %s", type, atoi(line("/sys/class/power_supply/%s/online", e->d_name)) ? "plugged in" : "not plugged in");
    }
    if (d)
        closedir(d);
    if (!n) {
        struct k86_probe_item *it = add(K86_PS_POWER, K86_PROBE_MISSING, "Battery and charger");
        VALUE(it, "none shown");
    }
}

static const char *pci_class(unsigned cls)
{
    switch (cls >> 8) {
    case 0x0101: return "IDE controller";
    case 0x0106: return "SATA controller";
    case 0x0200: return "Ethernet";
    case 0x0280: return "Wi-Fi / network";
    case 0x0300: return "Display";
    case 0x0403: return "Audio";
    case 0x0600: return "Host bridge";
    case 0x0601: return "ISA bridge";
    case 0x0604: return "PCI bridge";
    case 0x0c03: return "USB controller";
    case 0x0c05: return "SMBus";
    default:     return "device";
    }
}

static void probe_devices(void)
{
    DIR *d = opendir("/sys/bus/pci/devices");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        unsigned vendor = (unsigned)strtoul(line("/sys/bus/pci/devices/%s/vendor", e->d_name), 0, 16);
        unsigned device = (unsigned)strtoul(line("/sys/bus/pci/devices/%s/device", e->d_name), 0, 16);
        unsigned cls = (unsigned)strtoul(line("/sys/bus/pci/devices/%s/class", e->d_name), 0, 16);
        char drv[256] = "";
        char link[512];
        snprintf(link, sizeof(link), "/sys/bus/pci/devices/%s/driver", e->d_name);
        ssize_t k = readlink(link, drv, sizeof(drv) - 1);
        drv[k > 0 ? k : 0] = '\0';
        const char *name = strrchr(drv, '/');
        struct k86_probe_item *it = add(K86_PS_DEVICES, k > 0 ? K86_PROBE_OK : K86_PROBE_PARTIAL, pci_class(cls));
        VALUE(it, "%04x:%04x at %s", vendor, device, e->d_name + 5);
        NOTE(it, "%s", k > 0 ? (name ? name + 1 : drv) : "no Linux driver bound");
    }
    if (d)
        closedir(d);
}

int hwprobe(struct k86_probe_item *out, int max, uint32_t flags)
{
    (void)flags;
    items = out;
    maxitems = max;
    nitems = 0;
    probe_summary();
    probe_cpu();
    probe_firmware();
    probe_acpi();
    probe_ec();
    probe_disk();
    probe_power();
    probe_devices();
    return nitems;
}
