/* LiteKern X on Linux — apps (kernel/lkx.h). Phase L1: they're listed (the
 * app menu and dock look as they do on the EeePC: kernel/appreg.c reads
 * their kerns.json) but don't run yet; phase L2 runs them as Linux
 * processes. */
#include "kernel/lkx.h"
#include "kernel/apps.h"
#include "kernel/errno.h"
#include "kernel/printk.h"

int lkx_start(const struct app *a, struct wm_window *w)
{
    (void)w;
    kprintf("lkx: %s: apps run on Linux from phase L2\n", a->name);
    return -ENOSYS;
}

int lkx_schedule(void)
{
    return 0;
}

struct wm_window *lkx_window(void)
{
    return 0;
}

int lkx_set_timer(uint32_t ms)
{
    (void)ms;
    return -ENOSYS;
}

int lkx_set_memory(uint32_t bytes)
{
    (void)bytes;
    return -ENOSYS;
}

int lkx_running(const struct app *a)
{
    (void)a;
    return 0;
}
