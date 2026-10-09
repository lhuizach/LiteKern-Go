/* LiteKern X — settings that last across restarts (see kernel/settings.h). */
#include "kernel/settings.h"
#include "kernel/errno.h"
#include "kernel/lockscreen.h"
#include "kernel/pmm.h"
#include "kernel/printk.h"
#include "kernel/screensaver.h"
#include "kernel/storage.h"
#include "kernel/string.h"
#include "kernel/theme.h"
#include "kernel/timing.h"
#include "kernel/wallpaper.h"

#define FOLDER      "LiteKern"
#define TEXT_FILE   "settings.txt"
#define PIXEL_FILE  "wallpaper.lkxw"
#define LOCK_FILE   "lock.lkxw"
#define TEXT_MAX    512

static int loaded;                  /* settings.txt was there */
static char wallpaper[32];          /* what it said ("" = it didn't say) */

/* The boot disk's FAT32 volume, if it can be written. */
static struct fat_volume *boot_volume(void)
{
    for (int i = 0; i < storage_count(); i++) {
        struct storage *s = storage_get(i);
        if (s && s->supported && !s->fat.read_only && !memcmp(s->name, "Boot disk", 9))
            return &s->fat;
    }
    return 0;
}

/* The LiteKern folder's first cluster (made if `make` and it isn't there). */
static int folder(struct fat_volume *v, int make, uint32_t *out)
{
    struct fat_entry e;
    int err = fat_find(v, fat_root(v), FOLDER, &e);
    if (err == -ENOENT && make)
        err = fat_create(v, fat_root(v), FOLDER, 1, &e);
    if (!err && !e.is_dir)
        err = -EINVAL;
    if (!err)
        *out = e.cluster;
    return err;
}

static int save_file(const char *name, const void *data, uint32_t len)
{
    struct fat_volume *v = boot_volume();
    uint32_t dir;
    struct fat_entry e;
    if (!v)
        return -ENODEV;
    int err = folder(v, 1, &dir);
    if (!err) {
        err = fat_find(v, dir, name, &e);
        if (err == -ENOENT)
            err = fat_create(v, dir, name, 0, &e);
    }
    if (!err)
        err = fat_write(v, &e, data, len);
    return err;
}

/* Find a file in the LiteKern folder. */
static int find_file(const char *name, struct fat_volume **v, struct fat_entry *e)
{
    uint32_t dir;
    *v = boot_volume();
    if (!*v)
        return -ENODEV;
    int err = folder(*v, 0, &dir);
    if (!err)
        err = fat_find(*v, dir, name, e);
    if (!err && e->is_dir)
        err = -EINVAL;
    return err;
}

/* Read a file into buf; its size, or a negative error (-EINVAL if too big). */
static int load_file(const char *name, void *buf, uint32_t len)
{
    struct fat_volume *v;
    struct fat_entry e;
    int err = find_file(name, &v, &e);
    if (err)
        return err;
    if (e.size > len)
        return -EINVAL;
    return fat_read(v, &e, 0, buf, e.size);
}

int settings_write_file(const char *name, const void *data, uint32_t len)
{
    return save_file(name, data, len);
}

int settings_read_file(const char *name, void *buf, uint32_t len)
{
    return load_file(name, buf, len);
}

/* --- reading ------------------------------------------------------------------------------- */

static int number(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    if (!*s)
        return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000)
            return -1;
        v = v * 10 + (uint32_t)(*s - '0');
    }
    *out = v;
    return 0;
}

void settings_load(void)
{
    static char text[TEXT_MAX + 1];
    int n = load_file(TEXT_FILE, text, TEXT_MAX);
    if (n < 0) {
        kprintf("settings: %s\n", n == -ENOENT ? "none saved yet: the build's defaults"
                                  : n == -ENODEV ? "no writable boot disk: the build's defaults, not kept"
                                                 : "settings.txt unreadable: the build's defaults");
        return;
    }
    text[n] = '\0';
    int style = -1, accent = -1;
    uint32_t saver = 0xffffffffu;
    struct lock_settings lock = *lockscreen_settings();
    for (char *line = text; *line;) {
        char *end = line;
        while (*end && *end != '\n')
            end++;
        char *next = *end ? end + 1 : end;
        *end = '\0';
        if (end > line && end[-1] == '\r')
            end[-1] = '\0';
        char *value = line;
        while (*value && *value != ' ')
            value++;
        if (*value)
            *value++ = '\0';
        if (!strcmp(line, "style")) {
            style = theme_find_style(value);
        } else if (!strcmp(line, "accent")) {
            accent = theme_find_accent(value);
        } else if (!strcmp(line, "wallpaper")) {
            int k = 0;
            for (; value[k] && k < (int)sizeof(wallpaper) - 1; k++)
                wallpaper[k] = value[k];
            wallpaper[k] = '\0';
            if (!strcmp(wallpaper, "none"))
                wallpaper[0] = 1;           /* chosen: no wallpaper (told apart from unset) */
        } else if (!strcmp(line, "screensaver")) {
            uint32_t s;
            if (!number(value, &s) && s <= 3600)
                saver = s;
        } else if (!strcmp(line, "lock-wallpaper")) {
            int k = 0;
            for (; value[k] && k < (int)sizeof(lock.wallpaper) - 1; k++)
                lock.wallpaper[k] = value[k];
            lock.wallpaper[k] = '\0';
        } else if (!strcmp(line, "clock")) {
            lock.clock24 = strcmp(value, "12") != 0;
        } else if (!strcmp(line, "lock-on-wake")) {
            lock.on_wake = !strcmp(value, "yes");
        }
        line = next;
    }
    loaded = 1;
    theme_set(style, accent);               /* -1: keeps the build's */
    if (saver != 0xffffffffu)
        screensaver_set_timeout(saver);
    if (!strcmp(lock.wallpaper, LOCK_PICTURE) && settings_load_lock_picture())
        memcpy(lock.wallpaper, "desktop", 8);   /* its picture is gone: the desktop's */
    lockscreen_configure(&lock);
    kprintf("settings: loaded: %s, accent %s, wallpaper %s, screen saver %u s\n",
            theme_is_light() ? "light" : "dark", theme_accent(theme_accent_index(), 0),
            wallpaper[0] == 1 ? "none" : wallpaper[0] ? wallpaper : "(the build's)", screensaver_timeout());
    kprintf("settings: lock screen: wallpaper %s, %s-hour clock, lock when the screen saver ends: %s\n",
            lockscreen_settings()->wallpaper, lockscreen_settings()->clock24 ? "24" : "12",
            lockscreen_settings()->on_wake ? "yes" : "no");
}

const char *settings_wallpaper(const char *fallback)
{
    if (!loaded || !wallpaper[0])
        return fallback;
    return wallpaper[0] == 1 ? "" : wallpaper;
}

/* Read a packed picture (an .lkxw) and hand it to show(). */
static int load_packed(const char *file, const char *what, int (*show)(const void *, uint32_t))
{
    struct fat_volume *v;
    struct fat_entry e;
    uint32_t t0 = uptime_ms();
    int err = find_file(file, &v, &e);
    if (err)
        return err;
    if (!e.size || e.size > 32 * 1024 * 1024)
        return -EINVAL;
    uint32_t pages = (e.size + PAGE_SIZE - 1) / PAGE_SIZE, buf = pmm_alloc_contiguous(pages);
    if (!buf)
        return -ENOMEM;
    int n = fat_read(v, &e, 0, (void *)buf, e.size);
    uint32_t t1 = uptime_ms();
    err = n < 0 ? n : (uint32_t)n != e.size ? -EIO : show((const void *)buf, e.size);
    for (uint32_t i = 0; i < pages; i++)
        pmm_free(buf + i * PAGE_SIZE);
    if (err)
        kprintf("settings: the %s couldn't be read (%d)\n", what, err);
    else
        kprintf("settings: %s, %u KB, read in %u ms, unpacked in %u ms\n", what, e.size / 1024, t1 - t0,
                uptime_ms() - t1);
    return err;
}

int settings_load_wallpaper(void)
{
    return load_packed(PIXEL_FILE, "custom wallpaper", wallpaper_set_packed);
}

int settings_load_lock_picture(void)
{
    return load_packed(LOCK_FILE, "lock screen picture", lockscreen_load_picture);
}

/* --- writing ------------------------------------------------------------------------------- */

void settings_save(void)
{
    char text[TEXT_MAX];
    const char *wp = wallpaper_current();
    const struct lock_settings *lock = lockscreen_settings();
    ksnprintf(text, sizeof(text),
              "style %s\naccent %s\nwallpaper %s\nscreensaver %u\n"
              "lock-wallpaper %s\nclock %s\nlock-on-wake %s\n",
              theme_is_light() ? "light" : "dark", theme_accent(theme_accent_index(), 0),
              wp[0] ? wp : "none", screensaver_timeout(), lock->wallpaper, lock->clock24 ? "24" : "12",
              lock->on_wake ? "yes" : "no");
    int err = save_file(TEXT_FILE, text, (uint32_t)strlen(text));
    if (err && err != -ENODEV)
        kprintf("settings: couldn't save (%d)\n", err);
    else if (!err)
        kprintf("settings: saved\n");
}

/* Pack a picture (pack()) and write it to file, then settings.txt. */
static void save_packed(const char *file, const char *what, int (*pack)(uint32_t *, uint32_t *, uint32_t *))
{
    uint32_t phys, bytes, len, t0 = uptime_ms();
    int err = pack(&phys, &bytes, &len);
    if (err) {
        kprintf("settings: couldn't pack the %s (%d)\n", what, err);
        return;
    }
    uint32_t t1 = uptime_ms();
    err = save_file(file, (const void *)phys, len);
    wallpaper_pack_free(phys, bytes);
    if (err) {
        if (err != -ENODEV)
            kprintf("settings: couldn't save the %s (%d)\n", what, err);
        return;
    }
    kprintf("settings: %s saved, %u KB, packed in %u ms, written in %u ms\n", what, len / 1024, t1 - t0,
            uptime_ms() - t1);
    settings_save();
}

void settings_save_wallpaper(void)
{
    save_packed(PIXEL_FILE, "custom wallpaper", wallpaper_pack);
}

void settings_save_lock_picture(void)
{
    save_packed(LOCK_FILE, "lock screen picture", lockscreen_pack_picture);
}
