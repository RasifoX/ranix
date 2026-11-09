#ifndef ASM_IDT_H
#define ASM_IDT_H

#include <uapi/types.h>

void idt_init(void);
void idt_set_gate(uint8_t vec, void *handler, uint8_t flags);

/* exception/irq vector assignments */
#define VEC_TIMER    0x20
#define VEC_MSI_FIRST 0x30   /* fixed: the RTL8168 */
#define VEC_MSI_LAST  0x31   /* above FIRST up to here: handed out by pci_msi_attach */
#define VEC_YIELD    0x40
#define VEC_SYSCALL  0x80

/* exception stubs declared in exceptions_asm.S */
extern void exc_stub_0(void);  extern void exc_stub_1(void);
extern void exc_stub_2(void);  extern void exc_stub_3(void);
extern void exc_stub_4(void);  extern void exc_stub_5(void);
extern void exc_stub_6(void);  extern void exc_stub_7(void);
extern void exc_stub_8(void);  extern void exc_stub_9(void);
extern void exc_stub_10(void); extern void exc_stub_11(void);
extern void exc_stub_12(void); extern void exc_stub_13(void);
extern void exc_stub_14(void); extern void exc_stub_15(void);
extern void exc_stub_16(void); extern void exc_stub_17(void);
extern void exc_stub_18(void); extern void exc_stub_19(void);
extern void irq_stub_20(void); extern void irq_stub_21(void);
extern void irq_stub_22(void); extern void irq_stub_23(void);
extern void irq_stub_24(void); extern void irq_stub_25(void);
extern void irq_stub_26(void); extern void irq_stub_27(void);
extern void irq_stub_28(void); extern void irq_stub_29(void);
extern void irq_stub_2a(void); extern void irq_stub_2b(void);
extern void irq_stub_2c(void); extern void irq_stub_2d(void);
extern void irq_stub_2e(void); extern void irq_stub_2f(void);
extern void irq_stub_30(void); extern void irq_stub_31(void);
extern void irq_stub_yield(void);
extern void irq_stub_syscall(void);
extern void irq_stub_ipi(void);
extern void irq_stub_lapic(void);
extern void irq_stub_tlbshoot(void);

#endif
