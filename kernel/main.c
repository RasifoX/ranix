#include <kernel/board.h>
#include <kernel/driver.h>
#include <kernel/fdt.h>
#include <kernel/hal.h>
#include <kernel/irq.h>
#include <kernel/kprintf.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/pmm.h>
#include <kernel/sched.h>
#include <kernel/shell.h>
#include <kernel/tty/fb_tty.h>

extern char     __bss_end;
extern uint64_t _dtb_ptr;

static void setup_physical_memory(uint64_t fb_phys, uint64_t ram_limit)
{
	uint64_t start = ((uint64_t)&__bss_end + 15) & ~15ULL;
	uint64_t end   = fb_phys && fb_phys > start && fb_phys < ram_limit
	                 ? fb_phys : ram_limit;
	uint64_t size  = end - start;

	pmm_init(start, size);
	pmm_free_region(start, size);

	kprintf("PMM: %x - %x (%d MB)\n",
	        (unsigned int)start, (unsigned int)end, (int)(size >> 20));
}

void kernel_main(void)
{
	board_early_init();
	board_config_t *cfg = board_get_config();

	int fdt_ok = fdt_init((void *)_dtb_ptr);
	fdt_info_t *fi = fdt_get_info();

	uint64_t ram_size = (fdt_ok == 0 && fi->mem_size > 0)
	                    ? fi->mem_size : cfg->mem_size;
	if (ram_size > 0x40000000ULL) ram_size = 0x40000000ULL;
	mmu_set_ram_limit(ram_size);

	fb_info_t fb;
	uint64_t fb_phys = 0;
	if (board_fb_alloc(&fb) == 0) {
		uint64_t fb_start = (uint64_t)fb.fb & ~0x1FFFFFULL;
		mmu_set_fb_region(fb_start, 0x40000000ULL);
		fb_phys = (uint64_t)fb.fb & 0x3FFFFFFFUL;
	}

	mmu_init();

	if (fb_phys) {
		fb_tty_init(&fb);
		kprintf_set_fb();
	}

	kprintf("Ranix booting...\n");
	if (fdt_ok == 0) {
		kprintf("Board: %s\n", fi->model);
		kprintf("CPU: %d cores\n", (int)fi->num_cpus);
	} else {
		kprintf("Board: %s\n", cfg->name);
	}

	setup_physical_memory(fb_phys, ram_size);
	heap_init();
	irq_init();
	board_irq_init();

	kprintf("Kernel ready.\n");
	sched_init();
	irq_enable();

	board_drivers_register();
	task_create("shell", shell_task);

	while (1)
		arch_halt();
}
