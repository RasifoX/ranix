#include <arch/aarch64/exceptions.h>
#include <kernel/board.h>
#include <kernel/kprintf.h>

extern void exception_vectors(void);

void exceptions_init(void) {
    uint64_t vbar = (uint64_t)exception_vectors;
    if (vbar & 0x7FF)
        kpanic("Vector table not 2048-byte aligned!");
    __asm__ volatile("msr vbar_el1, %0" ::"r"(vbar));
    __asm__ volatile("isb");
}

void sync_dispatch(void) {
    uint64_t esr, elr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));

    board_config_t *cfg = board_get_config();
    volatile uint32_t *uart_dr = (uint32_t *)cfg->uart_base;
    volatile uint32_t *uart_fr = (uint32_t *)(cfg->uart_base + 0x18);

    /* print ESR and ELR via raw UART */
    char hex[] = "0123456789abcdef";
    char *pre = "\n[FATAL] ESR=";
    while (*pre) { while (*uart_fr & (1<<5)); *uart_dr = *pre++; }
    for (int i = 60; i >= 0; i -= 4) {
        while (*uart_fr & (1<<5));
        *uart_dr = hex[(esr >> i) & 0xf];
    }
    char *pre2 = " ELR=";
    while (*pre2) { while (*uart_fr & (1<<5)); *uart_dr = *pre2++; }
    for (int i = 60; i >= 0; i -= 4) {
        while (*uart_fr & (1<<5));
        *uart_dr = hex[(elr >> i) & 0xf];
    }
    char *nl = "\r\n";
    while (*nl) { while (*uart_fr & (1<<5)); *uart_dr = *nl++; }

    while (1)
        __asm__ volatile("wfe");
}