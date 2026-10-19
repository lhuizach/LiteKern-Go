/* LiteKern X — the apps in the ramdisk: each apps/<name>/kerns.json is read
 * and its app added to the app menu (kernel/apps.h). Split out of
 * kernel/lkx.c, which runs them, so the Linux build lists the same apps. */
#include "kernel/apps.h"
#include "kernel/lkx.h"
#include "kernel/printk.h"
#include "kernel/ramdisk.h"
#include "kernel/string.h"

#define MANIFEST_MAX 1024
#define PATH_MAX 56

/* Strings for the registered apps (struct app points into these). */
static struct {
    char name[24], icon[24], path[PATH_MAX];
    int order;              /* "order" in kerns.json: lower comes first (default 100) */
    int width, height, min_w, min_h, pinned;
    uint32_t size;
} found[APPS_MAX];
static int nfound;

static int strncmp_(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i])
            return 1;
        if (!a[i])
            return 0;
    }
    return 0;
}

/* v1's tiny JSON reader: the string value of "key", or 0 if absent. Enough
 * for kerns.json's flat object of strings; escapes aren't supported. */
static int json_str(const char *json, const char *key, char *out, int max)
{
    int klen = (int)strlen(key);
    for (const char *p = json; *p; p++) {
        if (*p != '"' || strncmp_(p + 1, key, klen) || p[1 + klen] != '"')
            continue;
        p += 2 + klen;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':')
            p++;
        if (*p != '"')
            return 0;
        int n = 0;
        for (p++; *p && *p != '"' && n < max - 1; p++)
            out[n++] = *p;
        out[n] = '\0';
        return *p == '"' && n > 0;
    }
    return 0;
}

/* The whole-number value of "key", or `fallback` if absent. */
static int json_int(const char *json, const char *key, int fallback)
{
    int klen = (int)strlen(key);
    for (const char *p = json; *p; p++) {
        if (*p != '"' || strncmp_(p + 1, key, klen) || p[1 + klen] != '"')
            continue;
        p += 2 + klen;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':')
            p++;
        if (*p < '0' || *p > '9')
            return fallback;
        int v = 0;
        while (*p >= '0' && *p <= '9' && v < 100000)
            v = v * 10 + (*p++ - '0');
        return v;
    }
    return fallback;
}

static int ends_with(const char *s, const char *tail)
{
    int n = (int)strlen(s), t = (int)strlen(tail);
    return n >= t && !strcmp(s + n - t, tail);
}

static void register_one(const char *manifest_name)
{
    const void *data;
    uint32_t size;
    char json[MANIFEST_MAX + 1], entry[24];
    if (nfound == APPS_MAX || ramdisk_find(manifest_name, &data, &size) || size > MANIFEST_MAX) {
        kprintf("lkx: %s: unreadable or too big\n", manifest_name);
        return;
    }
    memcpy(json, data, size);
    json[size] = '\0';
    typeof(found[0]) *f = &found[nfound];
    if (!json_str(json, "name", f->name, sizeof(f->name)) ||
        !json_str(json, "entry", entry, sizeof(entry))) {
        kprintf("lkx: %s: needs \"name\" and \"entry\"\n", manifest_name);
        return;
    }
    if (!json_str(json, "icon", f->icon, sizeof(f->icon)))
        f->icon[0] = '\0';
    /* The program sits next to its kerns.json. */
    int dir = (int)strlen(manifest_name) - (int)strlen("kerns.json");
    if (dir + (int)strlen(entry) >= PATH_MAX)
        return;
    memcpy(f->path, manifest_name, (size_t)dir);
    memcpy(f->path + dir, entry, strlen(entry) + 1);
    uint32_t prog_size;
    if (ramdisk_size(f->path, &prog_size)) {          /* read when it's opened */
        kprintf("lkx: %s: its program %s isn't in the ramdisk\n", manifest_name, f->path);
        return;
    }
    f->order = json_int(json, "order", 100);
    f->width = json_int(json, "width", 0);
    f->height = json_int(json, "height", 0);
    f->min_w = json_int(json, "min_width", 0);
    f->min_h = json_int(json, "min_height", 0);
    f->pinned = json_int(json, "pinned", 0);
    f->size = prog_size;
    nfound++;
}

void lkx_register_all(void)
{
    nfound = 0;
    for (int i = 0; i < ramdisk_count(); i++) {
        const char *n = ramdisk_name(i);
        if (n && !strncmp_(n, "apps/", 5) && ends_with(n, "/kerns.json"))
            register_one(n);
    }
    /* By "order", keeping the ramdisk's order for equal ones (insertion sort). */
    int idx[APPS_MAX];
    for (int i = 0; i < nfound; i++) {
        int j = i;
        while (j > 0 && found[idx[j - 1]].order > found[i].order) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = i;
    }
    for (int k = 0; k < nfound; k++) {
        typeof(found[0]) *f = &found[idx[k]];
        struct app a = { .name = f->name, .icon = f->icon, .lkx = f->path, .width = f->width,
                         .height = f->height, .min_width = f->min_w, .min_height = f->min_h,
                         .pinned = f->pinned };
        if (apps_add(&a) == 0)
            kprintf("lkx: %s (%s, %u KB)\n", f->name, f->path, f->size / 1024);
    }
    if (!nfound)
        kprintf("lkx: no apps in the ramdisk\n");
}

