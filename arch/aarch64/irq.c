#include <kernel/irq.h>

void irq_enable(void) {
    __asm__ volatile("msr daifclr, #0x3");  /* unmask IRQ + FIQ */
}

void irq_disable(void) {
    __asm__ volatile("msr daifset, #0x3");  /* mask IRQ + FIQ */
}

uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("mrs %0, daif" : "=r"(flags));
    irq_disable();
    return flags;
}

void irq_restore(uint64_t flags) {
    __asm__ volatile("msr daif, %0" :: "r"(flags));
}
