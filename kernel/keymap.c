/* LiteKern X — keyboard layouts (kernel/keymap.h). */
#include "kernel/keymap.h"
#include "kernel/string.h"

/* Scancode set 1 make codes 0x00-0x56 (0x56: the key left of Z on ISO
 * keyboards). Each layout: plain, shifted, and AltGr (right Alt; 0: none).
 * The fonts are ASCII, so AZERTY's accented keys type the plain letter
 * (é, è -> e; ç -> c; à -> a; ù -> u) and ², £, µ, §, ° and € type nothing. */
#define MAP_LEN 0x57
struct layout {
    const char *name;
    char plain[MAP_LEN], shifted[MAP_LEN], altgr[MAP_LEN];
};

static const struct layout qwerty = {
    "qwerty",
    "\0\x1b" "1234567890-=\b"
    "\tqwertyuiop[]\n"
    "\0asdfghjkl;'`"
    "\0\\zxcvbnm,./\0*\0 ",
    "\0\x1b" "!@#$%^&*()_+\b"
    "\tQWERTYUIOP{}\n"
    "\0ASDFGHJKL:\"~"
    "\0|ZXCVBNM<>?\0*\0 ",
    { 0 },
};

/* French AZERTY, as on the EeePC's French keyboard. */
static const struct layout azerty = {
    "azerty",
    { [0x01] = 0x1b, '&', 'e', '"', '\'', '(', '-', 'e', '_', 'c', 'a', ')', '=', '\b',
      '\t', 'a', 'z', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '^', '$', '\n',
      [0x1e] = 'q', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 'm', 'u', 0,
      [0x2b] = '*', 'w', 'x', 'c', 'v', 'b', 'n', ',', ';', ':', '!',
      [0x37] = '*', [0x39] = ' ', [0x56] = '<' },
    { [0x01] = 0x1b, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 0, '+', '\b',
      '\t', 'A', 'Z', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 0, 0, '\n',
      [0x1e] = 'Q', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 'M', '%', 0,
      [0x2b] = 0, 'W', 'X', 'C', 'V', 'B', 'N', '?', '.', '/', 0,
      [0x37] = '*', [0x39] = ' ', [0x56] = '>' },
    { [0x03] = '~', '#', '{', '[', '|', '`', '\\', '^', '@', ']', '}' },
};

static const struct layout *map = &qwerty;
static uint8_t mods, lshift, rshift, lctrl, rctrl, lalt, ralt;
static int extended, skip;

void keymap_select(const char *name)
{
    map = name && !strcmp(name, "azerty") ? &azerty : &qwerty;
}

const char *keymap_name(void)
{
    return map->name;
}

static void update_mods(uint16_t key, int pressed)
{
    switch (key) {
    case KEY_LSHIFT: lshift = (uint8_t)pressed; break;
    case KEY_RSHIFT: rshift = (uint8_t)pressed; break;
    case KEY_LCTRL:  lctrl = (uint8_t)pressed; break;
    case KEY_RCTRL:  rctrl = (uint8_t)pressed; break;
    case KEY_LALT:   lalt = (uint8_t)pressed; break;
    case KEY_RALT:   ralt = (uint8_t)pressed; break;
    case KEY_CAPSLOCK:
        if (pressed)
            mods ^= MOD_CAPSLOCK;
        break;
    }
    mods = (uint8_t)((mods & MOD_CAPSLOCK) | ((lshift || rshift) ? MOD_SHIFT : 0) |
                     ((lctrl || rctrl) ? MOD_CTRL : 0) | ((lalt || ralt) ? MOD_ALT : 0));
}

struct key_event keymap_event(uint16_t key, int pressed)
{
    update_mods(key, pressed);
    struct key_event ev = { .key = key, .pressed = (uint8_t)pressed, .mods = mods };
    if (!(key & KEY_EXTENDED) && key < MAP_LEN) {
        char c = map->plain[key];
        /* a letter: its shifted one is its capital (not AZERTY's é, whose is 2) */
        int is_letter = c >= 'a' && c <= 'z' && map->shifted[key] == c - 32;
        int shift = (mods & MOD_SHIFT) != 0;
        if (is_letter && (mods & MOD_CAPSLOCK))
            shift = !shift;
        ev.ascii = (uint8_t)(ralt && map->altgr[key] ? map->altgr[key] : shift ? map->shifted[key] : c);
        if (is_letter && (mods & MOD_CTRL) && !ralt)
            ev.ascii = (uint8_t)(c - 'a' + 1);
    }
    return ev;
}

int keymap_scancode(uint8_t sc, struct key_event *out)
{
    if (skip) {                     /* rest of the 6-byte Pause sequence */
        skip--;
        return 0;
    }
    if (sc == 0xe1) {
        skip = 5;
        return 0;
    }
    if (sc == 0xe0) {
        extended = 1;
        return 0;
    }
    if (sc == 0xfa || sc == 0xfe || sc == 0x00 || sc == 0xff)
        return 0;                   /* ACK / resend / errors, not keys */

    uint16_t key = (uint16_t)((sc & 0x7f) | (extended ? KEY_EXTENDED : 0));
    int pressed = !(sc & 0x80);
    extended = 0;
    if (key == (KEY_EXTENDED | 0x2a) || key == (KEY_EXTENDED | 0x36))
        return 0;                   /* fake shifts around PrtSc etc. */
    *out = keymap_event(key, pressed);
    return 1;
}
