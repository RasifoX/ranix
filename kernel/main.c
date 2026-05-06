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

static void setup_physical_memory(uint64_t fb_phys, uint64_t fb_size, uint64_t ram_limit)
{
	uint64_t start = ((uint64_t)&__kernel_phys_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
	uint64_t size  = ram_limit - start;

	uint64_t meta_size = pmm_init(start, ram_limit);
	uint64_t free_start = start + meta_size;

	if (fb_phys && fb_phys > start && fb_phys < ram_limit) {
		pmm_free_region(free_start, fb_phys - free_start);
		uint64_t fb_end = (fb_phys + fb_size + PAGE_SIZE - 1) & PAGE_MASK;
		if (fb_end < ram_limit) {
			pmm_free_region(fb_end, ram_limit - fb_end);
		}
	} else {
		pmm_free_region(free_start, ram_limit - free_start);
	}

	kprintf("[  mem] 0x%lx-0x%lx, %d MB\n", start, ram_limit, (int)(size >> 20));
}

void kernel_main(void)
{
	board_early_init();
	board_config_t *cfg = board_get_config();

	int fdt_ok = fdt_init((void *)_dtb_ptr);
	fdt_info_t *fi = fdt_get_info();

	uint64_t ram_base = (fdt_ok == 0 && fi->mem_size > 0) ? fi->mem_base : cfg->mem_base;
	uint64_t ram_size = (fdt_ok == 0 && fi->mem_size > 0) ? fi->mem_size : cfg->mem_size;

	static fb_info_t fb;
	uint64_t fb_phys = 0;
	uint64_t fb_sz = 0;
	if (board_fb_alloc(&fb) == 0) {
		fb_phys = (uint64_t)fb.fb;
		fb_sz = fb.fb_size;
	}

	mmu_init();
	arch_cpu_init();

	kprintf("[ranix] Ranix\n");
	if (fdt_ok == 0)
		kprintf("[ranix] %s, %d cores\n", fi->model, (int)fi->num_cpus);
	else
		kprintf("[ranix] %s\n", cfg->name);

	setup_physical_memory(fb_phys, fb_sz, ram_base + ram_size);
	heap_init();

	/* framebuffer TTY needs kmalloc — init after heap */
	if (fb_phys) {
		uint64_t va = PHYS_TO_VIRT(fb_phys);
		uint64_t aligned_sz = (fb_sz + PAGE_SIZE - 1) & PAGE_MASK;
		for (uint64_t i = 0; i < aligned_sz; i += PAGE_SIZE) {
			vm_map(kernel_vm_space, va + i, fb_phys + i, VM_READ | VM_WRITE | VM_NOCACHE);
		}
		fb.fb = (void *)va;
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
	vfs_mkdir("/mnt",  0755);
	vfs_mount("/proc", "proc",  NULL);
	vfs_mount("/dev",  "devfs", NULL);
	initrd_init();

	kprintf("[ranix] ready\n");
	sched_init();
	
	/* idle process needs an fd table to use VFS during boot */
	process_t *idle = task_current();
	if (!idle->fds) idle->fds = fd_table_create();

	irq_enable();
	board_drivers_register();

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
						kprintf("[boot] disk mounted at /mnt (%s)\n", bd->name);
						break;
					}
				}
			}
		}
	}

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
