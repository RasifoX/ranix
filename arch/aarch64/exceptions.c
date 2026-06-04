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

        /* Classify by the faulting exception level (SPSR), NOT by the ELR value:
         * a kernel control-flow corruption can branch to a low (user-range)
         * address, and keying off "elr < kernel base" would misreport that EL1
         * fault as a user segfault and kill a random process instead of panicking.
         * EL0 fault → kill the offending process with SIGSEGV; EL1 → fall through
         * to panic_report. (Delivering SIGSEGV to a user handler needs a nested-
         * fault-safe signal path on both arches — a separate change.) */
        if ((tf->spsr & 0xF) == 0) {
            kprintf("segfault: pid=%d (%s) ELR=0x%lx FAR=0x%lx EC=0x%x ISS=0x%x\n",
                    (int)p->pid, p->name, elr, far, ec, iss);
            kprintf("  x0=%lx x1=%lx x2=%lx x3=%lx\n",
                    tf->regs[0], tf->regs[1], tf->regs[2], tf->regs[3]);
            kprintf("  x20=%lx x21=%lx x29(fp)=%lx x30(lr)=%lx sp=%lx\n",
                    tf->regs[20], tf->regs[21], tf->regs[29], tf->regs[30], tf->sp);
            { extern uint64_t arch_vm_get_paddr(uint64_t *, uint64_t);
              uint64_t fpa = (p && p->vm) ? arch_vm_get_paddr(p->vm->root, far & ~0xFFFUL) : 0;
              int hasvma = 0;
              for (vma_t *v = p ? p->vmas : 0; v; v = v->next)
                  if (far >= v->vaddr && far < v->vaddr + v->size) { hasvma = 1; break; }
              kprintf("  FAR pte_pa=%lx vma=%d\n", fpa, hasvma); }
            /* Walk frame pointer chain to get call stack */
            uint64_t fp = tf->regs[29];
            for (int _i = 0; _i < 6 && fp && fp < 0xFFFF000000000000UL && (fp & 7) == 0; _i++) {
                /* The faulting fp may itself be the bad pointer — never deref it
                 * blind, or the stack walk faults in EL1 and turns a survivable
                 * user segfault into a kernel panic. */
                if (!access_ok((void *)fp, 16) || !user_pages_mapped((void *)fp, 16)) break;
                uint64_t saved_fp  = *(uint64_t *)fp;
                uint64_t saved_lr  = *(uint64_t *)(fp + 8);
                kprintf("  frame[%d]: fp=%lx lr=%lx\n", _i, saved_fp, saved_lr);
                if (!saved_fp || saved_fp == fp) break;
                fp = saved_fp;
            }
            extern void coredump(process_t *, int, arch_context_t *);
            coredump(p, SIGSEGV, tf);
            p->killed_by_sig = 1;
            p->exit_code     = SIGSEGV;
            task_exit();
            __builtin_unreachable();
        }

        panic_report(ec_name(ec), tf);
    }
