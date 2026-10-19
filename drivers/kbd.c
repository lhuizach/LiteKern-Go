/* LiteKern X — PS/2 keyboard driver (Phase 1 §6). Legacy device "kbd0" on
 * i8042 port 1, IRQ 1.
 *
 * The controller translates to scancode set 1. The IRQ handler decodes each
 * byte into a struct key_event (kernel/input.h) and queues it; read() drains
 * the queue without blocking. If the queue overflows, the oldest events are
 * dropped and counted (KBD_GET_DROPPED).
 *
 *   read   struct key_event records; len must be a multiple of their size
 *   write  -ENOSYS
 *   ioctl  KBD_GET_DROPPED (uint32_t *)
 *   shutdown  masks IRQ 1 and disables the port */
#include "drivers/builtin.h"
#include "drivers/i8042.h"
#include "kernel/errno.h"
#include "kernel/input.h"
#include "kernel/io.h"
#include "kernel/irq.h"
#include "kernel/printk.h"
#include "kernel/string.h"
#include "defaults.h"

#define KBD_IRQ     1
#define QUEUE_SIZE  64          /* power of two */
#define SC_ENABLE_SCANNING 0xf4

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

static struct key_event queue[QUEUE_SIZE];
static volatile uint32_t head, tail;    /* head: next write (IRQ), tail: next read */
static uint32_t dropped;
static int extended, skip;

static void push(uint16_t key, int pressed)
{
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
    if (head - tail == QUEUE_SIZE) {    /* full: drop the oldest */
        tail++;
        dropped++;
    }
    queue[head % QUEUE_SIZE] = ev;
    head++;
}

static void update_mods(uint16_t key, int pressed)
{
    switch (key) {
    case KEY_LSHIFT: lshift = pressed; break;
    case KEY_RSHIFT: rshift = pressed; break;
    case KEY_LCTRL:  lctrl = pressed; break;
    case KEY_RCTRL:  rctrl = pressed; break;
    case KEY_LALT:   lalt = pressed; break;
    case KEY_RALT:   ralt = pressed; break;
    case KEY_CAPSLOCK:
        if (pressed)
            mods ^= MOD_CAPSLOCK;
        break;
    }
    mods = (mods & MOD_CAPSLOCK) | ((lshift || rshift) ? MOD_SHIFT : 0) |
           ((lctrl || rctrl) ? MOD_CTRL : 0) | ((lalt || ralt) ? MOD_ALT : 0);
}

static void kbd_irq(void *ctx)
{
    (void)ctx;
    uint8_t status = inb(I8042_STATUS);
    if (!(status & I8042_STATUS_OUT) || (status & I8042_STATUS_AUX))
        return;                     /* nothing for us (port 2 data is the mouse's) */
    uint8_t sc = inb(I8042_DATA);

    if (skip) {                     /* rest of the 6-byte Pause sequence */
        skip--;
        return;
    }
    if (sc == 0xe1) {
        skip = 5;
        return;
    }
    if (sc == 0xe0) {
        extended = 1;
        return;
    }
    if (sc == 0xfa || sc == 0xfe || sc == 0x00 || sc == 0xff)
        return;                     /* ACK / resend / errors, not keys */

    uint16_t key = (sc & 0x7f) | (extended ? KEY_EXTENDED : 0);
    int pressed = !(sc & 0x80);
    extended = 0;
    if (key == (KEY_EXTENDED | 0x2a) || key == (KEY_EXTENDED | 0x36))
        return;                     /* fake shifts around PrtSc etc. */
    update_mods(key, pressed);
    push(key, pressed);
}

static int kbd_init(device_t *dev)
{
    (void)dev;
    int err = i8042_enable_port(1);
    if (err)
        return err;
    /* The BIOS normally leaves scanning on; make sure, without a slow reset. */
    if (i8042_device_cmd(1, SC_ENABLE_SCANNING)) {
        i8042_disable_port(1);
        return -ENODEV;             /* no keyboard answering on port 1 */
    }
    if (!strcmp(DEFAULT_KEYBOARD, "azerty"))
        map = &azerty;
    err = irq_register(KBD_IRQ, kbd_irq, 0);
    if (err) {
        i8042_disable_port(1);
        return err;
    }
    kprintf("kbd: layout %s\n", map->name);
    return 0;
}

static int kbd_read(device_t *dev, void *buf, size_t len)
{
    (void)dev;
    if (len % sizeof(struct key_event))
        return -EINVAL;
    struct key_event *out = buf;
    size_t n = 0;
    uint32_t flags = irq_save();            /* the IRQ handler moves tail on overflow */
    while (n < len / sizeof(struct key_event) && tail != head) {
        out[n++] = queue[tail % QUEUE_SIZE];
        tail++;
    }
    irq_restore(flags);
    return (int)(n * sizeof(struct key_event));
}

static int kbd_ioctl(device_t *dev, unsigned cmd, void *arg)
{
    (void)dev;
    if (cmd != KBD_GET_DROPPED)
        return -ENOSYS;
    if (!arg)
        return -EINVAL;
    *(uint32_t *)arg = dropped;
    return 0;
}

static void kbd_shutdown(device_t *dev)
{
    (void)dev;
    irq_unregister(KBD_IRQ);
    i8042_disable_port(1);
}

const driver_t kbd_driver = {
    .name = "ps2kbd",
    .pci_ids = NULL,
    .init = kbd_init,
    .read = kbd_read,
    .write = driver_nosys_write,
    .ioctl = kbd_ioctl,
    .shutdown = kbd_shutdown,
};
