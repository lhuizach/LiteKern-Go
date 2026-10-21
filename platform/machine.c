/* LiteKern X on Linux — the machine (kernel/machine.h), phase L4: what Linux's
 * drivers know about the EeePC's battery and backlight, and the time zone.
 *
 *   battery     /sys/class/power_supply: the one whose type is Battery (the
 *               EeePC's, through ACPI): how full, charging or not, and how
 *               long until empty or full when it says how fast it's going
 *   brightness  /sys/class/backlight: the firmware's (ACPI video) first, then
 *               the EeePC's own (eeepc-laptop), then the graphics chip's; never
 *               below 5% (0 can switch the panel right off). The Fn+F5/F6
 *               keys are the EeePC's own business: its firmware changes the
 *               brightness itself
 *   time zone   the TZ rule (kernel/timezones.h) for this program and the
 *               ones it starts. The EeePC's clock keeps local time, as its
 *               BIOS shows it: read in the zone at start (hwclock -s -l) and
 *               written back when the zone changes (net.c writes it back
 *               after the internet's time too)
 *   touchpad    its speed (platform/drm.c moves the pointer by it)
 *
 * Kept on the stick, /LiteKern/machine.txt ("timezone London", "brightness
 * 80", "touchpad 50"): for everyone, not per user. The brightness and the clock only on the
 * EeePC (LKX_TARGET, set by lkx-start): on the PC they aren't ours. Tests on
 * the PC point LKX_POWER_SUPPLY and LKX_BACKLIGHT at folders of their own. */
#define _GNU_SOURCE
#include "kernel/machine.h"
#include "kernel/errno.h"
#include "kernel/printk.h"
#include "kernel/settings.h"
#include "kernel/timezones.h"
#include "platform/platform.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MACHINE_FILE "machine.txt"

static int target;                  /* on the EeePC: the backlight and the clock are ours */
static int zone = LKX_ZONE_DEFAULT;
static int saved_brightness = -1;   /* from machine.txt, until it's applied or changed */
static char backlight[300];         /* the chosen /sys/class/backlight/<name> ("" none) */

static const char *dir_of(const char *env, const char *fallback)
{
    const char *d = getenv(env);
    return d && *d ? d : fallback;
}

/* The first line of dir/name/file ("" if there's none). */
static int read_value(const char *dir, const char *name, const char *file, char *out, int max)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/%s/%s", dir, name, file);
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(out, max, f))
        out[0] = '\0';
    fclose(f);
    out[strcspn(out, "\n")] = '\0';
    return out[0] ? 0 : -1;
}

static long read_long(const char *dir, const char *name, const char *file)
{
    char v[32];
    return read_value(dir, name, file, v, sizeof(v)) ? -1 : atol(v);
}

static void run(const char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return;
    if (!pid) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 1), dup2(null, 2);
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    waitpid(pid, 0, 0);
}

/* --- the backlight ---------------------------------------------------------------------------- */

static void find_backlight(void)
{
    static const char *const order[] = { "firmware", "platform", "raw" };
    const char *dir = dir_of("LKX_BACKLIGHT", "/sys/class/backlight");
    backlight[0] = '\0';
    if (!target && !getenv("LKX_BACKLIGHT"))
        return;
    for (int k = 0; k < 3 && !backlight[0]; k++) {
        DIR *d = opendir(dir);
        struct dirent *e;
        while (d && (e = readdir(d)) && !backlight[0]) {
            char type[32];
            if (e->d_name[0] == '.' || read_value(dir, e->d_name, "type", type, sizeof(type)) ||
                strcmp(type, order[k]) || read_long(dir, e->d_name, "max_brightness") <= 0)
                continue;
            snprintf(backlight, sizeof(backlight), "%s/%s", dir, e->d_name);
        }
        if (d)
            closedir(d);
    }
    if (backlight[0])
        kprintf("machine: backlight %s\n", strrchr(backlight, '/') + 1);
}

static int brightness_now(void)
{
    if (!backlight[0])
        return -1;
    long max = read_long(backlight, ".", "max_brightness"), cur = read_long(backlight, ".", "brightness");
    if (max <= 0 || cur < 0)
        return -1;
    return (int)((cur * 100 + max / 2) / max);
}

static int write_brightness(int percent)
{
    long max = read_long(backlight, ".", "max_brightness");
    if (max <= 0)
        return -ENODEV;
    long v = (percent * max + 50) / 100;
    if (v < 1)
        v = 1;
    char path[320], text[24];
    snprintf(path, sizeof(path), "%s/brightness", backlight);
    snprintf(text, sizeof(text), "%ld\n", v);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -EIO;
    ssize_t n = write(fd, text, strlen(text));
    close(fd);
    return n > 0 ? 0 : -EIO;
}

/* --- kept on the stick ------------------------------------------------------------------------ */

static void save(void)
{
    char text[128];
    int b = brightness_now();
    int n = snprintf(text, sizeof(text), "timezone %s\n", lkx_zones[zone].name);
    if (b >= 0)
        n += snprintf(text + n, sizeof(text) - (size_t)n, "brightness %d\n", b);
    n += snprintf(text + n, sizeof(text) - (size_t)n, "touchpad %d\n", plat_touchpad_speed);
    settings_write_file(MACHINE_FILE, text, (uint32_t)n);
}

static void load(void)
{
    char text[256];
    int n = settings_read_file(MACHINE_FILE, text, sizeof(text) - 1);
    if (n <= 0)
        return;
    text[n] = '\0';
    for (char *l = strtok(text, "\n"); l; l = strtok(0, "\n")) {
        if (!strncmp(l, "timezone ", 9)) {
            for (int i = 0; i < LKX_ZONES; i++)
                if (!strcmp(l + 9, lkx_zones[i].name))
                    zone = i;
        } else if (!strncmp(l, "brightness ", 11)) {
            int b = atoi(l + 11);
            saved_brightness = b < 5 ? 5 : b > 100 ? 100 : b;
        } else if (!strncmp(l, "touchpad ", 9)) {
            int t = atoi(l + 9);
            plat_touchpad_speed = t < 0 ? 0 : t > 100 ? 100 : t;
        }
    }
}

/* --- the time zone ---------------------------------------------------------------------------- */

static void apply_zone(void)
{
    setenv("TZ", lkx_zones[zone].rule, 1);
    tzset();                        /* localtime_r (the clock, rtc0) follows it now */
}

/* --- kernel/machine.h ------------------------------------------------------------------------- */

void machine_init(void)
{
    target = getenv("LKX_TARGET") != 0;
    load();
    apply_zone();
    find_backlight();
    if (backlight[0] && saved_brightness >= 0)
        write_brightness(saved_brightness);
    if (target) {
        const char *argv[] = { "/sbin/hwclock", "-s", "-l", 0 };
        run(argv);
    }
    time_t now = time(0);
    struct tm tm;
    localtime_r(&now, &tm);
    kprintf("machine: time zone %s; the clock says %04d-%02d-%02d %02d:%02d\n", lkx_zones[zone].name,
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

void machine_status(struct k86_machine *out)
{
    *out = (struct k86_machine){ .battery = -1, .minutes = -1, .brightness = brightness_now(), .timezone = zone,
                                 .touchpad = plat_touchpad_present || getenv("LKX_FAKE_TOUCHPAD") ? plat_touchpad_speed : -1 };
    const char *dir = dir_of("LKX_POWER_SUPPLY", "/sys/class/power_supply");
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char type[32], status[32];
        if (e->d_name[0] == '.' || read_value(dir, e->d_name, "type", type, sizeof(type)) || strcmp(type, "Battery"))
            continue;
        long present = read_long(dir, e->d_name, "present");
        if (present == 0)
            continue;
        /* How full: capacity, or now/full in energy (uWh) or charge (uAh). */
        long cap = read_long(dir, e->d_name, "capacity");
        long now = read_long(dir, e->d_name, "energy_now"), full = read_long(dir, e->d_name, "energy_full");
        long rate = read_long(dir, e->d_name, "power_now");
        if (now < 0 || full <= 0) {
            now = read_long(dir, e->d_name, "charge_now");
            full = read_long(dir, e->d_name, "charge_full");
            rate = read_long(dir, e->d_name, "current_now");
        }
        if (cap < 0 && now >= 0 && full > 0)
            cap = now * 100 / full;
        if (cap < 0)
            continue;
        out->battery = cap > 100 ? 100 : (int)cap;
        read_value(dir, e->d_name, "status", status, sizeof(status));
        out->charging = !strcmp(status, "Charging") ? K86_BATTERY_CHARGING
                      : !strcmp(status, "Full") || !strcmp(status, "Not charging") ? K86_BATTERY_FULL
                      : K86_BATTERY_DISCHARGING;
        if (rate > 0 && now >= 0 && full > 0) {
            long left = out->charging == K86_BATTERY_CHARGING ? full - now : now;
            if (out->charging != K86_BATTERY_FULL && left >= 0)
                out->minutes = (int)(left * 60 / rate);
        }
        break;
    }
    if (d)
        closedir(d);
}

int machine_set_brightness(int percent)
{
    if (percent < 5 || percent > 100)
        return -EINVAL;
    if (!backlight[0])
        return -ENODEV;
    int err = write_brightness(percent);
    if (!err) {
        kprintf("machine: brightness %d%%\n", percent);
        save();
    }
    return err;
}

int machine_set_timezone(int z)
{
    if (z < 0 || z >= LKX_ZONES)
        return -EINVAL;
    zone = z;
    apply_zone();
    if (target) {                   /* the EeePC's clock: local time in the new zone */
        const char *argv[] = { "/sbin/hwclock", "-w", "-l", 0 };
        run(argv);
    }
    kprintf("machine: time zone %s\n", lkx_zones[zone].name);
    save();
    return 0;
}

int machine_set_touchpad(int speed)
{
    if (speed < 0 || speed > 100)
        return -EINVAL;
    plat_touchpad_speed = speed;
    kprintf("machine: touchpad speed %d\n", speed);
    save();
    return 0;
}
