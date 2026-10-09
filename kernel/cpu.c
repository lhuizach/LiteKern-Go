/* LiteKern X — MSR reads that survive a missing register (kernel/cpu.h).
 *
 * rdmsr of a register the CPU doesn't have raises #GP. rdmsr_raw marks its
 * one rdmsr instruction (msr_try); the fault handler sees a #GP there and
 * resumes at msr_fault, which returns -1. Nothing else is ever resumed. */
#include "kernel/cpu.h"
#include "kernel/idt.h"

#define VECTOR_GP 13

int rdmsr_raw(uint32_t msr, uint32_t *lo, uint32_t *hi);
extern const char msr_try[], msr_fault[];

/* cdecl: (msr, lo *, hi *) -> 0, or -1 if the rdmsr faulted. */
__asm__(".text\n"
        ".globl rdmsr_raw\n"
        "rdmsr_raw:\n"
        "    mov 4(%esp), %ecx\n"
        ".globl msr_try\n"
        "msr_try:\n"
        "    rdmsr\n"
        "    mov 8(%esp), %ecx\n"
        "    mov %eax, (%ecx)\n"
        "    mov 12(%esp), %ecx\n"
        "    mov %edx, (%ecx)\n"
        "    xor %eax, %eax\n"
        "    ret\n"
        ".globl msr_fault\n"
        "msr_fault:\n"
        "    mov $-1, %eax\n"
        "    ret\n");

int rdmsr_safe(uint32_t msr, uint64_t *v)
{
    uint32_t lo = 0, hi = 0;
    if (rdmsr_raw(msr, &lo, &hi))
        return -1;
    *v = (uint64_t)hi << 32 | lo;
    return 0;
}

int cpu_fault_fixup(struct int_frame *f)
{
    if (f->vector != VECTOR_GP || f->eip != (uint32_t)msr_try)
        return 0;
    f->eip = (uint32_t)msr_fault;
    return 1;
}
