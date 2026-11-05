/* LiteKern X on Linux — memory, time, power, the log's output and the bits
 * of hardware the kernel code still names (platform/platform.h). */
#define _GNU_SOURCE                 /* memfd_create */
#include "platform/platform.h"
#include "kernel/pci.h"
#include "kernel/pmm.h"
#include "kernel/power.h"
#include "kernel/printk.h"
#include "kernel/serial.h"
#include "kernel/timing.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/reboot.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

/* --- pages (kernel/pmm.h) ------------------------------------------------------------------------
 * Each allocation is a memory file (memfd), mapped shared: a window's pixels
 * can then be handed to its app (platform/lkx.c), as the kernel maps them
 * into the app's address space. The kernel frees page by page; the file
 * goes when the last of its pages does. */

#define REGIONS 4096
static struct region {
    uintptr_t start;
    uint32_t pages, left;
    int fd;
} regions[REGIONS];

uintptr_t pmm_alloc(void)
{
    return pmm_alloc_contiguous(1);
}

uintptr_t pmm_alloc_contiguous(uint32_t count)
{
    if (!count)
        return 0;
    struct region *r = 0;
    for (int i = 0; i < REGIONS && !r; i++)
        if (!regions[i].pages)
            r = &regions[i];
    size_t bytes = (size_t)count * PAGE_SIZE;
    int fd = r ? memfd_create("lkx-pages", MFD_CLOEXEC) : -1;
    if (fd < 0 || ftruncate(fd, (off_t)bytes)) {
        if (fd >= 0)
            close(fd);
        return 0;
    }
    void *p = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        return 0;
    }
    *r = (struct region){ (uintptr_t)p, count, count, fd };
    return (uintptr_t)p;
}

void pmm_free(uintptr_t phys)
{
    if (!phys)
        return;
    munmap((void *)phys, PAGE_SIZE);
    for (int i = 0; i < REGIONS; i++) {
        struct region *r = &regions[i];
        if (r->pages && phys >= r->start && phys - r->start < r->pages * PAGE_SIZE) {
            if (!--r->left) {
                close(r->fd);
                r->pages = 0;
            }
            return;
        }
    }
}

int plat_memory_fd(uintptr_t start)
{
    for (int i = 0; i < REGIONS; i++)
        if (regions[i].pages && regions[i].start == start)
            return regions[i].fd;
    return -1;
}

uint32_t pmm_free_frames(void)
{
    struct sysinfo si;
    return sysinfo(&si) ? 0 : (uint32_t)((uint64_t)si.freeram * si.mem_unit / PAGE_SIZE);
}

uint32_t pmm_total_frames(void)
{
    struct sysinfo si;
    return sysinfo(&si) ? 0 : (uint32_t)((uint64_t)si.totalram * si.mem_unit / PAGE_SIZE);
}

uint32_t pmm_direct_map_end(void)
{
    return 0;
}

/* --- time (kernel/timing.h) -------------------------------------------------------------------- */

static uint64_t start_ns;
static uint32_t mhz;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static uint64_t tsc(void)
{
#if defined(__i386__) || defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
#else
    return now_ns();                        /* ARM: the clock itself (a "1000 MHz" TSC) */
#endif
}

/* The screen saver times its frames in TSC ticks: measure their rate once. */
void timing_init(const struct boot_info *bi)
{
    (void)bi;
    start_ns = now_ns();
    uint64_t t0 = tsc(), n0 = now_ns();
    struct timespec ten = { 0, 10000000 };
    nanosleep(&ten, 0);
    uint64_t dt = tsc() - t0, dn = now_ns() - n0;
    mhz = dn ? (uint32_t)(dt * 1000 / dn) : 1000;
    if (!mhz)
        mhz = 1000;
}

uint32_t tsc_mhz(void)
{
    return mhz;
}

uint32_t uptime_ms(void)
{
    return (uint32_t)((now_ns() - start_ns) / 1000000u);
}

uint32_t tsc_to_ms(uint64_t ticks)
{
    return (uint32_t)(ticks / ((uint64_t)mhz * 1000));
}

void boot_phase(const char *name, uint64_t start, uint64_t end)
{
    (void)name, (void)start, (void)end;
}

void boot_report(uint64_t ready)
{
    (void)ready;
    kprintf("[boot] ready t=%u\n", uptime_ms());
}

/* --- power (kernel/power.h): on the EeePC (LKX_TARGET, set by lkx-start)
 * Linux restarts or switches off, after writing everything out; on the PC
 * the window just closes. ------------------------------------------------------------------- */

int power_can_off(void)
{
    return 1;
}

static void __attribute__((noreturn)) power(int how, const char *what)
{
    if (!getenv("LKX_TARGET")) {
        kprintf("power: %s (the PC build exits)\n", what);
        plat_exit(0);
    }
    kprintf("power: %s\n", what);
    fflush(stdout);
    sync();
    reboot(how);
    kprintf("power: Linux refused (%s)\n", what);
    plat_exit(1);
}

void power_restart(void)
{
    power(RB_AUTOBOOT, "restart");
}

void power_off(void)
{
    power(RB_POWER_OFF, "off");
}

/* --- the log's other copy (kernel/serial.h): standard output, as the VMs' serial port ---------- */

void serial_init(void)
{
    setvbuf(stdout, 0, _IOLBF, 0);
}

void serial_putc(char c)
{
    putchar(c);
}

/* --- no PCI bus to scan here (Linux has the hardware) ------------------------------------------ */

struct pci_device pci_devices[PCI_MAX_DEVICES];
uint32_t pci_device_count;
