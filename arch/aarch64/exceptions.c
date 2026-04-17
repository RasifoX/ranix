#include <arch/aarch64/exceptions.h>
#include <kernel/kprintf.h>

extern void exception_vectors(void);

void exceptions_init(void)
{
	uint64_t vbar = (uint64_t)exception_vectors;
	if (vbar & 0x7FF)
		kpanic("vector table not 2048-byte aligned");
	__asm__ volatile("msr vbar_el1, %0; isb" :: "r"(vbar));
}

/* ESR_EL1 exception class descriptions */
static const char *ec_name(uint32_t ec)
{
	switch (ec) {
	case 0x01: return "WF* trap";
	case 0x07: return "SVE/SIMD access";
	case 0x0E: return "illegal execution state";
	case 0x15: return "SVC (AArch64)";
	case 0x20: return "instruction abort (EL0)";
	case 0x21: return "instruction abort (EL1)";
	case 0x22: return "PC alignment fault";
	case 0x24: return "data abort (EL0)";
	case 0x25: return "data abort (EL1)";
	case 0x26: return "SP alignment fault";
	case 0x30: return "breakpoint (EL0)";
	case 0x31: return "breakpoint (EL1)";
	case 0x32: return "software step (EL0)";
	case 0x33: return "software step (EL1)";
	case 0x34: return "watchpoint (EL0)";
	case 0x35: return "watchpoint (EL1)";
	default:   return "unknown";
	}
}

void sync_dispatch(void)
{
	uint64_t esr, elr, far;
	__asm__ volatile("mrs %0, esr_el1"  : "=r"(esr));
	__asm__ volatile("mrs %0, elr_el1"  : "=r"(elr));
	__asm__ volatile("mrs %0, far_el1"  : "=r"(far));

	uint32_t ec  = (uint32_t)(esr >> 26) & 0x3F;
	uint32_t iss = (uint32_t)(esr & 0x1FFFFFF);

	kprintf("\n--- kernel fault ---\n");
	kprintf("type:  %s\n", ec_name(ec));
	kprintf("ELR:   0x%x\n", (unsigned int)elr);
	kprintf("FAR:   0x%x\n", (unsigned int)far);
	kprintf("ESR:   0x%x  (EC=0x%x ISS=0x%x)\n",
	        (unsigned int)esr, (unsigned int)ec, (unsigned int)iss);

	kpanic("unhandled exception");
}
