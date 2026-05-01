#include <kernel/vfs.h>
#include <kernel/mm/buddy.h>
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

/* ── /dev/tty ────────────────────────────────────────────────── */

static ssize_t tty_read(vfs_file_t *f, void *buf, size_t len)
{
	(void)f;
	char *b = buf;
	size_t i;
	for (i = 0; i < len; i++) {
		while (!uart_has_data());
		char c = uart_getc();
		b[i] = c;
		if (c == '\n' || c == '\r') { i++; break; }
	}
	return (ssize_t)i;
}

static ssize_t tty_write(vfs_file_t *f, const void *buf, size_t len)
{
	(void)f;
	const char *s = buf;
	fb_tty_write(s, len);
	for (size_t i = 0; i < len; i++) {
		if (s[i] == '\n') uart_putc('\r');
		uart_putc(s[i]);
	}
	return (ssize_t)len;
}

static int tty_poll(vfs_file_t *f)
{
	(void)f;
	return uart_has_data() ? 1 : 0;
}

static int tty_ioctl(vfs_file_t *f, uint32_t cmd, uint64_t arg)
{
	(void)f; (void)arg;
#define TCGETS   0x5401
#define TCSETS   0x5402
#define TCSETSW  0x5403
#define TIOCGWINSZ 0x5413
#define TIOCGPGRP  0x540f
	switch (cmd) {
	case TCGETS: case TCSETS: case TCSETSW: return 0;
	case TIOCGWINSZ: {
		uint16_t *ws = (uint16_t *)(uintptr_t)arg;
		if (ws) { ws[0] = 24; ws[1] = 80; ws[2] = 0; ws[3] = 0; }
		return 0;
	}
	case TIOCGPGRP: {
		int *pg = (int *)(uintptr_t)arg;
		if (pg) *pg = 1;
		return 0;
	}
	default: return -25; /* ENOTTY */
	}
}

static vfs_fops_t tty_fops = {
	.read   = tty_read,
	.write  = tty_write,
	.poll   = tty_poll,
	.ioctl  = tty_ioctl,
};

/* ── device table ────────────────────────────────────────────── */

typedef struct {
	const char  *name;
	mode_t       mode;
	vfs_fops_t  *fops;
	dev_t        rdev;
} devfs_entry_t;

static const devfs_entry_t devfs_table[] = {
	{ "null",    S_IFCHR | 0666, &null_fops,    0x0101 },
	{ "zero",    S_IFCHR | 0666, &zero_fops,    0x0105 },
	{ "full",    S_IFCHR | 0666, &full_fops,    0x0107 },
	{ "urandom", S_IFCHR | 0444, &urandom_fops, 0x0109 },
	{ "random",  S_IFCHR | 0444, &urandom_fops, 0x0108 },
	{ "tty",     S_IFCHR | 0666, &tty_fops,     0x0500 },
	{ "console", S_IFCHR | 0600, &tty_fops,     0x0501 },
	{ "stdin",   S_IFCHR | 0444, &tty_fops,     0x0502 },
	{ "stdout",  S_IFCHR | 0222, &tty_fops,     0x0503 },
	{ "stderr",  S_IFCHR | 0222, &tty_fops,     0x0504 },
};
#define DEVFS_N 10

/* ── iops ────────────────────────────────────────────────────── */

static uint64_t dev_ino_next = 0xd000;

static vfs_inode_t *devfs_inode_for(const devfs_entry_t *e)
{
	vfs_inode_t *in = buddy_alloc(0);
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
	for (int i = 0; i < DEVFS_N; i++)
		if (strcmp(devfs_table[i].name, name) == 0)
			return devfs_inode_for(&devfs_table[i]);
	return NULL;
}

static int devfs_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx >= DEVFS_N) return -1;
	const devfs_entry_t *e = &devfs_table[idx];
	out->ino  = 0xd000 + idx;
	out->mode = e->mode;
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

static vfs_iops_t devfs_root_iops = {
	.lookup  = devfs_lookup,
	.readdir = devfs_readdir,
	.stat    = devfs_stat,
};

/* ── mount ────────────────────────────────────────────────────── */

int devfs_mount(vfs_mount_t *mnt, void *data)
{
	(void)data;

	vfs_inode_t *root_inode = buddy_alloc(0);
	if (!root_inode) return -1;
	memset(root_inode, 0, sizeof(*root_inode));
	root_inode->ino     = 1;
	root_inode->mode    = S_IFDIR | 0755;
	root_inode->nlink   = 2;
	root_inode->refcount= 1;
	root_inode->iops    = &devfs_root_iops;
	root_inode->fops    = &(vfs_fops_t){ 0 };

	vfs_dentry_t *root_dentry = vfs_dentry_create(NULL, "/", root_inode);
	if (!root_dentry) { buddy_free((uint64_t)root_inode, 0); return -1; }

	mnt->root = root_dentry;
	return 0;
}

void devfs_register(void)
{
	extern void vfs_register_fs(const char *, int (*)(vfs_mount_t *, void *));
	vfs_register_fs("devfs", devfs_mount);
}
