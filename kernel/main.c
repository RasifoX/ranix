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

static void setup_physical_memory(uint64_t fb_phys, uint64_t fb_size, uint64_t ram_limit)
{
	uint64_t start = ((uint64_t)&__kernel_phys_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
	
	/* Advance 'start' past initrd if they overlap or are very close */
	if (boot_initrd_start && boot_initrd_start < start + 0x200000) {
		uint64_t rd_end = (boot_initrd_start + boot_initrd_size + PAGE_SIZE - 1) & PAGE_MASK;
		if (rd_end > start) start = rd_end;
	}

	/* Advance 'start' past framebuffer if it overlaps */
	if (fb_phys && fb_phys < start + 0x200000) {
		uint64_t fb_end = (fb_phys + fb_size + PAGE_SIZE - 1) & PAGE_MASK;
		if (fb_end > start) start = fb_end;
	}

	uint64_t size  = ram_limit - start;

	uint64_t meta_size = pmm_init(start, ram_limit);
	uint64_t free_start = start + meta_size;

	uint64_t exc[2][2] = {0};
	int n_exc = 0;

	if (fb_phys && fb_phys > start && fb_phys < ram_limit) {
		exc[n_exc][0] = fb_phys;
		exc[n_exc][1] = (fb_phys + fb_size + PAGE_SIZE - 1) & PAGE_MASK;
		n_exc++;
	}
	if (boot_initrd_start && boot_initrd_start > start && boot_initrd_start < ram_limit) {
		exc[n_exc][0] = boot_initrd_start;
		exc[n_exc][1] = (boot_initrd_start + boot_initrd_size + PAGE_SIZE - 1) & PAGE_MASK;
		n_exc++;
	}

	if (n_exc == 2 && exc[0][0] > exc[1][0]) {
		uint64_t t0 = exc[0][0], t1 = exc[0][1];
		exc[0][0] = exc[1][0]; exc[0][1] = exc[1][1];
		exc[1][0] = t0; exc[1][1] = t1;
	}

	uint64_t current = free_start;
	for (int i = 0; i < n_exc; i++) {
		if (exc[i][0] > current) {
			if (exc[i][0] > ram_limit) exc[i][0] = ram_limit;
			pmm_free_region(current, exc[i][0] - current);
		}
		if (exc[i][1] > current) current = exc[i][1];
	}
	if (current < ram_limit) {
		pmm_free_region(current, ram_limit - current);
	}

	kprintf("Memory: %d MB (0x%lx-0x%lx)\n", (int)(size >> 20), start, ram_limit);
}


void kernel_main(void)
{
	board_early_init();
	board_config_t *cfg = board_get_config();

	int fdt_ok = fdt_init((void *)_dtb_ptr);
	fdt_info_t *fi = fdt_get_info();

	uint64_t ram_base = (fdt_ok == 0 && fi->mem_size > 0) ? fi->mem_base : cfg->mem_base;
	uint64_t ram_size = (fdt_ok == 0 && fi->mem_size > 0) ? fi->mem_size : cfg->mem_size;

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

	setup_physical_memory(global_fb_phys, fb_sz, ram_base + ram_size);
	heap_init();

	/* framebuffer TTY needs kmalloc — init after heap */
	if (global_fb_phys) {
		uint64_t va = PHYS_TO_VIRT(global_fb_phys);
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
