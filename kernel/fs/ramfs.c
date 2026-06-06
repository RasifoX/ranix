#include <kernel/fs/ramfs.h>
#include <kernel/vfs.h>
#include <kernel/hal.h>
#include <kernel/proc.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/sync.h>
#include <asm/mmu.h>

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

static uint64_t    next_ino   = 1;
static spinlock_t  ino_lock   = SPINLOCK_INIT;
static vfs_iops_t  ramfs_iops;   /* forward declaration */

static vfs_inode_t *ramfs_inode_alloc(mode_t mode)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	uint64_t f = spin_lock_irq(&ino_lock);
	in->ino = next_ino++;
	spin_unlock_irq(&ino_lock, f);
	in->mode     = mode;
	in->nlink    = 1;
	in->refcount = 1;
	process_t *p = task_current();
	in->uid = p ? p->euid : 0;
	in->gid = p ? p->egid : 0;
	int64_t s, ns; ranix_walltime(&s, &ns);
	in->atime = in->mtime = in->ctime = s;
	in->atime_nsec = in->mtime_nsec = in->ctime_nsec = (uint32_t)ns;
	return in;
}

/* ── file ops ────────────────────────────────────────────── */

static ssize_t ramfs_read(vfs_file_t *f, void *buf, size_t len)
{
	ramfs_file_t *rf = f->inode->priv;
	if (!rf || f->pos >= (off_t)rf->size) return 0;

	size_t avail = rf->size - (uint64_t)f->pos;
	if (len > avail) len = avail;
	if (copy_fop_to_user(buf, rf->data + f->pos, len) != 0) return -14;
	f->pos += len;
	return (ssize_t)len;
}

static ssize_t ramfs_write(vfs_file_t *f, const void *buf, size_t len)
{
	ramfs_file_t *rf = f->inode->priv;
	if (!rf) return -1;

	uint64_t end = (uint64_t)f->pos + len;

	if (end > rf->capacity) {
		if (f->inode->mmap_pinned) return -12;   /* a live MAP_SHARED mapping pins the buffer in place */
		/* grow: round up to next page boundary */
		uint64_t newcap = (end + 4095) & ~4095ULL;
		int new_order = 0;
		while (((uint64_t)4096 << new_order) < newcap) new_order++;
		uint8_t *newbuf_phys = buddy_alloc(new_order);
		if (!newbuf_phys) return -1;
		uint8_t *newbuf = (uint8_t *)PHYS_TO_VIRT(newbuf_phys);
		memset(newbuf, 0, (size_t)((uint64_t)4096 << new_order));
		if (rf->data) {
			memcpy(newbuf, rf->data, rf->size);
			int old_order = 0;
			while (((uint64_t)4096 << old_order) < rf->capacity) old_order++;
			buddy_free(VIRT_TO_PHYS(rf->data), old_order);
		}
		rf->data     = newbuf;
		rf->capacity = (uint64_t)4096 << new_order;
	}

	if (copy_fop_from_user(rf->data + f->pos, buf, len) != 0) return -14;
	f->pos += len;
	if ((uint64_t)f->pos > rf->size) {
		rf->size         = (uint64_t)f->pos;
		f->inode->size   = rf->size;
	}
	int64_t s, ns; ranix_walltime(&s, &ns);
	f->inode->mtime = f->inode->ctime = s;
	f->inode->mtime_nsec = f->inode->ctime_nsec = (uint32_t)ns;
	return (ssize_t)len;
}

/* Hand the generic mmap path the file's own backing page covering `off`, taking
 * an extra reference so the mapping's unmap leaves the buffer's reference intact.
 * The data buffer is one contiguous buddy block, so the physical page is just
 * VIRT_TO_PHYS(rf->data + off). Offsets past the buffer return 0 (EINVAL); the
 * mapper is expected to size the file (ftruncate/write) before mapping. */
static uint64_t ramfs_mmap(vfs_file_t *f, off_t off)
{
	ramfs_file_t *rf = f->inode->priv;
	if (!rf || off < 0 || !rf->data) return 0;
	if ((uint64_t)off + PAGE_SIZE > rf->capacity) return 0;
	uint64_t pa = VIRT_TO_PHYS(rf->data + off);
	pmm_hold((void *)pa);
	return pa;
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
	.mmap  = ramfs_mmap,
	.mmap_cacheable = 1,
};

static vfs_fops_t ramfs_dir_fops = { 0 };

/* ── inode ops ───────────────────────────────────────────── */

static vfs_inode_t *ramfs_lookup(vfs_inode_t *dir, const char *name)
{
	if (!S_ISDIR(dir->mode)) return NULL;   /* priv is a file struct, not an entry list */
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
	in->iops = &ramfs_iops;

	ramfs_entry_t *ent = kmalloc(sizeof(*ent));
	if (!ent) { kfree(rf); kfree(in); return -1; }
	int nlen = 0;
	while (name[nlen] && nlen < 255) nlen++;
	memcpy(ent->name, name, nlen); ent->name[nlen] = '\0';
	ent->inode    = in;
	ent->next     = rd->entries;
	rd->entries   = ent;
	rd->count++;
	return 0;
}

static int ramfs_mknod(vfs_inode_t *dir, const char *name, mode_t mode, dev_t dev)
{
	ramfs_dir_t *rd = dir->priv;
	if (!rd) return -1;
	int type = mode & 0xF000;
	if (type != S_IFIFO && type != S_IFCHR && type != S_IFBLK) return -1; /* unsupported node type */
	for (ramfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return -17; /* EEXIST */
	vfs_inode_t *in = ramfs_inode_alloc(mode);
	if (!in) return -1;
	in->iops = &ramfs_iops;
	if (type == S_IFIFO) {
		fifo_init_inode(in);                     /* fops + lazy pipe */
	} else {                                     /* char/block: route by major:minor */
		extern vfs_fops_t *devfs_fops_for_rdev(dev_t, void **);
		void *dpriv = NULL;
		in->rdev = dev;
		in->fops = devfs_fops_for_rdev(dev, &dpriv);  /* NULL → open fails (no driver) */
		in->priv = dpriv;
	}
	ramfs_entry_t *ent = kmalloc(sizeof(*ent));
	if (!ent) { kfree(in); return -1; }
	int nlen = 0; while (name[nlen] && nlen < 255) nlen++;
	memcpy(ent->name, name, nlen); ent->name[nlen] = '\0';
	ent->inode = in; ent->next = rd->entries; rd->entries = ent; rd->count++;
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

/* free a file's data buffer + inode after its last name and fd are gone */
static void ramfs_evict(vfs_inode_t *in)
{
	if (S_ISREG(in->mode) && in->priv) {
		ramfs_file_t *rf = in->priv;
		if (rf->data) {
			int order = 0;
			while (((uint64_t)4096 << order) < rf->capacity) order++;
			buddy_free(VIRT_TO_PHYS(rf->data), order);
		}
		kfree(rf);
	} else if (S_ISLNK(in->mode) && in->priv) {
		kfree(in->priv);
	}
	in->priv = NULL;
	vfs_inode_free_xattrs(in);
	kfree(in);
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
			vfs_inode_t *in = e->inode;
			/* Removing a subdirectory drops the parent's '..' backlink; removing
			 * a file drops one of that file's hard links. */
			if (S_ISDIR(in->mode)) dir->nlink--;
			else if (in->nlink > 0) in->nlink--;
			/* last link gone: free now, or defer past still-open fds */
			if (!S_ISDIR(in->mode) && in->nlink == 0) {
				if (in->open_refs > 0) in->deferred_unlink = 1;
				else                   ramfs_evict(in);
			}
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
	/* Synthesize "." and ".." — POSIX directories must list them, and readdir
	 * consumers (find, rm -r, du) expect them. (".." ino approximated by self;
	 * ramfs doesn't track the parent, and the name+type are what matters.) */
	if (idx == 0 || idx == 1) {
		out->ino      = dir->ino;
		out->mode     = dir->mode;
		out->next_off = idx + 1;
		out->name[0]  = '.';
		out->name[1]  = (idx == 1) ? '.' : '\0';
		out->name[2]  = '\0';
		return 0;
	}
	ramfs_entry_t *e = rd->entries;
	for (uint32_t i = 0; i < idx - 2 && e; i++) e = e->next;
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
	out->rdev  = inode->rdev;
	out->atime = inode->atime; out->atime_nsec = inode->atime_nsec;
	out->mtime = inode->mtime; out->mtime_nsec = inode->mtime_nsec;
	out->ctime = inode->ctime; out->ctime_nsec = inode->ctime_nsec;
	return 0;
}

static int ramfs_readlink(vfs_inode_t *inode, char *buf, size_t bufsize)
{
	if (!S_ISLNK(inode->mode)) return -22; /* EINVAL: priv is dir/file data, not a target */
	const char *target = (const char *)inode->priv;
	if (!target) return -1;
	int len = 0;
	while (target[len] && len < (int)bufsize - 1) { buf[len] = target[len]; len++; }
	buf[len] = '\0';
	return 0;
}

static int ramfs_symlink(vfs_inode_t *dir, const char *name, const char *target)
{
	ramfs_dir_t *rd = dir->priv;
	if (!rd) return -1;

	for (ramfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return -1;

	vfs_inode_t *in = ramfs_inode_alloc(S_IFLNK | 0777);
	if (!in) return -1;

	int tlen = 0; while (target[tlen]) tlen++;
	char *t = kmalloc(tlen + 1);
	if (!t) { kfree(in); return -1; }
	for (int i = 0; i <= tlen; i++) t[i] = target[i];
	in->priv = t;
	in->size = tlen;
	in->iops = &ramfs_iops;

	ramfs_entry_t *ent = kmalloc(sizeof(*ent));
	if (!ent) { kfree(t); kfree(in); return -1; }
	int nlen = 0; while (name[nlen] && nlen < 255) nlen++;
	memcpy(ent->name, name, nlen); ent->name[nlen] = '\0';
	ent->inode  = in;
	ent->next   = rd->entries;
	rd->entries = ent;
	rd->count++;
	return 0;
}

static int ramfs_link(vfs_inode_t *dir, const char *name, vfs_inode_t *target)
{
	if (S_ISDIR(target->mode)) return -21;
	ramfs_dir_t *rd = dir->priv;
	if (!rd) return -1;
	for (ramfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return -17;
	ramfs_entry_t *ent = kmalloc(sizeof(*ent));
	if (!ent) return -12;
	int nlen = 0;
	while (name[nlen] && nlen < 255) nlen++;
	memcpy(ent->name, name, nlen); ent->name[nlen] = '\0';
	ent->inode = target;
	ent->next  = rd->entries;
	rd->entries = ent;
	rd->count++;
	target->nlink++;
	return 0;
}

static int ramfs_truncate(vfs_inode_t *inode, off_t len)
{
	if (!S_ISREG(inode->mode)) return -22;
	ramfs_file_t *rf = inode->priv;
	if (!rf) return -22;
	if (len < 0) return -22;
	uint64_t newlen = (uint64_t)len;
	if (newlen > rf->capacity) {
		if (inode->mmap_pinned) return -12;   /* buffer pinned by a live MAP_SHARED mapping */
		int new_order = 0;
		while (((uint64_t)4096 << new_order) < newlen) new_order++;
		void *nb_phys = buddy_alloc(new_order);
		if (!nb_phys) return -12;
		uint8_t *nb = (uint8_t *)PHYS_TO_VIRT(nb_phys);
		uint64_t newcap = (uint64_t)4096 << new_order;
		memset(nb, 0, (size_t)newcap);
		if (rf->data) {
			memcpy(nb, rf->data, rf->size < newlen ? (size_t)rf->size : (size_t)newlen);
			int old_order = 0;
			while (((uint64_t)4096 << old_order) < rf->capacity) old_order++;
			buddy_free(VIRT_TO_PHYS(rf->data), old_order);
		}
		rf->data     = nb;
		rf->capacity = newcap;
	} else if (newlen < rf->size) {
		memset(rf->data + newlen, 0, (size_t)(rf->size - newlen));
	}
	rf->size      = newlen;
	inode->size   = newlen;
	return 0;
}

static vfs_iops_t ramfs_iops = {
	.lookup   = ramfs_lookup,
	.create   = ramfs_create,
	.mkdir    = ramfs_mkdir,
	.mknod    = ramfs_mknod,
	.unlink   = ramfs_unlink,
	.readdir  = ramfs_readdir,
	.rename   = ramfs_rename,
	.stat     = ramfs_stat,
	.readlink = ramfs_readlink,
	.symlink  = ramfs_symlink,
	.truncate = ramfs_truncate,
	.link     = ramfs_link,
	.evict    = ramfs_evict,
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

/* ── anonymous file for memfd_create ─────────────────────── */

vfs_inode_t *ramfs_create_anon(void)
{
	vfs_inode_t *in = ramfs_inode_alloc(S_IFREG | 0600);
	if (!in) return NULL;
	ramfs_file_t *rf = kmalloc(sizeof(*rf));
	if (!rf) { kfree(in); return NULL; }
	memset(rf, 0, sizeof(*rf));
	in->priv = rf;
	in->fops = &ramfs_fops;
	in->iops = &ramfs_iops;
	return in;
}
