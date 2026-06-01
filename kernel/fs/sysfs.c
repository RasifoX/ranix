/* ── sysfs (/sys) ─────────────────────────────────────────────────────────────
 * A small read-only pseudo-filesystem mirroring the Linux /sys layout that
 * tooling looks for, built like procfs (dir inodes carry lookup/readdir iops,
 * file inodes carry a read fop). Currently exposes:
 *   /sys/kernel/{ostype,osrelease,hostname}
 *   /sys/class/net/<dev>/{address,mtu,operstate}   (one dir per registered netdev)
 * Directory entries are generated on demand from the live netdev list, so lo
 * and eth0 appear automatically.
 * ───────────────────────────────────────────────────────────────────────── */
#include <kernel/vfs.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/mmu.h>
#include <kernel/lib/string.h>
#include <kernel/net/net.h>

extern char ranix_hostname[];

enum { SF_OSTYPE = 1, SF_OSRELEASE, SF_HOSTNAME, SF_NET_ADDR, SF_NET_MTU, SF_NET_STATE };

typedef struct { int type; int dev; } sysfs_priv_t;

static uint64_t sys_ino_next = 0x9000;
static uint64_t sys_ino_alloc(void) { return sys_ino_next++; }

static vfs_iops_t sysfs_root_iops, sysfs_kernel_iops, sysfs_class_iops,
                  sysfs_net_iops, sysfs_netdev_iops;
static vfs_fops_t sysfs_file_fops;
static vfs_fops_t sysfs_dir_fops = { 0 };

static vfs_inode_t *sysfs_dir(vfs_iops_t *iops, int dev)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = sys_ino_alloc();
	in->mode     = S_IFDIR | 0555;
	in->nlink    = 2;
	in->refcount = 1;
	in->iops     = iops;
	in->fops     = &sysfs_dir_fops;
	in->no_cache = 1;   /* netdev list is dynamic */
	if (dev >= 0) {
		sysfs_priv_t *p = kmalloc(sizeof(*p));
		if (p) { p->type = 0; p->dev = dev; in->priv = p; }
	}
	return in;
}

static vfs_inode_t *sysfs_file(int type, int dev)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = sys_ino_alloc();
	in->mode     = S_IFREG | 0444;
	in->nlink    = 1;
	in->refcount = 1;
	in->fops     = &sysfs_file_fops;
	sysfs_priv_t *p = kmalloc(sizeof(*p));
	if (!p) { kfree(in); return NULL; }
	p->type = type; p->dev = dev;
	in->priv = p;
	return in;
}

static int put_str(char *o, const char *s)
{
	int i = 0;
	while (s[i]) { o[i] = s[i]; i++; }
	return i;
}

static ssize_t sysfs_file_read(vfs_file_t *f, void *buf, size_t len)
{
	sysfs_priv_t *pr = f->inode->priv;
	if (!pr) return -1;

	uint64_t phys = (uint64_t)buddy_alloc(0);
	if (!phys) return -1;
	char *tmp = (char *)PHYS_TO_VIRT(phys);
	int n = 0;

	switch (pr->type) {
	case SF_OSTYPE:    n = put_str(tmp, "Ranix\n");  break;
	case SF_OSRELEASE: n = put_str(tmp, "1.0.0\n");  break;
	case SF_HOSTNAME:  n = put_str(tmp, ranix_hostname); tmp[n++] = '\n'; break;
	case SF_NET_ADDR: {
		netdev_t *d = net_dev(pr->dev);
		if (d) {
			const char *h = "0123456789abcdef";
			for (int i = 0; i < 6; i++) {
				if (i) tmp[n++] = ':';
				tmp[n++] = h[d->mac[i] >> 4];
				tmp[n++] = h[d->mac[i] & 15];
			}
			tmp[n++] = '\n';
		}
		break;
	}
	case SF_NET_MTU:   n = put_str(tmp, "1500\n"); break;
	case SF_NET_STATE: n = put_str(tmp, "up\n");   break;
	default:           n = 0; break;
	}

	ssize_t ret = 0;
	if (f->pos < (off_t)n) {
		size_t avail = (size_t)(n - f->pos);
		if (len > avail) len = avail;
		memcpy(buf, tmp + (size_t)f->pos, len);
		f->pos += (off_t)len;
		ret = (ssize_t)len;
	}
	buddy_free(phys, 0);
	return ret;
}

static vfs_fops_t sysfs_file_fops = { .read = sysfs_file_read };

/* ── /sys (root): kernel/, class/ ────────────────────────────── */
static vfs_inode_t *sysfs_root_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	if (strcmp(name, "kernel") == 0) return sysfs_dir(&sysfs_kernel_iops, -1);
	if (strcmp(name, "class")  == 0) return sysfs_dir(&sysfs_class_iops, -1);
	return NULL;
}
static int sysfs_root_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	const char *names[] = { "kernel", "class" };
	if (idx >= 2) return -1;
	out->ino = sys_ino_alloc(); out->mode = S_IFDIR | 0555; out->next_off = idx + 1;
	put_str(out->name, names[idx]); out->name[strlen(names[idx])] = '\0';
	return 0;
}

/* ── /sys/kernel ─────────────────────────────────────────────── */
static const struct { const char *name; int type; } kernel_files[] = {
	{ "ostype", SF_OSTYPE }, { "osrelease", SF_OSRELEASE }, { "hostname", SF_HOSTNAME },
};
static vfs_inode_t *sysfs_kernel_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	for (unsigned i = 0; i < 3; i++)
		if (strcmp(kernel_files[i].name, name) == 0)
			return sysfs_file(kernel_files[i].type, -1);
	return NULL;
}
static int sysfs_kernel_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx >= 3) return -1;
	out->ino = sys_ino_alloc(); out->mode = S_IFREG | 0444; out->next_off = idx + 1;
	put_str(out->name, kernel_files[idx].name);
	out->name[strlen(kernel_files[idx].name)] = '\0';
	return 0;
}

/* ── /sys/class: net/ ────────────────────────────────────────── */
static vfs_inode_t *sysfs_class_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	if (strcmp(name, "net") == 0) return sysfs_dir(&sysfs_net_iops, -1);
	return NULL;
}
static int sysfs_class_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx != 0) return -1;
	out->ino = sys_ino_alloc(); out->mode = S_IFDIR | 0555; out->next_off = 1;
	put_str(out->name, "net"); out->name[3] = '\0';
	return 0;
}

/* ── /sys/class/net: one dir per netdev ──────────────────────── */
static vfs_inode_t *sysfs_net_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	for (int i = 0; ; i++) {
		netdev_t *d = net_dev(i);
		if (!d) break;
		if (strcmp(d->name, name) == 0) return sysfs_dir(&sysfs_netdev_iops, i);
	}
	return NULL;
}
static int sysfs_net_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	netdev_t *d = net_dev((int)idx);
	if (!d) return -1;
	out->ino = sys_ino_alloc(); out->mode = S_IFDIR | 0555; out->next_off = idx + 1;
	put_str(out->name, d->name); out->name[strlen(d->name)] = '\0';
	return 0;
}

/* ── /sys/class/net/<dev>: address, mtu, operstate ───────────── */
static const struct { const char *name; int type; } netdev_files[] = {
	{ "address", SF_NET_ADDR }, { "mtu", SF_NET_MTU }, { "operstate", SF_NET_STATE },
};
static vfs_inode_t *sysfs_netdev_lookup(vfs_inode_t *dir, const char *name)
{
	sysfs_priv_t *pr = dir->priv;
	int dev = pr ? pr->dev : 0;
	for (unsigned i = 0; i < 3; i++)
		if (strcmp(netdev_files[i].name, name) == 0)
			return sysfs_file(netdev_files[i].type, dev);
	return NULL;
}
static int sysfs_netdev_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx >= 3) return -1;
	out->ino = sys_ino_alloc(); out->mode = S_IFREG | 0444; out->next_off = idx + 1;
	put_str(out->name, netdev_files[idx].name);
	out->name[strlen(netdev_files[idx].name)] = '\0';
	return 0;
}

static vfs_iops_t sysfs_root_iops   = { .lookup = sysfs_root_lookup,   .readdir = sysfs_root_readdir   };
static vfs_iops_t sysfs_kernel_iops = { .lookup = sysfs_kernel_lookup, .readdir = sysfs_kernel_readdir };
static vfs_iops_t sysfs_class_iops  = { .lookup = sysfs_class_lookup,  .readdir = sysfs_class_readdir  };
static vfs_iops_t sysfs_net_iops    = { .lookup = sysfs_net_lookup,    .readdir = sysfs_net_readdir    };
static vfs_iops_t sysfs_netdev_iops = { .lookup = sysfs_netdev_lookup, .readdir = sysfs_netdev_readdir };

int sysfs_mount(vfs_mount_t *mnt, void *data)
{
	(void)data;
	vfs_inode_t *root = kmalloc(sizeof(*root));
	if (!root) return -1;
	memset(root, 0, sizeof(*root));
	root->ino      = 1;
	root->mode     = S_IFDIR | 0555;
	root->nlink    = 2;
	root->refcount = 1;
	root->iops     = &sysfs_root_iops;
	root->fops     = &sysfs_dir_fops;
	root->no_cache = 1;

	vfs_dentry_t *rd = vfs_dentry_create(NULL, "/", root);
	if (!rd) { kfree(root); return -1; }
	mnt->root = rd;
	return 0;
}

void sysfs_register(void)
{
	extern void vfs_register_fs(const char *, int (*)(vfs_mount_t *, void *));
	vfs_register_fs("sysfs", sysfs_mount);
}
