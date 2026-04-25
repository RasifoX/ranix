#include <kernel/vfs.h>
#include <kernel/proc.h>
#include <kernel/mm/buddy.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>

/* ── mount table ─────────────────────────────────────────── */
#define MOUNT_MAX 16

static vfs_mount_t  mounts[MOUNT_MAX];
static int          mount_cnt = 0;
static vfs_dentry_t *root_dentry = NULL;

static spinlock_t vfs_lock = SPINLOCK_INIT;

/* ── dentry pool ─────────────────────────────────────────── */

static vfs_dentry_t *_dentry_create(vfs_dentry_t *parent, const char *name, vfs_inode_t *inode)
{
	vfs_dentry_t *d = buddy_alloc(0);
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

	vfs_dentry_t *d = root_dentry;
	const char   *p = path + 1;

	while (*p) {
		/* skip consecutive slashes */
		while (*p == '/') p++;
		if (!*p) break;

		/* extract next component */
		const char *end = p;
		while (*end && *end != '/') end++;

		size_t len = (size_t)(end - p);
		if (len == 0) break;

		/* look in dentry children first */
		vfs_dentry_t *child = d->child;
		while (child) {
			if (strncmp(child->name, p, len) == 0 && child->name[len] == '\0')
				break;
			child = child->next;
		}

		if (!child) {
			/* ask the inode */
			if (!d->inode || !d->inode->iops || !d->inode->iops->lookup) {
				spin_unlock_irq(&vfs_lock, f);
				return NULL;
			}
			vfs_inode_t *in = d->inode->iops->lookup(d->inode, p);
			if (!in) {
				spin_unlock_irq(&vfs_lock, f);
				return NULL;
			}
			/* cache the result */
			char tmp[256];
			int i = 0;
			while (i < 255 && p[i] && p[i] != '/') { tmp[i] = p[i]; i++; }
			tmp[i] = '\0';
			child = _dentry_create(d, tmp, in);
			if (!child) {
				spin_unlock_irq(&vfs_lock, f);
				return NULL;
			}
		}

		d = child;
		p = end;
	}

	spin_unlock_irq(&vfs_lock, f);
	return d;
}

/* ── mount ───────────────────────────────────────────────── */

typedef struct {
	const char *name;
	int (*fill)(vfs_mount_t *mnt, void *data);
} vfs_fstype_t;

static vfs_fstype_t fstypes[8];
static int          fstype_cnt = 0;

void vfs_register_fs(const char *name, int (*fill)(vfs_mount_t *, void *))
{
	uint64_t f = spin_lock_irq(&vfs_lock);
	if (fstype_cnt >= 8) {
		spin_unlock_irq(&vfs_lock, f);
		return;
	}
	fstypes[fstype_cnt].name = name;
	fstypes[fstype_cnt].fill = fill;
	fstype_cnt++;
	spin_unlock_irq(&vfs_lock, f);
}

int vfs_mount(const char *path, const char *fstype, void *data)
{
	uint64_t f = spin_lock_irq(&vfs_lock);
	if (mount_cnt >= MOUNT_MAX) {
		spin_unlock_irq(&vfs_lock, f);
		return -1;
	}

	int (*fill)(vfs_mount_t *, void *) = NULL;
	for (int i = 0; i < fstype_cnt; i++) {
		if (strcmp(fstypes[i].name, fstype) == 0) {
			fill = fstypes[i].fill;
			break;
		}
	}
	if (!fill) {
		spin_unlock_irq(&vfs_lock, f);
		return -1;
	}

	vfs_mount_t *mnt = &mounts[mount_cnt];
	mnt->fstype = fstype;
	mnt->priv   = data;

	/* Release lock during fill() as it might call other VFS functions */
	spin_unlock_irq(&vfs_lock, f);
	if (fill(mnt, data) != 0) return -1;
	f = spin_lock_irq(&vfs_lock);

	if (strcmp(path, "/") == 0) {
		root_dentry = mnt->root;
	} else {
		/* unlock to resolve path */
		spin_unlock_irq(&vfs_lock, f);
		vfs_dentry_t *mp = vfs_dentry_resolve(path);
		f = spin_lock_irq(&vfs_lock);
		if (!mp) {
			spin_unlock_irq(&vfs_lock, f);
			return -1;
		}
		mnt->mountpoint = mp;
		/* replace mountpoint's inode with fs root inode */
		mp->inode = mnt->root->inode;
	}

	mount_cnt++;
	spin_unlock_irq(&vfs_lock, f);
	return 0;
}

/* ── fd_table ────────────────────────────────────────────── */

fd_table_t *fd_table_create(void)
{
	fd_table_t *t = buddy_alloc(0);
	if (!t) return NULL;
	memset(t, 0, sizeof(*t));
	return t;
}

void fd_table_destroy(fd_table_t *t)
{
	if (!t) return;
	for (int i = 0; i < FD_MAX; i++) {
		if (t->files[i]) {
			vfs_file_t *f = t->files[i];
			if (--f->refcount == 0) {
				if (f->inode->fops->close) f->inode->fops->close(f);
				buddy_free((uint64_t)f, 0);
			}
		}
	}
	buddy_free((uint64_t)t, 0);
}

fd_table_t *fd_table_copy(fd_table_t *old)
{
	if (!old) return NULL;
	fd_table_t *new = fd_table_create();
	if (!new) return NULL;
	for (int i = 0; i < FD_MAX; i++) {
		if (old->files[i]) {
			new->files[i] = old->files[i];
			new->files[i]->refcount++;
			new->bitmap |= (1ULL << i);
		}
	}
	return new;
}

int fd_alloc(fd_table_t *t, vfs_file_t *f)
{
	for (int i = 0; i < FD_MAX; i++) {
		if (!(t->bitmap & (1ULL << i))) {
			t->files[i]  = f;
			t->bitmap   |= (1ULL << i);
			f->refcount++;
			return i;
		}
	}
	return -1;
}

void fd_free(fd_table_t *t, int fd)
{
	if (fd < 0 || fd >= FD_MAX) return;
	t->files[fd]  = NULL;
	t->bitmap    &= ~(1ULL << fd);
}

vfs_file_t *fd_get(fd_table_t *t, int fd)
{
	if (!t || fd < 0 || fd >= FD_MAX) return NULL;
	if (!(t->bitmap & (1ULL << fd))) return NULL;
	return t->files[fd];
}

/* ── file ops ────────────────────────────────────────────── */

int vfs_open(const char *path, int flags, mode_t mode)
{
	process_t *p = task_current();
	if (!p->fds) return -1;

	vfs_dentry_t *d = vfs_dentry_resolve(path);
	vfs_inode_t  *inode = NULL;

	if (!d) {
		if (!(flags & O_CREAT)) return -1;

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
		if (!parent || !parent->inode || !parent->inode->iops) return -1;

		const char *fname = slash + 1;
		if (parent->inode->iops->create(parent->inode, fname, mode) != 0)
			return -1;

		inode = parent->inode->iops->lookup(parent->inode, fname);
		if (!inode) return -1;
		d = vfs_dentry_create(parent, fname, inode);
	} else {
		inode = d->inode;
	}

	if (!inode || !inode->fops) return -1;

	vfs_file_t *f = buddy_alloc(0);
	if (!f) return -1;
	memset(f, 0, sizeof(*f));
	f->inode    = inode;
	f->flags    = flags;
	f->refcount = 0; /* incremented by fd_alloc */

	if (inode->fops->open && inode->fops->open(inode, f, flags) != 0) {
		buddy_free((uint64_t)f, 0);
		return -1;
	}

	int fd = fd_alloc(p->fds, f);
	if (fd < 0) buddy_free((uint64_t)f, 0);
	return fd;
}

int vfs_close(int fd)
{
	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return -1;

	fd_free(p->fds, fd);
	if (--f->refcount == 0) {
		if (f->inode->fops->close)
			f->inode->fops->close(f);
		buddy_free((uint64_t)f, 0);
	}
	return 0;
}

ssize_t vfs_read(int fd, void *buf, size_t len)
{
	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f || !f->inode->fops->read) return -1;
	return f->inode->fops->read(f, buf, len);
}

ssize_t vfs_write(int fd, const void *buf, size_t len)
{
	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f || !f->inode->fops->write) return -1;
	return f->inode->fops->write(f, buf, len);
}

off_t vfs_seek(int fd, off_t off, int whence)
{
	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return -1;

	if (f->inode->fops->seek)
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
	const char *slash = path + strlen(path) - 1;
	while (slash > path && *slash != '/') slash--;

	char dir_path[256];
	int dlen = (int)(slash - path);
	if (dlen == 0) { dir_path[0] = '/'; dir_path[1] = '\0'; }
	else           { memcpy(dir_path, path, dlen); dir_path[dlen] = '\0'; }

	vfs_dentry_t *parent = vfs_dentry_resolve(dir_path);
	if (!parent || !parent->inode || !parent->inode->iops) return -1;

	const char *dname = slash + 1;
	if (parent->inode->iops->mkdir(parent->inode, dname, mode) != 0) return -1;

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

	const char *fname = slash + 1;
	return parent->inode->iops->unlink(parent->inode, fname);
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

int vfs_readdir(const char *path, uint32_t idx, vfs_dirent_t *out)
{
	vfs_dentry_t *d = vfs_dentry_resolve(path);
	if (!d || !d->inode) return -1;
	if (d->inode->iops && d->inode->iops->readdir)
		return d->inode->iops->readdir(d->inode, idx, out);
	return -1;
}

/* ── init ────────────────────────────────────────────────── */

extern void ramfs_register(void);

void vfs_init(void)
{
	ramfs_register();
	kprintf("[  vfs] initialized\n");
}
