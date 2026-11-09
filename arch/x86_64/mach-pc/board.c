/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <drivers/serial/uart16550/uart16550.h>
#include <drivers/irqchip/pic8259/pic8259.h>
#include <drivers/irqchip/pit/pit.h>
#include <asm/gdt.h>
#include <asm/idt.h>
#include <kernel/board.h>
#include <kernel/cmdline.h>
#include <kernel/irq.h>
#include <kernel/lib/string.h>
#include <kernel/pstore.h>
#include <kernel/init.h>
#include <asm/cpu.h>
#include <kernel/sound.h>

static board_config_t config = {
	.name     = "x86 PC",
	.uart_base = 0x3F8,
	.mem_base  = 0,
	.mem_size  = 128UL * 1024 * 1024,
	.fb_width  = 0,
	.fb_height = 0,
};

void multiboot2_parse(uint64_t ptr, uint64_t *mem_size_out);

void board_early_init(void)
{
	uart16550_init();
	multiboot2_parse(_dtb_ptr, &config.mem_size);
}


int board_fb_alloc(fb_info_t *out)
{
	return mb2_get_fb(out);
}

#include <drivers/input/ps2.h>

void board_irq_init(void)
{
	gdt_init(0);
	idt_init();
	pic8259_init();
	irq_register(0x20, pit_handler);
	pit_init(TIMER_HZ);
	lapic_bsp_tick_takeover();
	ps2_kbd_init();
	uart16550_irq_init();
}

void board_secondary_irq_init(void)
{
	/* x86_64: PIT IRQ0 is broadcast via PIC to all CPUs.
	 * Secondary CPUs only need to load GDT/IDT (done in arch_secondary_init). */
}


#include <drivers/block/sdcard.h>

void board_drivers_register(void)
{
	uart16550_register();
	pit_register();
	virtio_blk_init();
	ahci_init();
	xhci_init();
	acpi_button_start();
	nvme_init();
	virtio_net_pci_init();
	rtl8168_init();
	hda_init();
	rtw8723_init();
	casper_ec_init();          /* one laptop's EC; off unless asked for */
}

board_config_t *board_get_config(void) { return &config; }

void board_foreach_mem_region(void (*cb)(uint64_t, uint64_t, void *), void *ud)
{
    mb2_foreach_mem_region(cb, ud);
}

void board_get_reserved(uint64_t *base, uint32_t *size)
{
    mb2_get_reserved(base, size);
}

static uint8_t inb_port(uint16_t port)
{
	uint8_t val;
	__asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
	return val;
}

static void outb_port(uint16_t port, uint8_t val)
{
	__asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

void board_poweroff(void)
{
	pstore_end(PSTORE_STATE_POWEROFF, NULL);
	/* QEMU isa-debug-exit (port 0x501, any value → QEMU exits with code 2*val+1) */
	__asm__ volatile("outb %0, %1" :: "a"((uint8_t)0x00), "Nd"((uint16_t)0x501));
	acpi_poweroff();
	/* QEMU ACPI shutdown PIIX4: PM1a_CNT at 0x604, SLP_EN|SLP_TYP_S5 */
	__asm__ volatile("outw %0, %1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
	/* Older QEMU: port 0xb004 */
	__asm__ volatile("outw %0, %1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0xb004));
	for (;;) __asm__ volatile("hlt");
}

#define KBC_STATUS     0x64
#define KBC_IBF        0x02
#define KBC_PULSE_RESET 0xFE
#define CF9_SYS_RST    0x02
#define CF9_RST_CPU    0x04

/* Long enough for a chipset reset to take effect. Port 0x80 writes take about
 * 1 us each and need no timer, which matters because the panic path has none. */
static void reset_settle(void)
{
	for (int i = 0; i < 200000; i++) outb_port(0x80, 0);
}

/* Warm: bit 3 (full reset) stays clear, so DRAM keeps its contents and the
 * persistent log survives. */
static void reset_cf9(void)
{
	uint8_t cf9 = inb_port(0xcf9) & ~(CF9_SYS_RST | CF9_RST_CPU);
	outb_port(0xcf9, cf9 | CF9_SYS_RST);
	for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause");
	outb_port(0xcf9, cf9 | CF9_SYS_RST | CF9_RST_CPU);
}

/* Pulse the reset line through the keyboard controller. A busy or absent
 * controller must not stop the sequence, so the wait is bounded. */
static void reset_kbc(void)
{
	for (int i = 0; i < 100000 && (inb_port(KBC_STATUS) & KBC_IBF); i++)
		__asm__ volatile("pause");
	outb_port(KBC_STATUS, KBC_PULSE_RESET);
}

static void reset_triple_fault(void)
{
	static const uint8_t null_idt[10] = {0};
	__asm__ volatile("cli; lidt %0; int3" :: "m"(null_idt) : "memory");
}

/* ranix.resetvia=cf9|kbc|acpi|triple forces one method, so each can be proven
 * alone; otherwise all run in turn. ACPI goes first: on the Casper Nirvana a CF9
 * reset left the machine dark until the power button, while the FADT reset
 * register rebooted it. Interrupts off: this also runs from panic. */
void board_reset(void)
{
	char via[8];
	const char *only = cmdline_get("ranix.resetvia", via, sizeof(via));

	__asm__ volatile("cli");
	pstore_end(PSTORE_STATE_REBOOT, NULL);
	__asm__ volatile("wbinvd");

	if (!only || strcmp(only, "acpi") == 0)   { acpi_reset(); reset_settle(); }
	if (!only || strcmp(only, "cf9") == 0)    { reset_cf9();  reset_settle(); }
	if (!only || strcmp(only, "kbc") == 0)    { reset_kbc();  reset_settle(); }
	if (!only || strcmp(only, "triple") == 0)   reset_triple_fault();
	for (;;) __asm__ volatile("hlt");
}
