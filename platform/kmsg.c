/* LiteKern X on Linux — Linux's own messages in the Log (phase L4), read from
 * /dev/kmsg, each logged as "linux: <message>".
 *
 * At start, what Linux said while booting, but only its warnings and errors
 * (all of it would be hundreds of lines about every bus and driver); after
 * that, as they come, its ordinary news too: a USB stick or mouse plugged
 * in, Wi-Fi joining. Only on the EeePC (LKX_TARGET): on the PC, the PC's
 * Linux isn't ours to show. */
#include "kernel/printk.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BOOT_LEVEL 4                /* at start: warnings (4) and worse */
#define LIVE_LEVEL 6                /* afterwards: news (6) and worse; not debug (7) */

static int fd = -1;

/* One record: "level,seq,usec,flags;text" (level: the low 3 bits). */
static void take(char *rec, int most)
{
    char *semi = strchr(rec, ';');
    if (!semi || (atoi(rec) & 7) > most)
        return;
    char *text = semi + 1;
    text[strcspn(text, "\n")] = '\0';
    if (*text)
        kprintf("linux: %s\n", text);
}

static void drain(int most)
{
    char rec[1024];
    for (;;) {
        ssize_t n = read(fd, rec, sizeof(rec) - 1);
        if (n < 0) {
            if (errno == EPIPE)
                continue;           /* some were overwritten before we read them: go on */
            return;                 /* nothing more (EAGAIN) */
        }
        rec[n] = '\0';
        take(rec, most);
    }
}

void kmsg_init(void)
{
    if (!getenv("LKX_TARGET"))
        return;
    fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return;
    kprintf("linux: (Linux's warnings while starting; its news from now on)\n");
    drain(BOOT_LEVEL);
}

void kmsg_poll(void)
{
    if (fd >= 0)
        drain(LIVE_LEVEL);
}
