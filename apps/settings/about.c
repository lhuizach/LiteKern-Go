/* LiteKern Go: About Phone. A picture of the phone with the wallpaper on its screen, its
 * name (tap to rename it), the version and build, then the hardware, the software, how
 * it's doing, and who made what. Settings is a Linux program, so it reads /proc and
 * /sys itself. Tap the version seven times. Logged for the tests. */
/* Long values are cut to fit on purpose. */
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include "apps/settings/about.h"
#include <string.h>
#include "kernel/text.h"
#include "kernel/theme.h"
#include "kernel/widget.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/utsname.h>
#include <unistd.h>

#define LKX_GO_VERSION "1.2"
#ifndef LKX_BUILD
#define LKX_BUILD "0"
#endif
#define SIDE 24
#define ROW_H 80
#define CARD_R 28
#define PHONE_W 210
#define PHONE_H 420
#define NAME_FILE "phone-name.txt"
#define EGG_TAPS 7
#define THUMB_W 256
#define THUMB_H 160

static struct gfx_surface *cv;
static int scroll, content_h, press_y, scroll0, dragging, pressed;
static char name[48] = "Galaxy A5";
static struct wg_entry entry;
static int editing, egg_taps, egg;
static int fresh;                       /* renaming, nothing typed yet: the first key replaces it */
static uint32_t thumb[THUMB_W * THUMB_H];
static int have_thumb;

struct row {
    const char *label;
    char value[64];
};
static struct row hardware[6], software[4], status[3];
static int mem_used_pct;
static int version_y, name_y;           /* where they are in the page, for taps */

static const struct theme *T(void) { return theme_get(); }

static int read_text(const char *path, char *out, int room)
{
    FILE *f = fopen(path, "r");
    int n = f ? (int)fread(out, 1, (size_t)room - 1, f) : 0;
    if (f)
        fclose(f);
    out[n > 0 ? n : 0] = 0;
    return n > 0 ? 0 : -1;
}

static long meminfo(const char *key)
{
    char text[2048], *p;
    if (read_text("/proc/meminfo", text, sizeof(text)) || !(p = strstr(text, key)))
        return 0;
    return strtol(p + strlen(key), 0, 10);
}

static void gather(void)
{
    char text[256];
    struct utsname u;
    struct k86_machine m;
    struct k86_net net;
    struct k86_volume vols[4];
    long cores = sysconf(_SC_NPROCESSORS_CONF), mhz = 0, total = meminfo("MemTotal:"), avail = meminfo("MemAvailable:");
    if (!read_text("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", text, sizeof(text)))
        mhz = strtol(text, 0, 10) / 1000;
#ifdef __aarch64__
    hardware[0] = (struct row){ "Model", "Galaxy A5 (2015), SM-A500FU" };
    hardware[2] = (struct row){ "Graphics", "Adreno 306 (drawn by the CPU)" };
    hardware[3] = (struct row){ "Display", "5.0 inch, 720 x 1280" };
    snprintf(hardware[1].value, sizeof(hardware[1].value), "Snapdragon 410, %ld cores, %ld MHz", cores, mhz);
#else
    hardware[0] = (struct row){ "Model", "A virtual machine" };
    hardware[2] = (struct row){ "Graphics", "Drawn by the CPU" };
    snprintf(hardware[3].value, sizeof(hardware[3].value), "%d x %d", cv->w, cv->h);
    hardware[3].label = "Display";
    snprintf(hardware[1].value, sizeof(hardware[1].value), mhz ? "%ld cores, %ld MHz" : "%ld cores", cores, mhz);
#endif
    hardware[1].label = "Processor";
    hardware[4].label = "Memory";
    mem_used_pct = total ? (int)((total - avail) * 100 / total) : 0;
    snprintf(hardware[4].value, sizeof(hardware[4].value), "%ld MB used of %ld MB", (total - avail) / 1024, total / 1024);
    hardware[5].label = "Storage";
    snprintf(hardware[5].value, sizeof(hardware[5].value), "Unknown");
    int nv = k86_volumes(vols, 4);
    for (int i = 0; i < nv && i < 4; i++)
        if (vols[i].supported) {
            snprintf(hardware[5].value, sizeof(hardware[5].value), "%u MB free", vols[i].free_kib / 1024);
            break;
        }

    software[0].label = "LiteKern Go";
    snprintf(software[0].value, sizeof(software[0].value), "%s (Build %s)", LKX_GO_VERSION, LKX_BUILD);
    software[1].label = "Linux";
    snprintf(software[1].value, sizeof(software[1].value), "%s", uname(&u) ? "Unknown" : u.release);
    software[2] = (struct row){ "Started by", "lk2nd, then our own init" };
    software[3].label = "Up for";
    long up = read_text("/proc/uptime", text, sizeof(text)) ? 0 : strtol(text, 0, 10);
    if (up >= 3600)
        snprintf(software[3].value, sizeof(software[3].value), "%ld h %ld min", up / 3600, up / 60 % 60);
    else
        snprintf(software[3].value, sizeof(software[3].value), "%ld min", up / 60);

    status[0].label = "Wi-Fi";
    status[1].label = "IP address";
    status[2].label = "Battery";
    memset(&net, 0, sizeof(net));
    k86_net_status(&net);
    snprintf(status[0].value, sizeof(status[0].value), "%s", net.wifi == K86_NET_UP ? net.ssid : "Not connected");
    snprintf(status[1].value, sizeof(status[1].value), "%s", net.address[0] ? net.address : "None");
    if (k86_machine(&m) == 0 && m.battery >= 0)
        snprintf(status[2].value, sizeof(status[2].value), "%d%%%s", m.battery,
                 m.charging == K86_BATTERY_CHARGING ? ", charging" : m.charging == K86_BATTERY_FULL ? ", full" : "");
    else
        snprintf(status[2].value, sizeof(status[2].value), "No battery");
}

static void name_load(void)
{
    char home[K86_PATH_MAX], got[sizeof(name)];
    int vol = k86_home(home), n;
    /* Into its own buffer: a failed read still hands back a cleared one. */
    n = vol >= 0 ? k86_read(vol, home, NAME_FILE, got, sizeof(got) - 1) : -1;
    if (n > 0) {
        got[n] = 0;
        memcpy(name, got, (size_t)n + 1);
    }
}

static void name_save(void)
{
    char home[K86_PATH_MAX];
    int vol = k86_home(home);
    if (vol >= 0)
        k86_write(vol, home, NAME_FILE, name, (uint32_t)strlen(name));
    k86_logf("settings: phone named %s", name);
}

const char *about_phone_name(void)
{
    static int loaded;
    if (!loaded) {
        loaded = 1;
        name_load();
    }
    return name;
}

void about_open(struct gfx_surface *canvas, const char *wallpaper)
{
    cv = canvas;
    scroll = 0;
    editing = 0;
    egg_taps = 0;
    egg = 0;
    about_phone_name();
    gather();
    have_thumb = wallpaper && wallpaper[0] && k86_wallpaper_thumb(wallpaper, thumb, THUMB_W, THUMB_H) == 0;
    k86_log("settings: about");
}

/* --- the picture of the phone ---------------------------------------------------------------- */

static void draw_phone(int x, int y)
{
    const int w = PHONE_W, h = PHONE_H;
    struct k86_time now;
    char hm[8] = "";
    /* The side keys, then the metal frame, then the black glass on it. */
    gfx_fill_round_rect(cv, x + w - 4, y + 96, 8, 44, 3, 0x9a9da2);
    gfx_fill_round_rect(cv, x - 4, y + 84, 8, 30, 3, 0x9a9da2);
    gfx_fill_round_rect(cv, x - 4, y + 124, 8, 30, 3, 0x9a9da2);
    gfx_fill_round_rect(cv, x, y, w, h, 34, 0x8d9095);
    gfx_fill_round_rect(cv, x + 3, y + 3, w - 6, h - 6, 31, 0xc9ccd1);
    gfx_fill_round_rect(cv, x + 7, y + 7, w - 14, h - 14, 27, 0x17181c);
    /* The screen, with the wallpaper's middle on it and the time. */
    int sx = x + 16, sy = y + 54, sw = w - 32, sh = h - 54 - 66;
    gfx_fill_rect(cv, sx, sy, sw, sh, 0x0b0c10);
    if (have_thumb) {
        int crop = THUMB_H * sw / sh, left = (THUMB_W - crop) / 2;
        for (int j = 0; j < sh; j++) {
            if (sy + j < 0 || sy + j >= cv->h)
                continue;           /* scrolled off */
            uint32_t *row = cv->px + (size_t)(sy + j) * (size_t)cv->stride + sx;
            const uint32_t *src = thumb + (size_t)(j * THUMB_H / sh) * THUMB_W + left;
            for (int i = 0; i < sw; i++)
                row[i] = gfx_mix(0, src[i * crop / sw], 70);    /* dimmed, as the lock screen is */
        }
    }
    if (k86_time(&now) == 0)
        snprintf(hm, sizeof(hm), "%02u:%02u", now.hour, now.minute);
    text_draw(cv, sx + (sw - text_width(hm, TEXT_HEADING)) / 2, sy + 40, hm, TEXT_HEADING, 0xffffff);
    /* The earpiece, the front camera and its sensor, and the home key. */
    gfx_fill_round_rect(cv, x + w / 2 - 26, y + 26, 52, 6, 3, 0x3b3d42);
    gfx_fill_circle(cv, x + w / 2 - 50, y + 29, 5, 0x2a2c33);
    gfx_fill_circle(cv, x + w / 2 + 44, y + 29, 3, 0x2a2c33);
    gfx_fill_round_rect(cv, x + w / 2 - 34, y + h - 46, 68, 24, 12, 0x3e4046);
    gfx_fill_round_rect(cv, x + w / 2 - 32, y + h - 44, 64, 20, 10, 0x24262b);
}

/* --- the page ------------------------------------------------------------------------------ */

static int card(int y, const char *title, const struct row *rows, int n, int bar_row)
{
    const struct theme *t = T();
    int w = cv->w - 2 * SIDE;
    text_draw(cv, SIDE + 8, y - scroll, title, TEXT_BOLD, t->fg);
    y += text_height(TEXT_BOLD) + 10;
    int h = n * ROW_H + (bar_row >= 0 ? 24 : 0);
    gfx_fill_round_rect(cv, SIDE, y - scroll, w, h, CARD_R, t->button_bg);
    for (int i = 0; i < n; i++) {
        int ry = y + i * ROW_H + (bar_row >= 0 && i > bar_row ? 24 : 0) - scroll;
        int ty = ry + (ROW_H - text_height(TEXT_BODY)) / 2;
        if (i)
            gfx_fill_rect(cv, SIDE + 24, ry, w - 48, 1, t->border);
        int lw = text_width(rows[i].label, TEXT_BODY);
        text_draw(cv, SIDE + 24, ty, rows[i].label, TEXT_BODY, t->fg);
        int vw = text_width(rows[i].value, TEXT_BODY), room = w - 48 - lw - 24;
        if (vw > room)
            vw = room;
        text_draw_fit(cv, SIDE + w - 24 - vw, ty, rows[i].value, room, TEXT_BODY, t->fg_dim);
        if (i == bar_row) {                     /* memory: how full */
            int bx = SIDE + 24, bw = w - 48, by = ry + ROW_H - 10;
            gfx_fill_round_rect(cv, bx, by, bw, 12, 6, t->button_active);
            gfx_fill_round_rect(cv, bx, by, bw * mem_used_pct / 100 > 12 ? bw * mem_used_pct / 100 : 12, 12, 6, t->accent_bg);
        }
    }
    return y + h + 36;
}

void about_draw(void)
{
    const struct theme *t = T();
    int y = 28, cx = cv->w / 2;
    gfx_fill_rect(cv, 0, 0, cv->w, cv->h, t->window_bg);
    draw_phone(cx - PHONE_W / 2, y - scroll);
    y += PHONE_H + 28;
    name_y = y;
    if (editing) {
        entry.r = (struct gfx_rect){ SIDE + 40, y - scroll, cv->w - 2 * SIDE - 80, t->row_h };
        entry.under = t->window_bg;
        wg_entry_draw(cv, &entry);
        y += t->row_h + 8;
        const char *hint = "Enter to save, Back to cancel";
        text_draw(cv, cx - text_width(hint, TEXT_SMALL) / 2, y - scroll, hint, TEXT_SMALL, t->fg_dim);
        y += text_height(TEXT_SMALL) + 18;
    } else {
        text_draw(cv, cx - text_width(name, TEXT_HEADING) / 2, y - scroll, name, TEXT_HEADING, t->fg);
        y += text_height(TEXT_HEADING) + 4;
        const char *hint = "Tap the name to change it";
        text_draw(cv, cx - text_width(hint, TEXT_SMALL) / 2, y - scroll, hint, TEXT_SMALL, t->fg_dim);
        y += text_height(TEXT_SMALL) + 18;
    }
    char ver[64];
    snprintf(ver, sizeof(ver), "LiteKern Go %s  " "\xb7" "  Build %s", LKX_GO_VERSION, LKX_BUILD);
    text_draw(cv, cx - text_width(ver, TEXT_BOLD) / 2, y - scroll, ver, TEXT_BOLD, t->accent);
    y += text_height(TEXT_BOLD) + 36;

    y = card(y, "This phone", hardware, 6, 4);
    version_y = y + text_height(TEXT_BOLD) + 10;
    y = card(y, "Software", software, 4, -1);
    y = card(y, "Right now", status, 3, -1);

    static const char *const credits[] = {
        "Made for my brother, from scratch, by hand.",
        "Built on Linux, lk2nd (msm8916-mainline),",
        "Buildroot, libcurl and OpenSSL. Type in",
        "Ubuntu Sans and DejaVu Serif. Camera",
        "registers from NVIDIA's Tegra driver.",
    };
    text_draw(cv, SIDE + 8, y - scroll, "Credits", TEXT_BOLD, t->fg);
    y += text_height(TEXT_BOLD) + 10;
    for (unsigned i = 0; i < sizeof(credits) / sizeof(credits[0]); i++, y += text_height(TEXT_BODY) + 4)
        text_draw(cv, SIDE + 8, y - scroll, credits[i], TEXT_BODY, t->fg_dim);
    y += 40;
    content_h = y;

    if (egg) {                                  /* found it */
        const char *a = "You found the egg!", *b = "Hi from Zach and Claude";
        int bw = 560, bh = 200, bx = cx - bw / 2, by = cv->h / 2 - bh / 2;
        gfx_fill_round_rect(cv, bx, by, bw, bh, 40, t->accent_bg);
        text_draw(cv, cx - text_width(a, TEXT_HEADING) / 2, by + 48, a, TEXT_HEADING, t->accent_fg);
        text_draw(cv, cx - text_width(b, TEXT_BODY) / 2, by + 118, b, TEXT_BODY, t->accent_fg);
    }
    k86_present(0, 0, cv->w, cv->h);
}

static void set_scroll(int s)
{
    int most = content_h - cv->h;
    scroll = s < 0 ? 0 : s > (most > 0 ? most : 0) ? (most > 0 ? most : 0) : s;
}

void about_pointer(const struct k86_event *ev)
{
    struct wg_pointer p = wg_pointer_make(ev->x, ev->y, ev->buttons, ev->changed, ev->time_ms);
    if (editing) {
        wg_entry_pointer(&entry, &p);
        about_draw();
        return;
    }
    if (p.down) {
        press_y = p.y;
        scroll0 = scroll;
        dragging = 0;
        pressed = 1;
        if (egg) {
            egg = 0;
            about_draw();
            pressed = 0;
        }
        return;
    }
    if (!pressed)
        return;
    if (p.held) {
        if (!dragging && (p.y - press_y > 20 || press_y - p.y > 20))
            dragging = 1;
        if (dragging) {
            set_scroll(scroll0 - (p.y - press_y));
            about_draw();
        }
        return;
    }
    pressed = 0;
    if (dragging)
        return;
    int y = p.y + scroll;
    if (y >= name_y - 10 && y < name_y + text_height(TEXT_HEADING) + 30) {
        editing = 1;
        fresh = 1;
        wg_entry_set(&entry, name);
        entry.focused = 1;
        about_draw();
    } else if (y >= version_y && y < version_y + ROW_H) {
        if (++egg_taps >= EGG_TAPS) {
            egg_taps = 0;
            egg = 1;
            k86_log("settings: easter egg");
        }
        about_draw();
    }
}

int about_key(const struct key_event *k)
{
    if (!editing)
        return 0;
    if (k->pressed && fresh && (k->key == KEY_BACKSPACE || (k->ascii >= 32 && k->ascii != 127))) {
        fresh = 0;
        wg_entry_set(&entry, "");
        if (k->key == KEY_BACKSPACE) {
            about_draw();
            return 1;
        }
    }
    if (k->pressed && k->key == KEY_ESC) {
        editing = 0;
    } else if (wg_entry_key(&entry, k) == WG_ENTRY_ACTIVATE) {
        if (entry.len) {
            memcpy(name, entry.text, (size_t)entry.len + 1);
            name_save();
        }
        editing = 0;
    }
    about_draw();
    return 1;
}

int about_editing(void)
{
    return editing;
}
