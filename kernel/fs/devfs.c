#include <kernel/vfs.h>
#include <kernel/hal.h>
#include <kernel/kprintf.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>
#include <kernel/sync.h>
#include <drivers/serial/uart.h>
#include <kernel/tty/fb_tty.h>
#include <kernel/proc.h>
#include <kernel/capabilities.h>

/*
 * devfs — virtual device filesystem at /dev.
 *
 * Devices:
 *   null      : reads return 0, writes discarded
 *   zero      : reads return zero bytes
 *   full      : writes return ENOSPC
 *   urandom   : reads return pseudo-random bytes
 *   random    : alias for urandom
 *   tty       : reads/writes to UART + framebuffer
 *   console   : alias for tty
 *   stdin     : alias for tty (read-only view)
 *   stdout    : alias for tty (write-only view)
 *   stderr    : alias for tty (write-only view)
 */

/* ── /dev/null ───────────────────────────────────────────────── */

static ssize_t null_read(vfs_file_t *f, void *buf, size_t len)
{
	(void)f; (void)buf; (void)len;
	return 0;
}

static ssize_t null_write(vfs_file_t *f, const void *buf, size_t len)
{
	(void)f; (void)buf;
	return (ssize_t)len;
}

static vfs_fops_t null_fops = {
	.read  = null_read,
	.write = null_write,
};

/* ── /dev/zero ───────────────────────────────────────────────── */

static ssize_t zero_read(vfs_file_t *f, void *buf, size_t len)
{
	(void)f;
	if (!access_ok(buf, len)) { memset(buf, 0, len); return (ssize_t)len; }
	char z[256] = {0};
	size_t done = 0;
	while (done < len) {
		size_t n = len - done < sizeof(z) ? len - done : sizeof(z);
		if (copy_to_user((uint8_t *)buf + done, z, n) != 0) break;
		done += n;
	}
	return (ssize_t)done;
}

static vfs_fops_t zero_fops = {
	.read  = zero_read,
	.write = null_write,
};

/* ── /dev/full ───────────────────────────────────────────────── */

static ssize_t full_write(vfs_file_t *f, const void *buf, size_t len)
{
	(void)f; (void)buf; (void)len;
	return -28; /* ENOSPC */
}

static vfs_fops_t full_fops = {
	.read  = zero_read,
	.write = full_write,
};

/* ── /dev/urandom (and /dev/random) ──────────────────────────── */

static ssize_t urandom_read(vfs_file_t *f, void *buf, size_t len)
{
	(void)f;
	extern void csprng_bytes(void *, size_t);
	if (!access_ok(buf, len)) { csprng_bytes(buf, len); return (ssize_t)len; }
	uint8_t tmp[256];
	size_t done = 0;
	while (done < len) {
		size_t n = len - done < sizeof(tmp) ? len - done : sizeof(tmp);
		csprng_bytes(tmp, n);
		if (copy_to_user((uint8_t *)buf + done, tmp, n) != 0) break;
		done += n;
	}
	return (ssize_t)done;
}

static vfs_fops_t urandom_fops = {
	.read  = urandom_read,
	.write = null_write,
};

/* ── /dev/kmsg ───────────────────────────────────────────────── */
/* Reads drain the kernel log ring (one read() returns the whole buffer once,
 * then EOF until more is logged), so `dmesg` shows the boot log; writes append
 * to the same ring via kprintf (the printk(3)/logger path). */

/* dmesg_restrict (/proc/sys/kernel/dmesg_restrict): when set, only a privileged
 * task may read the kernel log ring — the boot log carries addresses and device
 * details useful to an attacker. Writes (logging) stay open. */
extern int dmesg_restrict;

static ssize_t kmsg_read(vfs_file_t *f, void *buf, size_t len)
{
	if (dmesg_restrict) {
		process_t *c = task_current();
		if (c && c->euid != 0 && !cap_has(c->cap_effective, CAP_SYS_ADMIN))
			return -1;   /* EPERM */
	}
	size_t total = kprintf_log_len();
	if ((size_t)f->pos >= total) return 0;
	char *tmp = kmalloc(total ? total : 1);
	if (!tmp) return -12;
	size_t got = kprintf_read_log(tmp, total);
	size_t avail = (got > (size_t)f->pos) ? got - (size_t)f->pos : 0;
	size_t n = avail < len ? avail : len;
	if (n && copy_fop_to_user(buf, tmp + f->pos, n) != 0) { kfree(tmp); return -14; }
	kfree(tmp);
	f->pos += (off_t)n;
	return (ssize_t)n;
}

static ssize_t kmsg_write(vfs_file_t *f, const void *buf, size_t len)
{
	(void)f;
	char kb[256];
	size_t done = 0;
	while (done < len) {
		size_t n = len - done < sizeof(kb) ? len - done : sizeof(kb);
		if (copy_fop_from_user(kb, (const uint8_t *)buf + done, n) != 0) break;
		for (size_t i = 0; i < n; i++) kprintf("%c", kb[i]);
		done += n;
	}
	return (ssize_t)done;
}

static vfs_fops_t kmsg_fops = {
	.read  = kmsg_read,
	.write = kmsg_write,
};

/* ── /dev/rtc0 ───────────────────────────────────────────────── */
/* RTC_RD_TIME (the hwclock/dmesg-era ioctl) returns the current wall-clock
 * time broken down into a struct rtc_time. The wall clock comes from the same
 * RTC-seeded realtime offset the syscalls use. */

#define RTC_RD_TIME 0x80247009u   /* _IOR('p', 0x09, struct rtc_time) */

struct rtc_time {
	int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
	int tm_wday, tm_yday, tm_isdst;
};

static void epoch_to_rtc(int64_t t, struct rtc_time *r)
{
	int64_t days = t / 86400, rem = t % 86400;
	if (rem < 0) { rem += 86400; days--; }
	r->tm_hour = (int)(rem / 3600);
	r->tm_min  = (int)((rem % 3600) / 60);
	r->tm_sec  = (int)(rem % 60);
	r->tm_wday = (int)((days + 4) % 7);   /* 1970-01-01 was a Thursday */
	if (r->tm_wday < 0) r->tm_wday += 7;
	int64_t y = 1970;
	for (;;) {
		int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
		int diy  = leap ? 366 : 365;
		if (days < diy) break;
		days -= diy; y++;
	}
	int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
	static const int mdays[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
	r->tm_yday = (int)days;
	int mon = 0;
	for (; mon < 12; mon++) {
		int dim = mdays[mon] + (mon == 1 && leap ? 1 : 0);
		if (days < dim) break;
		days -= dim;
	}
	r->tm_mon  = mon;
	r->tm_mday = (int)days + 1;
	r->tm_year = (int)(y - 1900);
	r->tm_isdst = 0;
}

static int rtc_ioctl(vfs_file_t *f, uint32_t cmd, uint64_t arg)
{
	(void)f;
	if (cmd != RTC_RD_TIME) return -25;
	int64_t s, n; ranix_walltime(&s, &n);
	struct rtc_time r;
	memset(&r, 0, sizeof(r));
	epoch_to_rtc(s, &r);
	return copy_to_user((void *)(uintptr_t)arg, &r, sizeof(r));
}

static vfs_fops_t rtc_fops = {
	.ioctl = rtc_ioctl,
};

typedef struct devfs_entry {
	const char         *name;
	mode_t              mode;
	vfs_fops_t         *fops;
	dev_t               rdev;
	struct devfs_entry *next;
	void               *priv;
} devfs_entry_t;

/* ── /dev/stdin, /dev/stdout, /dev/stderr fd-proxy devices ──── */

typedef struct {
	const char *link_target; /* for readlink() */
	int         fd_n;        /* process fd to proxy */
} devfs_fdproxy_t;

static ssize_t fdproxy_read(vfs_file_t *f, void *buf, size_t len)
{
	devfs_fdproxy_t *dp = f->inode->priv;
	process_t *cur = task_current();
	if (!cur || !cur->fds) return -9;
	vfs_file_t *target = fget(cur->fds, dp->fd_n);  /* pin across the blocking read */
	if (!target || !target->inode || !target->inode->fops ||
	    !target->inode->fops->read) { if (target) fput(target); return -9; }
	ssize_t r = target->inode->fops->read(target, buf, len);
	fput(target);
	return r;
}

static ssize_t fdproxy_write(vfs_file_t *f, const void *buf, size_t len)
{
	devfs_fdproxy_t *dp = f->inode->priv;
	process_t *cur = task_current();
	if (!cur || !cur->fds) return -9;
	vfs_file_t *target = fget(cur->fds, dp->fd_n);  /* pin across the blocking write */
	if (!target || !target->inode || !target->inode->fops ||
	    !target->inode->fops->write) { if (target) fput(target); return -9; }
	ssize_t r = target->inode->fops->write(target, buf, len);
	fput(target);
	return r;
}

static vfs_fops_t fdproxy_fops = {
	.read  = fdproxy_read,
	.write = fdproxy_write,
};

static devfs_fdproxy_t fdproxy_stdin  = { "/proc/self/fd/0", 0 };
static devfs_fdproxy_t fdproxy_stdout = { "/proc/self/fd/1", 1 };
static devfs_fdproxy_t fdproxy_stderr = { "/proc/self/fd/2", 2 };

typedef struct {
	const char *name;
	const char *target;
} devfs_symlink_t;

static devfs_symlink_t devfs_symlinks[] = {
	{ "fd",  "/proc/self/fd" },
	{ "shm", "/run/shm"      },
};
#define DEVFS_NSYMLINKS 2

static devfs_entry_t devfs_builtins[] = {
	{ "null",    S_IFCHR | 0666, &null_fops,    0x0103, &devfs_builtins[1] },
	{ "zero",    S_IFCHR | 0666, &zero_fops,    0x0105, &devfs_builtins[2] },
	{ "full",    S_IFCHR | 0666, &full_fops,    0x0107, &devfs_builtins[3] },
	{ "urandom", S_IFCHR | 0444, &urandom_fops, 0x0109, &devfs_builtins[4] },
	{ "random",  S_IFCHR | 0444, &urandom_fops, 0x0108, NULL               },
};
static devfs_entry_t *devfs_list = devfs_builtins;

void devfs_register_device_priv(const char *name, mode_t mode, dev_t rdev,
                                vfs_fops_t *fops, void *priv)
{
	for (devfs_entry_t *e = devfs_list; e; e = e->next)
		if (strcmp(e->name, name) == 0) return;
	devfs_entry_t *e = kmalloc(sizeof(*e));
	if (!e) return;
	e->name = name;
	e->mode = mode;
	e->rdev = rdev;
	e->fops = fops;
	e->priv = priv;
	e->next = devfs_list;
	devfs_list = e;
}

void devfs_register_device(const char *name, mode_t mode, dev_t rdev, vfs_fops_t *fops)
{
	devfs_register_device_priv(name, mode, rdev, fops, NULL);
}

/* map a device number (major:minor) to its driver fops + priv — lets a node
 * created by mknod(2) in a regular fs route to the same device as /dev/<name>. */
vfs_fops_t *devfs_fops_for_rdev(dev_t rdev, void **priv_out)
{
	for (devfs_entry_t *e = devfs_list; e; e = e->next)
		if (e->rdev == rdev && (S_ISCHR(e->mode) || S_ISBLK(e->mode))) {
			if (priv_out) *priv_out = e->priv;
			return e->fops;
		}
	return NULL;
}

/* ── iops ────────────────────────────────────────────────────── */

static uint64_t   dev_ino_next = 0xd000;
static spinlock_t dev_ino_lock = SPINLOCK_INIT;

static vfs_inode_t *devfs_inode_for(const devfs_entry_t *e)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	uint64_t f = spin_lock_irq(&dev_ino_lock);
	in->ino = dev_ino_next++;
	spin_unlock_irq(&dev_ino_lock, f);
	in->mode    = e->mode;
	in->nlink   = 1;
	in->rdev    = e->rdev;
	in->refcount= 1;
	in->fops    = e->fops;
	in->priv    = e->priv;
	in->priv_static = 1; /* e->priv is caller/driver-owned, not this inode's to free */
	return in;
}

/* Subdirectory support: devices named "subdir/devname" appear under /dev/subdir/ */

static int devfs_stat(vfs_inode_t *in, vfs_stat_t *out); /* forward */
static vfs_fops_t devfs_dir_fops;                        /* forward */
static vfs_iops_t devfs_subdir_iops;                     /* forward */

static vfs_inode_t *devfs_subdir_make(const char *subdir_name)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = 0xde00;
	in->mode     = S_IFDIR | 0755;
	in->nlink    = 2;
	in->refcount = 1;
	in->iops     = &devfs_subdir_iops;
	in->fops     = &devfs_dir_fops;
	in->priv     = (void *)subdir_name;
	in->priv_static = 1; /* borrows the static subdir name string */
	return in;
}

static vfs_inode_t *devfs_subdir_lookup(vfs_inode_t *dir, const char *name)
{
	const char *ref = (const char *)dir->priv;
	int plen = 0; while (ref[plen] && ref[plen] != '/') plen++;
	for (devfs_entry_t *e = devfs_list; e; e = e->next) {
		if (strncmp(e->name, ref, (size_t)plen) == 0 && e->name[plen] == '/' &&
		    strcmp(e->name + plen + 1, name) == 0)
			return devfs_inode_for(e);
	}
	return NULL;
}

static int devfs_subdir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	const char *ref = (const char *)dir->priv;
	int plen = 0; while (ref[plen] && ref[plen] != '/') plen++;
	uint32_t count = 0;
	uint64_t ino = 0xd100;
	for (devfs_entry_t *e = devfs_list; e; e = e->next, ino++) {
		if (strncmp(e->name, ref, (size_t)plen) == 0 && e->name[plen] == '/') {
			if (count == idx) {
				const char *shortname = e->name + plen + 1;
				out->ino      = ino;
				out->mode     = e->mode;
				out->next_off = idx + 1;
				int nlen = 0; while (shortname[nlen]) nlen++;
				memcpy(out->name, shortname, nlen); out->name[nlen] = '\0';
				return 0;
			}
			count++;
		}
	}
	return -1;
}

static vfs_iops_t devfs_subdir_iops = {
	.lookup  = devfs_subdir_lookup,
	.readdir = devfs_subdir_readdir,
	.stat    = devfs_stat,
};

static vfs_iops_t devfs_symlink_iops; /* forward */

static vfs_inode_t *devfs_symlink_inode(const devfs_symlink_t *sl)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	uint64_t f = spin_lock_irq(&dev_ino_lock);
	in->ino = dev_ino_next++;
	spin_unlock_irq(&dev_ino_lock, f);
	in->mode     = S_IFLNK | 0777;
	in->nlink    = 1;
	in->refcount = 1;
	in->priv     = (void *)sl->target;
	in->priv_static = 1; /* borrows the static symlink-target string */
	in->size     = (uint64_t)strlen(sl->target);
	in->iops     = &devfs_symlink_iops;
	return in;
}

static int devfs_readlink(vfs_inode_t *inode, char *buf, size_t bufsize)
{
	const char *tgt = (const char *)inode->priv;
	if (!tgt) return -1;
	size_t len = strlen(tgt);
	if (len >= bufsize) len = bufsize - 1;
	memcpy(buf, tgt, len);
	buf[len] = '\0';
	return 0;
}

static vfs_iops_t devfs_symlink_iops = {
	.readlink = devfs_readlink,
	.stat     = devfs_stat,
};

static int fdproxy_readlink(vfs_inode_t *inode, char *buf, size_t bufsize)
{
	devfs_fdproxy_t *dp = inode->priv;
	if (!dp) return -1;
	size_t len = strlen(dp->link_target);
	if (len >= bufsize) len = bufsize - 1;
	memcpy(buf, dp->link_target, len);
	buf[len] = '\0';
	return 0;
}

static vfs_iops_t devfs_fdproxy_iops = {
	.readlink = fdproxy_readlink,
	.stat     = devfs_stat,
};

static vfs_inode_t *devfs_fdproxy_inode(devfs_fdproxy_t *dp)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	uint64_t f = spin_lock_irq(&dev_ino_lock);
	in->ino = dev_ino_next++;
	spin_unlock_irq(&dev_ino_lock, f);
	in->mode     = S_IFCHR | 0666;
	in->nlink    = 1;
	in->refcount = 1;
	in->priv     = dp;
	in->priv_static = 1; /* borrows the static fdproxy descriptor */
	in->fops     = &fdproxy_fops;
	in->iops     = &devfs_fdproxy_iops;
	return in;
}

static struct { const char *name; devfs_fdproxy_t *dp; } devfs_fdproxies[] = {
	{ "stdin",  &fdproxy_stdin  },
	{ "stdout", &fdproxy_stdout },
	{ "stderr", &fdproxy_stderr },
};
#define DEVFS_NFDPROXIES 3

static vfs_inode_t *devfs_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	for (int i = 0; i < DEVFS_NFDPROXIES; i++)
		if (strcmp(devfs_fdproxies[i].name, name) == 0)
			return devfs_fdproxy_inode(devfs_fdproxies[i].dp);
	for (int i = 0; i < DEVFS_NSYMLINKS; i++)
		if (strcmp(devfs_symlinks[i].name, name) == 0)
			return devfs_symlink_inode(&devfs_symlinks[i]);
	int nlen = 0; while (name[nlen]) nlen++;
	for (devfs_entry_t *e = devfs_list; e; e = e->next) {
		if (strncmp(e->name, name, (size_t)nlen) == 0 && e->name[nlen] == '/')
			return devfs_subdir_make(e->name);
	}
	for (devfs_entry_t *e = devfs_list; e; e = e->next)
		if (strcmp(e->name, name) == 0)
			return devfs_inode_for(e);
	return NULL;
}

static int devfs_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	uint32_t count = 0;

	/* emit fd-proxy devices (stdin/stdout/stderr) */
	for (int i = 0; i < DEVFS_NFDPROXIES; i++, count++) {
		if (count == idx) {
			out->ino      = 0xd700 + (uint64_t)i;
			out->mode     = S_IFCHR | 0666;
			out->next_off = idx + 1;
			int nlen = 0; while (devfs_fdproxies[i].name[nlen]) nlen++;
			memcpy(out->name, devfs_fdproxies[i].name, (size_t)nlen);
			out->name[nlen] = '\0';
			return 0;
		}
	}

	/* emit symlinks (fd → /proc/self/fd) */
	for (int i = 0; i < DEVFS_NSYMLINKS; i++, count++) {
		if (count == idx) {
			out->ino      = 0xd800 + (uint64_t)i;
			out->mode     = S_IFLNK | 0777;
			out->next_off = idx + 1;
			int nlen = 0; while (devfs_symlinks[i].name[nlen]) nlen++;
			memcpy(out->name, devfs_symlinks[i].name, (size_t)nlen);
			out->name[nlen] = '\0';
			return 0;
		}
	}

	uint64_t ino = 0xd000;
	for (devfs_entry_t *e = devfs_list; e; e = e->next, ino++) {
		int slash = -1;
		for (int j = 0; e->name[j]; j++) if (e->name[j] == '/') { slash = j; break; }
		if (slash >= 0) {
			/* Only emit subdir entry for the first device under that prefix */
			int is_first = 1;
			for (devfs_entry_t *k = devfs_list; k != e; k = k->next) {
				if (strncmp(k->name, e->name, (size_t)slash) == 0 &&
				    k->name[slash] == '/') { is_first = 0; break; }
			}
			if (!is_first) continue;
			if (count == idx) {
				out->ino      = 0xde00;
				out->mode     = S_IFDIR | 0755;
				out->next_off = idx + 1;
				memcpy(out->name, e->name, (size_t)slash); out->name[slash] = '\0';
				return 0;
			}
		} else {
			if (count == idx) {
				out->ino      = ino;
				out->mode     = e->mode;
				out->next_off = idx + 1;
				int nlen = 0; while (e->name[nlen]) nlen++;
				memcpy(out->name, e->name, (size_t)nlen); out->name[nlen] = '\0';
				return 0;
			}
		}
		count++;
	}
	return -1;
}

static int devfs_stat(vfs_inode_t *in, vfs_stat_t *out)
{
	out->ino   = in->ino;
	out->mode  = in->mode;
	out->nlink = 1;
	out->rdev  = in->rdev;
	out->size  = 0;
	return 0;
}

static vfs_fops_t devfs_dir_fops = { 0 };

static vfs_iops_t devfs_root_iops = {
	.lookup  = devfs_lookup,
	.readdir = devfs_readdir,
	.stat    = devfs_stat,
};

/* ── mount ────────────────────────────────────────────────────── */

int devfs_mount(vfs_mount_t *mnt, void *data)
{
	(void)data;

	vfs_inode_t *root_inode = kmalloc(sizeof(*root_inode));
	if (!root_inode) return -1;
	memset(root_inode, 0, sizeof(*root_inode));
	root_inode->ino     = 1;
	root_inode->mode    = S_IFDIR | 0755;
	root_inode->nlink   = 2;
	root_inode->refcount= 1;
	root_inode->iops    = &devfs_root_iops;
	root_inode->fops    = &devfs_dir_fops;

	vfs_dentry_t *root_dentry = vfs_dentry_create(NULL, "/", root_inode);
	if (!root_dentry) { kfree(root_inode); return -1; }

	mnt->root = root_dentry;
	return 0;
}

void devfs_register(void)
{
	extern void vfs_register_fs(const char *, int (*)(vfs_mount_t *, void *));
	vfs_register_fs("devfs", devfs_mount);
	vfs_register_fs("devtmpfs", devfs_mount);   /* keep the name for /proc/mounts + statfs */
	devfs_register_device("kmsg", S_IFCHR | 0644, 0x010b, &kmsg_fops);     /* 1:11 */
	devfs_register_device("rtc0", S_IFCHR | 0644, 0xfe00, &rtc_fops);      /* 254:0 */
}
