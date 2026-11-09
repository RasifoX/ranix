/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/lib/sha256.h>
#include <kernel/board.h>
#include <kernel/cmdline.h>
#include <kernel/input.h>
#include <kernel/driver.h>
#include <kernel/fdt.h>
#include <kernel/hal.h>
#include <kernel/irq.h>
#include <kernel/kprintf.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/vm.h>
#include <kernel/mm/hugetlb.h>
#include <kernel/proc.h>
#include <kernel/pstore.h>
#include <kernel/watchdog.h>
#include <kernel/block.h>
#include <kernel/fs/mbr.h>
#include <kernel/fs/gpt.h>

#include <kernel/tty/fb_tty.h>
#include <kernel/vfs.h>
#include <kernel/init.h>
#include <kernel/sync.h>
#include <kernel/hungtask.h>
#include <kernel/csprng.h>
#include <kernel/net/net.h>
#include <kernel/time.h>
#include <kernel/smp.h>
#include <asm/cpu.h>
#include <kernel/tty/tty_core.h>
#include <kernel/vdso.h>

/* Boot-path pets. Long steps with bounded timeouts (DHCP, disk probing) must not
 * add up to a false reset, and a step that truly hangs stops the pets. */
#if defined(__x86_64__)
#define WD_BOOT_PROGRESS() watchdog_touch()
#else
#define WD_BOOT_PROGRESS() do { } while (0)
#endif


fb_info_t global_fb;
uint64_t  global_fb_phys = 0;


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
	uint64_t  exc[8][2];
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

/* Find the largest contiguous region — the 1 GB hugepage pool is carved from
 * its top so a reserved block never straddles a PCI hole / RAM split. */
struct _big_ctx { uint64_t base, end; };
static void _pmm_biggest(uint64_t b, uint64_t s, void *ud)
{
	struct _big_ctx *c = ud;
	if (s > c->end - c->base) { c->base = b; c->end = b + s; }
}

struct _max_ctx { uint64_t max; int n; };
static void _pmm_max(uint64_t b, uint64_t s, void *ud)
{
	struct _max_ctx *c = ud;
	if (b + s > c->max) c->max = b + s;
	c->n++;
}

/* First free physical page after the kernel image, the initrd, the boot
 * framebuffer and the board's reserved region. */
static uint64_t pmm_first_free(uint64_t fb_phys, uint64_t fb_size)
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
	uint64_t rsv_phys; uint32_t rsv_sz;
	board_get_reserved(&rsv_phys, &rsv_sz);
	if (rsv_phys && rsv_sz) {
		uint64_t rsv_end = (rsv_phys + rsv_sz + PAGE_SIZE - 1) & PAGE_MASK;
		if (rsv_end > start) start = rsv_end;
	}
	return start;
}

/* Fill exc[] with the ranges the allocator must never hand out: framebuffer,
 * initrd, device tree, board reserved region. Returns how many. */
static int pmm_boot_exclusions(uint64_t exc[8][2], uint64_t fb_phys, uint64_t fb_size,
                               uint64_t max_limit)
{
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
	uint64_t dtb_phys = fdt_get_phys();
	uint32_t dtb_sz   = fdt_get_size();
	if (dtb_phys && dtb_sz && dtb_phys < max_limit) {
		exc[n_exc][0] = dtb_phys;
		exc[n_exc][1] = (dtb_phys + dtb_sz + PAGE_SIZE - 1) & PAGE_MASK;
		n_exc++;
	}
	uint64_t rsv_phys; uint32_t rsv_sz;
	board_get_reserved(&rsv_phys, &rsv_sz);
	if (rsv_phys && rsv_sz && rsv_phys < max_limit) {
		exc[n_exc][0] = rsv_phys;
		exc[n_exc][1] = (rsv_phys + rsv_sz + PAGE_SIZE - 1) & PAGE_MASK;
		n_exc++;
	}
	return n_exc;
}

/* How many 1 GB hugepages ranix.hugepages1g=N asks for (0..8). */
static int hugepages_1g_wanted(void)
{
	int hp_want = 0;
	char b[4];
	if (cmdline_get("ranix.hugepages1g", b, sizeof(b)))
		for (char *c = b; *c >= '0' && *c <= '9' && hp_want < 8; c++)
			hp_want = hp_want * 10 + (*c - '0');
	if (hp_want > 8) hp_want = 8;
	return hp_want;
}

/* Carve 1 GB-aligned blocks off the top of the largest contiguous region
 * for the 1 GB hugepage pool, leaving a margin for the buddy allocator.
 * The window is excluded from the buddy add so those blocks are never
 * handed out, so nothing is carved unless ranix.hugepages1g=N asks: a pool
 * nobody uses took 4 GB of an 8 GB laptop. Without it 1G mmap returns ENOMEM. */
static int pmm_carve_1g_pool(uint64_t exc[8][2], int n_exc, uint64_t free_start,
                             uint64_t *pool_lo_out, int *hp_blocks_out)
{
	int hp_want = hugepages_1g_wanted();
	struct _big_ctx bc = { 0, 0 };
	board_foreach_mem_region(_pmm_biggest, &bc);
	uint64_t pool_lo = bc.end & HUGE_1G_MASK;   /* exclusive top, walked down */
	uint64_t reg_lo  = bc.base > free_start ? bc.base : free_start;
	int hp_blocks = 0;
	while (hp_blocks < hp_want && pool_lo >= HUGE_1G_SIZE &&
	       pool_lo - HUGE_1G_SIZE >= reg_lo + (512UL << 20)) {
		pool_lo -= HUGE_1G_SIZE;
		hp_blocks++;
	}
	if (hp_blocks > 0) {
		hugetlb_1g_reserve(pool_lo, hp_blocks);
		if (n_exc < 8) {
			exc[n_exc][0] = pool_lo;
			exc[n_exc][1] = pool_lo + (uint64_t)hp_blocks * HUGE_1G_SIZE;
			n_exc++;
		}
	}
	*pool_lo_out = pool_lo;
	*hp_blocks_out = hp_blocks;
	return n_exc;
}

/* Persistent log: keep its fixed RAM region away from the allocator. It must
 * survive this boot's use of memory or the next boot reads garbage. */
static int pmm_keep_pstore(uint64_t exc[8][2], int n_exc, uint64_t free_start,
                           uint64_t pool_lo, int hp_blocks)
{
#if defined(__x86_64__)
	uint64_t ps_base, ps_size;
	if (pstore_region(free_start, &ps_base, &ps_size) == 0) {
		uint64_t pool_end = pool_lo + (uint64_t)hp_blocks * HUGE_1G_SIZE;
		if (n_exc >= 8 || (hp_blocks > 0 && ps_base < pool_end &&
		                   ps_base + ps_size > pool_lo)) {
			kprintf(KERN_WARN "pstore: region lost to a reservation, no persistent log\n");
			pstore_cancel();
		} else {
			exc[n_exc][0] = ps_base;
			exc[n_exc][1] = ps_base + ps_size;
			n_exc++;
		}
	}
#endif
	return n_exc;
}

/* sort exclusions by start address (bubble sort) */
static void pmm_sort_exclusions(uint64_t exc[8][2], int n_exc)
{
	for (int i = 0; i < n_exc - 1; i++)
		for (int j = 0; j < n_exc - 1 - i; j++)
			if (exc[j][0] > exc[j+1][0]) {
				uint64_t t0 = exc[j][0], t1 = exc[j][1];
				exc[j][0] = exc[j+1][0]; exc[j][1] = exc[j+1][1];
				exc[j+1][0] = t0;        exc[j+1][1] = t1;
			}
}

/* Round-trip + liveness check on the top RAM page: PHYS_TO_VIRT/VIRT_TO_PHYS
 * must invert (catches a mis-set direct-map base, e.g. x86's text-vs-dmap
 * boundary), and the direct-map VA must actually resolve through the kernel
 * page tables — i.e. arch_map_high_memory really mapped the top of RAM. */
static void pmm_check_top(uint64_t max_limit)
{
	uint64_t top = (max_limit - PAGE_SIZE) & PAGE_MASK;
	uint64_t va  = PHYS_TO_VIRT(top);
	if (VIRT_TO_PHYS(va) != top)
		kprintf(KERN_ERR "mm: phys_to_virt round-trip failed for phys %p\n", (void *)top);
	uint64_t resolved = arch_vm_get_paddr(kernel_vm_space->root, va);
	if (resolved != top)
		kprintf(KERN_ERR "mm: top of ram phys %p not mapped (got %p)\n",
		        (void *)top, (void *)resolved);
}

static void setup_physical_memory(uint64_t fb_phys, uint64_t fb_size,
                                  board_config_t *cfg)
{
	uint64_t start = pmm_first_free(fb_phys, fb_size);

	struct _max_ctx mc = { cfg->mem_base + cfg->mem_size, 1 };
	board_foreach_mem_region(_pmm_max, &mc);
	uint64_t max_limit = mc.max;

	uint64_t meta_size = pmm_init(start, max_limit);
	uint64_t free_start = start + meta_size;

	uint64_t exc[8][2] = {0};
	int n_exc = pmm_boot_exclusions(exc, fb_phys, fb_size, max_limit);

	uint64_t pool_lo;
	int hp_blocks;
	n_exc = pmm_carve_1g_pool(exc, n_exc, free_start, &pool_lo, &hp_blocks);
	n_exc = pmm_keep_pstore(exc, n_exc, free_start, pool_lo, hp_blocks);
	pmm_sort_exclusions(exc, n_exc);

	/* Map high RAM (1GB huge pages, no buddy dep) before freeing it */
	arch_map_high_memory(max_limit);

	pmm_check_top(max_limit);

	struct _pmm_ctx ac = { free_start, 0, (uint64_t)-1, {{{0}}}, n_exc, 0 };
	for (int i = 0; i < n_exc; i++) {
		ac.exc[i][0] = exc[i][0];
		ac.exc[i][1] = exc[i][1];
	}

	board_foreach_mem_region(_pmm_add, &ac);

	kprintf(KERN_INFO "mm: %d MB in %d regions\n",
	        (int)(ac.total_free >> 20), mc.n);
}


/* Read the command line from the device tree, then start the kernel log. */
static void boot_cmdline_and_log(int fdt_ok, fdt_info_t *fi, board_config_t *cfg)
{
	if (fdt_ok == 0) {
		uint32_t cl_len;
		const void *cl_val = fdt_get_prop("chosen", "bootargs", &cl_len);
		if (cl_val && cl_len > 0)
			cmdline_init((const char *)cl_val);
		klog_setup();
		kprintf(KERN_INFO "boot: ranix on %s, %d cores\n", fi->model, (int)fi->num_cpus);
	} else {
		klog_setup();
		kprintf(KERN_INFO "boot: ranix on %s\n", cfg->name);
	}
}

/* ranix.kbd=tr picks the Turkish keymap. */
static void keymap_from_cmdline(void)
{
	char kb[4];
	if (cmdline_get("ranix.kbd", kb, sizeof(kb)) && kb[0] == 't' && kb[1] == 'r')
		input_keymap = KEYMAP_TR;
}

/* "panic_test" on the kernel command line forces a fault so the branded
 * panic screen can be witnessed on real hardware (and in CI). An undefined
 * instruction traps on every board, independent of the memory map. */
static void panic_test_if_asked(void)
{
	if (cmdline_has("panic_test")) {
#if defined(__x86_64__)
		__asm__ volatile("ud2");
#else
		__asm__ volatile("udf #0");
#endif
	}
}

/* The root ramfs and the fixed mount points under it. */
static void mount_early_filesystems(void)
{
	vfs_init();
	kprintf(KERN_INFO "hash: self-test %s\n", hash_selftest() == 0 ? "ok" : "FAILED");
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
	/* POSIX shared memory: /dev/shm is a symlink to /run/shm, which must be a
	 * real tmpfs for shm_open/Python-multiprocessing/Chrome. */
	vfs_mkdir("/run/shm", 0777);
	vfs_mount("/run/shm", "tmpfs", NULL);
	/* cgroup2 hierarchy (pids controller). Canonical path is /sys/fs/cgroup;
	 * mounted here until sysfs exposes that mountpoint. */
	vfs_mkdir("/cgroup", 0755);
	vfs_mount("/cgroup", "cgroup2", NULL);
}

/* Auto-detect and mount first ext2/ext4 disk partition */
static void mount_first_disk(void)
{
	int ndev = block_device_count();
	for (int bi = 0; bi < ndev; bi++) {
		block_device_t *bd = block_device_get(bi);
		if (!bd) continue;
		/* GPT first (a GPT disk has only a protective MBR); fall back to MBR */
		if (gpt_probe(bd) != 0)
			mbr_probe(bd);
	}
	/* Try mounting the first filesystem at /mnt. Partition slices
	 * (named "<disk>pN" by gpt/mbr) are preferred over whole-disk
	 * devices so a partitioned boot disk wins /mnt over a bare
	 * secondary disk that also carries a filesystem. */
	int total = block_device_count();
	/* Booting beside another OS: leave its filesystems alone. */
	if (cmdline_has("ranix.noautomount")) total = 0;
	int mounted = 0;
	for (int pass = 0; pass < 2 && !mounted; pass++)
	for (int bi = 0; bi < total; bi++) {
		block_device_t *bd = block_device_get(bi);
		/* A protected disk is not mounted at boot in any mode: mounting one is
		 * a decision for its owner, not a default. */
		if (!bd || bd->kernel_only || bd->ro) continue;
		int is_part = 0;
		for (const char *p = bd->name; *p; p++) if (*p == 'p') { is_part = 1; break; }
		if ((pass == 0) != (is_part != 0)) continue;

		/* probe for ext2 magic before attempting mount */
		uint8_t sb_buf[1024];
		if (bd->read(bd, 2, sb_buf, 2) == 0) {
			uint16_t magic = *(uint16_t *)(sb_buf + 56); /* s_magic offset */
			if (magic == 0xEF53) {
				if (vfs_mount("/mnt", "ext2", bd) == 0) {
					kprintf(KERN_INFO "ext2: mounted /mnt (%s)\n", bd->name);
					mounted = 1; break;
				}
			}
		}
		/* not ext2 — try FAT (boot-sector 0x55AA + 512-byte sectors) */
		{
			uint8_t bpb[512];
			if (bd->read(bd, 0, bpb, 1) == 0 && bpb[510] == 0x55 && bpb[511] == 0xAA &&
			    (uint16_t)(bpb[11] | (bpb[12] << 8)) == 512) {
				if (vfs_mount("/mnt", "vfat", bd) == 0) {
					kprintf(KERN_INFO "fat: mounted /mnt (%s)\n", bd->name);
					mounted = 1; break;
				}
			}
		}
		/* not FAT — try ISO 9660 (PVD "CD001" at sector 16 = LBA 64) */
		{
			uint8_t pvd[512];
			if (bd->read(bd, 64, pvd, 1) == 0 && pvd[0] == 1 &&
			    pvd[1]=='C' && pvd[2]=='D' && pvd[3]=='0' && pvd[4]=='0' && pvd[5]=='1') {
				if (vfs_mount("/mnt", "iso9660", bd) == 0) {
					kprintf(KERN_INFO "iso9660: mounted /mnt (%s)\n", bd->name);
					mounted = 1; break;
				}
			}
		}
	}
}

/* Test images only: a direct call through the device's write pointer must meet
 * the gate as block_write does. The buffer is zeros, so a gate that failed here
 * would show on the image. */
static void disk_selftest_if_asked(void)
{
	if (cmdline_has("ranix.disk.selftest")) {
		static uint8_t zero[BLOCK_SIZE];
		for (int bi = 0; bi < block_device_count(); bi++) {
			block_device_t *bd = block_device_get(bi);
			if (!bd || !bd->ro || !bd->write) continue;
			kprintf(KERN_INFO "blockguard: %s direct=%d api=%d\n", bd->name,
			        bd->write(bd, 0, zero, 1), block_write(bd, 0, zero, 1));
		}
	}
}

/* Start init (PID 1) as the primary task */
static void start_init(void)
{
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
					kprintf(KERN_INFO "init: starting %s\n", init_path);
				proc_exec(init_path, buf, st.size);
			} else {
				kprintf(KERN_ERR "init: vfs_open %s failed\n", init_path);
			}
			kfree(buf);
		}
	} else {
		kpanic("init: no init binary found");
	}
}

/* cpu0 after boot: poll the network, run woken tasks, sleep when idle. */
static void boot_cpu_idle_loop(void)
{
	while (1) {
		net_poll();
		/* Non-preemptible kernel: cpu0's idle yields voluntarily when a task
		 * became runnable here (irq_handler preempts only user space). */
		cpu_context_t *c = arch_get_cpu_ctx();
		if (c->need_resched) {
			sched_bsp_wake();
			c->need_resched = 0;
			yield();
		} else {
			arch_idle_deep(&c->need_resched, c->tick_slow);   /* checks and sleeps as one */
		}
	}
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
	spin_diag_on = 1;

	atomic_selftest();

	hung_task_selftest();

	boot_cmdline_and_log(fdt_ok, fi, cfg);

	setup_physical_memory(global_fb_phys, fb_sz, cfg);
	keymap_from_cmdline();
#if defined(__x86_64__)
	pstore_attach();
#endif
	heap_init();

	/* framebuffer TTY needs kmalloc — init after heap */
	if (global_fb_phys) {
		global_fb.fb = ioremap(global_fb_phys, fb_sz, VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);
		fb_tty_init(&global_fb);
		kprintf_set_fb();
	}

	irq_init();
	board_irq_init();
#if defined(__x86_64__)
	watchdog_init();   /* armed before anything below can stall the boot */
#endif

	panic_test_if_asked();

	csprng_init();
	mount_early_filesystems();
	initrd_init();
	WD_BOOT_PROGRESS();

	sched_init();
	
	/* idle process needs an fd table to use VFS during boot */
	process_t *idle = task_current();
	if (!idle->fds) idle->fds = fd_table_create();

	net_init();

	irq_enable();
	rtc_init();
	smp_init();
#if defined(__x86_64__)
	arch_idle_init();
#endif
	buddy_init_percpu(smp_ncpus);   /* enable per-CPU order-0 page caches */
#if defined(__x86_64__)
	watchdog_start();
#endif
	WD_BOOT_PROGRESS();
	board_drivers_register();
	WD_BOOT_PROGRESS();
	net_dhcp_all();
	WD_BOOT_PROGRESS();
	fbdev_init();
	drm_init();                     /* /dev/dri: KMS on the firmware's mode */
	thermal_guard_init();           /* package temperature limit until ACPI drives the fan */
	rescue_init();                  /* udp rescue channel + /dev/rescue (kernel/rescue.c) */
	claim_init();                   /* /dev/claim: visible resource claims (kernel/claim.c) */
	input_core_init();
	tty_core_init();
	pty_init();
	vdso_init();

	mount_first_disk();

	disk_selftest_if_asked();

	WD_BOOT_PROGRESS();

	start_init();

#if defined(__x86_64__)
	watchdog_boot_done();
#endif

	boot_cpu_idle_loop();
}
