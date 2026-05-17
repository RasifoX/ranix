#include <kernel/vfs.h>
#include <kernel/proc.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/mmu.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>

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
#if defined(__aarch64__)
	pb_str(&b, "processor\t: 0\n");
	pb_str(&b, "BogoMIPS\t: 100.00\n");
	pb_str(&b, "Features\t: fp asimd\n");
	pb_str(&b, "CPU implementer\t: 0x41\n");
	pb_str(&b, "CPU architecture: 8\n");
	pb_str(&b, "CPU variant\t: 0x0\n");
	pb_str(&b, "CPU part\t: 0xd08\n");
	pb_str(&b, "CPU revision\t: 3\n\n");
#else
	pb_str(&b, "processor\t: 0\n");
	pb_str(&b, "vendor_id\t: GenuineRanix\n");
	pb_str(&b, "cpu family\t: 6\n");
	pb_str(&b, "model\t\t: 0\n");
	pb_str(&b, "model name\t: Ranix Virtual CPU\n");
	pb_str(&b, "stepping\t: 0\n");
	pb_str(&b, "cpu MHz\t\t: 1000.000\n");
	pb_str(&b, "cache size\t: 4096 KB\n");
	pb_str(&b, "bogomips\t: 2000.00\n\n");
#endif
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
	pb_t b = {buf, sz};
	pb_str(&b, "cpu  ");
	pb_u64(&b, kernel_ticks); pb_str(&b, " 0 0 0 0 0 0 0\n");
	pb_str(&b, "cpu0 ");
	pb_u64(&b, kernel_ticks); pb_str(&b, " 0 0 0 0 0 0 0\n");
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
	pb_str(&b, "State:\tR (running)\n");
	pb_str(&b, "Tgid:\t"); pb_u64(&b, p->pid); pb_char(&b, '\n');
	pb_str(&b, "Pid:\t");  pb_u64(&b, p->pid); pb_char(&b, '\n');
	pb_str(&b, "PPid:\t"); pb_u64(&b, p->ppid); pb_char(&b, '\n');
	pb_str(&b, "Uid:\t0\t0\t0\t0\n");
	pb_str(&b, "Gid:\t0\t0\t0\t0\n");
	pb_str(&b, "VmRSS:\t"); pb_u64(&b, 64); pb_str(&b, " kB\n");
	pb_str(&b, "Threads:\t1\n");
	return (int)(sz - b.rem);
}

static int fill_proc_stat(char *buf, size_t sz, int pid)
{
	process_t *p = find_proc(pid);
	pb_t b = {buf, sz};
	if (!p) { pb_u64(&b, (uint64_t)pid); pb_str(&b, " (gone) Z 0\n"); return (int)(sz - b.rem); }
	pb_u64(&b, p->pid); pb_char(&b, ' ');
	pb_char(&b, '('); pb_str(&b, p->name); pb_char(&b, ')');
	/* fields 3-52 matching Linux /proc/PID/stat layout */
	pb_str(&b, " R ");
	pb_u64(&b, p->ppid);                   /* 4: ppid */
	pb_str(&b, " 0 0 0 0 0 0 0 0 0 0 ");  /* 5-13: pgrp..cmajflt */
	pb_u64(&b, p->ticks);                  /* 14: utime (CPU ticks) */
	pb_str(&b, " 0 0 0 0 0 0 1 0 0 0"     /* 15-24: stime..itrealvalue */
	           " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0"  /* 25-44 */
	           " 0 0 0 0 0 0 0 0\n");                       /* 45-52 */
	return (int)(sz - b.rem);
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
#define PROC_F_P_STATUS  10
#define PROC_F_P_STAT    11
#define PROC_F_P_CMDLINE 12
#define PROC_F_P_MAPS    13

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
};
#define ROOT_FILES_N 7

/* static file table for per-pid subdirs */
static const struct {
	const char *name;
	int         type;
} pid_files[] = {
	{ "status",  PROC_F_P_STATUS  },
	{ "stat",    PROC_F_P_STAT    },
	{ "cmdline", PROC_F_P_CMDLINE },
	{ "maps",    PROC_F_P_MAPS    },
};
#define PID_FILES_N 4

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
	case PROC_F_P_STATUS: n = fill_proc_status(tmp, PROCFS_BUF_SIZE, pr->pid); break;
	case PROC_F_P_STAT:   n = fill_proc_stat(tmp,   PROCFS_BUF_SIZE, pr->pid); break;
	case PROC_F_P_CMDLINE:n = fill_proc_cmdline(tmp, PROCFS_BUF_SIZE, pr->pid); break;
	case PROC_F_P_MAPS:   n = fill_proc_maps(tmp,    PROCFS_BUF_SIZE, pr->pid); break;
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

static vfs_fops_t procfs_fops = {
	.read = procfs_file_read,
};

static vfs_fops_t procfs_dir_fops = { 0 };

/* ── inode allocation ────────────────────────────────────────── */

static uint64_t proc_ino_next = 0x8000;

static vfs_inode_t *procfs_inode_file(int pid, int type)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino     = proc_ino_next++;
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
	in->ino      = proc_ino_next++;
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
	in->ino      = proc_ino_next++;
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

static vfs_inode_t *procfs_piddir_lookup(vfs_inode_t *dir, const char *name)
{
	procfs_piddir_priv_t *pp = dir->priv;
	if (!pp) return NULL;
	int pid = pp->pid;
	if (strcmp(name, "fd") == 0)
		return procfs_fddir_inode(pid);
	for (int i = 0; i < PID_FILES_N; i++) {
		if (strcmp(pid_files[i].name, name) == 0)
			return procfs_inode_file(pid, pid_files[i].type);
	}
	return NULL;
}

static int procfs_piddir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	if (idx >= (uint32_t)(PID_FILES_N + 1)) return -1;
	procfs_piddir_priv_t *pp = dir->priv;
	int pid = pp ? pp->pid : 0;
	if (idx < PID_FILES_N) {
		out->ino      = 0x8000 + (uint64_t)pid * 100 + idx;
		out->mode     = S_IFREG | 0444;
		out->next_off = idx + 1;
		const char *n = pid_files[idx].name;
		int nlen = 0; while (n[nlen]) nlen++;
		memcpy(out->name, n, nlen); out->name[nlen] = '\0';
	} else {
		out->ino      = 0x8000 + (uint64_t)pid * 100 + 99;
		out->mode     = S_IFDIR | 0555;
		out->next_off = idx + 1;
		memcpy(out->name, "fd", 3);
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

	/* per-process dirs: find the idx-th non-reaped process */
	int live = 0;
	for (int i = 0; i < n_proc; i++) {
		process_t *p = task_get(i);
		if (!p) continue;
		if (live == (int)idx) {
			out->ino      = 0x8000 + (uint64_t)p->pid * 100 + 50;
			out->mode     = S_IFDIR | 0555;
			out->next_off = ROOT_FILES_N + 1 + idx + 1;
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
