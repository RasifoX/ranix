#include <kernel/irq.h>

void irq_enable(void)  { __asm__ volatile("sti"); }
void irq_disable(void) { __asm__ volatile("cli"); }

uint64_t irq_save(void)
{
	uint64_t flags;
	__asm__ volatile("pushfq; pop %0; cli" : "=r"(flags));
	return flags;
}

void irq_restore(uint64_t flags)
{
	__asm__ volatile("push %0; popfq" :: "r"(flags) : "memory");
}
