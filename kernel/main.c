#include <kernel/board.h>
#include <kernel/cmdline.h>
#include <kernel/driver.h>
#include <kernel/fdt.h>
#include <kernel/hal.h>
#include <kernel/irq.h>
#include <kernel/kprintf.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/pmm.h>
#include <kernel/proc.h>
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

struct _pmm_ctx {
	uint64_t  free_start;
	uint64_t  clip_lo, clip_hi;
	uint64_t  exc[4][2];
	int       n_exc;
	uint64_t  total_free;
};

static void _pmm_add(uint64_t b, uint64_t s, void *ud)
{
	struct _pmm_ctx *c = ud;
	uint64_t base = b > c->clip_lo ? b : c->clip_lo;
	if (base < c->free_start) base = c->free_start;
	uint64_t end = b + s;
	if (end > c->clip_hi) end = c->clip_hi;
	if (base >= end) return;
	add_range(base, end, c->exc, c->n_exc);
	c->total_free += end - base;
}

struct _max_ctx { uint64_t max; int n; };
static void _pmm_max(uint64_t b, uint64_t s, void *ud)
{
	struct _max_ctx *c = ud;
	if (b + s > c->max) c->max = b + s;
	c->n++;
}

static void setup_physical_memory(uint64_t fb_phys, uint64_t fb_size,
                                  board_config_t *cfg)
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
	/* Board-specific reserved region (e.g. multiboot2 info on x86_64) must be
	 * skipped before buddy_init writes metadata into that memory. */
	{
		uint64_t rsv_phys; uint32_t rsv_sz;
		board_get_reserved(&rsv_phys, &rsv_sz);
		if (rsv_phys && rsv_sz) {
			uint64_t rsv_end = (rsv_phys + rsv_sz + PAGE_SIZE - 1) & PAGE_MASK;
			if (rsv_end > start) start = rsv_end;
		}
	}

	struct _max_ctx mc = { cfg->mem_base + cfg->mem_size, 1 };
	board_foreach_mem_region(_pmm_max, &mc);
	uint64_t max_limit = mc.max;

	uint64_t meta_size = pmm_init(start, max_limit);
	uint64_t free_start = start + meta_size;

	uint64_t exc[4][2] = {0};
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
	{
		uint64_t dtb_phys = fdt_get_phys();
		uint32_t dtb_sz   = fdt_get_size();
		if (dtb_phys && dtb_sz && dtb_phys < max_limit) {
			exc[n_exc][0] = dtb_phys;
			exc[n_exc][1] = (dtb_phys + dtb_sz + PAGE_SIZE - 1) & PAGE_MASK;
			n_exc++;
		}
	}
	{
		uint64_t rsv_phys; uint32_t rsv_sz;
		board_get_reserved(&rsv_phys, &rsv_sz);
		if (rsv_phys && rsv_sz && rsv_phys < max_limit) {
			exc[n_exc][0] = rsv_phys;
			exc[n_exc][1] = (rsv_phys + rsv_sz + PAGE_SIZE - 1) & PAGE_MASK;
			n_exc++;
		}
	}
	/* sort exclusions by start address (bubble sort, 3 elements max) */
	for (int i = 0; i < n_exc - 1; i++)
		for (int j = 0; j < n_exc - 1 - i; j++)
			if (exc[j][0] > exc[j+1][0]) {
				uint64_t t0 = exc[j][0], t1 = exc[j][1];
				exc[j][0] = exc[j+1][0]; exc[j][1] = exc[j+1][1];
				exc[j+1][0] = t0;        exc[j+1][1] = t1;
			}

	/* Map high RAM (1GB huge pages, no buddy dep) before freeing it */
	arch_map_high_memory(max_limit);

	struct _pmm_ctx ac = { free_start, 0, (uint64_t)-1, {{{0}}}, n_exc, 0 };
	for (int i = 0; i < n_exc; i++) {
		ac.exc[i][0] = exc[i][0];
		ac.exc[i][1] = exc[i][1];
	}

	board_foreach_mem_region(_pmm_add, &ac);

	kprintf("Memory: %d MB total across %d region(s)\n",
	        (int)(ac.total_free >> 20), mc.n);
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
	fdt_remap();
	arch_cpu_init();
	extern volatile int spin_diag_on;
	spin_diag_on = 1;

	if (fdt_ok == 0) {
		uint32_t cl_len;
		const void *cl_val = fdt_get_prop("chosen", "bootargs", &cl_len);
		if (cl_val && cl_len > 0)
			cmdline_init((const char *)cl_val);
		kprintf("Ranix — %s (%d cores)\n", fi->model, (int)fi->num_cpus);
	} else {
		kprintf("Ranix — %s\n", cfg->name);
	}

	setup_physical_memory(global_fb_phys, fb_sz, cfg);
	heap_init();

	/* framebuffer TTY needs kmalloc — init after heap */
	if (global_fb_phys) {
		global_fb.fb = ioremap(global_fb_phys, fb_sz, VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);
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
	vfs_mkdir("/sys",  0555);
	vfs_mount("/sys",  "sysfs", NULL);
	vfs_mkdir("/run",  0755);
	vfs_mkdir("/run/shm", 0777);
	vfs_mount("/run/shm", "ramfs", NULL);
	initrd_init();

	sched_init();
	
	/* idle process needs an fd table to use VFS during boot */
	process_t *idle = task_current();
	if (!idle->fds) idle->fds = fd_table_create();

	extern void net_init(void);
	net_init();

	irq_enable();
	extern void smp_init(void);
	smp_init();
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
	static const char *init_candidates[] = {
		NULL, /* filled from cmdline init= */
		"/sbin/init",
		"/etc/init",
		"/bin/init",
		"/bin/sh",
		NULL
	};
	char cmdline_init_buf[256] = {0};
	if (cmdline_get("init", cmdline_init_buf, sizeof(cmdline_init_buf)))
		init_candidates[0] = cmdline_init_buf;

	vfs_stat_t st = {0};
	const char *init_path = NULL;
	for (int i = 0; i < 5; i++) {
		if (!init_candidates[i]) continue;
		if (vfs_stat(init_candidates[i], &st) == 0 && st.size > 0) {
			init_path = init_candidates[i];
			break;
		}
	}

	if (init_path) {
		uint8_t *buf = kmalloc(st.size);
		if (buf) {
			int fd = vfs_open(init_path, O_RDONLY, 0);
			if (fd >= 0) {
				vfs_read(fd, buf, st.size);
				vfs_close(fd);
				if (!cmdline_has("quiet"))
					kprintf("Starting %s...\n", init_path);
				proc_exec("init", buf, st.size);
			} else {
				kprintf("init: vfs_open %s failed\n", init_path);
			}
			kfree(buf);
		}
	} else {
		kpanic("init: no init binary found");
	}

	extern void net_poll(void);
	while (1) {
		net_poll();
		arch_halt();
	}
}
