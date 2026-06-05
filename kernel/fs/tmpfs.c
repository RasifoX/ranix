#include <kernel/fs/tmpfs.h>
#include <kernel/vfs.h>
#include <kernel/proc.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/buddy.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/sync.h>
#include <asm/mmu.h>

/*
 * tmpfs — in-memory filesystem with size accounting and mount options.
 *
 * Like ramfs (inodes/dirs in the heap, file data in page-rounded buddy
 * allocations) but every mount carries a tmpfs_sb_t tracking byte and inode
 * limits, so writes past size= fail with ENOSPC and statfs reports real
 * free/used. size= accepts a byte count with optional K/M/G suffix; absent it
 * defaults to half of RAM, matching Linux.
 */

typedef struct {
	uint64_t    max_bytes;
	uint64_t    used_bytes;
	uint64_t    max_inodes;
	uint64_t    used_inodes;
	spinlock_t  lock;
	uint32_t    refcount;   /* mounts sharing this sb across cloned namespaces */
} tmpfs_sb_t;

typedef struct {
	uint8_t  *data;
	uint64_t  size;
	uint64_t  capacity;
} tmpfs_file_t;

typedef struct tmpfs_entry {
	char                 name[256];
	vfs_inode_t         *inode;
	struct tmpfs_entry  *next;
} tmpfs_entry_t;

typedef struct {
	tmpfs_entry_t *entries;
	uint32_t       count;
} tmpfs_dir_t;

static uint64_t   next_ino = 1;
static spinlock_t ino_lock = SPINLOCK_INIT;
static vfs_iops_t tmpfs_iops;

static int order_for(uint64_t bytes)
{
	int order = 0;
	while (((uint64_t)PAGE_SIZE << order) < bytes) order++;
	return order;
}

/* charge n bytes against the sb limit; 0 ok, -1 over limit */
static int sb_charge(tmpfs_sb_t *sb, uint64_t n)
{
	uint64_t f = spin_lock_irq(&sb->lock);
	if (sb->used_bytes + n > sb->max_bytes) { spin_unlock_irq(&sb->lock, f); return -1; }
	sb->used_bytes += n;
	spin_unlock_irq(&sb->lock, f);
	return 0;
}

static void sb_uncharge(tmpfs_sb_t *sb, uint64_t n)
{
	uint64_t f = spin_lock_irq(&sb->lock);
	sb->used_bytes = (sb->used_bytes >= n) ? sb->used_bytes - n : 0;
	spin_unlock_irq(&sb->lock, f);
}

static int sb_charge_inode(tmpfs_sb_t *sb)
{
	uint64_t f = spin_lock_irq(&sb->lock);
	if (sb->used_inodes >= sb->max_inodes) { spin_unlock_irq(&sb->lock, f); return -1; }
	sb->used_inodes++;
	spin_unlock_irq(&sb->lock, f);
	return 0;
}

static void sb_uncharge_inode(tmpfs_sb_t *sb)
{
	uint64_t f = spin_lock_irq(&sb->lock);
	if (sb->used_inodes) sb->used_inodes--;
	spin_unlock_irq(&sb->lock, f);
}

static vfs_inode_t *tmpfs_inode_alloc(tmpfs_sb_t *sb, mode_t mode)
{
	if (sb_charge_inode(sb) != 0) return NULL;
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) { sb_uncharge_inode(sb); return NULL; }
	memset(in, 0, sizeof(*in));
	spin_lock(&ino_lock);
	in->ino = next_ino++;
	spin_unlock(&ino_lock);
	in->mode     = mode;
	in->nlink    = 1;
	in->refcount = 1;
	in->i_sb     = sb;
	process_t *p = task_current();
	in->uid = p ? p->euid : 0;
	in->gid = p ? p->egid : 0;
	int64_t s, ns; ranix_walltime(&s, &ns);
	in->atime = in->mtime = in->ctime = s;
	in->atime_nsec = in->mtime_nsec = in->ctime_nsec = (uint32_t)ns;
	return in;
}

/* ── file ops ────────────────────────────────────────────── */

static ssize_t tmpfs_read(vfs_file_t *f, void *buf, size_t len)
{
	tmpfs_file_t *rf = f->inode->priv;
	if (!rf || f->pos >= (off_t)rf->size) return 0;
	size_t avail = rf->size - (uint64_t)f->pos;
	if (len > avail) len = avail;
	memcpy(buf, rf->data + f->pos, len);
	f->pos += len;
	return (ssize_t)len;
}

static ssize_t tmpfs_write(vfs_file_t *f, const void *buf, size_t len)
{
	tmpfs_file_t *rf = f->inode->priv;
	if (!rf) return -1;
	tmpfs_sb_t *sb = f->inode->i_sb;

	uint64_t end = (uint64_t)f->pos + len;
	if (end > rf->capacity) {
		/* buddy gives power-of-2 page runs; charge the real allocation so the
		 * limit reflects memory actually consumed, and reject atomically. */
		int new_order = order_for(end);
		uint64_t real = (uint64_t)PAGE_SIZE << new_order;
		if (sb_charge(sb, real - rf->capacity) != 0) return -28; /* ENOSPC */
		uint8_t *nb_phys = buddy_alloc(new_order);
		if (!nb_phys) { sb_uncharge(sb, real - rf->capacity); return -28; }
		uint8_t *nb = (uint8_t *)PHYS_TO_VIRT(nb_phys);
		memset(nb, 0, (size_t)real);
		if (rf->data) {
			memcpy(nb, rf->data, rf->size);
			buddy_free(VIRT_TO_PHYS(rf->data), order_for(rf->capacity));
		}
		rf->data     = nb;
		rf->capacity = real;
	}

	memcpy(rf->data + f->pos, buf, len);
	f->pos += len;
	if ((uint64_t)f->pos > rf->size) { rf->size = (uint64_t)f->pos; f->inode->size = rf->size; }
	int64_t s, ns; ranix_walltime(&s, &ns);
	f->inode->mtime = f->inode->ctime = s;
	f->inode->mtime_nsec = f->inode->ctime_nsec = (uint32_t)ns;
	return (ssize_t)len;
}

static int tmpfs_file_open(vfs_inode_t *inode, vfs_file_t *file, int flags)
{
	if ((flags & O_TRUNC) && inode->priv) {
		tmpfs_file_t *rf = inode->priv;
		rf->size    = 0;
		inode->size = 0;
	}
	if (flags & O_APPEND) file->pos = (off_t)inode->size;
	return 0;
}

static vfs_fops_t tmpfs_fops = {
	.open  = tmpfs_file_open,
	.read  = tmpfs_read,
	.write = tmpfs_write,
};

static vfs_fops_t tmpfs_dir_fops = { 0 };

/* ── inode ops ───────────────────────────────────────────── */

static vfs_inode_t *tmpfs_lookup(vfs_inode_t *dir, const char *name)
{
	if (!S_ISDIR(dir->mode)) return NULL;   /* priv is a file struct, not an entry list */
	tmpfs_dir_t *rd = dir->priv;
	if (!rd) return NULL;
	for (tmpfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return e->inode;
	return NULL;
}

static tmpfs_entry_t *dir_new_entry(tmpfs_dir_t *rd, const char *name, vfs_inode_t *in)
{
	tmpfs_entry_t *ent = kmalloc(sizeof(*ent));
	if (!ent) return NULL;
	int nlen = 0; while (name[nlen] && nlen < 255) nlen++;
	memcpy(ent->name, name, nlen); ent->name[nlen] = '\0';
	ent->inode  = in;
	ent->next   = rd->entries;
	rd->entries = ent;
	rd->count++;
	return ent;
}

static int tmpfs_create(vfs_inode_t *dir, const char *name, mode_t mode)
{
	tmpfs_dir_t *rd = dir->priv;
	if (!rd) return -1;
	for (tmpfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return -17;

	vfs_inode_t *in = tmpfs_inode_alloc(dir->i_sb, mode | S_IFREG);
	if (!in) return -28; /* ENOSPC: out of inodes */
	tmpfs_file_t *rf = kmalloc(sizeof(*rf));
	if (!rf) { sb_uncharge_inode(dir->i_sb); kfree(in); return -12; }
	memset(rf, 0, sizeof(*rf));
	in->priv = rf;
	in->fops = &tmpfs_fops;
	in->iops = &tmpfs_iops;

	if (!dir_new_entry(rd, name, in)) { kfree(rf); sb_uncharge_inode(dir->i_sb); kfree(in); return -12; }
	return 0;
}

static int tmpfs_mknod(vfs_inode_t *dir, const char *name, mode_t mode, dev_t dev)
{
	tmpfs_dir_t *rd = dir->priv;
	if (!rd) return -1;
	int type = mode & 0xF000;
	if (type != S_IFIFO && type != S_IFCHR && type != S_IFBLK) return -1;
	for (tmpfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return -17;
	vfs_inode_t *in = tmpfs_inode_alloc(dir->i_sb, mode);
	if (!in) return -28;
	in->iops = &tmpfs_iops;
	if (type == S_IFIFO) {
		fifo_init_inode(in);
	} else {
		extern vfs_fops_t *devfs_fops_for_rdev(dev_t, void **);
		void *dpriv = NULL;
		in->rdev = dev;
		in->fops = devfs_fops_for_rdev(dev, &dpriv);
		in->priv = dpriv;
	}
	if (!dir_new_entry(rd, name, in)) { sb_uncharge_inode(dir->i_sb); kfree(in); return -12; }
	return 0;
}

static int tmpfs_mkdir(vfs_inode_t *dir, const char *name, mode_t mode)
{
	tmpfs_dir_t *rd = dir->priv;
	if (!rd) return -1;
	for (tmpfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return -17;

	vfs_inode_t *in = tmpfs_inode_alloc(dir->i_sb, mode | S_IFDIR);
	if (!in) return -28;
	tmpfs_dir_t *nrd = kmalloc(sizeof(*nrd));
	if (!nrd) { sb_uncharge_inode(dir->i_sb); kfree(in); return -12; }
	memset(nrd, 0, sizeof(*nrd));
	in->priv = nrd;
	in->iops = &tmpfs_iops;
	in->fops = &tmpfs_dir_fops;

	if (!dir_new_entry(rd, name, in)) { kfree(nrd); sb_uncharge_inode(dir->i_sb); kfree(in); return -12; }
	dir->nlink++;
	return 0;
}

/* free a regular file's data + inode charge after its last name and fd are gone */
static void tmpfs_evict(vfs_inode_t *in)
{
	if (S_ISREG(in->mode) && in->priv) {
		tmpfs_file_t *rf = in->priv;
		if (rf->data) buddy_free(VIRT_TO_PHYS(rf->data), order_for(rf->capacity));
		sb_uncharge(in->i_sb, rf->capacity);
		kfree(rf);
	} else if (S_ISLNK(in->mode) && in->priv) {
		kfree(in->priv);   /* symlink target string */
	}
	in->priv = NULL;
	sb_uncharge_inode(in->i_sb);
	vfs_inode_free_xattrs(in);
	kfree(in);
}

static int tmpfs_unlink(vfs_inode_t *dir, const char *name)
{
	tmpfs_dir_t *rd = dir->priv;
	if (!rd) return -1;
	tmpfs_entry_t **pp = &rd->entries;
	while (*pp) {
		if (strcmp((*pp)->name, name) == 0) {
			tmpfs_entry_t *e = *pp;
			*pp = e->next;
			rd->count--;
			vfs_inode_t *in = e->inode;
			if (S_ISDIR(in->mode)) {
				dir->nlink--;
			} else if (in->nlink > 0) {
				in->nlink--;
			}
			/* last link gone: free now, or defer past still-open fds */
			if (!S_ISDIR(in->mode) && in->nlink == 0) {
				if (in->open_refs > 0) in->deferred_unlink = 1;
				else                   tmpfs_evict(in);
			} else if (S_ISDIR(in->mode)) {
				sb_uncharge_inode(in->i_sb);
			}
			kfree(e);
			return 0;
		}
		pp = &(*pp)->next;
	}
	return -1;
}

static int tmpfs_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	tmpfs_dir_t *rd = dir->priv;
	if (!rd) return -1;
	if (idx == 0 || idx == 1) {
		out->ino      = dir->ino;
		out->mode     = dir->mode;
		out->next_off = idx + 1;
		out->name[0]  = '.';
		out->name[1]  = (idx == 1) ? '.' : '\0';
		out->name[2]  = '\0';
		return 0;
	}
	tmpfs_entry_t *e = rd->entries;
	for (uint32_t i = 0; i < idx - 2 && e; i++) e = e->next;
	if (!e) return -1;
	out->ino      = e->inode->ino;
	out->mode     = e->inode->mode;
	out->next_off = idx + 1;
	int nlen = 0; while (e->name[nlen] && nlen < 255) nlen++;
	memcpy(out->name, e->name, nlen); out->name[nlen] = '\0';
	return 0;
}

static int tmpfs_rename(vfs_inode_t *old_dir, const char *old_name,
                        vfs_inode_t *new_dir, const char *new_name)
{
	if (old_dir != new_dir) return -18; /* EXDEV */
	tmpfs_dir_t *rd = old_dir->priv;
	if (!rd) return -2;
	tmpfs_unlink(new_dir, new_name);
	for (tmpfs_entry_t *e = rd->entries; e; e = e->next) {
		if (strcmp(e->name, old_name) == 0) {
			int nlen = 0; while (new_name[nlen] && nlen < 254) nlen++;
			memcpy(e->name, new_name, nlen); e->name[nlen] = '\0';
			return 0;
		}
	}
	return -2;
}

static int tmpfs_stat(vfs_inode_t *inode, vfs_stat_t *out)
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

static int tmpfs_readlink(vfs_inode_t *inode, char *buf, size_t bufsize)
{
	if (!S_ISLNK(inode->mode)) return -22;
	const char *target = (const char *)inode->priv;
	if (!target) return -1;
	int len = 0;
	while (target[len] && len < (int)bufsize - 1) { buf[len] = target[len]; len++; }
	buf[len] = '\0';
	return 0;
}

static int tmpfs_symlink(vfs_inode_t *dir, const char *name, const char *target)
{
	tmpfs_dir_t *rd = dir->priv;
	if (!rd) return -1;
	for (tmpfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return -17;

	vfs_inode_t *in = tmpfs_inode_alloc(dir->i_sb, S_IFLNK | 0777);
	if (!in) return -28;
	int tlen = 0; while (target[tlen]) tlen++;
	char *t = kmalloc(tlen + 1);
	if (!t) { sb_uncharge_inode(dir->i_sb); kfree(in); return -12; }
	for (int i = 0; i <= tlen; i++) t[i] = target[i];
	in->priv = t;
	in->size = tlen;
	in->iops = &tmpfs_iops;

	if (!dir_new_entry(rd, name, in)) { kfree(t); sb_uncharge_inode(dir->i_sb); kfree(in); return -12; }
	return 0;
}

static int tmpfs_link(vfs_inode_t *dir, const char *name, vfs_inode_t *target)
{
	if (S_ISDIR(target->mode)) return -21;
	tmpfs_dir_t *rd = dir->priv;
	if (!rd) return -1;
	for (tmpfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return -17;
	if (!dir_new_entry(rd, name, target)) return -12;
	target->nlink++;
	return 0;
}

static int tmpfs_truncate(vfs_inode_t *inode, off_t len)
{
	if (!S_ISREG(inode->mode)) return -22;
	tmpfs_file_t *rf = inode->priv;
	if (!rf) return -22;
	if (len < 0) return -22;
	tmpfs_sb_t *sb = inode->i_sb;
	uint64_t newlen = (uint64_t)len;
	if (newlen > rf->capacity) {
		uint64_t newcap = (newlen + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
		int new_order = order_for(newcap);
		uint64_t real = (uint64_t)PAGE_SIZE << new_order;
		if (sb_charge(sb, real - rf->capacity) != 0) return -28;
		uint8_t *nb = (uint8_t *)PHYS_TO_VIRT(buddy_alloc(new_order));
		if (!nb) { sb_uncharge(sb, real - rf->capacity); return -28; }
		memset(nb, 0, (size_t)real);
		if (rf->data) {
			memcpy(nb, rf->data, rf->size < newlen ? (size_t)rf->size : (size_t)newlen);
			buddy_free(VIRT_TO_PHYS(rf->data), order_for(rf->capacity));
		}
		rf->data     = nb;
		rf->capacity = real;
	} else if (newlen < rf->size) {
		memset(rf->data + newlen, 0, (size_t)(rf->size - newlen));
	}
	rf->size    = newlen;
	inode->size = newlen;
	return 0;
}

static int tmpfs_statfs(vfs_inode_t *inode, vfs_statfs_t *out)
{
	tmpfs_sb_t *sb = inode->i_sb;
	if (!sb) return -38;
	uint64_t f = spin_lock_irq(&sb->lock);
	uint64_t used = sb->used_bytes, maxb = sb->max_bytes;
	uint64_t uino = sb->used_inodes, maxino = sb->max_inodes;
	spin_unlock_irq(&sb->lock, f);
	out->bsize   = PAGE_SIZE;
	out->blocks  = maxb / PAGE_SIZE;
	out->bfree   = (maxb > used) ? (maxb - used) / PAGE_SIZE : 0;
	out->bavail  = out->bfree;
	out->files   = maxino;
	out->ffree   = (maxino > uino) ? maxino - uino : 0;
	out->namelen = 255;
	return 0;
}

static vfs_iops_t tmpfs_iops = {
	.lookup   = tmpfs_lookup,
	.create   = tmpfs_create,
	.mkdir    = tmpfs_mkdir,
	.mknod    = tmpfs_mknod,
	.unlink   = tmpfs_unlink,
	.readdir  = tmpfs_readdir,
	.rename   = tmpfs_rename,
	.stat     = tmpfs_stat,
	.readlink = tmpfs_readlink,
	.symlink  = tmpfs_symlink,
	.truncate = tmpfs_truncate,
	.link     = tmpfs_link,
	.statfs   = tmpfs_statfs,
	.evict    = tmpfs_evict,
};

/* ── mount ───────────────────────────────────────────────── */

/* parse "size=" from a comma-separated option string; K/M/G suffix supported.
 * Returns bytes, or 0 if absent/unparseable (caller substitutes the default). */
static uint64_t parse_size_opt(const char *opts)
{
	if (!opts) return 0;
	const char *p = strstr(opts, "size=");
	if (!p) return 0;
	p += 5;
	uint64_t v = 0;
	int digits = 0;
	while (*p >= '0' && *p <= '9') { v = v * 10 + (uint64_t)(*p - '0'); p++; digits++; }
	if (!digits) return 0;
	switch (*p) {
	case 'k': case 'K': v *= 1024ULL; break;
	case 'm': case 'M': v *= 1024ULL * 1024; break;
	case 'g': case 'G': v *= 1024ULL * 1024 * 1024; break;
	default: break;
	}
	return v;
}

static void tmpfs_get_sb(void *priv)
{
	tmpfs_sb_t *sb = priv;
	if (!sb) return;
	uint64_t f = spin_lock_irq(&sb->lock);
	sb->refcount++;
	spin_unlock_irq(&sb->lock, f);
}

static void tmpfs_kill_sb(void *priv)
{
	tmpfs_sb_t *sb = priv;
	if (!sb) return;
	uint64_t f = spin_lock_irq(&sb->lock);
	int last = (--sb->refcount == 0);
	spin_unlock_irq(&sb->lock, f);
	/* inode/data pages are reclaimed by the buddy allocator at teardown; the
	 * dentry tree drops with the namespace. Free only the sb bookkeeping. */
	if (last) kfree(sb);
}

int tmpfs_mount(vfs_mount_t *mnt, void *data)
{
	tmpfs_sb_t *sb = kmalloc(sizeof(*sb));
	if (!sb) return -1;
	memset(sb, 0, sizeof(*sb));
	sb->lock = (spinlock_t)SPINLOCK_INIT;
	sb->refcount = 1;

	uint64_t ram = buddy_total_pages() * (uint64_t)PAGE_SIZE;
	uint64_t want = parse_size_opt((const char *)data);
	sb->max_bytes  = want ? want : (ram / 2);
	if (sb->max_bytes > ram) sb->max_bytes = ram;          /* never promise more than RAM */
	sb->max_inodes = sb->max_bytes / PAGE_SIZE + 16;       /* one per data page, plus headroom */
	if (sb->max_inodes < 64) sb->max_inodes = 64;

	vfs_inode_t *root_inode = tmpfs_inode_alloc(sb, S_IFDIR | 0755);
	if (!root_inode) { kfree(sb); return -1; }
	tmpfs_dir_t *rd = kmalloc(sizeof(*rd));
	if (!rd) { kfree(root_inode); kfree(sb); return -1; }
	memset(rd, 0, sizeof(*rd));
	root_inode->priv = rd;
	root_inode->iops = &tmpfs_iops;
	root_inode->fops = &tmpfs_dir_fops;

	vfs_dentry_t *root_dentry = vfs_dentry_create(NULL, "/", root_inode);
	if (!root_dentry) { kfree(rd); kfree(root_inode); kfree(sb); return -1; }

	mnt->root    = root_dentry;
	mnt->priv    = sb;
	mnt->kill_sb = tmpfs_kill_sb;
	mnt->get_sb  = tmpfs_get_sb;
	return 0;
}

void tmpfs_register(void)
{
	extern void vfs_register_fs(const char *, int (*)(vfs_mount_t *, void *));
	vfs_register_fs("tmpfs", tmpfs_mount);
}
