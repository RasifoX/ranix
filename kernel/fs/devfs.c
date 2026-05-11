#include <kernel/vfs.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>
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

static uint64_t rand_state = 0xdeadbeefcafebabe;

static ssize_t urandom_read(vfs_file_t *f, void *buf, size_t len)
{
	(void)f;
	extern volatile uint64_t kernel_ticks;
	rand_state ^= kernel_ticks;
	uint8_t *b = buf;
	for (size_t i = 0; i < len; i++) {
		rand_state = rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
		b[i] = (uint8_t)(rand_state >> 33);
	}
	return (ssize_t)len;
}

static vfs_fops_t urandom_fops = {
	.read  = urandom_read,
	.write = null_write,
};

typedef struct {
	const char  *name;
	mode_t       mode;
	vfs_fops_t  *fops;
	dev_t        rdev;
} devfs_entry_t;

#define MAX_DEVFS_ENTRIES 32
static devfs_entry_t devfs_table[MAX_DEVFS_ENTRIES] = {
	{ "null",    S_IFCHR | 0666, &null_fops,    0x0101 },
	{ "zero",    S_IFCHR | 0666, &zero_fops,    0x0105 },
	{ "full",    S_IFCHR | 0666, &full_fops,    0x0107 },
	{ "urandom", S_IFCHR | 0444, &urandom_fops, 0x0109 },
	{ "random",  S_IFCHR | 0444, &urandom_fops, 0x0108 },
};
static int devfs_n = 5;

void devfs_register_device(const char *name, mode_t mode, dev_t rdev, vfs_fops_t *fops)
{
	if (devfs_n >= MAX_DEVFS_ENTRIES) return;
	devfs_table[devfs_n].name = name;
	devfs_table[devfs_n].mode = mode;
	devfs_table[devfs_n].rdev = rdev;
	devfs_table[devfs_n].fops = fops;
	devfs_n++;
}

/* ── iops ────────────────────────────────────────────────────── */

static uint64_t dev_ino_next = 0xd000;

static vfs_inode_t *devfs_inode_for(const devfs_entry_t *e)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino     = dev_ino_next++;
	in->mode    = e->mode;
	in->nlink   = 1;
	in->rdev    = e->rdev;
	in->refcount= 1;
	in->fops    = e->fops;
	return in;
}

static vfs_inode_t *devfs_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	for (int i = 0; i < devfs_n; i++)
		if (strcmp(devfs_table[i].name, name) == 0)
			return devfs_inode_for(&devfs_table[i]);
	return NULL;
}

static int devfs_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if ((int)idx >= devfs_n) return -1;
	const devfs_entry_t *e = &devfs_table[idx];
	out->ino      = 0xd000 + idx;
	out->mode     = e->mode;
	out->next_off = idx + 1;
	int nlen = 0; const char *n = e->name;
	while (n[nlen]) nlen++;
	memcpy(out->name, n, nlen); out->name[nlen] = '\0';
	return 0;
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
