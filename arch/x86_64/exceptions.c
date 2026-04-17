#include <asm/context.h>
#include <asm/idt.h>
#include <asm/gdt.h>
#include <kernel/kprintf.h>

void exceptions_init(void)
{
	gdt_init();
	idt_init();
}

void exc_dispatch(arch_context_t *ctx)
{
	kprintf("[FAULT] vec=%d err=%d rip=%x\n",
	        (int)ctx->vector, (int)ctx->error_code, (unsigned int)ctx->rip);
	for (;;)
		__asm__ volatile("hlt");
}
