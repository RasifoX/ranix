#include <kernel/vfs.h>
#include <kernel/proc.h>
#include <kernel/capabilities.h>
#include <kernel/cmdline.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/hugetlb.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/mmu.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/sync.h>
#include <kernel/net/socket.h>
#include <kernel/net/net.h>
#include <kernel/block.h>
#include <kernel/hal.h>

#include <kernel/nsproxy.h>
#include <kernel/cgroup.h>

/*
 * procfs — virtual filesystem at /proc.
 *
 * Content is generated dynamically on every read; no data is stored.
 * pb_t (pointer-buffer) provides snprintf-free string building.
 */

/* ── pb_t: minimal string builder ───────────────────────────── */

typedef struct { char *p; size_t rem; } pb_t;

static void pb_char(pb_t *b, char c)
{
	if (!b->rem) return;
	*b->p++ = c; b->rem--;
}

static void pb_str(pb_t *b, const char *s)
{
	while (*s) pb_char(b, *s++);
}

static void pb_u64(pb_t *b, uint64_t v)
{
	char tmp[20]; int i = 0;
	if (v == 0) { pb_char(b, '0'); return; }
	while (v > 0 && i < 20) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
	for (int j = i - 1; j >= 0; j--) pb_char(b, tmp[j]);
}

static void pb_u64kib(pb_t *b, uint64_t bytes)
{
	pb_u64(b, bytes / 1024);
	pb_str(b, " kB\n");
}

static void pb_hex16(pb_t *b, uint64_t v)
{
	static const char hx[] = "0123456789abcdef";
	for (int i = 15; i >= 0; i--)
		pb_char(b, hx[(v >> (i * 4)) & 0xf]);
}

/* ── fill callbacks ──────────────────────────────────────────── */

static int fill_version(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	pb_str(&b, "Ranix version 1.0.0 (ranix@ranix) "
	           "(gcc version 12.0) #1 SMP\n");
	return (int)(sz - b.rem);
}

static int fill_cpuinfo(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	extern int smp_ncpus;
	for (int i = 0; i < smp_ncpus; i++) {
#if defined(__aarch64__)
		pb_str(&b, "processor\t: "); pb_u64(&b, i); pb_char(&b, '\n');
		pb_str(&b, "BogoMIPS\t: 100.00\n");
		pb_str(&b, "Features\t: fp asimd\n");
		pb_str(&b, "CPU implementer\t: 0x41\n");
		pb_str(&b, "CPU architecture: 8\n");
		pb_str(&b, "CPU variant\t: 0x0\n");
		pb_str(&b, "CPU part\t: 0xd08\n");
		pb_str(&b, "CPU revision\t: 3\n\n");
#else
		pb_str(&b, "processor\t: "); pb_u64(&b, i); pb_char(&b, '\n');
		pb_str(&b, "vendor_id\t: GenuineRanix\n");
		pb_str(&b, "cpu family\t: 6\n");
		pb_str(&b, "model\t\t: 0\n");
		pb_str(&b, "model name\t: Ranix Virtual CPU\n");
		pb_str(&b, "stepping\t: 0\n");
		pb_str(&b, "cpu MHz\t\t: 1000.000\n");
		pb_str(&b, "cache size\t: 4096 KB\n");
		pb_str(&b, "bogomips\t: 2000.00\n\n");
#endif
	}
	return (int)(sz - b.rem);
}

static int fill_meminfo(char *buf, size_t sz)
{
	uint64_t total = buddy_total_pages() * PAGE_SIZE;
	uint64_t free  = pmm_free_pages()    * PAGE_SIZE;
	uint64_t used  = total - free;
	pb_t b = {buf, sz};
	pb_str(&b, "MemTotal:       "); pb_u64kib(&b, total);
	pb_str(&b, "MemFree:        "); pb_u64kib(&b, free);
	pb_str(&b, "MemAvailable:   "); pb_u64kib(&b, free);
	pb_str(&b, "Buffers:        "); pb_u64kib(&b, 0);
	pb_str(&b, "Cached:         "); pb_u64kib(&b, 0);
	pb_str(&b, "SwapTotal:      "); pb_u64kib(&b, 0);
	pb_str(&b, "SwapFree:       "); pb_u64kib(&b, 0);
	pb_str(&b, "Active:         "); pb_u64kib(&b, used);
	pb_str(&b, "Inactive:       "); pb_u64kib(&b, 0);
	pb_str(&b, "AnonHugePages:  "); pb_u64kib(&b, huge2m_kb() * 1024);
	return (int)(sz - b.rem);
}

static int fill_mounts(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	pb_str(&b, "rootfs / ramfs rw 0 0\n");
	pb_str(&b, "proc /proc proc ro 0 0\n");
	pb_str(&b, "devfs /dev devfs rw 0 0\n");
	return (int)(sz - b.rem);
}

#include <kernel/board.h>

static int fill_uptime(char *buf, size_t sz)
{
	extern volatile uint64_t kernel_ticks;
	uint64_t secs = kernel_ticks / TIMER_HZ;
	uint64_t centi = ((kernel_ticks % TIMER_HZ) * 100) / TIMER_HZ;
	pb_t b = {buf, sz};
	pb_u64(&b, secs); pb_char(&b, '.'); pb_u64(&b, centi);
	pb_str(&b, " ");
	pb_u64(&b, secs); pb_char(&b, '.'); pb_u64(&b, centi);
	pb_char(&b, '\n');
	return (int)(sz - b.rem);
}

static int fill_stat(char *buf, size_t sz)
{
	extern volatile uint64_t kernel_ticks;
	extern int smp_ncpus;
	pb_t b = {buf, sz};
	pb_str(&b, "cpu  ");
	pb_u64(&b, kernel_ticks); pb_str(&b, " 0 0 0 0 0 0 0\n");
	for (int i = 0; i < smp_ncpus; i++) {
		pb_str(&b, "cpu"); pb_u64(&b, i); pb_str(&b, " ");
		pb_u64(&b, kernel_ticks); pb_str(&b, " 0 0 0 0 0 0 0\n");
	}
	pb_str(&b, "processes "); pb_u64(&b, (uint64_t)task_count()); pb_char(&b, '\n');
	pb_str(&b, "procs_running 1\n");
	pb_str(&b, "procs_blocked 0\n");
	return (int)(sz - b.rem);
}

static int fill_loadavg(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	pb_str(&b, "0.00 0.00 0.00 1/");
	pb_u64(&b, (uint64_t)task_count());
	pb_str(&b, " ");
	pb_u64(&b, (uint64_t)task_current()->pid);
	pb_char(&b, '\n');
	return (int)(sz - b.rem);
}

/* ── per-process fill callbacks ──────────────────────────────── */

static process_t *find_proc(int pid)
{
	return proc_find((pid_t)pid);
}

static int fill_proc_status(char *buf, size_t sz, int pid)
{
	process_t *p = find_proc(pid);
	pb_t b = {buf, sz};
	if (!p) { pb_str(&b, "Name:\t(gone)\n"); return (int)(sz - b.rem); }
	pb_str(&b, "Name:\t"); pb_str(&b, p->name); pb_char(&b, '\n');
	const char *st = (p->state == PROC_SLEEPING) ? "S (sleeping)"
	               : (p->state == PROC_ZOMBIE || p->state == PROC_DEAD) ? "Z (zombie)"
	               : "R (running)";
	pb_str(&b, "State:\t"); pb_str(&b, st); pb_char(&b, '\n');
	pb_str(&b, "Tgid:\t"); pb_u64(&b, p->tgid ? p->tgid : p->pid); pb_char(&b, '\n');
	pb_str(&b, "Pid:\t");  pb_u64(&b, p->pid); pb_char(&b, '\n');
	pb_str(&b, "PPid:\t"); pb_u64(&b, p->ppid); pb_char(&b, '\n');
	pb_str(&b, "Uid:\t");
	pb_u64(&b, p->uid);  pb_char(&b, '\t');
	pb_u64(&b, p->euid); pb_char(&b, '\t');
	pb_u64(&b, p->suid); pb_char(&b, '\t');
	pb_u64(&b, p->euid); pb_char(&b, '\n'); /* fsuid = euid */
	pb_str(&b, "Gid:\t");
	pb_u64(&b, p->gid);  pb_char(&b, '\t');
	pb_u64(&b, p->egid); pb_char(&b, '\t');
	pb_u64(&b, p->sgid); pb_char(&b, '\t');
	pb_u64(&b, p->egid); pb_char(&b, '\n'); /* fsgid = egid */
	pb_str(&b, "Groups:\t");
	for (int _i = 0; _i < p->ngroups; _i++) {
		if (_i) pb_char(&b, ' ');
		pb_u64(&b, p->groups[_i]);
	}
	pb_char(&b, '\n');
	pb_str(&b, "VmSize:\t"); pb_u64(&b, (p->brk ? p->brk : 0) / 1024); pb_str(&b, " kB\n");
	pb_str(&b, "VmRSS:\t"); pb_u64(&b, 64); pb_str(&b, " kB\n");
	pb_str(&b, "Threads:\t"); pb_u64(&b, (uint64_t)proc_thread_count(p->tgid)); pb_char(&b, '\n');
	uint64_t sigign = 0, sigcgt = 0;
	for (int _i = 0; _i < NSIG; _i++) {
		void *h = (void *)p->sig_actions[_i].sa_handler;
		if ((uintptr_t)h == 1) sigign |= (1UL << _i);
		else if (h)            sigcgt |= (1UL << _i);
	}
	pb_str(&b, "SigPnd:\t"); pb_hex16(&b, p->sig_pending); pb_char(&b, '\n');
	pb_str(&b, "SigBlk:\t"); pb_hex16(&b, p->sig_mask);    pb_char(&b, '\n');
	pb_str(&b, "SigIgn:\t"); pb_hex16(&b, sigign);         pb_char(&b, '\n');
	pb_str(&b, "SigCgt:\t"); pb_hex16(&b, sigcgt);         pb_char(&b, '\n');
	pb_str(&b, "CapInh:\t"); pb_hex16(&b, p->cap_inheritable); pb_char(&b, '\n');
	pb_str(&b, "CapPrm:\t"); pb_hex16(&b, p->cap_permitted);   pb_char(&b, '\n');
	pb_str(&b, "CapEff:\t"); pb_hex16(&b, p->cap_effective);   pb_char(&b, '\n');
	return (int)(sz - b.rem);
}

static int fill_proc_stat(char *buf, size_t sz, int pid)
{
	process_t *p = find_proc(pid);
	pb_t b = {buf, sz};
	if (!p) { pb_u64(&b, (uint64_t)pid); pb_str(&b, " (gone) Z 0\n"); return (int)(sz - b.rem); }
	pb_u64(&b, p->pid); pb_char(&b, ' ');
	pb_char(&b, '('); pb_str(&b, p->name); pb_char(&b, ')');
	/* fields 3-52 matching the /proc/PID/stat layout */
	pb_str(&b, " R ");
	pb_u64(&b, p->ppid);                   /* 4: ppid */
	pb_str(&b, " 0 0 0 0 0 0 0 0 0 0 ");  /* 5-13: pgrp..cmajflt */
	pb_u64(&b, p->ticks);                  /* 14: utime (CPU ticks) */
	pb_str(&b, " 0 0 0 0 0 0 1 0 0 0"     /* 15-24: stime..itrealvalue */
	           " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0"  /* 25-44 */
	           " 0 0 0 0 0 0 0 0\n");                       /* 45-52 */
	return (int)(sz - b.rem);
}

static int fill_cmdline(char *buf, size_t sz)
{
	const char *cl = cmdline_str();
	size_t n = strlen(cl);
	if (n >= sz) n = sz - 1;
	memcpy(buf, cl, n);
	buf[n++] = '\n';
	return (int)n;
}

static int fill_proc_cmdline(char *buf, size_t sz, int pid)
{
	process_t *p = find_proc(pid);
	if (!p) return 0;
	pb_t b = {buf, sz};
	pb_str(&b, p->name);
	pb_char(&b, '\0');
	return (int)(sz - b.rem);
}

static int fill_proc_comm(char *buf, size_t sz, int pid)
{
	process_t *p = find_proc(pid);
	if (!p) return 0;
	pb_t b = {buf, sz};
	pb_str(&b, p->name);
	pb_char(&b, '\n');
	return (int)(sz - b.rem);
}

static int fill_proc_filesystems(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	for (int i = 0; ; i++) {
		const char *name = vfs_fstype_name(i);
		if (!name) break;
		/* block-backed filesystems omit the "nodev" tag */
		if (strcmp(name, "ext2") != 0) pb_str(&b, "nodev");
		pb_char(&b, '\t');
		pb_str(&b, name);
		pb_char(&b, '\n');
	}
	return (int)(sz - b.rem);
}

static int fill_proc_maps(char *buf, size_t sz, int pid)
{
	process_t *p = find_proc(pid);
	pb_t b = {buf, sz};
	if (!p) return (int)(sz - b.rem);
	/* iterate VMAs */
	for (vma_t *v = p->vmas; v; v = v->next) {
		/* start-end perms offset dev:ino path */
		uint64_t end = v->vaddr + v->size;
		/* print hex addr */
		char hex[17]; int hi;
		uint64_t tmp = v->vaddr;
		for (hi = 15; hi >= 0; hi--) { hex[hi] = "0123456789abcdef"[tmp & 0xf]; tmp >>= 4; }
		hex[16] = '\0'; pb_str(&b, hex); pb_char(&b, '-');
		tmp = end;
		for (hi = 15; hi >= 0; hi--) { hex[hi] = "0123456789abcdef"[tmp & 0xf]; tmp >>= 4; }
		pb_str(&b, hex); pb_char(&b, ' ');
		pb_char(&b, (v->flags & VMA_READ)  ? 'r' : '-');
		pb_char(&b, (v->flags & VMA_WRITE) ? 'w' : '-');
		pb_char(&b, (v->flags & VMA_EXEC)  ? 'x' : '-');
		pb_char(&b, 'p');
		pb_str(&b, " 00000000 00:00 0\n");
	}
	return (int)(sz - b.rem);
}

/* ── procfs inode types ──────────────────────────────────────── */

/* priv for a virtual file — fill callback + pid */
typedef struct {
	int  pid;   /* -1 for top-level files */
	int  type;  /* proc file type index */
} procfs_inode_priv_t;

#define PROC_F_VERSION   0
#define PROC_F_CPUINFO   1
#define PROC_F_MEMINFO   2
#define PROC_F_MOUNTS    3
#define PROC_F_UPTIME    4
#define PROC_F_STAT      5
#define PROC_F_LOADAVG   6
#define PROC_F_CMDLINE   7
#define PROC_F_FILESYSTEMS 8
#define PROC_F_MODULES   9
#define PROC_F_SWAPS     15
#define PROC_F_VMSTAT    16
#define PROC_F_PARTITIONS 17
#define PROC_F_DISKSTATS 18
#define PROC_F_P_STATUS  10
#define PROC_F_P_STAT    11
#define PROC_F_P_CMDLINE 12
#define PROC_F_P_MAPS    13
#define PROC_F_P_COMM    14
#define PROC_F_NET_DEV   20
#define PROC_F_NET_TCP   21
#define PROC_F_NET_UDP   22
#define PROC_F_NET_ROUTE 23
#define PROC_F_NET_IF6   24

/* /proc/sys entries */
#define PROC_F_SYS_HZ            30
#define PROC_F_SYS_HOSTNAME      31
#define PROC_F_SYS_DOMAINNAME    32
#define PROC_F_SYS_OVERCOMMIT    33
#define PROC_F_SYS_NGROUPS_MAX   34
#define PROC_F_SYS_PID_MAX       35
#define PROC_F_SYS_FILE_MAX      36
#define PROC_F_SYS_SOMAXCONN     37
#define PROC_F_SYS_RMEM_DEFAULT  38
#define PROC_F_SYS_WMEM_DEFAULT  39
#define PROC_F_SYS_RMEM_MAX      40
#define PROC_F_SYS_WMEM_MAX      41
#define PROC_F_SYS_NFILE         42
#define PROC_F_SYS_RANDOMIZE_VA  43

/* /proc/modules — monolithic kernel: no loadable modules (lsmod shows nothing). */
static int fill_modules(char *buf, size_t sz) { (void)buf; (void)sz; return 0; }

/* /proc/swaps — no swap areas configured; header only (swapon -s). */
static int fill_swaps(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	pb_str(&b, "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n");
	return (int)(sz - b.rem);
}

/* /proc/partitions — registered block devices (lsblk/fdisk/df probe this).
 * #blocks is in 1 KiB units; size_blocks counts 512-byte sectors. */
static int fill_partitions(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	pb_str(&b, "major minor  #blocks  name\n");
	int n = block_device_count();
	for (int i = 0; i < n; i++) {
		block_device_t *d = block_device_get(i);
		if (!d) continue;
		pb_str(&b, "   8 "); pb_u64(&b, (uint64_t)i); pb_str(&b, " ");
		pb_u64(&b, d->size_blocks / 2); pb_str(&b, " ");
		pb_str(&b, d->name); pb_char(&b, '\n');
	}
	return (int)(sz - b.rem);
}

/* /proc/diskstats — one line per block device (iostat/df probe it). We don't
 * track per-device I/O counters yet, so the 11 stat fields are zero. */
static int fill_diskstats(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	int n = block_device_count();
	for (int i = 0; i < n; i++) {
		block_device_t *d = block_device_get(i);
		if (!d) continue;
		pb_str(&b, "   8 "); pb_u64(&b, (uint64_t)i); pb_str(&b, " ");
		pb_str(&b, d->name);
		pb_str(&b, " 0 0 0 0 0 0 0 0 0 0 0\n");
	}
	return (int)(sz - b.rem);
}

/* /proc/vmstat — a few real counters tools may read. */
static int fill_vmstat(char *buf, size_t sz)
{
	extern uint64_t buddy_free_pages(void);
	pb_t b = {buf, sz};
	pb_str(&b, "nr_free_pages "); pb_u64(&b, buddy_free_pages()); pb_char(&b, '\n');
	pb_str(&b, "nr_anon_pages 0\nnr_mapped 0\npgfault 0\npgmajfault 0\n");
	return (int)(sz - b.rem);
}

static int fill_net_dev(char *buf, size_t sz);
static int fill_net_tcp(char *buf, size_t sz);
static int fill_net_udp(char *buf, size_t sz);
static int fill_net_route(char *buf, size_t sz);
static int fill_net_if_inet6(char *buf, size_t sz);

/* static file table for root dir */
static const struct {
	const char *name;
	int         type;
} root_files[] = {
	{ "version",  PROC_F_VERSION  },
	{ "cpuinfo",  PROC_F_CPUINFO  },
	{ "meminfo",  PROC_F_MEMINFO  },
	{ "mounts",   PROC_F_MOUNTS   },
	{ "uptime",   PROC_F_UPTIME   },
	{ "stat",     PROC_F_STAT     },
	{ "loadavg",  PROC_F_LOADAVG  },
	{ "cmdline",  PROC_F_CMDLINE  },
	{ "filesystems", PROC_F_FILESYSTEMS },
	{ "modules",  PROC_F_MODULES  },
	{ "swaps",    PROC_F_SWAPS    },
	{ "vmstat",   PROC_F_VMSTAT   },
	{ "partitions", PROC_F_PARTITIONS },
	{ "diskstats", PROC_F_DISKSTATS },
};
#define ROOT_FILES_N ((int)(sizeof(root_files) / sizeof(root_files[0])))

/* static file table for per-pid subdirs */
static const struct {
	const char *name;
	int         type;
} pid_files[] = {
	{ "status",  PROC_F_P_STATUS  },
	{ "stat",    PROC_F_P_STAT    },
	{ "cmdline", PROC_F_P_CMDLINE },
	{ "maps",    PROC_F_P_MAPS    },
	{ "comm",    PROC_F_P_COMM    },
};
#define PID_FILES_N ((int)(sizeof(pid_files) / sizeof(pid_files[0])))

/* ── virtual file ops ────────────────────────────────────────── */

#define PROCFS_BUF_SIZE 4096

static ssize_t procfs_file_read(vfs_file_t *f, void *buf, size_t len)
{
	procfs_inode_priv_t *pr = f->inode->priv;
	if (!pr) return -1;

	uint64_t phys = (uint64_t)buddy_alloc(0);
	if (!phys) return -1;
	char *tmp = (char *)PHYS_TO_VIRT(phys);

	int n = 0;
	switch (pr->type) {
	case PROC_F_VERSION:  n = fill_version(tmp,  PROCFS_BUF_SIZE); break;
	case PROC_F_CPUINFO:  n = fill_cpuinfo(tmp,  PROCFS_BUF_SIZE); break;
	case PROC_F_MEMINFO:  n = fill_meminfo(tmp,  PROCFS_BUF_SIZE); break;
	case PROC_F_MOUNTS:   n = fill_mounts(tmp,   PROCFS_BUF_SIZE); break;
	case PROC_F_UPTIME:   n = fill_uptime(tmp,   PROCFS_BUF_SIZE); break;
	case PROC_F_STAT:     n = fill_stat(tmp,     PROCFS_BUF_SIZE); break;
	case PROC_F_LOADAVG:  n = fill_loadavg(tmp,  PROCFS_BUF_SIZE); break;
	case PROC_F_CMDLINE:  n = fill_cmdline(tmp,  PROCFS_BUF_SIZE); break;
	case PROC_F_FILESYSTEMS: n = fill_proc_filesystems(tmp, PROCFS_BUF_SIZE); break;
	case PROC_F_MODULES:  n = fill_modules(tmp,  PROCFS_BUF_SIZE); break;
	case PROC_F_SWAPS:    n = fill_swaps(tmp,    PROCFS_BUF_SIZE); break;
	case PROC_F_VMSTAT:   n = fill_vmstat(tmp,   PROCFS_BUF_SIZE); break;
	case PROC_F_PARTITIONS: n = fill_partitions(tmp, PROCFS_BUF_SIZE); break;
	case PROC_F_DISKSTATS: n = fill_diskstats(tmp, PROCFS_BUF_SIZE); break;
	case PROC_F_P_STATUS: n = fill_proc_status(tmp, PROCFS_BUF_SIZE, pr->pid); break;
	case PROC_F_P_STAT:   n = fill_proc_stat(tmp,   PROCFS_BUF_SIZE, pr->pid); break;
	case PROC_F_P_CMDLINE:n = fill_proc_cmdline(tmp, PROCFS_BUF_SIZE, pr->pid); break;
	case PROC_F_P_MAPS:   n = fill_proc_maps(tmp,    PROCFS_BUF_SIZE, pr->pid); break;
	case PROC_F_P_COMM:   n = fill_proc_comm(tmp,    PROCFS_BUF_SIZE, pr->pid); break;
	case PROC_F_NET_DEV:   n = fill_net_dev(tmp,   PROCFS_BUF_SIZE); break;
	case PROC_F_NET_TCP:   n = fill_net_tcp(tmp,   PROCFS_BUF_SIZE); break;
	case PROC_F_NET_UDP:   n = fill_net_udp(tmp,   PROCFS_BUF_SIZE); break;
	case PROC_F_NET_ROUTE: n = fill_net_route(tmp, PROCFS_BUF_SIZE); break;
	case PROC_F_NET_IF6:   n = fill_net_if_inet6(tmp, PROCFS_BUF_SIZE); break;
	case PROC_F_SYS_HZ:           { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"100\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_HOSTNAME:     { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,current_uts()->nodename); pb_char(&b,'\n'); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_DOMAINNAME:   { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,current_uts()->domainname); pb_char(&b,'\n'); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_OVERCOMMIT:   { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"0\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_NGROUPS_MAX:  { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"65536\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_PID_MAX:      { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"32768\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_FILE_MAX:     { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"65536\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_SOMAXCONN:    { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"128\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_RMEM_DEFAULT: { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"212992\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_WMEM_DEFAULT: { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"212992\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_RMEM_MAX:     { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"134217728\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_WMEM_MAX:     { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"134217728\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_NFILE:        { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"4096\t0\t65536\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_RANDOMIZE_VA: { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"0\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	default: n = 0; break;
	}

	/* serve from pos */
	ssize_t ret = 0;
	if (f->pos < (off_t)n) {
		size_t avail = (size_t)(n - f->pos);
		if (len > avail) len = avail;
		memcpy(buf, tmp + f->pos, len);
		f->pos += (off_t)len;
		ret = (ssize_t)len;
	}

	buddy_free(phys, 0);
	return ret;
}

static ssize_t procfs_sysctl_write(vfs_file_t *f, const void *buf, size_t len)
{
	procfs_inode_priv_t *pr = f->inode->priv;
	if (!pr) return -1;

	if (pr->type == PROC_F_SYS_HOSTNAME) {
		if (len == 0 || len >= 65) return -22;
		char tmp[65];
		if (copy_from_user(tmp, buf, len) != 0) return -14;
		size_t n = len;
		if (n > 0 && tmp[n-1] == '\n') n--;
		if (n >= 65) return -22;
		if (n > 0) {
			uts_namespace_t *ns = current_uts();
			memcpy(ns->nodename, tmp, n);
			ns->nodename[n] = '\0';
		}
		return (ssize_t)len;
	}
	return -1;
}

static vfs_fops_t procfs_fops = {
	.read  = procfs_file_read,
	.write = procfs_sysctl_write,
};

static vfs_fops_t procfs_dir_fops = { 0 };

/* ── inode allocation ────────────────────────────────────────── */

static uint64_t   proc_ino_next = 0x8000;
static spinlock_t proc_ino_lock = SPINLOCK_INIT;

static uint64_t proc_ino_alloc(void)
{
	spin_lock(&proc_ino_lock);
	uint64_t n = proc_ino_next++;
	spin_unlock(&proc_ino_lock);
	return n;
}

/* ── /proc/<pid>/mem — cross-process memory R/W (gdb) ────────────────────────
 * The file offset is the virtual address in the target's address space. We
 * briefly switch to the target's page tables to copy, like ptrace PEEKDATA. */
extern void arch_vm_switch(uint64_t root, unsigned asid);

static ssize_t procfs_mem_rw(vfs_file_t *f, void *ubuf, size_t len, int write)
{
	procfs_inode_priv_t *pr = (procfs_inode_priv_t *)f->inode->priv;
	process_t *t = proc_find(pr->pid);
	if (!t || !t->vm) return -3;   /* ESRCH */
	process_t *cur = task_current();
	uint64_t vaddr = (uint64_t)f->pos;
	char tmp[256];
	size_t done = 0;
	while (done < len) {
		size_t n = len - done; if (n > sizeof(tmp)) n = sizeof(tmp);
		if (write) {
			if (copy_from_user(tmp, (char *)ubuf + done, n) != 0) break;
			arch_vm_switch((uint64_t)t->vm->root, t->vm->asid);
			memcpy((void *)(vaddr + done), tmp, n);
			arch_vm_switch((uint64_t)cur->vm->root, cur->vm->asid);
		} else {
			arch_vm_switch((uint64_t)t->vm->root, t->vm->asid);
			memcpy(tmp, (void *)(vaddr + done), n);
			arch_vm_switch((uint64_t)cur->vm->root, cur->vm->asid);
			if (copy_to_user((char *)ubuf + done, tmp, n) != 0) break;
		}
		done += n;
	}
	f->pos += (long)done;
	return (ssize_t)done;
}

static ssize_t procfs_mem_read(vfs_file_t *f, void *buf, size_t len)
{ return procfs_mem_rw(f, buf, len, 0); }
static ssize_t procfs_mem_write(vfs_file_t *f, const void *buf, size_t len)
{ return procfs_mem_rw(f, (void *)buf, len, 1); }

static vfs_fops_t procfs_mem_fops = { .read = procfs_mem_read, .write = procfs_mem_write };

static vfs_inode_t *procfs_mem_inode(int pid)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino = proc_ino_alloc();
	in->mode = S_IFREG | 0600;
	in->nlink = 1; in->refcount = 1;
	in->fops = &procfs_mem_fops;
	procfs_inode_priv_t *pr = kmalloc(sizeof(*pr));
	if (!pr) { kfree(in); return NULL; }
	memset(pr, 0, sizeof(*pr));
	pr->pid = pid;
	in->priv = pr;
	return in;
}

static vfs_inode_t *procfs_inode_file(int pid, int type)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino     = proc_ino_alloc();
	in->mode    = S_IFREG | 0444;
	in->nlink   = 1;
	in->refcount= 1;
	in->fops    = &procfs_fops;

	procfs_inode_priv_t *pr = kmalloc(sizeof(*pr));
	if (!pr) { kfree(in); return NULL; }
	memset(pr, 0, sizeof(*pr));
	pr->pid  = pid;
	pr->type = type;
	in->priv = pr;
	return in;
}

/* ── /proc/<pid>/fd/<n> symlink ──────────────────────────────── */

typedef struct { int pid; int fd_n; } procfs_fdlink_priv_t;

static int procfs_fdlink_readlink(vfs_inode_t *in, char *buf, size_t sz)
{
	procfs_fdlink_priv_t *pr = in->priv;
	if (!pr) return -1;
	process_t *p = pr->pid == 0 ? task_current() : proc_find((pid_t)pr->pid);
	if (!p || !p->fds) return -1;
	vfs_file_t *f = fd_get(p->fds, pr->fd_n);
	if (!f || !f->inode) return -1;
	const char *path = f->path[0] ? f->path : "anon";
	size_t n = strlen(path);
	if (n + 1 > sz) return -1;
	memcpy(buf, path, n + 1);
	return 0;
}

static vfs_iops_t procfs_fdlink_iops = { .readlink = procfs_fdlink_readlink };

static vfs_inode_t *procfs_fdlink_inode(int pid, int fd_n)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = proc_ino_alloc();
	in->mode     = S_IFLNK | 0777;
	in->nlink    = 1;
	in->refcount = 1;
	in->iops     = &procfs_fdlink_iops;
	procfs_fdlink_priv_t *pr = kmalloc(sizeof(*pr));
	if (!pr) { kfree(in); return NULL; }
	pr->pid = pid; pr->fd_n = fd_n;
	in->priv = pr;
	return in;
}

/* ── /proc/<pid>/fd directory ────────────────────────────────── */

typedef struct { int pid; } procfs_fddir_priv_t;

static vfs_inode_t *procfs_fddir_lookup(vfs_inode_t *dir, const char *name)
{
	procfs_fddir_priv_t *pp = dir->priv;
	int pid = pp ? pp->pid : 0;
	int fd_n = 0; const char *s = name;
	if (!*s) return NULL;
	while (*s >= '0' && *s <= '9') { fd_n = fd_n * 10 + (*s - '0'); s++; }
	if (*s) return NULL;
	process_t *p = pid == 0 ? task_current() : proc_find((pid_t)pid);
	if (!p || !p->fds) return NULL;
	if (!fd_get(p->fds, fd_n)) return NULL;
	return procfs_fdlink_inode(pid, fd_n);
}

static int procfs_fddir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	procfs_fddir_priv_t *pp = dir->priv;
	int pid = pp ? pp->pid : 0;
	process_t *p = pid == 0 ? task_current() : proc_find((pid_t)pid);
	if (!p || !p->fds) return -1;
	uint32_t found = 0;
	for (int i = 0; i < p->fds->cap; i++) {
		if (!fd_get(p->fds, i)) continue;
		if (found == idx) {
			out->ino      = proc_ino_next;
			out->mode     = S_IFLNK | 0777;
			out->next_off = idx + 1;
			char tmp[12]; int ti = 0;
			int v = i;
			if (v == 0) { tmp[ti++] = '0'; } else {
				char rev[12]; int ri = 0;
				while (v) { rev[ri++] = '0' + v % 10; v /= 10; }
				for (int j = ri - 1; j >= 0; j--) tmp[ti++] = rev[j];
			}
			memcpy(out->name, tmp, (size_t)ti);
			out->name[ti] = '\0';
			return 0;
		}
		found++;
	}
	return -1;
}

static vfs_iops_t procfs_fddir_iops = {
	.lookup  = procfs_fddir_lookup,
	.readdir = procfs_fddir_readdir,
};

static vfs_inode_t *procfs_fddir_inode(int pid)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = proc_ino_alloc();
	in->mode     = S_IFDIR | 0555;
	in->nlink    = 2;
	in->refcount = 1;
	in->iops     = &procfs_fddir_iops;
	in->fops     = &procfs_dir_fops;
	procfs_fddir_priv_t *pp = kmalloc(sizeof(*pp));
	if (!pp) { kfree(in); return NULL; }
	pp->pid = pid;
	in->priv = pp;
	return in;
}

/* ── pid-dir iops ────────────────────────────────────────────── */

typedef struct {
	int pid;
} procfs_piddir_priv_t;

/* ── /proc/<pid>/{exe,cwd,root} symlinks ─────────────────────── */

enum { PIDLINK_EXE = 0, PIDLINK_CWD, PIDLINK_ROOT };
static const char *pid_links[] = { "exe", "cwd", "root" };
#define PID_LINKS_N ((int)(sizeof(pid_links) / sizeof(pid_links[0])))

typedef struct { int pid; int kind; } procfs_pidlink_priv_t;

static int procfs_pidlink_readlink(vfs_inode_t *in, char *buf, size_t sz)
{
	procfs_pidlink_priv_t *pr = in->priv;
	if (!pr) return -1;
	process_t *p = pr->pid == 0 ? task_current() : proc_find((pid_t)pr->pid);
	if (!p) return -1;
	const char *path;
	switch (pr->kind) {
	case PIDLINK_CWD:  path = p->cwd[0] ? p->cwd : "/"; break;
	case PIDLINK_ROOT: path = "/"; break;
	default:           path = p->exe_path[0] ? p->exe_path : p->name; break;
	}
	size_t n = strlen(path);
	if (n + 1 > sz) return -1;
	memcpy(buf, path, n + 1);
	return 0;
}

static vfs_iops_t procfs_pidlink_iops = { .readlink = procfs_pidlink_readlink };

static vfs_inode_t *procfs_pidlink_inode(int pid, int kind)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = proc_ino_alloc();
	in->mode     = S_IFLNK | 0777;
	in->nlink    = 1;
	in->refcount = 1;
	in->iops     = &procfs_pidlink_iops;
	procfs_pidlink_priv_t *pr = kmalloc(sizeof(*pr));
	if (!pr) { kfree(in); return NULL; }
	pr->pid = pid; pr->kind = kind;
	in->priv = pr;
	return in;
}

/* ── /proc/<pid>/ns/{uts,ipc} — namespace identity ("<kind>:[<inum>]") ── */

enum { NS_UTS = 0, NS_IPC, NS_TIME, NS_MNT, NS_PID, NS_USER, NS_NET, NS_CGROUP };
static const char *ns_kinds[] = { "uts", "ipc", "time", "mnt", "pid", "user", "net", "cgroup" };
#define NS_KINDS_N ((int)(sizeof(ns_kinds) / sizeof(ns_kinds[0])))

typedef struct { int pid; int kind; } procfs_nslink_priv_t;

static int procfs_nslink_readlink(vfs_inode_t *in, char *buf, size_t sz)
{
	procfs_nslink_priv_t *pp = in->priv;
	if (!pp) return -1;
	process_t *p = pp->pid == 0 ? task_current() : proc_find((pid_t)pp->pid);
	if (!p) return -1;

	const char *prefix;
	unsigned long inum;
	if (pp->kind == NS_IPC) {
		ipc_namespace_t *ns = p->ipc_ns ? p->ipc_ns : &init_ipc_ns;
		prefix = "ipc:["; inum = ns->inum;
	} else if (pp->kind == NS_TIME) {
		time_namespace_t *ns = p->time_ns ? p->time_ns : &init_time_ns;
		prefix = "time:["; inum = ns->inum;
	} else if (pp->kind == NS_MNT) {
		mnt_namespace_t *ns = p->mnt_ns ? p->mnt_ns : &init_mnt_ns;
		prefix = "mnt:["; inum = ns->inum;
	} else if (pp->kind == NS_PID) {
		pid_namespace_t *ns = p->pid_ns ? p->pid_ns : &init_pid_ns;
		prefix = "pid:["; inum = ns->inum;
	} else if (pp->kind == NS_USER) {
		user_namespace_t *ns = p->user_ns ? p->user_ns : &init_user_ns;
		prefix = "user:["; inum = ns->inum;
	} else if (pp->kind == NS_NET) {
		net_namespace_t *ns = p->net_ns ? p->net_ns : &init_net_ns;
		prefix = "net:["; inum = ns->inum;
	} else if (pp->kind == NS_CGROUP) {
		prefix = "cgroup:["; inum = cgroup_ns_inum(p->cgroup_ns);
	} else {
		uts_namespace_t *ns = p->uts_ns ? p->uts_ns : &init_uts_ns;
		prefix = "uts:["; inum = ns->inum;
	}

	char tmp[40]; int i = 0;
	for (const char *s = prefix; *s; s++) tmp[i++] = *s;
	char rev[20]; int r = 0;
	if (inum == 0) rev[r++] = '0';
	while (inum) { rev[r++] = '0' + (int)(inum % 10); inum /= 10; }
	while (r) tmp[i++] = rev[--r];
	tmp[i++] = ']';
	tmp[i] = '\0';
	if ((size_t)(i + 1) > sz) return -1;
	memcpy(buf, tmp, (size_t)i + 1);
	return 0;
}

static vfs_iops_t procfs_nslink_iops = { .readlink = procfs_nslink_readlink };

/*
 * Opening the ns link captures the target's namespace and pins it for the
 * life of the fd, so setns(2) can rejoin exactly that namespace even after
 * the target process unshares or exits. Separate fops per kind let setns
 * tell a UTS fd from an IPC one.
 */
static int procfs_utsns_open(vfs_inode_t *in, vfs_file_t *f, int flags)
{
	(void)flags;
	procfs_nslink_priv_t *pp = in->priv;
	int pid = pp ? pp->pid : 0;
	process_t *p = pid == 0 ? task_current() : proc_find((pid_t)pid);
	if (!p) return -1;
	f->priv = uts_ns_get(p->uts_ns);
	return 0;
}

static int procfs_utsns_close(vfs_file_t *f)
{
	uts_ns_put((uts_namespace_t *)f->priv);
	f->priv = NULL;
	return 0;
}

static vfs_fops_t procfs_utsns_fops = {
	.open  = procfs_utsns_open,
	.close = procfs_utsns_close,
};

static int procfs_ipcns_open(vfs_inode_t *in, vfs_file_t *f, int flags)
{
	(void)flags;
	procfs_nslink_priv_t *pp = in->priv;
	int pid = pp ? pp->pid : 0;
	process_t *p = pid == 0 ? task_current() : proc_find((pid_t)pid);
	if (!p) return -1;
	f->priv = ipc_ns_get(p->ipc_ns);
	return 0;
}

static int procfs_ipcns_close(vfs_file_t *f)
{
	ipc_ns_put((ipc_namespace_t *)f->priv);
	f->priv = NULL;
	return 0;
}

static vfs_fops_t procfs_ipcns_fops = {
	.open  = procfs_ipcns_open,
	.close = procfs_ipcns_close,
};

static int procfs_timens_open(vfs_inode_t *in, vfs_file_t *f, int flags)
{
	(void)flags;
	procfs_nslink_priv_t *pp = in->priv;
	int pid = pp ? pp->pid : 0;
	process_t *p = pid == 0 ? task_current() : proc_find((pid_t)pid);
	if (!p) return -1;
	f->priv = time_ns_get(p->time_ns);
	return 0;
}

static int procfs_timens_close(vfs_file_t *f)
{
	time_ns_put((time_namespace_t *)f->priv);
	f->priv = NULL;
	return 0;
}

static vfs_fops_t procfs_timens_fops = {
	.open  = procfs_timens_open,
	.close = procfs_timens_close,
};

static int procfs_mntns_open(vfs_inode_t *in, vfs_file_t *f, int flags)
{
	(void)flags;
	procfs_nslink_priv_t *pp = in->priv;
	int pid = pp ? pp->pid : 0;
	process_t *p = pid == 0 ? task_current() : proc_find((pid_t)pid);
	if (!p) return -1;
	f->priv = mnt_ns_get(p->mnt_ns);
	return 0;
}

static int procfs_mntns_close(vfs_file_t *f)
{
	mnt_ns_put((mnt_namespace_t *)f->priv);
	f->priv = NULL;
	return 0;
}

static vfs_fops_t procfs_mntns_fops = {
	.open  = procfs_mntns_open,
	.close = procfs_mntns_close,
};

static int procfs_pidns_open(vfs_inode_t *in, vfs_file_t *f, int flags)
{
	(void)flags;
	procfs_nslink_priv_t *pp = in->priv;
	int pid = pp ? pp->pid : 0;
	process_t *p = pid == 0 ? task_current() : proc_find((pid_t)pid);
	if (!p) return -1;
	f->priv = pid_ns_get(p->pid_ns);
	return 0;
}

static int procfs_pidns_close(vfs_file_t *f)
{
	pid_ns_put((pid_namespace_t *)f->priv);
	f->priv = NULL;
	return 0;
}

static vfs_fops_t procfs_pidns_fops = {
	.open  = procfs_pidns_open,
	.close = procfs_pidns_close,
};

static int procfs_userns_open(vfs_inode_t *in, vfs_file_t *f, int flags)
{
	(void)flags;
	procfs_nslink_priv_t *pp = in->priv;
	int pid = pp ? pp->pid : 0;
	process_t *p = pid == 0 ? task_current() : proc_find((pid_t)pid);
	if (!p) return -1;
	f->priv = user_ns_get(p->user_ns);
	return 0;
}

static int procfs_userns_close(vfs_file_t *f)
{
	user_ns_put((user_namespace_t *)f->priv);
	f->priv = NULL;
	return 0;
}

static vfs_fops_t procfs_userns_fops = {
	.open  = procfs_userns_open,
	.close = procfs_userns_close,
};

static int procfs_netns_open(vfs_inode_t *in, vfs_file_t *f, int flags)
{
	(void)flags;
	procfs_nslink_priv_t *pp = in->priv;
	int pid = pp ? pp->pid : 0;
	process_t *p = pid == 0 ? task_current() : proc_find((pid_t)pid);
	if (!p) return -1;
	f->priv = net_ns_get(p->net_ns);
	return 0;
}

static int procfs_netns_close(vfs_file_t *f)
{
	net_ns_put((net_namespace_t *)f->priv);
	f->priv = NULL;
	return 0;
}

static vfs_fops_t procfs_netns_fops = {
	.open  = procfs_netns_open,
	.close = procfs_netns_close,
};

static int procfs_cgroupns_open(vfs_inode_t *in, vfs_file_t *f, int flags)
{
	(void)flags;
	procfs_nslink_priv_t *pp = in->priv;
	int pid = pp ? pp->pid : 0;
	process_t *p = pid == 0 ? task_current() : proc_find((pid_t)pid);
	if (!p) return -1;
	f->priv = cgroup_ns_get(p->cgroup_ns);
	return 0;
}

static int procfs_cgroupns_close(vfs_file_t *f)
{
	cgroup_ns_put((struct cgroup_namespace *)f->priv);
	f->priv = NULL;
	return 0;
}

static vfs_fops_t procfs_cgroupns_fops = {
	.open  = procfs_cgroupns_open,
	.close = procfs_cgroupns_close,
};

/* setns(2) helpers: the namespace an open ns fd refers to, or NULL */
uts_namespace_t *nsfile_to_uts(vfs_file_t *f)
{
	if (!f || !f->inode || f->inode->fops != &procfs_utsns_fops) return NULL;
	return (uts_namespace_t *)f->priv;
}

ipc_namespace_t *nsfile_to_ipc(vfs_file_t *f)
{
	if (!f || !f->inode || f->inode->fops != &procfs_ipcns_fops) return NULL;
	return (ipc_namespace_t *)f->priv;
}

time_namespace_t *nsfile_to_time(vfs_file_t *f)
{
	if (!f || !f->inode || f->inode->fops != &procfs_timens_fops) return NULL;
	return (time_namespace_t *)f->priv;
}

mnt_namespace_t *nsfile_to_mnt(vfs_file_t *f)
{
	if (!f || !f->inode || f->inode->fops != &procfs_mntns_fops) return NULL;
	return (mnt_namespace_t *)f->priv;
}

pid_namespace_t *nsfile_to_pid(vfs_file_t *f)
{
	if (!f || !f->inode || f->inode->fops != &procfs_pidns_fops) return NULL;
	return (pid_namespace_t *)f->priv;
}

user_namespace_t *nsfile_to_user(vfs_file_t *f)
{
	if (!f || !f->inode || f->inode->fops != &procfs_userns_fops) return NULL;
	return (user_namespace_t *)f->priv;
}

net_namespace_t *nsfile_to_net(vfs_file_t *f)
{
	if (!f || !f->inode || f->inode->fops != &procfs_netns_fops) return NULL;
	return (net_namespace_t *)f->priv;
}

struct cgroup_namespace *nsfile_to_cgroup(vfs_file_t *f)
{
	if (!f || !f->inode || f->inode->fops != &procfs_cgroupns_fops) return NULL;
	return (struct cgroup_namespace *)f->priv;
}

static vfs_inode_t *procfs_nslink_inode(int pid, int kind)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = proc_ino_alloc();
	in->mode     = S_IFLNK | 0777;
	in->nlink    = 1;
	in->refcount = 1;
	in->iops     = &procfs_nslink_iops;
	in->fops     = (kind == NS_IPC)  ? &procfs_ipcns_fops  :
	               (kind == NS_TIME) ? &procfs_timens_fops :
	               (kind == NS_MNT)  ? &procfs_mntns_fops  :
	               (kind == NS_PID)  ? &procfs_pidns_fops  :
	               (kind == NS_USER) ? &procfs_userns_fops :
	               (kind == NS_NET)  ? &procfs_netns_fops  :
	               (kind == NS_CGROUP) ? &procfs_cgroupns_fops : &procfs_utsns_fops;
	procfs_nslink_priv_t *pp = kmalloc(sizeof(*pp));
	if (!pp) { kfree(in); return NULL; }
	pp->pid = pid; pp->kind = kind;
	in->priv = pp;
	return in;
}

static vfs_inode_t *procfs_nsdir_lookup(vfs_inode_t *dir, const char *name)
{
	procfs_fddir_priv_t *pp = dir->priv;
	int pid = pp ? pp->pid : 0;
	for (int k = 0; k < NS_KINDS_N; k++)
		if (strcmp(name, ns_kinds[k]) == 0) return procfs_nslink_inode(pid, k);
	return NULL;
}

static int procfs_nsdir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	if (idx >= (uint32_t)NS_KINDS_N) return -1;
	procfs_fddir_priv_t *pp = dir->priv;
	int pid = pp ? pp->pid : 0;
	const char *n = ns_kinds[idx];
	int nlen = 0; while (n[nlen]) nlen++;
	out->ino      = 0x8000 + (uint64_t)pid * 100 + 80 + idx;
	out->mode     = S_IFLNK | 0777;
	out->next_off = idx + 1;
	memcpy(out->name, n, nlen); out->name[nlen] = '\0';
	return 0;
}

static vfs_iops_t procfs_nsdir_iops = {
	.lookup  = procfs_nsdir_lookup,
	.readdir = procfs_nsdir_readdir,
};

static vfs_inode_t *procfs_nsdir_inode(int pid)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = proc_ino_alloc();
	in->mode     = S_IFDIR | 0555;
	in->nlink    = 2;
	in->refcount = 1;
	in->iops     = &procfs_nsdir_iops;
	in->fops     = &procfs_dir_fops;
	procfs_fddir_priv_t *pp = kmalloc(sizeof(*pp));
	if (!pp) { kfree(in); return NULL; }
	pp->pid = pid;
	in->priv = pp;
	return in;
}

/* ── /proc/<pid>/timens_offsets — read/write the time ns offsets ── */

static int procfs_off_fmt(char *b, const char *name, long sec, long nsec)
{
	int i = 0;
	for (const char *s = name; *s; s++) b[i++] = *s;
	b[i++] = ' ';
	long v = sec;
	if (v < 0) { b[i++] = '-'; v = -v; }
	char rev[24]; int r = 0;
	if (v == 0) rev[r++] = '0';
	while (v) { rev[r++] = '0' + (int)(v % 10); v /= 10; }
	while (r) b[i++] = rev[--r];
	b[i++] = ' ';
	v = nsec; r = 0;
	if (v < 0) { b[i++] = '-'; v = -v; }
	if (v == 0) rev[r++] = '0';
	while (v) { rev[r++] = '0' + (int)(v % 10); v /= 10; }
	while (r) b[i++] = rev[--r];
	b[i++] = '\n';
	return i;
}

static ssize_t procfs_timens_off_read(vfs_file_t *f, void *buf, size_t len)
{
	procfs_fddir_priv_t *pr = f->inode->priv;
	int pid = pr ? pr->pid : 0;
	process_t *p = pid == 0 ? task_current() : proc_find((pid_t)pid);
	if (!p) return -1;
	time_namespace_t *ns = p->time_ns_new ? p->time_ns_new
	                     : (p->time_ns ? p->time_ns : &init_time_ns);
	char tmp[96]; int n = 0;
	n += procfs_off_fmt(tmp + n, "monotonic", ns->mono_sec, ns->mono_nsec);
	n += procfs_off_fmt(tmp + n, "boottime",  ns->boot_sec, ns->boot_nsec);
	if (f->pos >= (off_t)n) return 0;
	size_t avail = (size_t)(n - f->pos);
	if (len > avail) len = avail;
	if (copy_to_user(buf, tmp + f->pos, len) != 0) return -14;
	f->pos += (off_t)len;
	return (ssize_t)len;
}

static ssize_t procfs_timens_off_write(vfs_file_t *f, const void *buf, size_t len)
{
	procfs_fddir_priv_t *pr = f->inode->priv;
	int pid = pr ? pr->pid : 0;
	process_t *p = pid == 0 ? task_current() : proc_find((pid_t)pid);
	if (!p) return -1;
	/* offsets may only be set on a pending time ns (one no task has joined) */
	if (!p->time_ns_new) return -13; /* EACCES */

	char kb[128];
	if (len >= sizeof(kb)) len = sizeof(kb) - 1;
	if (copy_from_user(kb, buf, len) != 0) return -14;
	kb[len] = '\0';

	const char *s = kb;
	while (*s) {
		while (*s == ' ' || *s == '\t' || *s == '\n') s++;
		if (!*s) break;
		char first = *s;
		while (*s && *s != ' ' && *s != '\t') s++;          /* skip name */
		while (*s == ' ' || *s == '\t') s++;
		int neg = 0; if (*s == '-') { neg = 1; s++; }
		long sec = 0; while (*s >= '0' && *s <= '9') sec = sec * 10 + (*s++ - '0');
		if (neg) sec = -sec;
		while (*s == ' ' || *s == '\t') s++;
		int neg2 = 0; if (*s == '-') { neg2 = 1; s++; }
		long nsec = 0; while (*s >= '0' && *s <= '9') nsec = nsec * 10 + (*s++ - '0');
		if (neg2) nsec = -nsec;
		while (*s && *s != '\n') s++;

		if (first == 'm') { p->time_ns_new->mono_sec = sec; p->time_ns_new->mono_nsec = nsec; }
		else if (first == 'b') { p->time_ns_new->boot_sec = sec; p->time_ns_new->boot_nsec = nsec; }
	}
	return (ssize_t)len;
}

static vfs_fops_t procfs_timens_off_fops = {
	.read  = procfs_timens_off_read,
	.write = procfs_timens_off_write,
};

static vfs_inode_t *procfs_timens_off_inode(int pid)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = proc_ino_alloc();
	in->mode     = S_IFREG | 0644;
	in->nlink    = 1;
	in->refcount = 1;
	in->fops     = &procfs_timens_off_fops;
	procfs_fddir_priv_t *pp = kmalloc(sizeof(*pp));
	if (!pp) { kfree(in); return NULL; }
	pp->pid = pid;
	in->priv = pp;
	return in;
}

/* ── /proc/<pid>/{uid,gid}_map — user namespace id mappings ──── */

typedef struct { int pid; int is_gid; } procfs_uidmap_priv_t;

static ssize_t procfs_uidmap_read(vfs_file_t *f, void *buf, size_t len)
{
	procfs_uidmap_priv_t *pr = f->inode->priv;
	process_t *p = pr->pid == 0 ? task_current() : proc_find((pid_t)pr->pid);
	if (!p) return -1;
	user_namespace_t *ns = p->user_ns ? p->user_ns : &init_user_ns;
	char tmp[256];
	int n = user_ns_format_map(ns, tmp, sizeof(tmp), pr->is_gid);
	if (f->pos >= (off_t)n) return 0;
	size_t avail = (size_t)(n - f->pos);
	if (len > avail) len = avail;
	if (copy_to_user(buf, tmp + f->pos, len) != 0) return -14;
	f->pos += (off_t)len;
	return (ssize_t)len;
}

static ssize_t procfs_uidmap_write(vfs_file_t *f, const void *buf, size_t len)
{
	procfs_uidmap_priv_t *pr = f->inode->priv;
	process_t *p = pr->pid == 0 ? task_current() : proc_find((pid_t)pr->pid);
	if (!p || !p->user_ns) return -1;   /* only a non-initial user ns is mappable */
	char kb[256];
	size_t n = len < sizeof(kb) - 1 ? len : sizeof(kb) - 1;
	if (copy_from_user(kb, buf, n) != 0) return -14;
	kb[n] = '\0';
	if (user_ns_set_map(p->user_ns, kb, pr->is_gid) != 0) return -1;
	return (ssize_t)len;
}

static vfs_fops_t procfs_uidmap_fops = { .read = procfs_uidmap_read, .write = procfs_uidmap_write };

static vfs_inode_t *procfs_uidmap_inode(int pid, int is_gid)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = proc_ino_alloc();
	in->mode     = S_IFREG | 0644;
	in->nlink    = 1;
	in->refcount = 1;
	in->fops     = &procfs_uidmap_fops;
	procfs_uidmap_priv_t *pr = kmalloc(sizeof(*pr));
	if (!pr) { kfree(in); return NULL; }
	pr->pid = pid; pr->is_gid = is_gid;
	in->priv = pr;
	return in;
}

/* ── /proc/<pid>/cgroup — cgroup v2 membership ("0::<path>") ── */

static ssize_t procfs_cgroup_read(vfs_file_t *f, void *buf, size_t len)
{
	procfs_fddir_priv_t *pr = f->inode->priv;
	int pid = pr ? pr->pid : 0;
	process_t *p = pid == 0 ? task_current() : proc_find((pid_t)pid);
	if (!p) return -1;
	char tmp[160];
	tmp[0] = '0'; tmp[1] = ':'; tmp[2] = ':';
	int n = 3 + cgroup_proc_path(p, tmp + 3, (int)sizeof(tmp) - 4);
	tmp[n++] = '\n';
	if (f->pos >= (off_t)n) return 0;
	size_t avail = (size_t)(n - f->pos);
	if (len > avail) len = avail;
	if (copy_to_user(buf, tmp + f->pos, len) != 0) return -14;
	f->pos += (off_t)len;
	return (ssize_t)len;
}

static vfs_fops_t procfs_cgroup_fops = { .read = procfs_cgroup_read };

static vfs_inode_t *procfs_cgroup_inode(int pid)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = proc_ino_alloc();
	in->mode     = S_IFREG | 0444;
	in->nlink    = 1;
	in->refcount = 1;
	in->fops     = &procfs_cgroup_fops;
	procfs_fddir_priv_t *pp = kmalloc(sizeof(*pp));
	if (!pp) { kfree(in); return NULL; }
	pp->pid = pid;
	in->priv = pp;
	return in;
}

static vfs_inode_t *procfs_piddir_lookup(vfs_inode_t *dir, const char *name)
{
	procfs_piddir_priv_t *pp = dir->priv;
	if (!pp) return NULL;
	int pid = pp->pid;
	if (strcmp(name, "fd") == 0)
		return procfs_fddir_inode(pid);
	if (strcmp(name, "ns") == 0)
		return procfs_nsdir_inode(pid);
	if (strcmp(name, "timens_offsets") == 0)
		return procfs_timens_off_inode(pid);
	if (strcmp(name, "uid_map") == 0)
		return procfs_uidmap_inode(pid, 0);
	if (strcmp(name, "gid_map") == 0)
		return procfs_uidmap_inode(pid, 1);
	if (strcmp(name, "cgroup") == 0)
		return procfs_cgroup_inode(pid);
	if (strcmp(name, "mem") == 0)
		return procfs_mem_inode(pid);
	for (int i = 0; i < PID_LINKS_N; i++) {
		if (strcmp(pid_links[i], name) == 0)
			return procfs_pidlink_inode(pid, i);
	}
	for (int i = 0; i < PID_FILES_N; i++) {
		if (strcmp(pid_files[i].name, name) == 0)
			return procfs_inode_file(pid, pid_files[i].type);
	}
	return NULL;
}

static int procfs_piddir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	if (idx >= (uint32_t)(PID_FILES_N + 3 + PID_LINKS_N)) return -1;
	procfs_piddir_priv_t *pp = dir->priv;
	int pid = pp ? pp->pid : 0;
	if (idx < PID_FILES_N) {
		out->ino      = 0x8000 + (uint64_t)pid * 100 + idx;
		out->mode     = S_IFREG | 0444;
		out->next_off = idx + 1;
		const char *n = pid_files[idx].name;
		int nlen = 0; while (n[nlen]) nlen++;
		memcpy(out->name, n, nlen); out->name[nlen] = '\0';
	} else if (idx == PID_FILES_N) {
		out->ino      = 0x8000 + (uint64_t)pid * 100 + 99;
		out->mode     = S_IFDIR | 0555;
		out->next_off = idx + 1;
		memcpy(out->name, "fd", 3);
	} else if (idx < (uint32_t)(PID_FILES_N + 1 + PID_LINKS_N)) {
		const char *n = pid_links[idx - PID_FILES_N - 1];
		int nlen = 0; while (n[nlen]) nlen++;
		out->ino      = 0x8000 + (uint64_t)pid * 100 + 90 + (idx - PID_FILES_N - 1);
		out->mode     = S_IFLNK | 0777;
		out->next_off = idx + 1;
		memcpy(out->name, n, nlen); out->name[nlen] = '\0';
	} else if (idx == (uint32_t)(PID_FILES_N + 1 + PID_LINKS_N)) {
		out->ino      = 0x8000 + (uint64_t)pid * 100 + 81;
		out->mode     = S_IFDIR | 0555;
		out->next_off = idx + 1;
		memcpy(out->name, "ns", 3);
	} else {
		out->ino      = 0x8000 + (uint64_t)pid * 100 + 82;
		out->mode     = S_IFREG | 0644;
		out->next_off = idx + 1;
		memcpy(out->name, "timens_offsets", 15);
	}
	return 0;
}

static int procfs_piddir_stat(vfs_inode_t *in, vfs_stat_t *out)
{
	out->ino   = in->ino;
	out->mode  = in->mode;
	out->nlink = in->nlink;
	out->size  = 0;
	return 0;
}

static vfs_iops_t procfs_piddir_iops = {
	.lookup  = procfs_piddir_lookup,
	.readdir = procfs_piddir_readdir,
	.stat    = procfs_piddir_stat,
};

static vfs_inode_t *procfs_piddir_inode(int pid)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino     = 0x8000 + (uint64_t)pid * 100 + 50;
	in->mode    = S_IFDIR | 0555;
	in->nlink   = 2;
	in->refcount= 1;
	in->iops    = &procfs_piddir_iops;
	in->fops    = &procfs_dir_fops;

	procfs_piddir_priv_t *pp = kmalloc(sizeof(*pp));
	if (!pp) { kfree(in); return NULL; }
	pp->pid  = pid;
	in->priv = pp;
	return in;
}

/* ── /proc/net fill functions ────────────────────────────────── */

static void pb_hex8(pb_t *b, uint32_t v)
{
	static const char hx[] = "0123456789ABCDEF";
	for (int i = 7; i >= 0; i--)
		pb_char(b, hx[(v >> (i * 4)) & 0xF]);
}

static void pb_hex4(pb_t *b, uint16_t v)
{
	static const char hx[] = "0123456789ABCDEF";
	pb_char(b, hx[(v >> 12) & 0xF]);
	pb_char(b, hx[(v >>  8) & 0xF]);
	pb_char(b, hx[(v >>  4) & 0xF]);
	pb_char(b, hx[(v      ) & 0xF]);
}

static int fill_net_dev(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	pb_str(&b, "Inter-|   Receive                                                |  Transmit\n");
	pb_str(&b, " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");
	/* lo */
	pb_str(&b, "    lo:       0       0    0    0    0     0          0         0        0       0    0    0    0     0       0          0\n");
	/* eth0 */
	netdev_t *dev = net_dev(0);
	if (dev) {
		pb_str(&b, "  eth0:       0       0    0    0    0     0          0         0        0       0    0    0    0     0       0          0\n");
	}
	return (int)(sz - b.rem);
}

static int fill_net_tcp(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	pb_str(&b, "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
	int sl = 0;
	for (int i = 0; i < gsock_cap; i++) {
		socket_t *s = gsock[i];
		if (!s || s->type != SOCK_STREAM || s->domain != AF_INET) continue;
		/* format: "   N: LLLLLLLL:PPPP RRRRRRRR:PPPP SS 00000000:00000000 ..." */
		pb_str(&b, "   ");
		pb_u64(&b, (uint64_t)sl);
		pb_str(&b, ": ");
		/* local addr in network byte order (little-endian hex = host byte order reversed) */
		uint32_t la = s->local_ip;
		pb_hex8(&b, ((la & 0xFF) << 24) | (((la >> 8) & 0xFF) << 16) |
		             (((la >> 16) & 0xFF) << 8) | ((la >> 24) & 0xFF));
		pb_char(&b, ':');
		pb_hex4(&b, s->local_port);
		pb_str(&b, " ");
		uint32_t ra = s->remote_ip;
		pb_hex8(&b, ((ra & 0xFF) << 24) | (((ra >> 8) & 0xFF) << 16) |
		             (((ra >> 16) & 0xFF) << 8) | ((ra >> 24) & 0xFF));
		pb_char(&b, ':');
		pb_hex4(&b, s->remote_port);
		pb_str(&b, " ");
		pb_hex4(&b, (uint16_t)s->tcp_state);
		pb_str(&b, " 00000000:00000000 00:00000000 00000000     0        0 ");
		pb_u64(&b, (uint64_t)(0x10000 + i));
		pb_str(&b, " 1 0000000000000000 100 0 0 10 0\n");
		sl++;
	}
	return (int)(sz - b.rem);
}

static int fill_net_udp(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	pb_str(&b, "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
	int sl = 0;
	for (int i = 0; i < gsock_cap; i++) {
		socket_t *s = gsock[i];
		if (!s || s->type != SOCK_DGRAM || s->domain != AF_INET) continue;
		pb_str(&b, "   ");
		pb_u64(&b, (uint64_t)sl);
		pb_str(&b, ": ");
		uint32_t la = s->local_ip;
		pb_hex8(&b, ((la & 0xFF) << 24) | (((la >> 8) & 0xFF) << 16) |
		             (((la >> 16) & 0xFF) << 8) | ((la >> 24) & 0xFF));
		pb_char(&b, ':');
		pb_hex4(&b, s->local_port);
		pb_str(&b, " 00000000:0000 07 00000000:00000000 00:00000000 00000000     0        0 ");
		pb_u64(&b, (uint64_t)(0x10000 + i));
		pb_str(&b, " 1 0000000000000000\n");
		sl++;
	}
	return (int)(sz - b.rem);
}

static int fill_net_route(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	pb_str(&b, "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");
	netdev_t *dev = net_dev(0);
	if (dev) {
		/* default route */
		pb_str(&b, "eth0\t00000000\t");
		uint32_t gw = dev->gw;
		pb_hex8(&b, ((gw & 0xFF) << 24) | (((gw >> 8) & 0xFF) << 16) |
		             (((gw >> 16) & 0xFF) << 8) | ((gw >> 24) & 0xFF));
		pb_str(&b, "\t0003\t0\t0\t0\t00000000\t0\t0\t0\n");
		/* local subnet */
		pb_str(&b, "eth0\t");
		uint32_t net = dev->ip & dev->mask;
		pb_hex8(&b, ((net & 0xFF) << 24) | (((net >> 8) & 0xFF) << 16) |
		             (((net >> 16) & 0xFF) << 8) | ((net >> 24) & 0xFF));
		pb_str(&b, "\t00000000\t0001\t0\t0\t0\t");
		pb_hex8(&b, ((dev->mask & 0xFF) << 24) | (((dev->mask >> 8) & 0xFF) << 16) |
		             (((dev->mask >> 16) & 0xFF) << 8) | ((dev->mask >> 24) & 0xFF));
		pb_str(&b, "\t0\t0\t0\n");
	}
	return (int)(sz - b.rem);
}

static int fill_net_if_inet6(char *buf, size_t sz)
{
	(void)buf; (void)sz; return 0; /* no IPv6 */
}

static const struct { const char *name; int type; } net_files[] = {
	{ "dev",     PROC_F_NET_DEV   },
	{ "tcp",     PROC_F_NET_TCP   },
	{ "udp",     PROC_F_NET_UDP   },
	{ "route",   PROC_F_NET_ROUTE },
	{ "if_inet6",PROC_F_NET_IF6   },
};
#define NET_FILES_N 5

static vfs_inode_t *procfs_netdir_inode(void);

static vfs_inode_t *procfs_netdir_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	for (int i = 0; i < NET_FILES_N; i++)
		if (strcmp(net_files[i].name, name) == 0)
			return procfs_inode_file(-1, net_files[i].type);
	return NULL;
}

static int procfs_netdir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx >= (uint32_t)NET_FILES_N) return -1;
	out->ino      = 0x9000 + idx;
	out->mode     = S_IFREG | 0444;
	out->next_off = idx + 1;
	const char *n = net_files[idx].name;
	int nlen = 0; while (n[nlen]) nlen++;
	memcpy(out->name, n, nlen); out->name[nlen] = '\0';
	return 0;
}

static int procfs_netdir_stat(vfs_inode_t *in, vfs_stat_t *out)
{
	out->ino = in->ino; out->mode = in->mode; out->nlink = 2; out->size = 0;
	return 0;
}

static vfs_iops_t procfs_netdir_iops = {
	.lookup  = procfs_netdir_lookup,
	.readdir = procfs_netdir_readdir,
	.stat    = procfs_netdir_stat,
};

static vfs_inode_t *procfs_netdir_inode(void)
{
	static vfs_inode_t ndir;
	static int init = 0;
	if (!init) {
		memset(&ndir, 0, sizeof(ndir));
		ndir.ino      = 0x9000;
		ndir.mode     = S_IFDIR | 0555;
		ndir.nlink    = 2;
		ndir.refcount = 1;
		ndir.iops     = &procfs_netdir_iops;
		ndir.fops     = &procfs_dir_fops;
		ndir.cached   = 1;
		init = 1;
	}
	return &ndir;
}

/* ── /proc/sys ───────────────────────────────────────────────── */

typedef struct { const char *name; int type; } procfs_sysfile_t;

static procfs_sysfile_t sys_kernel_files[] = {
	{ "ostype",        PROC_F_SYS_HOSTNAME    }, /* returns the hostname */
	{ "hostname",      PROC_F_SYS_HOSTNAME    },
	{ "domainname",    PROC_F_SYS_DOMAINNAME  },
	{ "ngroups_max",   PROC_F_SYS_NGROUPS_MAX },
	{ "pid_max",       PROC_F_SYS_PID_MAX     },
	{ "overcommit_memory", PROC_F_SYS_OVERCOMMIT },
	{ "randomize_va_space", PROC_F_SYS_RANDOMIZE_VA },
};
#define SYS_KERNEL_N 7

static procfs_sysfile_t sys_net_core_files[] = {
	{ "somaxconn",       PROC_F_SYS_SOMAXCONN    },
	{ "rmem_default",    PROC_F_SYS_RMEM_DEFAULT },
	{ "wmem_default",    PROC_F_SYS_WMEM_DEFAULT },
	{ "rmem_max",        PROC_F_SYS_RMEM_MAX     },
	{ "wmem_max",        PROC_F_SYS_WMEM_MAX     },
};
#define SYS_NET_CORE_N 5

static procfs_sysfile_t sys_fs_files[] = {
	{ "file-max",    PROC_F_SYS_FILE_MAX },
	{ "file-nr",     PROC_F_SYS_NFILE    },
};
#define SYS_FS_N 2

/* forward declarations */
static vfs_inode_t *procfs_sysdir_inode(void);
static vfs_inode_t *procfs_syskerneldir_inode(void);
static vfs_inode_t *procfs_sysnetcoredir_inode(void);
static vfs_inode_t *procfs_sysfsdir_inode(void);
/* generic sysfile inode */
static vfs_inode_t *procfs_sysfile_inode(int type)
{
	vfs_inode_t *in = procfs_inode_file(-1, type);
	if (in && type == PROC_F_SYS_HOSTNAME)
		in->mode = S_IFREG | 0644;
	return in;
}

/* /proc/sys/kernel */
static vfs_inode_t *procfs_syskerneldir_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	for (int i = 0; i < SYS_KERNEL_N; i++)
		if (strcmp(sys_kernel_files[i].name, name) == 0)
			return procfs_sysfile_inode(sys_kernel_files[i].type);
	return NULL;
}
static int procfs_syskerneldir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx >= (uint32_t)SYS_KERNEL_N) return -1;
	out->ino = 0xa100 + idx; out->mode = S_IFREG | 0444; out->next_off = idx + 1;
	const char *n = sys_kernel_files[idx].name;
	int nlen = 0; while (n[nlen]) nlen++;
	memcpy(out->name, n, nlen); out->name[nlen] = '\0';
	return 0;
}
static int procfs_syssubdir_stat(vfs_inode_t *in, vfs_stat_t *out)
{
	out->ino = in->ino; out->mode = in->mode; out->nlink = 2; out->size = 0;
	return 0;
}
static vfs_iops_t procfs_syskerneldir_iops = {
	.lookup  = procfs_syskerneldir_lookup,
	.readdir = procfs_syskerneldir_readdir,
	.stat    = procfs_syssubdir_stat,
};
static vfs_inode_t *procfs_syskerneldir_inode(void)
{
	static vfs_inode_t d; static int init = 0;
	if (!init) {
		memset(&d, 0, sizeof(d)); d.ino = 0xa100; d.mode = S_IFDIR | 0555;
		d.nlink = 2; d.refcount = 1; d.iops = &procfs_syskerneldir_iops;
		d.fops = &procfs_dir_fops; d.cached = 1; init = 1;
	}
	return &d;
}

/* /proc/sys/net/core */
static vfs_inode_t *procfs_sysnetcoredir_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	for (int i = 0; i < SYS_NET_CORE_N; i++)
		if (strcmp(sys_net_core_files[i].name, name) == 0)
			return procfs_sysfile_inode(sys_net_core_files[i].type);
	return NULL;
}
static int procfs_sysnetcoredir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx >= (uint32_t)SYS_NET_CORE_N) return -1;
	out->ino = 0xa200 + idx; out->mode = S_IFREG | 0444; out->next_off = idx + 1;
	const char *n = sys_net_core_files[idx].name;
	int nlen = 0; while (n[nlen]) nlen++;
	memcpy(out->name, n, nlen); out->name[nlen] = '\0';
	return 0;
}
static vfs_iops_t procfs_sysnetcoredir_iops = {
	.lookup  = procfs_sysnetcoredir_lookup,
	.readdir = procfs_sysnetcoredir_readdir,
	.stat    = procfs_syssubdir_stat,
};
static vfs_inode_t *procfs_sysnetcoredir_inode(void)
{
	static vfs_inode_t d; static int init = 0;
	if (!init) {
		memset(&d, 0, sizeof(d)); d.ino = 0xa200; d.mode = S_IFDIR | 0555;
		d.nlink = 2; d.refcount = 1; d.iops = &procfs_sysnetcoredir_iops;
		d.fops = &procfs_dir_fops; d.cached = 1; init = 1;
	}
	return &d;
}

/* /proc/sys/net — contains core/ */
static vfs_inode_t *procfs_sysnetdir_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	if (strcmp(name, "core") == 0) return procfs_sysnetcoredir_inode();
	return NULL;
}
static int procfs_sysnetdir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx != 0) return -1;
	out->ino = 0xa210; out->mode = S_IFDIR | 0555; out->next_off = 1;
	out->name[0] = 'c'; out->name[1] = 'o'; out->name[2] = 'r';
	out->name[3] = 'e'; out->name[4] = '\0';
	return 0;
}
static vfs_iops_t procfs_sysnetdir_iops = {
	.lookup  = procfs_sysnetdir_lookup,
	.readdir = procfs_sysnetdir_readdir,
	.stat    = procfs_syssubdir_stat,
};
static vfs_inode_t *procfs_sysnetdir_inode(void)
{
	static vfs_inode_t d; static int init = 0;
	if (!init) {
		memset(&d, 0, sizeof(d)); d.ino = 0xa210; d.mode = S_IFDIR | 0555;
		d.nlink = 2; d.refcount = 1; d.iops = &procfs_sysnetdir_iops;
		d.fops = &procfs_dir_fops; d.cached = 1; init = 1;
	}
	return &d;
}

/* /proc/sys/fs */
static vfs_inode_t *procfs_sysfsdir_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	for (int i = 0; i < SYS_FS_N; i++)
		if (strcmp(sys_fs_files[i].name, name) == 0)
			return procfs_sysfile_inode(sys_fs_files[i].type);
	return NULL;
}
static int procfs_sysfsdir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx >= (uint32_t)SYS_FS_N) return -1;
	out->ino = 0xa300 + idx; out->mode = S_IFREG | 0444; out->next_off = idx + 1;
	const char *n = sys_fs_files[idx].name;
	int nlen = 0; while (n[nlen]) nlen++;
	memcpy(out->name, n, nlen); out->name[nlen] = '\0';
	return 0;
}
static vfs_iops_t procfs_sysfsdir_iops = {
	.lookup  = procfs_sysfsdir_lookup,
	.readdir = procfs_sysfsdir_readdir,
	.stat    = procfs_syssubdir_stat,
};
static vfs_inode_t *procfs_sysfsdir_inode(void)
{
	static vfs_inode_t d; static int init = 0;
	if (!init) {
		memset(&d, 0, sizeof(d)); d.ino = 0xa300; d.mode = S_IFDIR | 0555;
		d.nlink = 2; d.refcount = 1; d.iops = &procfs_sysfsdir_iops;
		d.fops = &procfs_dir_fops; d.cached = 1; init = 1;
	}
	return &d;
}

/* /proc/sys root — contains kernel/, net/, fs/ */
static vfs_inode_t *procfs_sysdir_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	if (strcmp(name, "kernel") == 0) return procfs_syskerneldir_inode();
	if (strcmp(name, "net")    == 0) return procfs_sysnetdir_inode();
	if (strcmp(name, "fs")     == 0) return procfs_sysfsdir_inode();
	return NULL;
}
static int procfs_sysdir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	static const char *subdirs[] = { "kernel", "net", "fs" };
	if (idx >= 3) return -1;
	out->ino = 0xa000 + idx; out->mode = S_IFDIR | 0555; out->next_off = idx + 1;
	const char *n = subdirs[idx];
	int nlen = 0; while (n[nlen]) nlen++;
	memcpy(out->name, n, nlen); out->name[nlen] = '\0';
	return 0;
}
static int procfs_sysdir_stat(vfs_inode_t *in, vfs_stat_t *out)
{
	out->ino = in->ino; out->mode = in->mode; out->nlink = 5; out->size = 0;
	return 0;
}
static vfs_iops_t procfs_sysdir_iops = {
	.lookup  = procfs_sysdir_lookup,
	.readdir = procfs_sysdir_readdir,
	.stat    = procfs_sysdir_stat,
};
static vfs_inode_t *procfs_sysdir_inode(void)
{
	static vfs_inode_t d; static int init = 0;
	if (!init) {
		memset(&d, 0, sizeof(d)); d.ino = 0xa000; d.mode = S_IFDIR | 0555;
		d.nlink = 5; d.refcount = 1; d.iops = &procfs_sysdir_iops;
		d.fops = &procfs_dir_fops; d.cached = 1; init = 1;
	}
	return &d;
}

/* ── root iops ───────────────────────────────────────────────── */

static int procfs_self_readlink(vfs_inode_t *in, char *buf, size_t sz)
{
	(void)in;
	process_t *cur = task_current();
	uint64_t pid = cur ? cur->pid : 0;
	/* build "/proc/N" */
	char tmp[32]; int ti = 0;
	const char *prefix = "/proc/";
	while (prefix[ti]) { tmp[ti] = prefix[ti]; ti++; }
	if (pid == 0) { tmp[ti++] = '0'; }
	else {
		char rev[20]; int ri = 0;
		uint64_t v = pid;
		while (v) { rev[ri++] = '0' + (int)(v % 10); v /= 10; }
		for (int j = ri - 1; j >= 0; j--) tmp[ti++] = rev[j];
	}
	tmp[ti] = '\0';
	if (sz < (size_t)ti + 1) return -1;
	memcpy(buf, tmp, (size_t)ti + 1);
	return 0;
}

static vfs_iops_t procfs_self_iops = {
	.readlink = procfs_self_readlink,
};

static vfs_inode_t *procfs_self_inode(void)
{
	static vfs_inode_t self_inode;
	static int init = 0;
	if (!init) {
		memset(&self_inode, 0, sizeof(self_inode));
		self_inode.ino     = 0x7ffe;
		self_inode.mode    = S_IFLNK | 0777;
		self_inode.nlink   = 1;
		self_inode.refcount= 1;
		self_inode.iops    = &procfs_self_iops;
		self_inode.cached  = 1; /* static — must not be freed by vfs_inode_free */
		init = 1;
	}
	return &self_inode;
}

static vfs_inode_t *procfs_root_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;

	/* "self" → symlink that resolves to /proc/<current_pid> at traversal time */
	if (strcmp(name, "self") == 0)
		return procfs_self_inode();

	/* "net" subdirectory */
	if (strcmp(name, "net") == 0)
		return procfs_netdir_inode();

	/* "sys" subdirectory */
	if (strcmp(name, "sys") == 0)
		return procfs_sysdir_inode();

	/* static files */
	for (int i = 0; i < ROOT_FILES_N; i++)
		if (strcmp(root_files[i].name, name) == 0)
			return procfs_inode_file(-1, root_files[i].type);

	/* numeric PID dirs */
	int pid = 0; const char *p = name;
	while (*p >= '0' && *p <= '9') { pid = pid * 10 + (*p - '0'); p++; }
	if (*p == '\0' && p != name)
		return procfs_piddir_inode(pid);

	return NULL;
}

static int procfs_root_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	int n_proc = task_count();

	/* static files first */
	if (idx < ROOT_FILES_N) {
		out->ino      = 0x8000 + idx;
		out->mode     = S_IFREG | 0444;
		out->next_off = idx + 1;
		const char *n = root_files[idx].name;
		int nlen = 0; while (n[nlen]) nlen++;
		memcpy(out->name, n, nlen); out->name[nlen] = '\0';
		return 0;
	}
	idx -= ROOT_FILES_N;

	/* "self" entry */
	if (idx == 0) {
		out->ino      = 0x7ffe;
		out->mode     = S_IFLNK | 0777;
		out->next_off = ROOT_FILES_N + 1;
		out->name[0] = 's'; out->name[1] = 'e'; out->name[2] = 'l';
		out->name[3] = 'f'; out->name[4] = '\0';
		return 0;
	}
	idx--;

	/* "net" dir entry */
	if (idx == 0) {
		out->ino      = 0x9000;
		out->mode     = S_IFDIR | 0555;
		out->next_off = ROOT_FILES_N + 2;
		out->name[0] = 'n'; out->name[1] = 'e'; out->name[2] = 't';
		out->name[3] = '\0';
		return 0;
	}
	idx--;

	/* "sys" dir entry */
	if (idx == 0) {
		out->ino      = 0xa000;
		out->mode     = S_IFDIR | 0555;
		out->next_off = ROOT_FILES_N + 3;
		out->name[0] = 's'; out->name[1] = 'y'; out->name[2] = 's';
		out->name[3] = '\0';
		return 0;
	}
	idx--;

	/* per-process dirs: find the idx-th non-reaped process */
	int live = 0;
	for (int i = 0; i < n_proc; i++) {
		process_t *p = task_get(i);
		if (!p) continue;
		if (live == (int)idx) {
			out->ino      = 0x8000 + (uint64_t)p->pid * 100 + 50;
			out->mode     = S_IFDIR | 0555;
			out->next_off = ROOT_FILES_N + 3 + idx + 1;
			char tmp[16]; int ti = 0;
			uint32_t pid = p->pid;
			if (pid == 0) { tmp[ti++] = '0'; }
			else { char rev[10]; int ri = 0; while (pid) { rev[ri++] = '0' + (pid % 10); pid /= 10; } for (int j = ri-1; j >= 0; j--) tmp[ti++] = rev[j]; }
			memcpy(out->name, tmp, ti); out->name[ti] = '\0';
			return 0;
		}
		live++;
	}
	return -1;
}

static int procfs_root_stat(vfs_inode_t *in, vfs_stat_t *out)
{
	out->ino   = in->ino;
	out->mode  = in->mode;
	out->nlink = in->nlink;
	out->size  = 0;
	return 0;
}

static vfs_iops_t procfs_root_iops = {
	.lookup  = procfs_root_lookup,
	.readdir = procfs_root_readdir,
	.stat    = procfs_root_stat,
};

/* ── mount ───────────────────────────────────────────────── */

int procfs_mount(vfs_mount_t *mnt, void *data)
{
	(void)data;

	vfs_inode_t *root_inode = kmalloc(sizeof(*root_inode));
	if (!root_inode) return -1;
	memset(root_inode, 0, sizeof(*root_inode));
	root_inode->ino      = 1;
	root_inode->mode     = S_IFDIR | 0555;
	root_inode->nlink    = 2;
	root_inode->refcount = 1;
	root_inode->iops     = &procfs_root_iops;
	root_inode->fops     = &procfs_dir_fops;
	root_inode->no_cache = 1; /* PID dirs are dynamic — never cache children */

	vfs_dentry_t *root_dentry = vfs_dentry_create(NULL, "/", root_inode);
	if (!root_dentry) { kfree(root_inode); return -1; }

	mnt->root = root_dentry;
	return 0;
}

void procfs_register(void)
{
	extern void vfs_register_fs(const char *, int (*)(vfs_mount_t *, void *));
	vfs_register_fs("proc", procfs_mount);
}
