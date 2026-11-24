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
#include "kernel/timing.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MACHINE_FILE "machine.txt"

static int target;                  /* on the EeePC: the backlight and the clock are ours */
static int zone = LKX_ZONE_DEFAULT;
static int saved_volume = 60;       /* the loudspeaker's level, 0-100 (apps read it from VOLUME_FILE) */
#define VOLUME_FILE "/dev/shm/lkx-volume"
static void write_volume_file(void);
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
    n += snprintf(text + n, sizeof(text) - (size_t)n, "volume %d\n", saved_volume);
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
        } else if (!strncmp(l, "volume ", 7)) {
            int v = atoi(l + 7);
            saved_volume = v < 0 ? 0 : v > 100 ? 100 : v;
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

/* The CPU's speed policy. The shell draws in short bursts, too short for Linux's
 * usual policy to speed the CPU up, so animations crawled at a low clock: the
 * whole clock while the screen is on, the usual policy while it's off. */
static void cpu_governor(const char *name)
{
    if (!target)
        return;
    FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "w");
    if (!f) {
        kprintf("machine: no cpu speed control\n");
        return;
    }
    fputs(name, f);
    int bad = fclose(f);
    kprintf("machine: cpu policy %s%s\n", name, bad ? " refused" : "");
}

/* The lights behind the touch keys: on with a touch, off KEY_LIGHT_MS later. */
#define KEY_LIGHT "/sys/class/leds/tm2-touchkey/brightness"
#define KEY_LIGHT_MS 5000
static int key_light = -1;              /* -1: not set yet */
static uint32_t key_light_until;

static void key_light_set(int on)
{
    if (!target || on == key_light)
        return;
    key_light = on;
    FILE *f = fopen(KEY_LIGHT, "w");
    if (!f) {
        kprintf("machine: no key lights here\n");
        return;
    }
    fputs(on ? "255" : "0", f);
    fclose(f);
}

void plat_key_light_poke(void)
{
    key_light_until = uptime_ms() + KEY_LIGHT_MS;
    if (plat_screen_on())
        key_light_set(1);
}

void plat_key_light_tick(void)
{
    if (key_light == 1 && (int32_t)(uptime_ms() - key_light_until) >= 0)
        key_light_set(0);
}

void machine_init(void)
{
    target = getenv("LKX_TARGET") != 0;
    load();
    apply_zone();
    long cur = read_long("/sys/devices/system/cpu/cpu0/cpufreq", ".", "scaling_cur_freq"),
         top = read_long("/sys/devices/system/cpu/cpu0/cpufreq", ".", "cpuinfo_max_freq");
    if (target)
        kprintf("machine: cpu at %ld MHz, %ld at most (-1: no speed control)\n", cur < 0 ? -1 : cur / 1000,
                top < 0 ? -1 : top / 1000);
    cpu_governor("performance");
    write_volume_file();
    plat_key_light_poke();
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

/* The vibrator: any input device with force feedback (the A5's pwm-vibrator). */
void machine_haptic(int ms)
{
    static int fd = -2, effect = -1;    /* -2: not looked for yet */
    if (fd == -2) {
        fd = -1;
        for (int n = 0; n < 32 && fd < 0; n++) {
            char path[32];
            unsigned long bits[FF_MAX / (8 * sizeof(long)) + 1] = { 0 };
            snprintf(path, sizeof(path), "/dev/input/event%d", n);
            int f = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (f < 0)
                continue;
            if (ioctl(f, EVIOCGBIT(EV_FF, sizeof(bits)), bits) >= 0 &&
                (bits[FF_RUMBLE / (8 * sizeof(long))] >> (FF_RUMBLE % (8 * sizeof(long))) & 1))
                fd = f;
            else
                close(f);
        }
        kprintf(fd >= 0 ? "haptic: a vibrator\n" : "haptic: no vibrator here\n");
    }
    if (fd < 0)
        return;
    struct ff_effect e;
    memset(&e, 0, sizeof(e));
    e.type = FF_RUMBLE;
    e.id = (short)effect;
    e.replay.length = (unsigned short)ms;
    e.u.rumble.strong_magnitude = 0xc000;
    if (ioctl(fd, EVIOCSFF, &e) < 0)
        return;
    effect = e.id;
    struct input_event play;
    memset(&play, 0, sizeof(play));
    play.type = EV_FF;
    play.code = (unsigned short)e.id;
    play.value = 1;
    if (write(fd, &play, sizeof(play)) < 0)
        return;
}

/* The volume: told to the apps through a file in memory (sdk/audio.c reads it) at every change,
 * and kept on the stick when the changing has stopped (machine_volume_commit). */
static void write_volume_file(void)
{
    char t[8];
    int n = snprintf(t, sizeof(t), "%d\n", saved_volume);
    FILE *f = fopen(VOLUME_FILE ".tmp", "w");          /* written whole, then renamed in: a reader never sees it empty */
    if (f) {
        fwrite(t, 1, (size_t)n, f);
        fclose(f);
        rename(VOLUME_FILE ".tmp", VOLUME_FILE);
    }
}

#define WEATHER_STATE "/dev/shm/lkx-weather"
#define WEATHER_STALE_S 1800

int machine_weather(struct weather_now *w)
{
    FILE *f = fopen(WEATHER_STATE, "r");
    if (!f)
        return 0;
    char head[8];
    long when;
    int ok = fscanf(f, "%7s %ld\n", head, &when) == 2 && !strcmp(head, "ok") && fgets(w->city, sizeof(w->city), f);
    if (ok) {
        size_t n = strlen(w->city);
        while (n && (w->city[n - 1] == '\n' || w->city[n - 1] == '\r'))
            w->city[--n] = '\0';
        ok = fscanf(f, "%d %d %d", &w->temp, &w->code, &w->day) == 3 && fscanf(f, "%d %d", &w->hi, &w->lo) == 2;
    }
    fclose(f);
    return ok;
}

void machine_weather_refresh(void)
{
    static time_t last_try;
    struct stat st;
    time_t now = time(0);
    if (now - last_try < 180)
        return;                                 /* one try every few minutes at most */
    if (!stat(WEATHER_STATE, &st) && now - st.st_mtime < WEATHER_STALE_S)
        return;
    last_try = now;
    char path[512];
    const char *dir = getenv("LKX_APPS");
    snprintf(path, sizeof(path), "%s/apps/weather/weather", dir ? dir : "build-linux");
    if (access(path, X_OK))
        return;
    pid_t pid = fork();
    if (pid < 0)
        return;
    if (!pid) {
        pid_t grand = fork();                   /* a grandchild, so nobody has to wait for it */
        if (!grand) {
            int null = open("/dev/null", O_RDWR);
            dup2(null, 0), dup2(null, 1), dup2(null, 2);
            for (int fd = 3; fd < 64; fd++)
                close(fd);
            execl(path, path, "--fetch", (char *)0);
            _exit(127);
        }
        _exit(0);
    }
    waitpid(pid, 0, 0);
}

#define MUSIC_STATE "/dev/shm/lkx-music"
#define MUSIC_CMD "/dev/shm/lkx-music-cmd"

int machine_music(char *title, int max)
{
    title[0] = '\0';
    FILE *f = fopen(MUSIC_STATE, "r");
    if (!f)
        return 0;
    char state[16] = "";
    if (fgets(state, sizeof(state), f) && fgets(title, max, f)) {
        size_t n = strlen(title);
        while (n && (title[n - 1] == '\n' || title[n - 1] == '\r'))
            title[--n] = '\0';
    }
    fclose(f);
    return !strncmp(state, "playing", 7) ? 1 : 2;
}

void machine_music_command(const char *cmd)
{
    FILE *f = fopen(MUSIC_CMD, "w");
    if (f) {
        fputs(cmd, f);
        fclose(f);
    }
}

int machine_volume(void)
{
    return saved_volume;
}

void machine_set_volume(int percent)
{
    saved_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    write_volume_file();
}

void machine_volume_commit(void)
{
    save();
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

void machine_screen(int on)
{
    if (on != plat_screen_on())
        cpu_governor(on ? "performance" : "schedutil");
    plat_screen_power(on);
    if (on)
        plat_key_light_poke();
    else
        key_light_set(0);
}

int machine_screen_on(void)
{
    return plat_screen_on();
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
