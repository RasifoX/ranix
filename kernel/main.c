#include <kernel/board.h>
#include <kernel/driver.h>
#include <kernel/fdt.h>
#include <kernel/hal.h>
#include <kernel/irq.h>
#include <kernel/kprintf.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/pmm.h>
#include <kernel/proc.h>
#include <kernel/shell.h>

extern void initrd_init(void);
#include <kernel/tty/fb_tty.h>
#include <kernel/vfs.h>

extern char     __kernel_phys_end;
extern uint64_t _dtb_ptr;

static void setup_physical_memory(uint64_t fb_phys, uint64_t ram_limit)
{
	uint64_t start = ((uint64_t)&__kernel_phys_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
	uint64_t end   = fb_phys && fb_phys > start && fb_phys < ram_limit
	                 ? fb_phys : ram_limit;
	uint64_t size  = end - start;

	pmm_init(start, size);
	pmm_free_region(start, size);

	kprintf("[  mem] 0x%lx-0x%lx, %d MB\n", start, end, (int)(size >> 20));
}

void kernel_main(void)
{
	board_early_init();
	board_config_t *cfg = board_get_config();

	int fdt_ok = fdt_init((void *)_dtb_ptr);
	fdt_info_t *fi = fdt_get_info();

	uint64_t ram_base = (fdt_ok == 0 && fi->mem_size > 0) ? fi->mem_base : cfg->mem_base;
	uint64_t ram_size = (fdt_ok == 0 && fi->mem_size > 0) ? fi->mem_size : cfg->mem_size;
	if (ram_size > 0x40000000ULL) ram_size = 0x40000000ULL;
	mmu_set_ram_limit(ram_base + ram_size);

	/* allocate framebuffer before MMU so we can mark the region NC */
	static fb_info_t fb;
	uint64_t fb_phys = 0;
	if (board_fb_alloc(&fb) == 0) {
		uint64_t fb_start = (uint64_t)fb.fb & ~0x1FFFFFULL;
		mmu_set_fb_region(fb_start, 0x40000000ULL);
		fb_phys = (uint64_t)fb.fb & 0x3FFFFFFFUL;
	}

	mmu_init();
	arch_cpu_init();

	kprintf("[ranix] Ranix\n");
	if (fdt_ok == 0)
		kprintf("[ranix] %s, %d cores\n", fi->model, (int)fi->num_cpus);
	else
		kprintf("[ranix] %s\n", cfg->name);

	setup_physical_memory(fb_phys, ram_base + ram_size);
	heap_init();

	/* framebuffer TTY needs kmalloc — init after heap */
	if (fb_phys) {
		fb_tty_init(&fb);
		kprintf_set_fb();
	}

	irq_init();
	board_irq_init();

	vfs_init();
	vfs_mount("/", "ramfs", NULL);
	vfs_mkdir("/dev",  0755);
	vfs_mkdir("/proc", 0555);
	vfs_mkdir("/tmp",  0777);
	initrd_init();

	kprintf("[ranix] ready\n");
	sched_init();
	
	/* idle process needs an fd table to use VFS during boot */
	process_t *idle = task_current();
	if (!idle->fds) idle->fds = fd_table_create();

	irq_enable();
	board_drivers_register();

	/* Start userspace shell as the primary task */
	vfs_stat_t st = {0};
	if (vfs_stat("/bin/sh", &st) == 0 && st.size > 0) {
		uint8_t *buf = kmalloc(st.size);
		if (buf) {
			int fd = vfs_open("/bin/sh", O_RDONLY, 0);
			if (fd >= 0) {
				vfs_read(fd, buf, st.size);
				vfs_close(fd);
				kprintf("[boot] launching /bin/sh...\n");
				proc_exec("sh", buf, st.size);
			} else {
				kprintf("[boot] vfs_open /bin/sh failed\n");
			}
			kfree(buf);
		}
	} else {
		kprintf("[boot] /bin/sh not found in initrd, falling back to kshell\n");
		task_create("shell", shell_task);
	}

	while (1)
		arch_halt();
}
