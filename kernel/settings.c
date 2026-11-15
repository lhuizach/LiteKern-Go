/* LiteKern X — settings that last across restarts (see kernel/settings.h). */
#include "kernel/settings.h"
#include "defaults.h"
#include "kernel/desktop.h"
#include "kernel/errno.h"
#include "kernel/lockscreen.h"
#include "kernel/pmm.h"
#include "kernel/printk.h"
#include "kernel/screensaver.h"
#include "kernel/storage.h"
#include "kernel/string.h"
#include "kernel/theme.h"
#include "kernel/timing.h"
#include "kernel/users.h"
#include "kernel/wallpaper.h"
#include "kernel/wm.h"

#define FOLDER      "LiteKern"
#define USERS_DIR   "users"             /* in it: one folder a user (accounts mode) */
#define HOMES       "Users"             /* at the root: the home folders */
#define TEXT_FILE   "settings.txt"
#define PIXEL_FILE  "wallpaper.lkxw"
#define LOCK_FILE   "lock.lkxw"
#define LAST_FILE   "last-user.txt"
#define TEXT_MAX    2048

static int loaded;                  /* settings.txt was there */
static char wallpaper[32];          /* what it said ("" = it didn't say) */
static char user_dir[USER_NAME_MAX];    /* whose folder the files are in: "" = FOLDER itself */

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

int settings_boot_volume(void)
{
    for (int i = 0; i < storage_count(); i++) {
        struct storage *s = storage_get(i);
        if (s && s->supported && !s->fat.read_only && !memcmp(s->name, "Boot disk", 9))
            return i;
    }
    return -ENODEV;
}

/* A folder `name` in `dir` (made if `make` and it isn't there): its cluster. */
static int subdir(struct fat_volume *v, uint32_t dir, const char *name, int make, uint32_t *out)
{
    struct fat_entry e;
    int err = fat_find(v, dir, name, &e);
    if (err == -ENOENT && make)
        err = fat_create(v, dir, name, 1, &e);
    if (!err && !e.is_dir)
        err = -EINVAL;
    if (!err)
        *out = e.cluster;
    return err;
}

/* The folder the files are in: LiteKern, or the user's in it. */
static int folder(struct fat_volume *v, int make, uint32_t *out)
{
    int err = subdir(v, fat_root(v), FOLDER, make, out);
    if (!err && user_dir[0])
        err = subdir(v, *out, USERS_DIR, make, out);
    if (!err && user_dir[0])
        err = subdir(v, *out, user_dir, make, out);
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

/* Find a file in the folder. */
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

/* The same in LiteKern itself, whoever's logged in (users.txt, last-user.txt). */
static int top_save(const char *name, const void *data, uint32_t len)
{
    char keep[USER_NAME_MAX];
    memcpy(keep, user_dir, sizeof(keep));
    user_dir[0] = '\0';
    int err = save_file(name, data, len);
    memcpy(user_dir, keep, sizeof(keep));
    return err;
}

static int top_load(const char *name, void *buf, uint32_t len)
{
    char keep[USER_NAME_MAX];
    memcpy(keep, user_dir, sizeof(keep));
    user_dir[0] = '\0';
    int err = load_file(name, buf, len);
    memcpy(user_dir, keep, sizeof(keep));
    return err;
}

int settings_write_file(const char *name, const void *data, uint32_t len)
{
    return top_save(name, data, len);
}

int settings_read_file(const char *name, void *buf, uint32_t len)
{
    return top_load(name, buf, len);
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

#define STR_(x) #x
#define STR(x) STR_(x)

/* The build's own settings, as a settings.txt would say them. */
static const char build_text[] =
    "style " DEFAULT_STYLE "\naccent " DEFAULT_ACCENT "\nwallpaper " DEFAULT_WALLPAPER
    "\nscreensaver " STR(DEFAULT_SCREENSAVER_S) "\nlock-wallpaper desktop\nclock 24\nlock-on-wake no\n";

/* Read settings.txt (the folder's, or LiteKern's if `top`; with BUILD, the
 * build's own) and apply it. At boot the desktop shows the wallpaper
 * (settings_wallpaper()); later (a login) it's shown here. 0, or the read's
 * error. */
#define BUILD 2
static int apply(int top, int at_boot)
{
    static char text[TEXT_MAX + 1];
    int n = top == BUILD ? (int)sizeof(build_text) - 1
          : top ? top_load(TEXT_FILE, text, TEXT_MAX) : load_file(TEXT_FILE, text, TEXT_MAX);
    if (n < 0)
        return n;
    if (top == BUILD)
        memcpy(text, build_text, (size_t)n);
    text[n] = '\0';
    int style = -1, accent = -1;
    uint32_t saver = 0xffffffffu;
    struct lock_settings lock = *lockscreen_settings();
    struct home_clock home = *lockscreen_home_clock();
    static char layout_text[400], style_text[48];
    layout_text[0] = '\0';
    style_text[0] = '\0';
    wallpaper[0] = '\0';
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
        } else if (!strcmp(line, "lock-clock-y")) {
            uint32_t n;
            lock.clock_y = number(value, &n) ? -1 : (int)n;
        } else if (!strcmp(line, "home-style")) {
            int k = 0;
            for (; value[k] && k < (int)sizeof(style_text) - 1; k++)
                style_text[k] = value[k];
            style_text[k] = '\0';
        } else if (!strcmp(line, "home-layout")) {
            int k = 0;
            for (; value[k] && k < (int)sizeof(layout_text) - 1; k++)
                layout_text[k] = value[k];
            layout_text[k] = '\0';
        } else if (strlen(line) > 11 && !memcmp(line, "home-clock-", 11)) {
            uint32_t n;
            if (!strcmp(line, "home-clock-y"))
                home.y = number(value, &n) ? -1 : (int)n;
            else if (!number(value, &n)) {
                if (!strcmp(line, "home-clock-show"))
                    home.show = (int)n;
                else if (!strcmp(line, "home-clock-font"))
                    home.font = (int)n;
                else if (!strcmp(line, "home-clock-style"))
                    home.style = (int)n;
                else if (!strcmp(line, "home-clock-opacity"))
                    home.opacity = (int)n;
                else if (!strcmp(line, "home-clock-stretch"))
                    home.stretch = (int)n;
                else if (!strcmp(line, "home-clock-size"))
                    home.size = (int)n;
                else if (!strcmp(line, "home-clock-x"))
                    home.x = (int)n;
                else if (!strcmp(line, "home-clock-date"))
                    home.date = (int)n;
            }
        } else if (strlen(line) > 11 && !memcmp(line, "lock-clock-", 11)) {
            uint32_t n;
            if (!number(value, &n)) {
                if (!strcmp(line, "lock-clock-font"))
                    lock.clock_font = (int)n;
                else if (!strcmp(line, "lock-clock-style"))
                    lock.clock_style = (int)n;
                else if (!strcmp(line, "lock-clock-opacity"))
                    lock.clock_opacity = (int)n;
                else if (!strcmp(line, "lock-clock-stretch"))
                    lock.clock_stretch = (int)n;
                else if (!strcmp(line, "lock-clock-x"))
                    lock.clock_x = (int)n;
            }
        }
        line = next;
    }
    loaded = 1;
    char keep[USER_NAME_MAX];               /* the pictures are where the text was */
    memcpy(keep, user_dir, sizeof(keep));
    if (top)
        user_dir[0] = '\0';
    theme_set(style, accent);               /* -1: keeps the build's */
    if (saver != 0xffffffffu)
        screensaver_set_timeout(saver);
    if (!strcmp(lock.wallpaper, LOCK_PICTURE) && settings_load_lock_picture())
        memcpy(lock.wallpaper, "desktop", 8);   /* its picture is gone: the desktop's */
    lockscreen_configure(&lock);
    lockscreen_set_home_clock(&home);
    desktop_layout_set(layout_text);
    desktop_style_set(style_text);
    if (!at_boot) {
        const char *wp = settings_wallpaper("");
        if (!strcmp(wp, WALLPAPER_CUSTOM)) {
            if (settings_load_wallpaper())
                wallpaper_set("");
        } else if (wallpaper[0]) {
            wallpaper_set(wp);
        }
        desktop_refresh();
        wm_theme_changed();
    }
    memcpy(user_dir, keep, sizeof(keep));
    kprintf("settings: loaded%s%s: %s, accent %s, wallpaper %s, screen saver %u s\n", user_dir[0] && !top ? " for " : "",
            user_dir[0] && !top ? user_dir : "", theme_is_light() ? "light" : "dark",
            theme_accent(theme_accent_index(), 0), wallpaper[0] == 1 ? "none" : wallpaper[0] ? wallpaper : "(the build's)",
            screensaver_timeout());
    kprintf("settings: lock screen: wallpaper %s, %s-hour clock, lock when the screen saver ends: %s\n",
            lockscreen_settings()->wallpaper, lockscreen_settings()->clock24 ? "24" : "12",
            lockscreen_settings()->on_wake ? "yes" : "no");
    return 0;
}

void settings_load(void)
{
    /* With accounts, the login screen looks as whoever logged in last left it. */
    if (users_need_login()) {
        char last[USER_NAME_MAX + 2];
        int n = top_load(LAST_FILE, last, sizeof(last) - 1);
        if (n > 0) {
            last[n] = '\0';
            while (n && (last[n - 1] == '\n' || last[n - 1] == '\r'))
                last[--n] = '\0';
            if (users_find(last) >= 0)
                memcpy(user_dir, last, (size_t)n + 1);
        }
        if (user_dir[0] && apply(0, 1) == 0)
            return;
        user_dir[0] = '\0';
    }
    int n = apply(1, 1);
    if (n < 0)
        kprintf("settings: %s\n", n == -ENOENT ? "none saved yet: the build's defaults"
                                  : n == -ENODEV ? "no writable boot disk: the build's defaults, not kept"
                                                 : "settings.txt unreadable: the build's defaults");
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
    uint32_t pages = (e.size + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t buf = pmm_alloc_contiguous(pages);
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
    char clock_y[12], home_y[12];
    static char layout_text[400];
    char style_text[48];
    desktop_layout_text(layout_text, sizeof(layout_text));
    desktop_style_text(style_text, sizeof(style_text));
    const struct home_clock *home = lockscreen_home_clock();
    if (home->y < 0)
        memcpy(home_y, "auto", 5);
    else
        ksnprintf(home_y, sizeof(home_y), "%d", home->y);
    if (lock->clock_y < 0)
        memcpy(clock_y, "auto", 5);
    else
        ksnprintf(clock_y, sizeof(clock_y), "%d", lock->clock_y);
    ksnprintf(text, sizeof(text),
              "style %s\naccent %s\nwallpaper %s\nscreensaver %u\n"
              "lock-wallpaper %s\nclock %s\nlock-on-wake %s\n"
              "lock-clock-font %d\nlock-clock-style %d\nlock-clock-opacity %d\n"
              "lock-clock-stretch %d\nlock-clock-x %d\nlock-clock-y %s\n"
              "home-clock-show %d\nhome-clock-font %d\nhome-clock-style %d\nhome-clock-opacity %d\n"
              "home-clock-stretch %d\nhome-clock-size %d\nhome-clock-x %d\nhome-clock-y %s\nhome-clock-date %d\nhome-layout %s\nhome-style %s\n",
              theme_is_light() ? "light" : "dark", theme_accent(theme_accent_index(), 0),
              wp[0] ? wp : "none", screensaver_timeout(), lock->wallpaper, lock->clock24 ? "24" : "12",
              lock->on_wake ? "yes" : "no", lock->clock_font, lock->clock_style, lock->clock_opacity,
              lock->clock_stretch, lock->clock_x, clock_y, home->show, home->font, home->style, home->opacity,
              home->stretch, home->size, home->x, home_y, home->date, layout_text, style_text);
    int err = save_file(TEXT_FILE, text, (uint32_t)strlen(text));
    if (err && err != -ENODEV)
        kprintf("settings: couldn't save (%d)\n", err);
    else if (!err)
        kprintf("settings: saved%s%s\n", user_dir[0] ? " for " : "", user_dir);
}

/* Pack a picture (pack()) and write it to file, then settings.txt. */
static void save_packed(const char *file, const char *what, int (*pack)(uintptr_t *, uint32_t *, uint32_t *))
{
    uintptr_t phys;
    uint32_t bytes, len, t0 = uptime_ms();
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

/* --- users: their folders ------------------------------------------------------------------- */

/* The user's home folder, /Users/<name>, with Documents and Pictures in it. Made when
 * they log in, and again whenever an app asks where it is (settings_home): setting a PIN
 * on the one account, while it's logged in, turns accounts on, and with them this folder,
 * and nobody logs in then. Without it Gallery saw nothing and Camera couldn't save. */
static void make_home(const char *name)
{
    struct fat_volume *v = boot_volume();
    uint32_t homes, home, d;
    struct fat_entry e;
    if (!v || subdir(v, fat_root(v), HOMES, 1, &homes))
        return;
    int fresh = fat_find(v, homes, name, &e) == -ENOENT;
    if (subdir(v, homes, name, 1, &home))
        return;
    subdir(v, home, "Documents", 1, &d);
    subdir(v, home, "Pictures", 1, &d);
    if (fresh)
        kprintf("settings: made %s's home folder\n", name);
}

int settings_save_screenshot(const char *name, const void *data, uint32_t len)
{
    struct fat_volume *v = boot_volume();
    const struct user *u = users_get(users_current());
    uint32_t dir;
    struct fat_entry e;
    if (!v)
        return -ENODEV;
    dir = fat_root(v);
    int err = 0;
    if (u && users_need_login())            /* accounts: their home folder's Pictures */
        err = subdir(v, dir, HOMES, 1, &dir) || subdir(v, dir, u->name, 1, &dir) ? -EIO : 0;
    if (!err)
        err = subdir(v, dir, "Pictures", 1, &dir);
    if (!err)
        err = subdir(v, dir, "Screenshots", 1, &dir);
    if (!err)
        err = fat_create(v, dir, name, 0, &e);
    if (!err)
        err = fat_write(v, &e, data, len);
    return err;
}

void settings_user_changed(const char *name)
{
    if (!users_need_login()) {              /* one user, no password: as before accounts */
        user_dir[0] = '\0';
        return;
    }
    int n = (int)strlen(name);
    top_save(LAST_FILE, name, (uint32_t)n);
    make_home(name);
    if (!strcmp(user_dir, name))
        return;                             /* theirs is showing already */
    memcpy(user_dir, name, (size_t)n + 1);
    /* Their own settings, if they've saved any; else everyone's (from
     * before there were accounts), else the build's. Their first change
     * makes them theirs. */
    if (apply(0, 0) == -ENOENT) {
        kprintf("settings: %s has none of their own yet\n", name);
        if (apply(1, 0) < 0)
            apply(BUILD, 0);
    }
}

int settings_home(char *out, int size)
{
    const struct user *u = users_get(users_current());
    if (!u || !users_need_login() || size < 8 + USER_NAME_MAX) {
        if (size >= 2)
            out[0] = '/', out[1] = '\0';
        return 0;
    }
    make_home(u->name);                     /* there for whatever asked */
    memcpy(out, "/" HOMES "/", 7);
    memcpy(out + 7, u->name, strlen(u->name) + 1);
    return 0;
}

/* Rename or delete one of a user's folders: `parent` (in the root, then
 * `inner` in it if given), the entry `name`. */
static int user_folder(const char *parent, const char *inner, const char *name, const char *new_name)
{
    struct fat_volume *v = boot_volume();
    uint32_t dir;
    struct fat_entry e;
    if (!v)
        return -ENODEV;
    int err = subdir(v, fat_root(v), parent, 0, &dir);
    if (!err && inner)
        err = subdir(v, dir, inner, 0, &dir);
    if (!err)
        err = fat_find(v, dir, name, &e);
    if (err)
        return err;
    return new_name ? fat_rename(v, &e, new_name) : fat_delete(v, &e);
}

void settings_user_renamed(const char *old_name, const char *new_name)
{
    user_folder(FOLDER, USERS_DIR, old_name, new_name);
    user_folder(HOMES, 0, old_name, new_name);
    if (!strcmp(user_dir, old_name))
        memcpy(user_dir, new_name, strlen(new_name) + 1);
}

void settings_user_removed(const char *name, int delete_files)
{
    user_folder(FOLDER, USERS_DIR, name, 0);
    if (delete_files && user_folder(HOMES, 0, name, 0) == 0)
        kprintf("settings: %s's files deleted\n", name);
}

/* --- who may see what (kernel/sys_app.c checks every app's paths) ----------------------------- */

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

/* The n bytes at a and the string b the same, ignoring case (FAT does). */
static int same_n(const char *a, int n, const char *b)
{
    int i = 0;
    for (; i < n && b[i]; i++)
        if (lower(a[i]) != lower(b[i]))
            return 0;
    return i == n && !b[i];
}

int settings_private(const char *path)
{
    const struct user *u = users_get(users_current());
    if (!users_need_login() || (u && u->admin))
        return 0;                           /* one user, or an admin: everything */
    const char *p = path;
    while (*p == '/')
        p++;
    int n = 0;
    while (p[n] && p[n] != '/')
        n++;
    if (same_n(p, n, FOLDER))
        return 1;                           /* the system's (password hashes included) */
    if (!same_n(p, n, HOMES))
        return 0;
    p += n;
    while (*p == '/')
        p++;
    for (n = 0; p[n] && p[n] != '/'; n++)
        ;
    return n && (!u || !same_n(p, n, u->name));     /* someone else's home */
}
