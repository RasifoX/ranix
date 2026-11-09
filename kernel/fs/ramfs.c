#include <kernel/fs/ramfs.h>
#include <kernel/fs/pagefile.h>
#include <kernel/vfs.h>
#include <kernel/hal.h>
#include <kernel/proc.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/sync.h>
#include <kernel/cgroup.h>
#include <asm/mmu.h>
#include <kernel/fs/devfs.h>
#include <kernel/errno.h>

/*
 * ramfs — volatile in-memory filesystem.
 *
 * File data:  priv = pagefile_t*
 * Dir data:   priv = ramfs_dir_t*   (linked list of entries)
 *
 * Inode numbers are assigned sequentially.
 */

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
static spinlock_t  dir_lock   = SPINLOCK_INIT;
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
	in->cached   = 1;   /* fs-owned: freed via unlink/evict, never by the VFS transient-free path */
	process_t *p = task_current();
	in->uid = p ? p->euid : 0;
	in->gid = p ? p->egid : 0;
	int64_t s, ns; ranix_walltime(&s, &ns);
	in->atime = in->mtime = in->ctime = s;
	in->atime_nsec = in->mtime_nsec = in->ctime_nsec = (uint32_t)ns;
	return in;
}

/* ── file ops ────────────────────────────────────────────── */

static vfs_fops_t ramfs_fops = {
	.open  = pagefile_open,
	.read  = pagefile_read,
	.write = pagefile_write,
	.mmap  = pagefile_mmap,
	.mmap_cacheable = 1,
};

static vfs_fops_t ramfs_dir_fops = { 0 };

/* ── inode ops ───────────────────────────────────────────── */

static vfs_inode_t *ramfs_lookup(vfs_inode_t *dir, const char *name)
{
	if (!S_ISDIR(dir->mode)) return NULL;   /* priv is a file struct, not an entry list */
	ramfs_dir_t *rd = dir->priv;
	if (!rd) return NULL;
	vfs_inode_t *in = NULL;
	uint64_t f = spin_lock_irq(&dir_lock);
	for (ramfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) { in = e->inode; break; }
	spin_unlock_irq(&dir_lock, f);
	return in;
}

static int ramfs_create_locked(vfs_inode_t *dir, const char *name, mode_t mode)
{
	ramfs_dir_t *rd = dir->priv;
	if (!rd) return -1;

	/* check for duplicate */
	for (ramfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return -EEXIST;

	vfs_inode_t *in = ramfs_inode_alloc(mode | S_IFREG);
	if (!in) return -1;

	pagefile_t *pf = pagefile_new(NULL, NULL);
	if (!pf) { kfree(in); return -1; }
	in->priv = pf;
	in->fops = &ramfs_fops;
	in->iops = &ramfs_iops;

	ramfs_entry_t *ent = kmalloc(sizeof(*ent));
	if (!ent) { pagefile_evict(pf); kfree(in); return -1; }
	int nlen = 0;
	while (name[nlen] && nlen < 255) nlen++;
	memcpy(ent->name, name, nlen); ent->name[nlen] = '\0';
	ent->inode    = in;
	ent->next     = rd->entries;
	rd->entries   = ent;
	rd->count++;
	return 0;
}

static int ramfs_mknod_locked(vfs_inode_t *dir, const char *name, mode_t mode, dev_t dev)
{
	ramfs_dir_t *rd = dir->priv;
	if (!rd) return -1;
	int type = mode & 0xF000;
	if (type != S_IFIFO && type != S_IFCHR && type != S_IFBLK && type != 0140000 /* S_IFSOCK */)
		return -1; /* unsupported node type */
	for (ramfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return -EEXIST;
	vfs_inode_t *in = ramfs_inode_alloc(mode);
	if (!in) return -1;
	in->iops = &ramfs_iops;
	if (type == S_IFIFO) {
		fifo_init_inode(in);                     /* fops + lazy pipe */
	} else if (type != 0140000) {                /* char/block: route by major:minor */
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

static int ramfs_mkdir_locked(vfs_inode_t *dir, const char *name, mode_t mode)
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

/* free an inode's data after its last name and fd are gone */
static void ramfs_evict(vfs_inode_t *in)
{
	if (S_ISREG(in->mode) && in->priv) {
		pagefile_evict(in->priv);
	} else if (S_ISLNK(in->mode) || S_ISDIR(in->mode)) {
		kfree(in->priv);
	} else if (S_ISFIFO(in->mode)) {
		fifo_free_inode(in);
	}
	in->priv = NULL;
	vfs_inode_free_xattrs(in);
	kfree(in);
}

/* Drop one name of in. */
static void ramfs_drop_name(vfs_inode_t *in)
{
	if (!S_ISDIR(in->mode) && in->nlink > 0) in->nlink--;
	if (S_ISDIR(in->mode) || !in->nlink) vfs_inode_orphan(in);
}

static int ramfs_unlink_locked(vfs_inode_t *dir, const char *name)
{
	ramfs_dir_t   *rd   = dir->priv;
	if (!rd) return -1;
	ramfs_entry_t **pp = &rd->entries;
	while (*pp) {
		if (strcmp((*pp)->name, name) == 0) {
			ramfs_entry_t *e = *pp;
			vfs_inode_t *in = e->inode;
			if (S_ISDIR(in->mode) && ((ramfs_dir_t *)in->priv)->count) return -ENOTEMPTY;
			*pp = e->next;
			rd->count--;
			/* Removing a subdirectory drops the parent's '..' backlink; removing
			 * a file drops one of that file's hard links. */
			if (S_ISDIR(in->mode)) dir->nlink--;
			ramfs_drop_name(in);
			kfree(e);
			return 0;
		}
		pp = &(*pp)->next;
	}
	return -1;
}

static int ramfs_readdir_locked(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
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

static int ramfs_rename_locked(vfs_inode_t *old_dir, const char *old_name,
                        vfs_inode_t *new_dir, const char *new_name)
{
	ramfs_dir_t *od = old_dir->priv;
	ramfs_dir_t *nd = new_dir->priv;
	if (!od || !nd) return -ENOENT;

	ramfs_entry_t **pp = &od->entries;
	while (*pp && strcmp((*pp)->name, old_name) != 0) pp = &(*pp)->next;
	if (!*pp) return -ENOENT;
	ramfs_entry_t *e = *pp;

	if (old_dir == new_dir) {
		if (ramfs_unlink_locked(new_dir, new_name) == -39) return -ENOTEMPTY;
		/* unlink may have unlinked the source if old_name==new_name; re-find */
		pp = &od->entries;
		while (*pp && strcmp((*pp)->name, old_name) != 0) pp = &(*pp)->next;
		if (!*pp) return 0;
		e = *pp;
		int nlen = 0;
		while (new_name[nlen] && nlen < 255) nlen++;
		memcpy(e->name, new_name, nlen);
		e->name[nlen] = '\0';
		return 0;
	}

	/* cross-directory: unlink any existing target, then splice the entry out
	 * of the old list and into the new one, fixing parent dir link counts. */
	if (ramfs_unlink_locked(new_dir, new_name) == -39) return -ENOTEMPTY;
	pp = &od->entries;
	while (*pp && *pp != e) pp = &(*pp)->next;
	if (!*pp) return -ENOENT;
	*pp = e->next;
	od->count--;
	if (S_ISDIR(e->inode->mode)) old_dir->nlink--;

	int nlen = 0;
	while (new_name[nlen] && nlen < 255) nlen++;
	memcpy(e->name, new_name, nlen);
	e->name[nlen] = '\0';
	e->next = nd->entries;
	nd->entries = e;
	nd->count++;
	if (S_ISDIR(e->inode->mode)) new_dir->nlink++;
	return 0;
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
	if (!S_ISLNK(inode->mode)) return -EINVAL;   /* priv is dir/file data, not a target */
	const char *target = (const char *)inode->priv;
	if (!target) return -1;
	int len = 0;
	while (target[len] && len < (int)bufsize - 1) { buf[len] = target[len]; len++; }
	buf[len] = '\0';
	return 0;
}

static int ramfs_symlink_locked(vfs_inode_t *dir, const char *name, const char *target)
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

static int ramfs_link_locked(vfs_inode_t *dir, const char *name, vfs_inode_t *target)
{
	if (S_ISDIR(target->mode)) return -EISDIR;
	if (!target->nlink || target->deferred_unlink) return -ENOENT;   /* no name left to add to */
	ramfs_dir_t *rd = dir->priv;
	if (!rd) return -1;
	for (ramfs_entry_t *e = rd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return -EEXIST;
	ramfs_entry_t *ent = kmalloc(sizeof(*ent));
	if (!ent) return -ENOMEM;
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
	if (!S_ISREG(inode->mode)) return -EINVAL;
	return pagefile_truncate(inode, len);
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

DIR_LOCKED(ramfs_create, (vfs_inode_t *d, const char *n, mode_t m), (d, n, m))
DIR_LOCKED(ramfs_mknod, (vfs_inode_t *d, const char *n, mode_t m, dev_t v), (d, n, m, v))
DIR_LOCKED(ramfs_mkdir, (vfs_inode_t *d, const char *n, mode_t m), (d, n, m))
DIR_LOCKED(ramfs_unlink, (vfs_inode_t *d, const char *n), (d, n))
DIR_LOCKED(ramfs_readdir, (vfs_inode_t *d, uint32_t i, vfs_dirent_t *o), (d, i, o))
DIR_LOCKED(ramfs_rename, (vfs_inode_t *od, const char *on, vfs_inode_t *nd, const char *nn), (od, on, nd, nn))
DIR_LOCKED(ramfs_symlink, (vfs_inode_t *d, const char *n, const char *t), (d, n, t))
DIR_LOCKED(ramfs_link, (vfs_inode_t *d, const char *n, vfs_inode_t *t), (d, n, t))

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

/* The last mount is gone, so nothing reaches these names any more; a file still
 * open keeps its inode. Subdirectories are spliced into root's list rather than
 * recursed into: depth is user-chosen. */
static void ramfs_release(vfs_inode_t *root)
{
	ramfs_dir_t *rd = root->priv;
	while (rd->entries) {
		ramfs_entry_t *e = rd->entries;
		rd->entries = e->next;
		vfs_inode_t *in = e->inode;
		if (S_ISDIR(in->mode)) {
			ramfs_dir_t *sub = in->priv;
			ramfs_entry_t *tail = sub->entries;
			while (tail && tail->next) tail = tail->next;
			if (tail) { tail->next = rd->entries; rd->entries = sub->entries; }
			sub->entries = NULL;
			sub->count = 0;
		}
		ramfs_drop_name(in);
		kfree(e);
	}
	rd->count = 0;
	ramfs_drop_name(root);
}

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
	return vfs_simple_sb(mnt, ramfs_release);
}

void ramfs_register(void)
{
	vfs_register_fs("ramfs", ramfs_mount);
}

/* ── anonymous file for memfd_create ─────────────────────── */

vfs_inode_t *ramfs_create_anon(void)
{
	vfs_inode_t *in = ramfs_inode_alloc(S_IFREG | 0600);
	if (!in) return NULL;
	pagefile_t *pf = pagefile_new(NULL, NULL);
	if (!pf) { kfree(in); return NULL; }
	in->priv = pf;
	in->fops = &ramfs_fops;
	in->iops = &ramfs_iops;
	/* no name to lose: it goes with the last close */
	in->nlink = 0;
	in->deferred_unlink = 1;
	return in;
}
