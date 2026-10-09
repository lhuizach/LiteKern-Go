/* LiteKern X — settings that last across restarts (see kernel/settings.h). */
#include "kernel/settings.h"
#include "kernel/errno.h"
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
        }
        line = next;
    }
    loaded = 1;
    theme_set(style, accent);               /* -1: keeps the build's */
    if (saver != 0xffffffffu)
        screensaver_set_timeout(saver);
    kprintf("settings: loaded: %s, accent %s, wallpaper %s, screen saver %u s\n",
            theme_is_light() ? "light" : "dark", theme_accent(theme_accent_index(), 0),
            wallpaper[0] == 1 ? "none" : wallpaper[0] ? wallpaper : "(the build's)", screensaver_timeout());
}

const char *settings_wallpaper(const char *fallback)
{
    if (!loaded || !wallpaper[0])
        return fallback;
    return wallpaper[0] == 1 ? "" : wallpaper;
}

int settings_load_wallpaper(void)
{
    struct fat_volume *v;
    struct fat_entry e;
    uint32_t t0 = uptime_ms();
    int err = find_file(PIXEL_FILE, &v, &e);
    if (err)
        return err;
    if (!e.size || e.size > 32 * 1024 * 1024)
        return -EINVAL;
    uint32_t pages = (e.size + PAGE_SIZE - 1) / PAGE_SIZE, buf = pmm_alloc_contiguous(pages);
    if (!buf)
        return -ENOMEM;
    int n = fat_read(v, &e, 0, (void *)buf, e.size);
    uint32_t t1 = uptime_ms();
    err = n < 0 ? n : (uint32_t)n != e.size ? -EIO : wallpaper_set_packed((const void *)buf, e.size);
    for (uint32_t i = 0; i < pages; i++)
        pmm_free(buf + i * PAGE_SIZE);
    if (err)
        kprintf("settings: the custom wallpaper couldn't be read (%d)\n", err);
    else
        kprintf("settings: custom wallpaper, %u KB, read in %u ms, unpacked in %u ms\n", e.size / 1024,
                t1 - t0, uptime_ms() - t1);
    return err;
}

/* --- writing ------------------------------------------------------------------------------- */

void settings_save(void)
{
    char text[TEXT_MAX];
    const char *wp = wallpaper_current();
    ksnprintf(text, sizeof(text),
              "style %s\naccent %s\nwallpaper %s\nscreensaver %u\n",
              theme_is_light() ? "light" : "dark", theme_accent(theme_accent_index(), 0),
              wp[0] ? wp : "none", screensaver_timeout());
    int err = save_file(TEXT_FILE, text, (uint32_t)strlen(text));
    if (err && err != -ENODEV)
        kprintf("settings: couldn't save (%d)\n", err);
    else if (!err)
        kprintf("settings: saved\n");
}

void settings_save_wallpaper(void)
{
    uint32_t phys, bytes, len, t0 = uptime_ms();
    int err = wallpaper_pack(&phys, &bytes, &len);
    if (err) {
        kprintf("settings: couldn't pack the custom wallpaper (%d)\n", err);
        return;
    }
    uint32_t t1 = uptime_ms();
    err = save_file(PIXEL_FILE, (const void *)phys, len);
    wallpaper_pack_free(phys, bytes);
    if (err) {
        if (err != -ENODEV)
            kprintf("settings: couldn't save the custom wallpaper (%d)\n", err);
        return;
    }
    kprintf("settings: custom wallpaper saved, %u KB, packed in %u ms, written in %u ms\n", len / 1024,
            t1 - t0, uptime_ms() - t1);
    settings_save();
}
