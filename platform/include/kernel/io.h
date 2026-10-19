/* LiteKern X on Linux — kernel/io.h for a Linux program. platform/Makefile
 * puts platform/include before the repository root, so the kernel code's
 * #include "kernel/io.h" finds this one: no port I/O or interrupt flags in
 * user space (the instructions would fault); the TSC is still there. */
#ifndef LKX_IO_H
#define LKX_IO_H

#include <stdint.h>
#include <stdlib.h>

static inline void outb(uint16_t port, uint8_t v) { (void)port, (void)v; }
static inline uint8_t inb(uint16_t port) { (void)port; return 0xff; }
static inline void outw(uint16_t port, uint16_t v) { (void)port, (void)v; }
static inline uint16_t inw(uint16_t port) { (void)port; return 0xffff; }
static inline void outl(uint16_t port, uint32_t v) { (void)port, (void)v; }
static inline uint32_t inl(uint16_t port) { (void)port; return 0xffffffffu; }

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

static inline uint32_t read_cr2(void) { return 0; }
static inline uint32_t irq_save(void) { return 0; }
static inline void irq_restore(uint32_t flags) { (void)flags; }

static inline __attribute__((noreturn)) void halt_forever(void)
{
    abort();                        /* panic(): stop here, so it's seen */
}

#endif
