    #include <arch/aarch64/exceptions.h>
    #include <kernel/kprintf.h>
    #include <kernel/proc.h>
    #include <kernel/hal.h>

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

    void sync_dispatch(arch_context_t *tf)
    {
        uint64_t esr, elr, far;
        __asm__ volatile("mrs %0, esr_el1"  : "=r"(esr));
        __asm__ volatile("mrs %0, elr_el1"  : "=r"(elr));
        __asm__ volatile("mrs %0, far_el1"  : "=r"(far));

        uint32_t ec  = (uint32_t)(esr >> 26) & 0x3F;
        uint32_t iss = (uint32_t)(esr & 0x1FFFFFF);

        if (ec == 0x20 || ec == 0x24 || ec == 0x25) {
            int is_write = (iss & (1 << 6)) != 0;
            uint32_t dfsc = iss & 0x3F;
            /* Translation fault (0x04-0x07) or Permission fault (0x0C-0x0F) */
            if ((dfsc >= 0x04 && dfsc <= 0x07) || (dfsc >= 0x0C && dfsc <= 0x0F)) {
                if (vm_handle_fault(far, is_write) == 0)
                    return;
            }
        }

        process_t *p = task_current();

        /* EL0 faults — kill the offending process instead of panicking */
        if (ec == 0x20 || ec == 0x24) {
            kprintf("segfault: pid=%d (%s) ELR=0x%lx FAR=0x%lx EC=0x%x\n",
                    (int)p->pid, p->name, elr, far, ec);
            task_exit();
            __builtin_unreachable();
        }

        kprintf("\n--- fault: pid=%d (%s) ---\n",
                (int)p->pid, p->name);
        kprintf("type:  %s\n", ec_name(ec));
        kprintf("ELR:   0x%lx\n", elr);
        kprintf("FAR:   0x%lx\n", far);
        kprintf("ESR:   0x%lx  (EC=0x%x ISS=0x%x)\n", esr, ec, iss);
        kprintf("x0=%lx x1=%lx x2=%lx x3=%lx\n",
                tf->regs[0], tf->regs[1], tf->regs[2], tf->regs[3]);
        kprintf("x29(fp)=%lx x30(lr)=%lx sp=%lx\n",
                tf->regs[29], tf->regs[30], tf->sp);

        kpanic("unhandled exception");
    }
