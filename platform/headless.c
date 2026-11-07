/* LiteKern X on Linux — the headless backend: no window. Input comes from a
 * script in the tests' key language (tests/lib/qemu_monitor.py), so the same
 * KEYS strings drive the kernel in QEMU and the desktop here:
 *
 *   move:DX,DY   the pointer, relative       press:BUTTONS (1 left, 2 right,
 *   release      buttons up                  4 middle)
 *   sleep:S      wait S seconds              gap:S  the pause after each step
 *   NAME, MOD-NAME  a key, QEMU's names: a, 1, ret, esc, backspace, spc,
 *                shift-a, ctrl-c, meta_l-l, alt_r-0, up, f5 ...
 *   shot:FILE    a PNG of the screen         quit   exit
 *
 * The script starts once the desktop is ready (plat_ready()). */
#include "platform/platform.h"
#include "kernel/input.h"
#include "kernel/printk.h"

#include <stdlib.h>
#include <string.h>
#include "kernel/timing.h"

static char *script, *at;
static uint32_t next_ms;        /* uptime when the next step may run */
static uint32_t gap_ms = 150;
static int sw, sh, x, y;
static uint8_t buttons;

void headless_script(const char *s)
{
    free(script);
    script = strdup(s ? s : "");
    at = script;
}

static const struct { const char *name; uint16_t key; } names[] = {
    { "esc", 0x01 }, { "1", 0x02 }, { "2", 0x03 }, { "3", 0x04 }, { "4", 0x05 }, { "5", 0x06 },
    { "6", 0x07 }, { "7", 0x08 }, { "8", 0x09 }, { "9", 0x0a }, { "0", 0x0b }, { "minus", 0x0c },
    { "equal", 0x0d }, { "backspace", 0x0e }, { "tab", 0x0f }, { "q", 0x10 }, { "w", 0x11 },
    { "e", 0x12 }, { "r", 0x13 }, { "t", 0x14 }, { "y", 0x15 }, { "u", 0x16 }, { "i", 0x17 },
    { "o", 0x18 }, { "p", 0x19 }, { "bracket_left", 0x1a }, { "bracket_right", 0x1b },
    { "ret", 0x1c }, { "ctrl", 0x1d }, { "a", 0x1e }, { "s", 0x1f }, { "d", 0x20 }, { "f", 0x21 },
    { "g", 0x22 }, { "h", 0x23 }, { "j", 0x24 }, { "k", 0x25 }, { "l", 0x26 },
    { "semicolon", 0x27 }, { "apostrophe", 0x28 }, { "grave_accent", 0x29 }, { "shift", 0x2a },
    { "backslash", 0x2b }, { "z", 0x2c }, { "x", 0x2d }, { "c", 0x2e }, { "v", 0x2f },
    { "b", 0x30 }, { "n", 0x31 }, { "m", 0x32 }, { "comma", 0x33 }, { "dot", 0x34 },
    { "slash", 0x35 }, { "shift_r", 0x36 }, { "asterisk", 0x37 }, { "alt", 0x38 },
    { "spc", 0x39 }, { "caps_lock", 0x3a }, { "f1", 0x3b }, { "f2", 0x3c }, { "f3", 0x3d },
    { "f4", 0x3e }, { "f5", 0x3f }, { "f6", 0x40 }, { "f7", 0x41 }, { "f8", 0x42 }, { "f9", 0x43 },
    { "f10", 0x44 }, { "less", 0x56 }, { "f11", 0x57 }, { "f12", 0x58 },
    { "ctrl_r", KEY_RCTRL }, { "alt_r", KEY_RALT }, { "up", KEY_UP }, { "down", KEY_DOWN },
    { "left", KEY_LEFT }, { "right", KEY_RIGHT }, { "home", KEY_HOME }, { "end", KEY_END },
    { "pgup", KEY_PAGEUP }, { "pgdn", KEY_PAGEDOWN }, { "insert", KEY_INSERT },
    { "delete", KEY_DELETE }, { "meta_l", KEY_LSUPER }, { "meta_r", KEY_RSUPER },
    { "kp_enter", KEY_EXTENDED | 0x1c },
    /* LiteKern Go: the phone's keys ("esc" stays the PC's Esc) */
    { "phone_back", KEY_PHONE_BACK },
    { "phone_home", KEY_PHONE_HOME }, { "phone_recents", KEY_PHONE_RECENTS },
    { "phone_power", KEY_PHONE_POWER }, { "vol_up", KEY_VOLUME_UP }, { "vol_down", KEY_VOLUME_DOWN },
};

static uint16_t key_named(const char *s, size_t n)
{
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strlen(names[i].name) == n && !strncmp(names[i].name, s, n))
            return names[i].key;
    return 0;
}

/* "shift-a", "meta_l-l", "ctrl-alt-delete": each part down, then up in reverse. */
static void keys(const char *tok)
{
    uint16_t held[4];
    int n = 0;
    for (const char *p = tok; *p && n < 4;) {
        const char *dash = strchr(p, '-');
        size_t len = dash ? (size_t)(dash - p) : strlen(p);
        uint16_t k = key_named(p, len);
        if (!k) {
            kprintf("headless: unknown key \"%s\"\n", tok);
            break;
        }
        plat_key(k, 1);
        held[n++] = k;
        if (!dash)
            break;
        p = dash + 1;
    }
    while (n--)
        plat_key(held[n], 0);
}

/* Run one step; returns its pause in ms. */
static uint32_t step(const char *tok)
{
    if (!strncmp(tok, "sleep:", 6))
        return (uint32_t)(atof(tok + 6) * 1000);
    if (!strncmp(tok, "gap:", 4)) {
        gap_ms = (uint32_t)(atof(tok + 4) * 1000);
        return 0;
    }
    if (!strncmp(tok, "move:", 5)) {
        int dx = 0, dy = 0;
        dx = atoi(tok + 5);
        const char *c = strchr(tok, ',');
        if (c)
            dy = atoi(c + 1);
        x += dx, y += dy;
        x = x < 0 ? 0 : x >= sw ? sw - 1 : x;
        y = y < 0 ? 0 : y >= sh ? sh - 1 : y;
        plat_pointer(x, y, buttons);
    } else if (!strncmp(tok, "press:", 6)) {
        buttons = (uint8_t)atoi(tok + 6);
        plat_pointer(x, y, buttons);
    } else if (!strncmp(tok, "wheel:", 6)) {        /* notches, > 0 down */
        plat_wheel(atoi(tok + 6));
    } else if (!strcmp(tok, "release")) {
        buttons = 0;
        plat_pointer(x, y, buttons);
    } else if (!strncmp(tok, "shot:", 5)) {
        if (plat_screenshot(tok + 5))
            kprintf("headless: couldn't write %s\n", tok + 5);
        else
            kprintf("headless: screenshot %s\n", tok + 5);
    } else if (!strncmp(tok, "keydown:", 8) || !strncmp(tok, "keyup:", 6)) {    /* a held key */
        int down = tok[3] == 'd';
        const char *name = tok + (down ? 8 : 6);
        uint16_t k = key_named(name, strlen(name));
        if (k)
            plat_key(k, down);
        else
            kprintf("headless: unknown key \"%s\"\n", name);
    } else if (!strcmp(tok, "quit")) {
        plat_exit(0);
    } else {
        keys(tok);
    }
    return gap_ms;
}

static int h_open(int *w, int *h)
{
    sw = *w, sh = *h;
    x = sw / 2, y = sh / 2;           /* where the desktop's pointer starts */
    return 0;
}

static void h_present(const uint32_t *px, int stride, int x0, int y0, int w, int h)
{
    (void)px, (void)stride, (void)x0, (void)y0, (void)w, (void)h;
}

static int h_fd(void)
{
    return -1;
}

static int h_due(void)
{
    if (!plat_ready() || !at || !*at)
        return -1;
    int32_t left = (int32_t)(next_ms - uptime_ms());
    return left < 0 ? 0 : left;
}

/* The next step, if it's due. */
static void h_pump(void)
{
    if (h_due() != 0)
        return;
    while (*at == ' ' || *at == '\n' || *at == '\t')
        at++;
    if (!*at)
        return;
    char tok[256];
    size_t n = strcspn(at, " \n\t");
    if (n >= sizeof(tok))
        n = sizeof(tok) - 1;
    memcpy(tok, at, n);
    tok[n] = '\0';
    at += n;
    next_ms = uptime_ms() + step(tok);
}

static void h_close(void)
{
}

const struct backend backend_headless = { "headless", h_open, h_present, h_fd, h_due, h_pump, h_close, 0, 0 };
