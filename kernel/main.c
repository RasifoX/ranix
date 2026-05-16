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
#include <kernel/block.h>
#include <kernel/fs/mbr.h>

extern void initrd_init(void);
#include <kernel/tty/fb_tty.h>
#include <kernel/vfs.h>

extern char     __kernel_phys_end;
extern uint64_t _dtb_ptr;

fb_info_t global_fb;
uint64_t  global_fb_phys = 0;

extern uint64_t boot_initrd_start;
extern uint64_t boot_initrd_size;

/* Add [base, end) to buddy, skipping exclusion windows in exc[0..n-1]. */
static void add_range(uint64_t base, uint64_t end,
                      uint64_t exc[][2], int n)
{
	uint64_t cur = base;
	for (int i = 0; i < n; i++) {
		if (exc[i][0] >= end || exc[i][1] <= cur) continue;
		if (exc[i][0] > cur)
			pmm_free_region(cur, exc[i][0] - cur);
		if (exc[i][1] > cur) cur = exc[i][1];
	}
	if (cur < end)
		pmm_free_region(cur, end - cur);
}

static void setup_physical_memory(uint64_t fb_phys, uint64_t fb_size,
                                  fdt_info_t *fi, board_config_t *cfg)
{
	uint64_t start = ((uint64_t)&__kernel_phys_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

	if (boot_initrd_start && boot_initrd_start < start + 0x200000) {
		uint64_t rd_end = (boot_initrd_start + boot_initrd_size + PAGE_SIZE - 1) & PAGE_MASK;
		if (rd_end > start) start = rd_end;
	}
	if (fb_phys && fb_phys < start + 0x200000) {
		uint64_t fb_end = (fb_phys + fb_size + PAGE_SIZE - 1) & PAGE_MASK;
		if (fb_end > start) start = fb_end;
	}

	/* Collect non-contiguous memory regions, find highest address */
	int n_reg = 0;
	uint64_t reg_base[FDT_MAX_MEM_REGIONS];
	uint64_t reg_end[FDT_MAX_MEM_REGIONS];
	uint64_t max_limit = 0;

	if (fi && fi->num_mem_regions > 0) {
		for (int i = 0; i < fi->num_mem_regions; i++) {
			uint64_t b = fi->mem_regions[i].base;
			uint64_t e = b + fi->mem_regions[i].size;
			if (e > b && n_reg < FDT_MAX_MEM_REGIONS) {
				reg_base[n_reg] = b;
				reg_end[n_reg]  = e;
				if (e > max_limit) max_limit = e;
				n_reg++;
			}
		}
	}
	if (n_reg == 0) {
		uint64_t b = cfg->mem_base;
		uint64_t e = b + cfg->mem_size;
		reg_base[0] = b; reg_end[0] = e;
		max_limit = e; n_reg = 1;
	}

	uint64_t meta_size = pmm_init(start, max_limit);
	uint64_t free_start = start + meta_size;

	/* Exclusion windows: initrd and framebuffer */
	uint64_t exc[2][2] = {0};
	int n_exc = 0;
	if (fb_phys && fb_phys < max_limit) {
		exc[n_exc][0] = fb_phys;
		exc[n_exc][1] = (fb_phys + fb_size + PAGE_SIZE - 1) & PAGE_MASK;
		n_exc++;
	}
	if (boot_initrd_start && boot_initrd_start < max_limit) {
		exc[n_exc][0] = boot_initrd_start;
		exc[n_exc][1] = (boot_initrd_start + boot_initrd_size + PAGE_SIZE - 1) & PAGE_MASK;
		n_exc++;
	}
	/* Sort exclusions by start address */
	if (n_exc == 2 && exc[0][0] > exc[1][0]) {
		uint64_t t0 = exc[0][0], t1 = exc[0][1];
		exc[0][0] = exc[1][0]; exc[0][1] = exc[1][1];
		exc[1][0] = t0;        exc[1][1] = t1;
	}

	/* Add each RAM region separately — avoids device memory holes */
	uint64_t total_free = 0;
	for (int r = 0; r < n_reg; r++) {
		uint64_t base = (reg_base[r] < free_start) ? free_start : reg_base[r];
		uint64_t end  = reg_end[r];
		if (base >= end) continue;
		add_range(base, end, exc, n_exc);
		total_free += end - base;
	}

	kprintf("Memory: %d MB total across %d region(s)\n",
	        (int)(total_free >> 20), n_reg);
}


void kernel_main(void)
{
	board_early_init();
	board_config_t *cfg = board_get_config();

	int fdt_ok = fdt_init((void *)_dtb_ptr);
	fdt_info_t *fi = fdt_get_info();

	global_fb_phys = 0;
	uint64_t fb_sz = 0;
	if (board_fb_alloc(&global_fb) == 0) {
		global_fb_phys = (uint64_t)global_fb.fb;
		fb_sz = global_fb.fb_size;
	}

	mmu_init();
	arch_cpu_init();

	if (fdt_ok == 0)
		kprintf("Ranix — %s (%d cores)\n", fi->model, (int)fi->num_cpus);
	else
		kprintf("Ranix — %s\n", cfg->name);

	setup_physical_memory(global_fb_phys, fb_sz,
	                      (fdt_ok == 0) ? fi : NULL, cfg);
	heap_init();

	/* framebuffer TTY needs kmalloc — init after heap */
	if (global_fb_phys) {
		/* PHYS_TO_VIRT overflows for phys >= 2GB (typical MMIO range); use MMIO window */
		uint64_t va = (global_fb_phys < 0x80000000UL)
		              ? PHYS_TO_VIRT(global_fb_phys)
		              : MMIO_TO_VIRT(global_fb_phys);
		uint64_t aligned_sz = (fb_sz + PAGE_SIZE - 1) & PAGE_MASK;
		for (uint64_t i = 0; i < aligned_sz; i += PAGE_SIZE) {
			vm_map(kernel_vm_space, va + i, global_fb_phys + i, VM_READ | VM_WRITE | VM_NOCACHE);
		}
		global_fb.fb = (void *)va;
		fb_tty_init(&global_fb);
		kprintf_set_fb();
	}

	irq_init();
	board_irq_init();

	vfs_init();
	vfs_mount("/", "ramfs", NULL);
	vfs_mkdir("/dev",  0755);
	vfs_mkdir("/proc", 0555);
	vfs_mkdir("/tmp",  0777);
	vfs_mkdir("/mnt",  0755);
	vfs_mount("/proc", "proc",  NULL);
	vfs_mount("/dev",  "devfs", NULL);
	initrd_init();

	sched_init();
	
	/* idle process needs an fd table to use VFS during boot */
	process_t *idle = task_current();
	if (!idle->fds) idle->fds = fd_table_create();

	irq_enable();
	board_drivers_register();
	extern void fbdev_init(void);
	fbdev_init();
	extern void input_core_init(void);
	input_core_init();
	extern void tty_core_init(void);
	tty_core_init();

	/* Auto-detect and mount first ext2/ext4 disk partition */
	{
		int ndev = block_device_count();
		for (int bi = 0; bi < ndev; bi++) {
			block_device_t *bd = block_device_get(bi);
			if (!bd) continue;
			/* try MBR — registers partitions as bdev->name + "p0" etc. */
			mbr_probe(bd);
		}
		/* try mounting first block device or partition as ext2/ext4 */
		int total = block_device_count();
		for (int bi = 0; bi < total; bi++) {
			block_device_t *bd = block_device_get(bi);
			if (!bd) continue;
			
			/* probe for ext2 magic before attempting mount */
			uint8_t sb_buf[1024];
			if (bd->read(bd, 2, sb_buf, 2) == 0) {
				uint16_t magic = *(uint16_t *)(sb_buf + 56); /* s_magic offset */
				if (magic == 0xEF53) {
					if (vfs_mount("/mnt", "ext2", bd) == 0) {
						kprintf("ext2: mounted /mnt (%s)\n", bd->name);
						break;
					}
				}
			}
		}
	}

	/* Start init (PID 1) as the primary task */
	vfs_stat_t st = {0};
	const char *init_path = vfs_stat("/bin/init", &st) == 0 && st.size > 0
	                        ? "/bin/init" : NULL;
	if (!init_path && vfs_stat("/bin/sh", &st) == 0 && st.size > 0)
		init_path = "/bin/sh";
	if (init_path) {
		uint8_t *buf = kmalloc(st.size);
		if (buf) {
			int fd = vfs_open(init_path, O_RDONLY, 0);
			if (fd >= 0) {
				vfs_read(fd, buf, st.size);
				vfs_close(fd);
				kprintf("Starting %s...\n", init_path);
				proc_exec("init", buf, st.size);
			} else {
				kprintf("init: vfs_open %s failed\n", init_path);
			}
			kfree(buf);
		}
	} else {
		kprintf("init: not found, falling back to kshell\n");
		task_create("shell", shell_task);
	}

	while (1)
		arch_halt();
}
