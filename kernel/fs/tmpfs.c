#include <kernel/fs/tmpfs.h>
#include <kernel/fs/pagefile.h>
#include <kernel/vfs.h>
#include <kernel/hal.h>
#include <kernel/proc.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/pmm.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/sync.h>
#include <kernel/cgroup.h>
#include <asm/mmu.h>
#include <kernel/fs/devfs.h>
#include <kernel/errno.h>

/*
 * tmpfs — in-memory filesystem with size accounting and mount options.
 *
 * Like ramfs (inodes/dirs in the heap, file data in single pages, see
 * pagefile.c) but every mount carries a tmpfs_sb_t tracking byte and inode
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
	int         dead;       /* last mount gone: the last inode frees the sb */
	vfs_dentry_t *root;
} tmpfs_sb_t;

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
static spinlock_t dir_lock = SPINLOCK_INIT;
static vfs_iops_t tmpfs_iops;

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

static int sb_charge_cb(void *sb, uint64_t n) { return sb_charge(sb, n); }
static void sb_uncharge_cb(void *sb, uint64_t n) { sb_uncharge(sb, n); }

static const pagefile_limit_t sb_limit = { sb_charge_cb, sb_uncharge_cb };

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
	uint64_t f = spin_lock_irq(&ino_lock);
	in->ino = next_ino++;
	spin_unlock_irq(&ino_lock, f);
	in->mode     = mode;
	in->nlink    = 1;
	in->refcount = 1;
	in->cached   = 1;   /* fs-owned: freed via unlink/evict, never by the VFS transient-free path */
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

static vfs_fops_t tmpfs_fops = {
	.open  = pagefile_open,
	.read  = pagefile_read,
	.write = pagefile_write,
	.mmap  = pagefile_mmap,
	.mmap_cacheable = 1,
};

static vfs_fops_t tmpfs_dir_fops = { 0 };

/* ── inode ops ───────────────────────────────────────────── */

static vfs_inode_t *tmpfs_lookup(vfs_inode_t *dir, const char *name)
{
	if (!S_ISDIR(dir->mode)) return NULL;   /* priv is a file struct, not an entry list */
	tmpfs_dir_t *rd = dir->priv;
	if (!rd) return NULL;
	vfs_inode_t *in = NULL;
	uint64_t f = spin_lock_irq(&dir_lock);
	for (tmpfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) { in = e->inode; break; }
	spin_unlock_irq(&dir_lock, f);
	return in;
}

static tmpfs_entry_t *tmpfs_lookup_locked(tmpfs_dir_t *rd, const char *name)
{
	for (tmpfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return e;
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

static int tmpfs_create_locked(vfs_inode_t *dir, const char *name, mode_t mode)
{
	tmpfs_dir_t *rd = dir->priv;
	if (!rd) return -1;
	for (tmpfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return -EEXIST;

	vfs_inode_t *in = tmpfs_inode_alloc(dir->i_sb, mode | S_IFREG);
	if (!in) return -ENOSPC;   /* out of inodes */
	pagefile_t *pf = pagefile_new(&sb_limit, dir->i_sb);
	if (!pf) { sb_uncharge_inode(dir->i_sb); kfree(in); return -ENOMEM; }
	in->priv = pf;
	in->fops = &tmpfs_fops;
	in->iops = &tmpfs_iops;

	if (!dir_new_entry(rd, name, in)) { pagefile_evict(pf); sb_uncharge_inode(dir->i_sb); kfree(in); return -ENOMEM; }
	return 0;
}

static int tmpfs_mknod_locked(vfs_inode_t *dir, const char *name, mode_t mode, dev_t dev)
{
	tmpfs_dir_t *rd = dir->priv;
	if (!rd) return -1;
	int type = mode & 0xF000;
	/* S_IFSOCK: the name an AF_UNIX bind leaves, so stat and its mode work */
	if (type != S_IFIFO && type != S_IFCHR && type != S_IFBLK && type != 0140000) return -1;
	for (tmpfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return -EEXIST;
	vfs_inode_t *in = tmpfs_inode_alloc(dir->i_sb, mode);
	if (!in) return -ENOSPC;
	in->iops = &tmpfs_iops;
	if (type == S_IFIFO) {
		fifo_init_inode(in);
	} else if (type != 0140000) {   /* a socket node has no fops: open fails */
		void *dpriv = NULL;
		in->rdev = dev;
		in->fops = devfs_fops_for_rdev(dev, &dpriv);
		in->priv = dpriv;
	}
	if (!dir_new_entry(rd, name, in)) { sb_uncharge_inode(dir->i_sb); kfree(in); return -ENOMEM; }
	return 0;
}

static int tmpfs_mkdir_locked(vfs_inode_t *dir, const char *name, mode_t mode)
{
	tmpfs_dir_t *rd = dir->priv;
	if (!rd) return -1;
	for (tmpfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return -EEXIST;

	vfs_inode_t *in = tmpfs_inode_alloc(dir->i_sb, mode | S_IFDIR);
	if (!in) return -ENOSPC;
	tmpfs_dir_t *nrd = kmalloc(sizeof(*nrd));
	if (!nrd) { sb_uncharge_inode(dir->i_sb); kfree(in); return -ENOMEM; }
	memset(nrd, 0, sizeof(*nrd));
	in->priv = nrd;
	in->iops = &tmpfs_iops;
	in->fops = &tmpfs_dir_fops;

	if (!dir_new_entry(rd, name, in)) { kfree(nrd); sb_uncharge_inode(dir->i_sb); kfree(in); return -ENOMEM; }
	dir->nlink++;
	return 0;
}

/* free an inode's data and charge after its last name and fd are gone */
static void tmpfs_evict(vfs_inode_t *in)
{
	tmpfs_sb_t *sb = in->i_sb;
	if (S_ISREG(in->mode) && in->priv) {
		pagefile_evict(in->priv);
	} else if (S_ISLNK(in->mode) || S_ISDIR(in->mode)) {
		kfree(in->priv);   /* symlink target string, or the directory's list head */
	} else if (S_ISFIFO(in->mode)) {
		fifo_free_inode(in);
	}
	in->priv = NULL;
	vfs_inode_free_xattrs(in);
	kfree(in);
	/* the count and the verdict in one lock: two evicts racing to zero would
	 * otherwise both see it and both free sb */
	uint64_t f = spin_lock_irq(&sb->lock);
	if (sb->used_inodes) sb->used_inodes--;
	int last = sb->dead && !sb->used_inodes;
	spin_unlock_irq(&sb->lock, f);
	if (last) kfree(sb);
}

/* Drop one name of in. */
static void tmpfs_drop_name(vfs_inode_t *in)
{
	if (!S_ISDIR(in->mode) && in->nlink > 0) in->nlink--;
	if (S_ISDIR(in->mode) || !in->nlink) vfs_inode_orphan(in);
}

static int tmpfs_unlink_locked(vfs_inode_t *dir, const char *name)
{
	tmpfs_dir_t *rd = dir->priv;
	if (!rd) return -1;
	tmpfs_entry_t **pp = &rd->entries;
	while (*pp) {
		if (strcmp((*pp)->name, name) == 0) {
			tmpfs_entry_t *e = *pp;
			vfs_inode_t *in = e->inode;
			if (S_ISDIR(in->mode) && ((tmpfs_dir_t *)in->priv)->count) return -ENOTEMPTY;
			*pp = e->next;
			rd->count--;
			if (S_ISDIR(in->mode)) dir->nlink--;
			tmpfs_drop_name(in);
			kfree(e);
			return 0;
		}
		pp = &(*pp)->next;
	}
	return -1;
}

static int tmpfs_readdir_locked(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
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

static int tmpfs_rename_locked(vfs_inode_t *old_dir, const char *old_name,
                        vfs_inode_t *new_dir, const char *new_name)
{
	tmpfs_dir_t *rd = old_dir->priv, *nrd = new_dir->priv;
	if (!rd || !nrd) return -ENOENT;
	if (!tmpfs_lookup_locked(rd, old_name)) return -ENOENT;
	if (tmpfs_unlink_locked(new_dir, new_name) == -39) return -ENOTEMPTY;

	/* found again: the unlink may have changed this very list */
	tmpfs_entry_t **pp = &rd->entries;
	while (strcmp((*pp)->name, old_name) != 0) pp = &(*pp)->next;
	tmpfs_entry_t *e = *pp;
	int nlen = 0; while (new_name[nlen] && nlen < 254) nlen++;
	memcpy(e->name, new_name, nlen); e->name[nlen] = '\0';
	if (old_dir != new_dir) {
		*pp = e->next;
		rd->count--;
		e->next = nrd->entries;
		nrd->entries = e;
		nrd->count++;
		if (S_ISDIR(e->inode->mode)) { old_dir->nlink--; new_dir->nlink++; }
	}
	return 0;
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
	if (!S_ISLNK(inode->mode)) return -EINVAL;
	const char *target = (const char *)inode->priv;
	if (!target) return -1;
	int len = 0;
	while (target[len] && len < (int)bufsize - 1) { buf[len] = target[len]; len++; }
	buf[len] = '\0';
	return 0;
}

static int tmpfs_symlink_locked(vfs_inode_t *dir, const char *name, const char *target)
{
	tmpfs_dir_t *rd = dir->priv;
	if (!rd) return -1;
	for (tmpfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return -EEXIST;

	vfs_inode_t *in = tmpfs_inode_alloc(dir->i_sb, S_IFLNK | 0777);
	if (!in) return -ENOSPC;
	int tlen = 0; while (target[tlen]) tlen++;
	char *t = kmalloc(tlen + 1);
	if (!t) { sb_uncharge_inode(dir->i_sb); kfree(in); return -ENOMEM; }
	for (int i = 0; i <= tlen; i++) t[i] = target[i];
	in->priv = t;
	in->size = tlen;
	in->iops = &tmpfs_iops;

	if (!dir_new_entry(rd, name, in)) { kfree(t); sb_uncharge_inode(dir->i_sb); kfree(in); return -ENOMEM; }
	return 0;
}

static int tmpfs_link_locked(vfs_inode_t *dir, const char *name, vfs_inode_t *target)
{
	if (S_ISDIR(target->mode)) return -EISDIR;
	if (!target->nlink || target->deferred_unlink) return -ENOENT;   /* no name left to add to */
	tmpfs_dir_t *rd = dir->priv;
	if (!rd) return -1;
	for (tmpfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return -EEXIST;
	if (!dir_new_entry(rd, name, target)) return -ENOMEM;
	target->nlink++;
	return 0;
}

static int tmpfs_truncate(vfs_inode_t *inode, off_t len)
{
	if (!S_ISREG(inode->mode)) return -EINVAL;
	return pagefile_truncate(inode, len);
}

static int tmpfs_statfs(vfs_inode_t *inode, vfs_statfs_t *out)
{
	tmpfs_sb_t *sb = inode->i_sb;
	if (!sb) return -ENOSYS;
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

/* Directory lists change under two tasks at once, a lookup among them. */
#define DIR_LOCKED(name, params, args) \
static int name params \
{ \
	uint64_t f = spin_lock_irq(&dir_lock); \
	int r = name##_locked args; \
	spin_unlock_irq(&dir_lock, f); \
	return r; \
}

DIR_LOCKED(tmpfs_create, (vfs_inode_t *d, const char *n, mode_t m), (d, n, m))
DIR_LOCKED(tmpfs_mknod, (vfs_inode_t *d, const char *n, mode_t m, dev_t v), (d, n, m, v))
DIR_LOCKED(tmpfs_mkdir, (vfs_inode_t *d, const char *n, mode_t m), (d, n, m))
DIR_LOCKED(tmpfs_unlink, (vfs_inode_t *d, const char *n), (d, n))
DIR_LOCKED(tmpfs_readdir, (vfs_inode_t *d, uint32_t i, vfs_dirent_t *o), (d, i, o))
DIR_LOCKED(tmpfs_rename, (vfs_inode_t *od, const char *on, vfs_inode_t *nd, const char *nn), (od, on, nd, nn))
DIR_LOCKED(tmpfs_symlink, (vfs_inode_t *d, const char *n, const char *t), (d, n, t))
DIR_LOCKED(tmpfs_link, (vfs_inode_t *d, const char *n, vfs_inode_t *t), (d, n, t))

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

/* "mode=1777": the root directory's permission bits, octal (rootsw mounts /tmp
 * 1777 and /run 0755). Without it the root is 0755: closed unless asked. */
static mode_t parse_mode_opt(const char *opts)
{
	if (!opts) return 0755;
	const char *p = strstr(opts, "mode=");
	if (!p || (p != opts && p[-1] != ',')) return 0755;
	p += 5;
	mode_t v = 0;
	int digits = 0;
	while (*p >= '0' && *p <= '7' && digits < 5) { v = (mode_t)(v * 8 + (mode_t)(*p - '0')); p++; digits++; }
	return digits ? (v & 07777) : 0755;
}

static void tmpfs_get_sb(void *priv)
{
	tmpfs_sb_t *sb = priv;
	if (!sb) return;
	uint64_t f = spin_lock_irq(&sb->lock);
	sb->refcount++;
	spin_unlock_irq(&sb->lock, f);
}

/* The last mount is gone, so nothing reaches these names any more. A file still
 * open keeps its inode, which holds sb alive through used_inodes. Subdirectories
 * are spliced into root's list rather than recursed into: depth is user-chosen. */
static void tmpfs_kill_sb(void *priv)
{
	tmpfs_sb_t *sb = priv;
	if (!sb) return;
	uint64_t f = spin_lock_irq(&sb->lock);
	int last = (--sb->refcount == 0);
	spin_unlock_irq(&sb->lock, f);
	if (!last) return;

	vfs_inode_t *root = sb->root->inode;
	vfs_dentry_free_tree(sb->root, 0);
	f = spin_lock_irq(&sb->lock);
	sb->dead = 1;
	spin_unlock_irq(&sb->lock, f);
	tmpfs_dir_t *rd = root->priv;
	while (rd->entries) {
		tmpfs_entry_t *e = rd->entries;
		rd->entries = e->next;
		vfs_inode_t *in = e->inode;
		if (S_ISDIR(in->mode)) {
			tmpfs_dir_t *sub = in->priv;
			tmpfs_entry_t *tail = sub->entries;
			while (tail && tail->next) tail = tail->next;
			if (tail) { tail->next = rd->entries; rd->entries = sub->entries; }
			sub->entries = NULL;
			sub->count = 0;
		}
		tmpfs_drop_name(in);
		kfree(e);
	}
	rd->count = 0;
	tmpfs_drop_name(root);
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

	vfs_inode_t *root_inode = tmpfs_inode_alloc(sb, S_IFDIR | parse_mode_opt((const char *)data));
	if (!root_inode) { kfree(sb); return -1; }
	tmpfs_dir_t *rd = kmalloc(sizeof(*rd));
	if (!rd) { kfree(root_inode); kfree(sb); return -1; }
	memset(rd, 0, sizeof(*rd));
	root_inode->priv = rd;
	root_inode->iops = &tmpfs_iops;
	root_inode->fops = &tmpfs_dir_fops;

	vfs_dentry_t *root_dentry = vfs_dentry_create(NULL, "/", root_inode);
	if (!root_dentry) { kfree(rd); kfree(root_inode); kfree(sb); return -1; }

	sb->root     = root_dentry;
	mnt->root    = root_dentry;
	mnt->priv    = sb;
	mnt->kill_sb = tmpfs_kill_sb;
	mnt->get_sb  = tmpfs_get_sb;
	return 0;
}

void tmpfs_register(void)
{
	vfs_register_fs("tmpfs", tmpfs_mount);
}
