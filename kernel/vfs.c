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

	/* mutable work buffer so symlink splicing can rewrite the path */
	char work[512];
	int wi = 0;
	while (wi < 511 && path[wi]) { work[wi] = path[wi]; wi++; }
	work[wi] = '\0';

	vfs_dentry_t *d = root_dentry;
	const char   *p = work + 1;   /* skip leading '/' */
	int symlink_depth = 0;

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
			if (d->parent) d = d->parent;
			p = end; continue;
		}

		/* look in dentry children first */
		vfs_dentry_t *child = d->child;
		while (child) {
			if (strcmp(child->name, tmp) == 0) break;
			child = child->next;
		}

		if (!child) {
			if (!d->inode || !d->inode->iops || !d->inode->iops->lookup) {
				spin_unlock_irq(&vfs_lock, f);
				return NULL;
			}
			/* Release lock during I/O so timer/scheduler can run */
			vfs_inode_t *dir_inode = d->inode;
			spin_unlock_irq(&vfs_lock, f);
			vfs_inode_t *in = dir_inode->iops->lookup(dir_inode, tmp);
			f = spin_lock_irq(&vfs_lock);
			if (!in) {
				spin_unlock_irq(&vfs_lock, f);
				return NULL;
			}
			/* Re-check: another task may have added this dentry while unlocked */
			child = d->child;
			while (child) {
				if (strcmp(child->name, tmp) == 0) break;
				child = child->next;
			}
			if (!child) {
				child = _dentry_create(d, tmp, in);
				if (!child) {
					spin_unlock_irq(&vfs_lock, f);
					return NULL;
				}
			}
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
			} else {
				p = work; /* resolve relative target from current parent d */
			}
			continue;
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

static vfs_fstype_t fstypes[16];
static int          fstype_cnt = 0;

void vfs_register_fs(const char *name, int (*fill)(vfs_mount_t *, void *))
{
	uint64_t f = spin_lock_irq(&vfs_lock);
	if (fstype_cnt >= 16) {
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
	for (int i = 3; i < FD_MAX; i++) {
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

	if (!inode) return -1;
	if (!inode->fops && !S_ISDIR(inode->mode)) return -1;

	vfs_file_t *f = buddy_alloc(0);
	if (!f) return -1;
	memset(f, 0, sizeof(*f));
	f->inode    = inode;
	f->flags    = flags;
	f->refcount = 0; /* incremented by fd_alloc */

	if (inode->fops && inode->fops->open && inode->fops->open(inode, f, flags) != 0) {
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
		if (f->inode->fops && f->inode->fops->close)
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

/* ── init ────────────────────────────────────────────────── */

extern void ramfs_register(void);
extern void procfs_register(void);
extern void devfs_register(void);
extern void ext2_register(void);

void vfs_init(void)
{
	ramfs_register();
	procfs_register();
	devfs_register();
	ext2_register();
	kprintf("[  vfs] initialized\n");
}
