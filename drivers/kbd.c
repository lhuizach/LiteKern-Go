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
#include "kernel/keymap.h"
#include "kernel/printk.h"
#include "defaults.h"

#define KBD_IRQ     1
#define QUEUE_SIZE  64          /* power of two */
#define SC_ENABLE_SCANNING 0xf4

static struct key_event queue[QUEUE_SIZE];
static volatile uint32_t head, tail;    /* head: next write (IRQ), tail: next read */
static uint32_t dropped;

static void push(struct key_event ev)
{
    if (head - tail == QUEUE_SIZE) {    /* full: drop the oldest */
        tail++;
        dropped++;
    }
    queue[head % QUEUE_SIZE] = ev;
    head++;
}

static void kbd_irq(void *ctx)
{
    (void)ctx;
    uint8_t status = inb(I8042_STATUS);
    if (!(status & I8042_STATUS_OUT) || (status & I8042_STATUS_AUX))
        return;                     /* nothing for us (port 2 data is the mouse's) */
    struct key_event ev;
    if (keymap_scancode(inb(I8042_DATA), &ev))
        push(ev);
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
    keymap_select(DEFAULT_KEYBOARD);
    err = irq_register(KBD_IRQ, kbd_irq, 0);
    if (err) {
        i8042_disable_port(1);
        return err;
    }
    kprintf("kbd: layout %s\n", keymap_name());
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
