#include <kernel/vfs.h>
#include <kernel/proc.h>
#include <kernel/config.h>
#include <kernel/cmdline.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>

static vfs_dentry_t *root_dentry = NULL;

vfs_inotify_fn_t vfs_inotify_hook = NULL;

static spinlock_t vfs_lock = SPINLOCK_INIT;
static int dentry_count = 0;
static int dentry_max_val = DENTRY_MAX;

static void vfs_inode_free(vfs_inode_t *in)
{
	if (!in || in->cached) return; /* cached=1: owned by dentry tree, pipe, or static */
	if (in->priv) kfree(in->priv);
	kfree(in);
}

/* ── permission check ────────────────────────────────────── */
/* accmode: bitmask of MAY_READ=4, MAY_WRITE=2, MAY_EXEC=1   */
#define MAY_READ  4
#define MAY_WRITE 2
#define MAY_EXEC  1

static int vfs_check_perm(vfs_inode_t *inode, int accmode, uid_t uid, gid_t gid)
{
	if (!inode) return -13;
	if (uid == 0) return 0; /* root bypasses all permission checks */

	mode_t m = inode->mode & 0777;
	int shift;
	if (inode->uid == uid) {
		shift = 6;
	} else {
		/* check egid and supplementary groups */
		int in_group = (inode->gid == gid);
		if (!in_group) {
			process_t *p = task_current();
			if (p) {
				for (int i = 0; i < p->ngroups; i++) {
					if (p->groups[i] == inode->gid) { in_group = 1; break; }
				}
			}
		}
		shift = in_group ? 3 : 0;
	}

	if (((m >> shift) & accmode) == (unsigned)accmode) return 0;
	return -13;
}

/* ── dentry pool ─────────────────────────────────────────── */

static vfs_dentry_t *_dentry_create(vfs_dentry_t *parent, const char *name, vfs_inode_t *inode)
{
	vfs_dentry_t *d = kmalloc(sizeof(*d));
	if (!d) return NULL;

	int nlen = 0;
	while (name[nlen] && nlen < 255) nlen++;
	memcpy(d->name, name, nlen);
	d->name[nlen] = '\0';

	d->inode  = inode;
	d->parent = parent;
	d->child  = NULL;
	d->next   = NULL;

	if (parent) {
		d->next        = parent->child;
		parent->child  = d;
		if (inode) inode->nlink++;
	}

	if (inode) inode->cached = 1;
	dentry_count++;

	return d;
}

vfs_dentry_t *vfs_dentry_create(vfs_dentry_t *parent, const char *name, vfs_inode_t *inode)
{
	uint64_t f = spin_lock_irq(&vfs_lock);
	vfs_dentry_t *d = _dentry_create(parent, name, inode);
	spin_unlock_irq(&vfs_lock, f);
	return d;
}

/* ── path resolution ─────────────────────────────────────── */

vfs_dentry_t *vfs_dentry_resolve(const char *path)
{
	uint64_t f = spin_lock_irq(&vfs_lock);
	if (!path || path[0] != '/') {
		spin_unlock_irq(&vfs_lock, f);
		return NULL;
	}
	if (!root_dentry) {
		spin_unlock_irq(&vfs_lock, f);
		return NULL;
	}

	/* mutable work buffer so symlink splicing can rewrite the path */
	char work[512];
	int wi = 0;
	while (wi < 511 && path[wi]) { work[wi] = path[wi]; wi++; }
	work[wi] = '\0';

	vfs_dentry_t *d = root_dentry;
	const char   *p = work + 1;   /* skip leading '/' */
	int symlink_depth = 0;
	int d_is_volatile = 0;          /* d points to volatile_dentry (not in dentry tree) */
	vfs_inode_t  *prev_volatile = NULL; /* previous volatile inode to free after lookup */
	vfs_dentry_t  volatile_dentry;     /* stable for entire function; reused each volatile hop */

	while (*p) {
		/* skip consecutive slashes */
		while (*p == '/') p++;
		if (!*p) break;

		/* extract next component (null-terminated in tmp) */
		char tmp[256];
		int i = 0;
		while (i < 255 && p[i] && p[i] != '/') { tmp[i] = p[i]; i++; }
		tmp[i] = '\0';
		const char *end = p + i;

		size_t len = (size_t)i;
		if (len == 0) break;

		/* handle . and .. */
		if (len == 1 && tmp[0] == '.') { p = end; continue; }
		if (len == 2 && tmp[0] == '.' && tmp[1] == '.') {
			if (d_is_volatile) {
				/* step back to real parent */
				d = volatile_dentry.parent;
				d_is_volatile = 0;
				if (prev_volatile) { vfs_inode_free(prev_volatile); prev_volatile = NULL; }
			} else if (d->parent) {
				d = d->parent;
			}
			p = end; continue;
		}

		/* look in dentry children first (skip if parent marks children volatile) */
		vfs_dentry_t *child = NULL;
		if (!d_is_volatile && (!d->inode || !d->inode->no_cache)) {
			child = d->child;
			while (child) {
				if (strcmp(child->name, tmp) == 0) break;
				child = child->next;
			}
		}

		if (!child) {
			if (!d->inode || !d->inode->iops || !d->inode->iops->lookup) {
				spin_unlock_irq(&vfs_lock, f);
				if (prev_volatile) { vfs_inode_free(prev_volatile); prev_volatile = NULL; }
				return NULL;
			}
			vfs_inode_t *dir_inode = d->inode;
			int parent_was_volatile = d_is_volatile;
			spin_unlock_irq(&vfs_lock, f);
			/* dir_inode may equal prev_volatile — do NOT free until after lookup */
			vfs_inode_t *in = dir_inode->iops->lookup(dir_inode, tmp);
			/* Now free the parent volatile inode (lookup is done with it) */
			if (prev_volatile) { vfs_inode_free(prev_volatile); prev_volatile = NULL; }
			f = spin_lock_irq(&vfs_lock);
			if (!in) {
				spin_unlock_irq(&vfs_lock, f);
				return NULL;
			}
			/* Cache only if parent is real (in dentry tree) and caching is allowed */
			if (!parent_was_volatile && !dir_inode->no_cache && dentry_count < dentry_max_val) {
				/* Re-check: another task may have added this dentry while unlocked */
				child = d->child;
				while (child) {
					if (strcmp(child->name, tmp) == 0) break;
					child = child->next;
				}
				if (!child) {
					child = _dentry_create(d, tmp, in);
					if (!child) {
						vfs_inode_free(in);
						spin_unlock_irq(&vfs_lock, f);
						return NULL;
					}
				} else {
					vfs_inode_free(in);
				}
				d_is_volatile = 0;
			} else {
				/* volatile: reuse stable volatile_dentry (function-scoped) */
				/* in->cached: keep as-is; dynamic inodes are 0 via memset; static must stay 1 */
				volatile_dentry.inode   = in;
				volatile_dentry.parent  = d;
				volatile_dentry.child   = NULL;
				volatile_dentry.next    = NULL;
				volatile_dentry.name[0] = '\0';
				child        = &volatile_dentry;
				prev_volatile = in;
				d_is_volatile = 1;
			}
		} else {
			/* Found in dentry cache; free any prior volatile inode */
			if (prev_volatile) { vfs_inode_free(prev_volatile); prev_volatile = NULL; }
			d_is_volatile = 0;
		}

		/* follow symlinks */
		if (S_ISLNK(child->inode->mode)) {
			if (++symlink_depth > 8 ||
			    !child->inode->iops || !child->inode->iops->readlink) {
				spin_unlock_irq(&vfs_lock, f);
				return NULL;
			}
			char target[256];
			if (child->inode->iops->readlink(child->inode, target, sizeof(target)) != 0) {
				spin_unlock_irq(&vfs_lock, f);
				return NULL;
			}
			/* splice: work = target + remaining(end) */
			int tlen = strlen(target);
			int rlen = strlen(end);
			if (tlen + rlen + 2 >= (int)sizeof(work)) {
				spin_unlock_irq(&vfs_lock, f);
				return NULL;
			}
			memcpy(work, target, tlen);
			if (rlen > 0) {
				if (end[0] != '/') work[tlen++] = '/';
				memcpy(work + tlen, end, rlen + 1);
			} else {
				work[tlen] = '\0';
			}
			if (target[0] == '/') {
				d = root_dentry;
				p = work + 1;
				d_is_volatile = 0;
				/* Absolute symlink restart — free any volatile context */
				if (prev_volatile) { vfs_inode_free(prev_volatile); prev_volatile = NULL; }
			} else {
				p = work;
			}
			continue;
		}

		d = child;
		p = end;
	}

	/* prev_volatile (if set) is the FINAL component's inode — freed by vfs_close */
	spin_unlock_irq(&vfs_lock, f);
	return d;
}

/* ── filesystem type registry ────────────────────────────── */

typedef struct vfs_fstype {
	const char        *name;
	int               (*fill)(vfs_mount_t *mnt, void *data);
	struct vfs_fstype *next;
} vfs_fstype_t;

static vfs_fstype_t *fstype_list = NULL;

void vfs_register_fs(const char *name, int (*fill)(vfs_mount_t *, void *))
{
	vfs_fstype_t *ft = kmalloc(sizeof(*ft));
	if (!ft) return;
	ft->name = name;
	ft->fill = fill;
	uint64_t f = spin_lock_irq(&vfs_lock);
	ft->next    = fstype_list;
	fstype_list = ft;
	spin_unlock_irq(&vfs_lock, f);
}

/* ── mount list ──────────────────────────────────────────── */

static vfs_mount_t *mount_list = NULL;

int vfs_mount(const char *path, const char *fstype, void *data)
{
	uint64_t f = spin_lock_irq(&vfs_lock);

	int (*fill)(vfs_mount_t *, void *) = NULL;
	for (vfs_fstype_t *ft = fstype_list; ft; ft = ft->next) {
		if (strcmp(ft->name, fstype) == 0) {
			fill = ft->fill;
			break;
		}
	}
	spin_unlock_irq(&vfs_lock, f);
	if (!fill) return -1;

	vfs_mount_t *mnt = kmalloc(sizeof(*mnt));
	if (!mnt) return -1;
	mnt->fstype     = fstype;
	mnt->priv       = data;
	mnt->mountpoint = NULL;
	mnt->root       = NULL;
	mnt->next       = NULL;

	if (fill(mnt, data) != 0) { kfree(mnt); return -1; }

	f = spin_lock_irq(&vfs_lock);
	if (strcmp(path, "/") == 0) {
		root_dentry = mnt->root;
	} else {
		spin_unlock_irq(&vfs_lock, f);
		vfs_dentry_t *mp = vfs_dentry_resolve(path);
		f = spin_lock_irq(&vfs_lock);
		if (!mp) {
			spin_unlock_irq(&vfs_lock, f);
			kfree(mnt);
			return -1;
		}
		mnt->mountpoint  = mp;
		mnt->orig_inode  = mp->inode;
		mp->inode = mnt->root->inode;
	}
	mnt->next  = mount_list;
	mount_list = mnt;
	spin_unlock_irq(&vfs_lock, f);
	return 0;
}

int vfs_umount(const char *path)
{
	if (!path || path[0] != '/') return -22;

	vfs_dentry_t *mp = vfs_dentry_resolve(path);
	if (!mp || !mp->inode) return -2;

	uint64_t f = spin_lock_irq(&vfs_lock);
	vfs_mount_t *prev = NULL;
	vfs_mount_t *mnt  = mount_list;
	while (mnt) {
		if (mnt->mountpoint == mp) break;
		prev = mnt;
		mnt  = mnt->next;
	}
	if (!mnt) { spin_unlock_irq(&vfs_lock, f); return -22; }

	if (prev) prev->next = mnt->next;
	else      mount_list = mnt->next;

	mp->inode = mnt->orig_inode;
	/* flush stale children cached from the now-unmounted fs */
	vfs_dentry_t *ch = mp->child;
	mp->child = NULL;
	spin_unlock_irq(&vfs_lock, f);
	while (ch) {
		vfs_dentry_t *nx = ch->next;
		kfree(ch);
		ch = nx;
	}
	kfree(mnt);
	return 0;
}

/* ── fd_table ────────────────────────────────────────────── */

#define BM_SET(bm, fd)  ((bm)[(fd)/64] |=  (1ULL << ((fd)%64)))
#define BM_CLR(bm, fd)  ((bm)[(fd)/64] &= ~(1ULL << ((fd)%64)))
#define BM_TST(bm, fd)  (!!((bm)[(fd)/64] & (1ULL << ((fd)%64))))

static int fd_table_grow(fd_table_t *t)
{
	int new_cap = t->cap * 2;
	int words   = new_cap / 64;
	vfs_file_t **nf = kmalloc((size_t)new_cap * sizeof(vfs_file_t *));
	uint64_t   *no  = kmalloc((size_t)words   * sizeof(uint64_t));
	uint64_t   *nc  = kmalloc((size_t)words   * sizeof(uint64_t));
	if (!nf || !no || !nc) { kfree(nf); kfree(no); kfree(nc); return -1; }
	memcpy(nf, t->files,   (size_t)t->cap  * sizeof(vfs_file_t *));
	memset(nf + t->cap, 0, (size_t)t->cap  * sizeof(vfs_file_t *));
	memcpy(no, t->open_bm, (size_t)(t->cap/64) * sizeof(uint64_t));
	memset(no + t->cap/64, 0, (size_t)(t->cap/64) * sizeof(uint64_t));
	memcpy(nc, t->cloexec, (size_t)(t->cap/64) * sizeof(uint64_t));
	memset(nc + t->cap/64, 0, (size_t)(t->cap/64) * sizeof(uint64_t));
	kfree(t->files); kfree(t->open_bm); kfree(t->cloexec);
	t->files = nf; t->open_bm = no; t->cloexec = nc; t->cap = new_cap;
	return 0;
}

fd_table_t *fd_table_create(void)
{
	fd_table_t *t = kmalloc(sizeof(*t));
	if (!t) return NULL;
	t->cap     = FD_INIT_CAP;
	t->files   = kmalloc((size_t)FD_INIT_CAP * sizeof(vfs_file_t *));
	t->open_bm = kmalloc((size_t)(FD_INIT_CAP/64) * sizeof(uint64_t));
	t->cloexec = kmalloc((size_t)(FD_INIT_CAP/64) * sizeof(uint64_t));
	if (!t->files || !t->open_bm || !t->cloexec) {
		kfree(t->files); kfree(t->open_bm); kfree(t->cloexec); kfree(t);
		return NULL;
	}
	memset(t->files,   0, (size_t)FD_INIT_CAP * sizeof(vfs_file_t *));
	memset(t->open_bm, 0, (size_t)(FD_INIT_CAP/64) * sizeof(uint64_t));
	memset(t->cloexec, 0, (size_t)(FD_INIT_CAP/64) * sizeof(uint64_t));
	t->refcount = 1;
	return t;
}

fd_table_t *fd_table_get(fd_table_t *t)
{
	if (t) t->refcount++;
	return t;
}

void fd_table_put(fd_table_t *t)
{
	if (!t) return;
	if (--t->refcount <= 0) fd_table_destroy(t);
}

void fd_table_destroy(fd_table_t *t)
{
	if (!t) return;
	for (int i = 0; i < t->cap; i++) {
		if (t->files[i]) {
			vfs_file_t *f = t->files[i];
			if (--f->refcount == 0) {
				if (f->inode && f->inode->fops && f->inode->fops->close)
					f->inode->fops->close(f);
				if (f->inode && !f->inode->cached)
					vfs_inode_free(f->inode);
				kfree(f);
			}
		}
	}
	kfree(t->files); kfree(t->open_bm); kfree(t->cloexec); kfree(t);
}

fd_table_t *fd_table_copy(fd_table_t *old)
{
	if (!old) return NULL;
	fd_table_t *n = kmalloc(sizeof(*n));
	if (!n) return NULL;
	n->cap     = old->cap;
	n->files   = kmalloc((size_t)old->cap * sizeof(vfs_file_t *));
	n->open_bm = kmalloc((size_t)(old->cap/64) * sizeof(uint64_t));
	n->cloexec = kmalloc((size_t)(old->cap/64) * sizeof(uint64_t));
	if (!n->files || !n->open_bm || !n->cloexec) {
		kfree(n->files); kfree(n->open_bm); kfree(n->cloexec); kfree(n);
		return NULL;
	}
	n->refcount = 1;
	memset(n->files, 0, (size_t)old->cap * sizeof(vfs_file_t *));
	memcpy(n->open_bm, old->open_bm, (size_t)(old->cap/64) * sizeof(uint64_t));
	memcpy(n->cloexec, old->cloexec, (size_t)(old->cap/64) * sizeof(uint64_t));
	for (int i = 0; i < old->cap; i++) {
		if (old->files[i]) {
			n->files[i] = old->files[i];
			n->files[i]->refcount++;
		}
	}
	return n;
}

int fd_alloc(fd_table_t *t, vfs_file_t *f)
{
	for (;;) {
		for (int i = 0; i < t->cap; i++) {
			if (!BM_TST(t->open_bm, i)) {
				t->files[i] = f;
				BM_SET(t->open_bm, i);
				f->refcount++;
				return i;
			}
		}
		if (fd_table_grow(t) != 0) return -1;
	}
}

int fd_alloc_from(fd_table_t *t, vfs_file_t *f, int start)
{
	for (;;) {
		for (int i = start; i < t->cap; i++) {
			if (!BM_TST(t->open_bm, i)) {
				t->files[i] = f;
				BM_SET(t->open_bm, i);
				f->refcount++;
				return i;
			}
		}
		if (fd_table_grow(t) != 0) return -1;
	}
}

int fd_alloc_at(fd_table_t *t, vfs_file_t *f, int fd)
{
	while (fd >= t->cap)
		if (fd_table_grow(t) != 0) return -1;
	t->files[fd] = f;
	BM_SET(t->open_bm, fd);
	f->refcount++;
	return fd;
}

void fd_free(fd_table_t *t, int fd)
{
	if (!t || fd < 0 || fd >= t->cap) return;
	t->files[fd] = NULL;
	BM_CLR(t->open_bm, fd);
	BM_CLR(t->cloexec, fd);
}

vfs_file_t *fd_get(fd_table_t *t, int fd)
{
	if (!t || fd < 0 || fd >= t->cap) return NULL;
	if (!BM_TST(t->open_bm, fd)) return NULL;
	return t->files[fd];
}

/* ── file ops ────────────────────────────────────────────── */

int vfs_open(const char *path, int flags, mode_t mode)
{
	process_t *p = task_current();
	if (!p->fds) return -1;

	uid_t uid = p ? p->euid : 0;
	gid_t gid = p ? p->egid : 0;

	vfs_dentry_t *d = vfs_dentry_resolve(path);
	vfs_inode_t  *inode = NULL;

	if (!d) {
		if (!(flags & O_CREAT)) return -2; /* ENOENT */

		/* find parent directory */
		const char *slash = path + strlen(path) - 1;
		while (slash > path && *slash != '/') slash--;

		char dir_path[256];
		int dlen = (int)(slash - path);
		if (dlen == 0) {
			dir_path[0] = '/';
			dir_path[1] = '\0';
		} else {
			memcpy(dir_path, path, dlen);
			dir_path[dlen] = '\0';
		}

		vfs_dentry_t *parent = vfs_dentry_resolve(dir_path);
		if (!parent || !parent->inode || !parent->inode->iops) return -2;

		int pr = vfs_check_perm(parent->inode, MAY_WRITE, uid, gid);
		if (pr) return pr;

		const char *fname = slash + 1;
		mode_t cmode = mode & ~(p ? p->umask : 022);
		if (parent->inode->iops->create(parent->inode, fname, cmode) != 0)
			return -1;

		inode = parent->inode->iops->lookup(parent->inode, fname);
		if (!inode) return -1;
		d = vfs_dentry_create(parent, fname, inode);
		if (vfs_inotify_hook) vfs_inotify_hook(dir_path, 0x00000100 /*IN_CREATE*/, fname);
	} else {
		inode = d->inode;
		/* check read/write permission on existing file */
		int accmode = 0;
		int acc = flags & 3; /* O_ACCMODE */
		if (acc == 0 || acc == 2) accmode |= MAY_READ;
		if (acc == 1 || acc == 2) accmode |= MAY_WRITE;
		int pr = vfs_check_perm(inode, accmode, uid, gid);
		if (pr) return pr;
	}

	if (!inode) return -1;
	if (!inode->fops && !S_ISDIR(inode->mode)) return -1;

	vfs_file_t *f = kmalloc(sizeof(*f));
	if (!f) return -1;
	memset(f, 0, sizeof(*f));
	f->inode    = inode;
	f->flags    = flags & ~O_CLOEXEC; /* cloexec is per-fd, stored in fd_table */
	f->refcount = 0; /* incremented by fd_alloc */
	{ size_t pl = strlen(path); if (pl < sizeof(f->path)) memcpy(f->path, path, pl + 1); }

	if (inode->fops && inode->fops->open && inode->fops->open(inode, f, flags) != 0) {
		kfree(f);
		return -1;
	}

	int fd = fd_alloc(p->fds, f);
	if (fd < 0) { kfree(f); return -1; }
	if (flags & O_CLOEXEC) BM_SET(p->fds->cloexec, fd);
	return fd;
}

int vfs_close(int fd)
{
	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return -1;

	fd_free(p->fds, fd);
	if (--f->refcount == 0) {
		if (f->inode && f->inode->fops && f->inode->fops->close)
			f->inode->fops->close(f);
		if (f->inode && !f->inode->cached)
			vfs_inode_free(f->inode);
		kfree(f);
	}
	return 0;
}

ssize_t vfs_read(int fd, void *buf, size_t len)
{
	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return -9; /* EBADF */
	if (!f->inode->fops || !f->inode->fops->read) return -22; /* EINVAL */
	return f->inode->fops->read(f, buf, len);
}

ssize_t vfs_write(int fd, const void *buf, size_t len)
{
	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return -9; /* EBADF */
	if (!f->inode->fops || !f->inode->fops->write) return -22; /* EINVAL */
	return f->inode->fops->write(f, buf, len);
}

off_t vfs_seek(int fd, off_t off, int whence)
{
	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return -9; /* EBADF */

	/* pipes and character devices are not seekable */
	if (f->inode && (f->inode->mode & 0170000) == 0010000) return -29; /* ESPIPE: S_IFIFO */
	if (f->inode && (f->inode->mode & 0170000) == 0020000) return -29; /* ESPIPE: S_IFCHR */

	if (f->inode->fops && f->inode->fops->seek)
		return f->inode->fops->seek(f, off, whence);

	off_t newpos;
	switch (whence) {
	case SEEK_SET: newpos = off; break;
	case SEEK_CUR: newpos = f->pos + off; break;
	case SEEK_END: newpos = (off_t)f->inode->size + off; break;
	default:       return -1;
	}
	if (newpos < 0) return -1;
	f->pos = newpos;
	return newpos;
}

int vfs_mkdir(const char *path, mode_t mode)
{
	if (vfs_dentry_resolve(path)) return -17; /* EEXIST */

	const char *slash = path + strlen(path) - 1;
	while (slash > path && *slash != '/') slash--;

	char dir_path[256];
	int dlen = (int)(slash - path);
	if (dlen == 0) { dir_path[0] = '/'; dir_path[1] = '\0'; }
	else           { memcpy(dir_path, path, dlen); dir_path[dlen] = '\0'; }

	vfs_dentry_t *parent = vfs_dentry_resolve(dir_path);
	if (!parent || !parent->inode || !parent->inode->iops) return -1;

	process_t *p = task_current();
	int pr = vfs_check_perm(parent->inode, MAY_WRITE, p ? p->euid : 0, p ? p->egid : 0);
	if (pr) return pr;

	const char *dname = slash + 1;
	mode_t cmode = mode & ~(p ? p->umask : 022);
	if (parent->inode->iops->mkdir(parent->inode, dname, cmode) != 0) return -1;

	vfs_inode_t *in = parent->inode->iops->lookup(parent->inode, dname);
	if (in) vfs_dentry_create(parent, dname, in);
	return 0;
}

int vfs_unlink(const char *path)
{
	const char *slash = path + strlen(path) - 1;
	while (slash > path && *slash != '/') slash--;

	char dir_path[256];
	int dlen = (int)(slash - path);
	if (dlen == 0) { dir_path[0] = '/'; dir_path[1] = '\0'; }
	else           { memcpy(dir_path, path, dlen); dir_path[dlen] = '\0'; }

	vfs_dentry_t *parent = vfs_dentry_resolve(dir_path);
	if (!parent || !parent->inode || !parent->inode->iops || !parent->inode->iops->unlink)
		return -1;

	process_t *p = task_current();
	uid_t uid = p ? p->euid : 0;
	gid_t gid = p ? p->egid : 0;

	int pr = vfs_check_perm(parent->inode, MAY_WRITE, uid, gid);
	if (pr) return pr;

	/* sticky bit: only file owner or dir owner can unlink */
	if (parent->inode->mode & 01000) { /* S_ISVTX */
		const char *fname2 = slash + 1;
		vfs_dentry_t *target = NULL;
		vfs_dentry_t *ch = parent->child;
		while (ch) { if (strcmp(ch->name, fname2) == 0) { target = ch; break; } ch = ch->next; }
		if (target && target->inode && target->inode->uid != uid && parent->inode->uid != uid)
			return -13; /* EACCES */
	}

	const char *fname = slash + 1;
	int r = parent->inode->iops->unlink(parent->inode, fname);
	if (r == 0 && vfs_inotify_hook) vfs_inotify_hook(dir_path, 0x00000200 /*IN_DELETE*/, fname);
	if (r == 0) {
		uint64_t lf = spin_lock_irq(&vfs_lock);
		vfs_dentry_t **dp = &parent->child;
		while (*dp) {
			if (strcmp((*dp)->name, fname) == 0) {
				vfs_dentry_t *dead = *dp;
				*dp = dead->next;
				kfree(dead);
				break;
			}
			dp = &(*dp)->next;
		}
		spin_unlock_irq(&vfs_lock, lf);
	}
	return r;
}

/* write a file from kernel data without going through the fd table */
int vfs_kernel_install(const char *path, const void *data, size_t len)
{
	const char *slash = path + strlen(path) - 1;
	while (slash > path && *slash != '/') slash--;

	char dir_path[256];
	int dlen = (int)(slash - path);
	if (dlen == 0) { dir_path[0] = '/'; dir_path[1] = '\0'; }
	else           { memcpy(dir_path, path, dlen); dir_path[dlen] = '\0'; }

	vfs_dentry_t *parent = vfs_dentry_resolve(dir_path);
	if (!parent || !parent->inode || !parent->inode->iops) return -1;

	const char *fname = slash + 1;
	if (parent->inode->iops->create(parent->inode, fname, 0755) != 0) return -1;

	vfs_inode_t *inode = parent->inode->iops->lookup(parent->inode, fname);
	if (!inode) return -1;
	vfs_dentry_create(parent, fname, inode);

	if (!inode->fops || !inode->fops->write) return -1;

	vfs_file_t f = {0};
	f.inode = inode;
	f.pos   = 0;
	f.flags = O_WRONLY;

	return inode->fops->write(&f, data, len) == (ssize_t)len ? 0 : -1;
}

int vfs_symlink(const char *target, const char *linkpath)
{
	const char *slash = linkpath + strlen(linkpath) - 1;
	while (slash > linkpath && *slash != '/') slash--;

	char dir_path[256];
	int dlen = (int)(slash - linkpath);
	if (dlen == 0) { dir_path[0] = '/'; dir_path[1] = '\0'; }
	else           { memcpy(dir_path, linkpath, dlen); dir_path[dlen] = '\0'; }

	vfs_dentry_t *parent = vfs_dentry_resolve(dir_path);
	if (!parent || !parent->inode || !parent->inode->iops) return -1;
	if (!parent->inode->iops->symlink) return -38;

	const char *fname = slash + 1;
	if (parent->inode->iops->symlink(parent->inode, fname, target) != 0) return -1;

	vfs_inode_t *in = parent->inode->iops->lookup(parent->inode, fname);
	if (in) vfs_dentry_create(parent, fname, in);
	return 0;
}

int vfs_stat(const char *path, vfs_stat_t *out)
{
	vfs_dentry_t *d = vfs_dentry_resolve(path);
	if (!d || !d->inode) return -1;
	if (d->inode->iops && d->inode->iops->stat)
		return d->inode->iops->stat(d->inode, out);
	out->ino   = d->inode->ino;
	out->mode  = d->inode->mode;
	out->nlink = d->inode->nlink;
	out->uid   = d->inode->uid;
	out->gid   = d->inode->gid;
	out->size  = d->inode->size;
	out->rdev  = d->inode->rdev;
	return 0;
}

int vfs_rename(const char *old_path, const char *new_path)
{
	/* parse old path */
	const char *s = old_path + strlen(old_path) - 1;
	while (s > old_path && *s != '/') s--;
	char old_dir[256];
	int dlen = (int)(s - old_path);
	if (dlen == 0) { old_dir[0] = '/'; old_dir[1] = '\0'; }
	else           { memcpy(old_dir, old_path, dlen); old_dir[dlen] = '\0'; }
	const char *old_name = s + 1;

	/* parse new path */
	const char *s2 = new_path + strlen(new_path) - 1;
	while (s2 > new_path && *s2 != '/') s2--;
	char new_dir[256];
	int dlen2 = (int)(s2 - new_path);
	if (dlen2 == 0) { new_dir[0] = '/'; new_dir[1] = '\0'; }
	else            { memcpy(new_dir, new_path, dlen2); new_dir[dlen2] = '\0'; }
	const char *new_name = s2 + 1;

	vfs_dentry_t *old_parent = vfs_dentry_resolve(old_dir);
	if (!old_parent || !old_parent->inode) return -2;
	vfs_dentry_t *new_parent = vfs_dentry_resolve(new_dir);
	if (!new_parent || !new_parent->inode) return -2;

	if (!old_parent->inode->iops || !old_parent->inode->iops->rename)
		return -38;

	int ret = old_parent->inode->iops->rename(
		old_parent->inode, old_name, new_parent->inode, new_name);
	if (ret == 0 && vfs_inotify_hook) {
		vfs_inotify_hook(old_dir, 0x00000040 /*IN_MOVED_FROM*/, old_name);
		vfs_inotify_hook(new_dir, 0x00000080 /*IN_MOVED_TO*/,   new_name);
	}
	if (ret == 0) {
		/* update dentry name in the tree */
		uint64_t f = spin_lock_irq(&vfs_lock);
		vfs_dentry_t *d = old_parent->child;
		while (d) {
			if (strcmp(d->name, old_name) == 0) {
				int nlen = 0;
				while (new_name[nlen] && nlen < 254) nlen++;
				memcpy(d->name, new_name, nlen);
				d->name[nlen] = '\0';
				break;
			}
			d = d->next;
		}
		spin_unlock_irq(&vfs_lock, f);
	}
	return ret;
}

int vfs_readdir(const char *path, uint32_t idx, vfs_dirent_t *out)
{
	vfs_dentry_t *d = vfs_dentry_resolve(path);
	if (!d || !d->inode) return -1;
	if (d->inode->iops && d->inode->iops->readdir)
		return d->inode->iops->readdir(d->inode, idx, out);
	return -1;
}

int vfs_chmod(const char *path, mode_t mode)
{
	vfs_dentry_t *d = vfs_dentry_resolve(path);
	if (!d || !d->inode) return -2;
	d->inode->mode = (d->inode->mode & 0xF000) | (mode & 07777);
	return 0;
}

int vfs_link(const char *oldpath, const char *newpath)
{
	vfs_dentry_t *od = vfs_dentry_resolve(oldpath);
	if (!od || !od->inode) return -2;
	if (S_ISDIR(od->inode->mode)) return -21;

	const char *s = newpath + strlen(newpath) - 1;
	while (s > newpath && *s != '/') s--;
	char new_dir[256];
	int dlen = (int)(s - newpath);
	if (dlen == 0) { new_dir[0] = '/'; new_dir[1] = '\0'; }
	else           { memcpy(new_dir, newpath, dlen); new_dir[dlen] = '\0'; }
	const char *new_name = s + 1;

	vfs_dentry_t *nd = vfs_dentry_resolve(new_dir);
	if (!nd || !nd->inode) return -2;
	if (!nd->inode->iops || !nd->inode->iops->link) return -1;

	return nd->inode->iops->link(nd->inode, new_name, od->inode);
}

/* ── init ────────────────────────────────────────────────── */

extern void ramfs_register(void);
extern void procfs_register(void);
extern void devfs_register(void);
extern void ext2_register(void);

void vfs_init(void)
{
	{
		char buf[16];
		if (cmdline_get("vfs_dentry_max", buf, sizeof(buf))) {
			int v = 0;
			for (const char *s = buf; *s >= '0' && *s <= '9'; s++)
				v = v * 10 + (*s - '0');
			if (v > 0) dentry_max_val = v;
		}
	}
	ramfs_register();
	procfs_register();
	devfs_register();
	ext2_register();
	kprintf("VFS: initialized\n");
}
