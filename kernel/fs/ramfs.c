#include <kernel/fs/ramfs.h>
#include <kernel/vfs.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>

/*
 * ramfs — volatile in-memory filesystem.
 *
 * File data:  priv = ramfs_file_t*  (data buffer + size)
 * Dir data:   priv = ramfs_dir_t*   (linked list of entries)
 *
 * Inode numbers are assigned sequentially.
 */

typedef struct {
	uint8_t *data;
	uint64_t size;
	uint64_t capacity;
} ramfs_file_t;

typedef struct ramfs_entry {
	char                name[256];
	vfs_inode_t        *inode;
	struct ramfs_entry *next;
} ramfs_entry_t;

typedef struct {
	ramfs_entry_t *entries;
	uint32_t       count;
} ramfs_dir_t;

static uint64_t next_ino = 1;
static vfs_iops_t ramfs_iops;   /* forward declaration */

static vfs_inode_t *ramfs_inode_alloc(mode_t mode)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = next_ino++;
	in->mode     = mode;
	in->nlink    = 1;
	in->refcount = 1;
	return in;
}

/* ── file ops ────────────────────────────────────────────── */

static ssize_t ramfs_read(vfs_file_t *f, void *buf, size_t len)
{
	ramfs_file_t *rf = f->inode->priv;
	if (!rf || f->pos >= (off_t)rf->size) return 0;

	size_t avail = rf->size - (uint64_t)f->pos;
	if (len > avail) len = avail;
	memcpy(buf, rf->data + f->pos, len);
	f->pos += len;
	return (ssize_t)len;
}

static ssize_t ramfs_write(vfs_file_t *f, const void *buf, size_t len)
{
	ramfs_file_t *rf = f->inode->priv;
	if (!rf) return -1;

	uint64_t end = (uint64_t)f->pos + len;

	if (end > rf->capacity) {
		/* grow: round up to next page boundary */
		uint64_t newcap = (end + 4095) & ~4095ULL;
		int new_order = 0;
		while (((uint64_t)4096 << new_order) < newcap) new_order++;
		uint8_t *newbuf = buddy_alloc(new_order);
		if (!newbuf) return -1;
		memset(newbuf, 0, (size_t)((uint64_t)4096 << new_order));
		if (rf->data) {
			memcpy(newbuf, rf->data, rf->size);
			int old_order = 0;
			while (((uint64_t)4096 << old_order) < rf->capacity) old_order++;
			buddy_free((uint64_t)rf->data, old_order);
		}
		rf->data     = newbuf;
		rf->capacity = (uint64_t)4096 << new_order;
	}

	memcpy(rf->data + f->pos, buf, len);
	f->pos += len;
	if ((uint64_t)f->pos > rf->size) {
		rf->size         = (uint64_t)f->pos;
		f->inode->size   = rf->size;
	}
	return (ssize_t)len;
}

static int ramfs_file_open(vfs_inode_t *inode, vfs_file_t *file, int flags)
{
	if ((flags & O_TRUNC) && inode->priv) {
		ramfs_file_t *rf = inode->priv;
		rf->size       = 0;
		inode->size    = 0;
	}
	if (flags & O_APPEND)
		file->pos = (off_t)inode->size;
	return 0;
}

static vfs_fops_t ramfs_fops = {
	.open  = ramfs_file_open,
	.read  = ramfs_read,
	.write = ramfs_write,
};

static vfs_fops_t ramfs_dir_fops = { 0 };

/* ── inode ops ───────────────────────────────────────────── */

static vfs_inode_t *ramfs_lookup(vfs_inode_t *dir, const char *name)
{
	ramfs_dir_t *rd = dir->priv;
	if (!rd) return NULL;
	for (ramfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0)
			return e->inode;
	return NULL;
}

static int ramfs_create(vfs_inode_t *dir, const char *name, mode_t mode)
{
	ramfs_dir_t *rd = dir->priv;
	if (!rd) return -1;

	/* check for duplicate */
	for (ramfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return -1;

	vfs_inode_t *in = ramfs_inode_alloc(mode | S_IFREG);
	if (!in) return -1;

	ramfs_file_t *rf = kmalloc(sizeof(*rf));
	if (!rf) { kfree(in); return -1; }
	memset(rf, 0, sizeof(*rf));
	in->priv = rf;
	in->fops = &ramfs_fops;

	ramfs_entry_t *ent = kmalloc(sizeof(*ent));
	if (!ent) { kfree(rf); kfree(in); return -1; }
	int nlen = 0;
	while (name[nlen] && nlen < 255) nlen++;
	memcpy(ent->name, name, nlen); ent->name[nlen] = '\0';
	ent->inode    = in;
	ent->next     = rd->entries;
	rd->entries   = ent;
	rd->count++;
	dir->nlink++;
	return 0;
}

static int ramfs_mkdir(vfs_inode_t *dir, const char *name, mode_t mode)
{
	ramfs_dir_t *rd = dir->priv;
	if (!rd) return -1;

	for (ramfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return -1;

	vfs_inode_t *in = ramfs_inode_alloc(mode | S_IFDIR);
	if (!in) return -1;

	ramfs_dir_t *nrd = kmalloc(sizeof(*nrd));
	if (!nrd) { kfree(in); return -1; }
	memset(nrd, 0, sizeof(*nrd));
	in->priv = nrd;
	in->iops = &ramfs_iops;
	in->fops = &ramfs_dir_fops;

	ramfs_entry_t *ent = kmalloc(sizeof(*ent));
	if (!ent) { kfree(nrd); kfree(in); return -1; }
	int nlen = 0;
	while (name[nlen] && nlen < 255) nlen++;
	memcpy(ent->name, name, nlen); ent->name[nlen] = '\0';
	ent->inode    = in;
	ent->next     = rd->entries;
	rd->entries   = ent;
	rd->count++;
	dir->nlink++;
	return 0;
}

static int ramfs_unlink(vfs_inode_t *dir, const char *name)
{
	ramfs_dir_t   *rd   = dir->priv;
	if (!rd) return -1;
	ramfs_entry_t **pp = &rd->entries;
	while (*pp) {
		if (strcmp((*pp)->name, name) == 0) {
			ramfs_entry_t *e = *pp;
			*pp = e->next;
			rd->count--;
			dir->nlink--;
			kfree(e);
			return 0;
		}
		pp = &(*pp)->next;
	}
	return -1;
}

static int ramfs_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	ramfs_dir_t *rd = dir->priv;
	if (!rd) return -1;
	ramfs_entry_t *e = rd->entries;
	for (uint32_t i = 0; i < idx && e; i++) e = e->next;
	if (!e) return -1;
	out->ino      = e->inode->ino;
	out->mode     = e->inode->mode;
	out->next_off = idx + 1;
	int nlen = 0;
	while (e->name[nlen] && nlen < 255) nlen++;
	memcpy(out->name, e->name, nlen); out->name[nlen] = '\0';
	return 0;
}

static int ramfs_rename(vfs_inode_t *old_dir, const char *old_name,
                        vfs_inode_t *new_dir, const char *new_name)
{
	if (old_dir != new_dir) return -18; /* EXDEV: cross-dir not supported */
	ramfs_dir_t *rd = old_dir->priv;
	if (!rd) return -2;
	/* remove new_name if it exists */
	ramfs_unlink(new_dir, new_name);
	ramfs_entry_t *e = rd->entries;
	while (e) {
		if (strcmp(e->name, old_name) == 0) {
			int nlen = 0;
			while (new_name[nlen] && nlen < 254) nlen++;
			memcpy(e->name, new_name, nlen);
			e->name[nlen] = '\0';
			return 0;
		}
		e = e->next;
	}
	return -2;
}

static int ramfs_stat(vfs_inode_t *inode, vfs_stat_t *out)
{
	out->ino   = inode->ino;
	out->mode  = inode->mode;
	out->nlink = inode->nlink ? inode->nlink : 1;
	out->uid   = inode->uid;
	out->gid   = inode->gid;
	out->size  = inode->size;
	return 0;
}

static vfs_iops_t ramfs_iops = {
	.lookup  = ramfs_lookup,
	.create  = ramfs_create,
	.mkdir   = ramfs_mkdir,
	.unlink  = ramfs_unlink,
	.readdir = ramfs_readdir,
	.rename  = ramfs_rename,
	.stat    = ramfs_stat,
};

/* ── mount ───────────────────────────────────────────────── */

int ramfs_mount(vfs_mount_t *mnt, void *data)
{
	(void)data;

	vfs_inode_t *root_inode = ramfs_inode_alloc(S_IFDIR | 0755);
	if (!root_inode) return -1;

	ramfs_dir_t *rd = kmalloc(sizeof(*rd));
	if (!rd) { kfree(root_inode); return -1; }
	memset(rd, 0, sizeof(*rd));
	root_inode->priv  = rd;
	root_inode->iops  = &ramfs_iops;
	root_inode->fops  = &ramfs_dir_fops;

	vfs_dentry_t *root_dentry = vfs_dentry_create(NULL, "/", root_inode);
	if (!root_dentry) return -1;

	mnt->root = root_dentry;
	return 0;
}

void ramfs_register(void)
{
	extern void vfs_register_fs(const char *, int (*)(vfs_mount_t *, void *));
	vfs_register_fs("ramfs", ramfs_mount);
}
