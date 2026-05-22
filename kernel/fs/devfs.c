#include <kernel/vfs.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>
#include <kernel/sync.h>
#include <drivers/serial/uart.h>
#include <kernel/tty/fb_tty.h>

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
	memset(buf, 0, len);
	return (ssize_t)len;
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

/* ── /dev/urandom ────────────────────────────────────────────── */

static uint64_t   rand_state = 0xdeadbeefcafebabe;
static spinlock_t rand_lock  = SPINLOCK_INIT;

static ssize_t urandom_read(vfs_file_t *f, void *buf, size_t len)
{
	(void)f;
	extern volatile uint64_t kernel_ticks;
	uint8_t *b = buf;
	spin_lock(&rand_lock);
	rand_state ^= kernel_ticks;
	for (size_t i = 0; i < len; i++) {
		rand_state = rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
		b[i] = (uint8_t)(rand_state >> 33);
	}
	spin_unlock(&rand_lock);
	return (ssize_t)len;
}

static vfs_fops_t urandom_fops = {
	.read  = urandom_read,
	.write = null_write,
};

typedef struct devfs_entry {
	const char         *name;
	mode_t              mode;
	vfs_fops_t         *fops;
	dev_t               rdev;
	struct devfs_entry *next;
} devfs_entry_t;

static devfs_entry_t devfs_builtins[] = {
	{ "null",    S_IFCHR | 0666, &null_fops,    0x0101, &devfs_builtins[1] },
	{ "zero",    S_IFCHR | 0666, &zero_fops,    0x0105, &devfs_builtins[2] },
	{ "full",    S_IFCHR | 0666, &full_fops,    0x0107, &devfs_builtins[3] },
	{ "urandom", S_IFCHR | 0444, &urandom_fops, 0x0109, &devfs_builtins[4] },
	{ "random",  S_IFCHR | 0444, &urandom_fops, 0x0108, NULL               },
};
static devfs_entry_t *devfs_list = devfs_builtins;

void devfs_register_device(const char *name, mode_t mode, dev_t rdev, vfs_fops_t *fops)
{
	devfs_entry_t *e = kmalloc(sizeof(*e));
	if (!e) return;
	e->name = name;
	e->mode = mode;
	e->rdev = rdev;
	e->fops = fops;
	e->next = devfs_list;
	devfs_list = e;
}

/* ── iops ────────────────────────────────────────────────────── */

static uint64_t   dev_ino_next = 0xd000;
static spinlock_t dev_ino_lock = SPINLOCK_INIT;

static vfs_inode_t *devfs_inode_for(const devfs_entry_t *e)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	spin_lock(&dev_ino_lock);
	in->ino = dev_ino_next++;
	spin_unlock(&dev_ino_lock);
	in->mode    = e->mode;
	in->nlink   = 1;
	in->rdev    = e->rdev;
	in->refcount= 1;
	in->fops    = e->fops;
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

static vfs_inode_t *devfs_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
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
}
