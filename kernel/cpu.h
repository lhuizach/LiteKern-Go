/* LiteKern X — CPUID and model-specific registers. */
#ifndef LKX_CPU_H
#define LKX_CPU_H

#include <stdint.h>

struct int_frame;

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

/* Read an MSR that may not exist on this CPU: 0, or -1 if reading it
 * faulted (#GP) instead of panicking. For probing; known registers use
 * rdmsr(). */
int rdmsr_safe(uint32_t msr, uint64_t *v);

/* Called for a fault in ring 0 before it's treated as a kernel bug: 1 if it
 * was rdmsr_safe()'s, now resumed at its failure path. */
int cpu_fault_fixup(struct int_frame *f);

#endif
