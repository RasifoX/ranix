#include <kernel/vfs.h>
#include <kernel/proc.h>
#include <kernel/config.h>
#include <kernel/cmdline.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/nsproxy.h>

static vfs_dentry_t *root_dentry = NULL;
static vfs_dentry_t *follow_mounts(vfs_dentry_t *d);

vfs_inotify_fn_t vfs_inotify_hook = NULL;

static spinlock_t vfs_lock = SPINLOCK_INIT;
static int dentry_count = 0;
static int dentry_max_val = DENTRY_MAX;

/* ── extended attributes (generic per-inode key/value list) ── */

typedef struct vfs_xattr {
	struct vfs_xattr *next;
	char             *name;
	void             *value;
	size_t            len;
} vfs_xattr_t;

static spinlock_t xattr_lock = SPINLOCK_INIT;

void vfs_inode_free_xattrs(vfs_inode_t *in)
{
	if (!in) return;
	vfs_xattr_t *x = in->xattrs;
	in->xattrs = NULL;
	while (x) {
		vfs_xattr_t *n = x->next;
		kfree(x->name);
		if (x->value) kfree(x->value);
		kfree(x);
		x = n;
	}
}

int vfs_inode_setxattr(vfs_inode_t *in, const char *name, const void *val, size_t len, int flags)
{
	if (!in || !name || !name[0]) return -22;
	uint64_t f = spin_lock_irq(&xattr_lock);
	vfs_xattr_t *x = in->xattrs, *prev = NULL;
	while (x) { if (strcmp(x->name, name) == 0) break; prev = x; x = x->next; }
	(void)prev;
	if (x) {
		if (flags & XATTR_CREATE) { spin_unlock_irq(&xattr_lock, f); return -17; /* EEXIST */ }
		void *nv = len ? kmalloc(len) : NULL;
		if (len && !nv) { spin_unlock_irq(&xattr_lock, f); return -12; }
		if (len) memcpy(nv, val, len);
		if (x->value) kfree(x->value);
		x->value = nv; x->len = len;
		spin_unlock_irq(&xattr_lock, f);
		return 0;
	}
	if (flags & XATTR_REPLACE) { spin_unlock_irq(&xattr_lock, f); return -61; /* ENODATA */ }
	vfs_xattr_t *nx = kmalloc(sizeof(*nx));
	if (!nx) { spin_unlock_irq(&xattr_lock, f); return -12; }
	size_t nlen = strlen(name);
	nx->name = kmalloc(nlen + 1);
	nx->value = len ? kmalloc(len) : NULL;
	if (!nx->name || (len && !nx->value)) {
		if (nx->name) kfree(nx->name);
		if (nx->value) kfree(nx->value);
		kfree(nx);
		spin_unlock_irq(&xattr_lock, f);
		return -12;
	}
	memcpy(nx->name, name, nlen + 1);
	if (len) memcpy(nx->value, val, len);
	nx->len = len;
	nx->next = in->xattrs;
	in->xattrs = nx;
	spin_unlock_irq(&xattr_lock, f);
	return 0;
}

ssize_t vfs_inode_getxattr(vfs_inode_t *in, const char *name, void *val, size_t size)
{
	if (!in || !name) return -22;
	uint64_t f = spin_lock_irq(&xattr_lock);
	for (vfs_xattr_t *x = in->xattrs; x; x = x->next) {
		if (strcmp(x->name, name) != 0) continue;
		ssize_t r = (ssize_t)x->len;
		if (size == 0) { spin_unlock_irq(&xattr_lock, f); return r; }
		if (size < x->len) { spin_unlock_irq(&xattr_lock, f); return -34; /* ERANGE */ }
		if (x->len) memcpy(val, x->value, x->len);
		spin_unlock_irq(&xattr_lock, f);
		return r;
	}
	spin_unlock_irq(&xattr_lock, f);
	return -61; /* ENODATA */
}

ssize_t vfs_inode_listxattr(vfs_inode_t *in, char *list, size_t size)
{
	if (!in) return -22;
	uint64_t f = spin_lock_irq(&xattr_lock);
	size_t total = 0;
	for (vfs_xattr_t *x = in->xattrs; x; x = x->next) total += strlen(x->name) + 1;
	if (size == 0) { spin_unlock_irq(&xattr_lock, f); return (ssize_t)total; }
	if (size < total) { spin_unlock_irq(&xattr_lock, f); return -34; /* ERANGE */ }
	size_t off = 0;
	for (vfs_xattr_t *x = in->xattrs; x; x = x->next) {
		size_t l = strlen(x->name) + 1;
		memcpy(list + off, x->name, l);
		off += l;
	}
	spin_unlock_irq(&xattr_lock, f);
	return (ssize_t)total;
}

int vfs_inode_removexattr(vfs_inode_t *in, const char *name)
{
	if (!in || !name) return -22;
	uint64_t f = spin_lock_irq(&xattr_lock);
	vfs_xattr_t *x = in->xattrs, *prev = NULL;
	while (x) {
		if (strcmp(x->name, name) == 0) {
			if (prev) prev->next = x->next; else in->xattrs = x->next;
			kfree(x->name);
			if (x->value) kfree(x->value);
			kfree(x);
			spin_unlock_irq(&xattr_lock, f);
			return 0;
		}
		prev = x; x = x->next;
	}
	spin_unlock_irq(&xattr_lock, f);
	return -61; /* ENODATA */
}

static void vfs_inode_free(vfs_inode_t *in)
{
	if (!in || in->cached) return; /* cached=1: owned by dentry tree, pipe, or static */
	vfs_inode_free_xattrs(in);
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
		/* Caching a dentry is not a hard link: nlink is the directory-entry
		 * count, managed by the filesystem (create/link/unlink), not bumped on
		 * every path lookup. */
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

	/* chroot: an absolute path is relative to the process's own root, not the
	 * global one. NULL ->root means it sees "/" as the global root. */
	process_t *_cur = task_current();
	vfs_dentry_t *proc_root = (_cur && _cur->root) ? (vfs_dentry_t *)_cur->root : root_dentry;

	vfs_dentry_t *d = follow_mounts(proc_root);
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

		/* follow symlinks — except magic links that are also openable
		 * objects (nsfs-style, e.g. /proc/<pid>/ns/uts): those carry fops,
		 * so resolve to the link itself and let open() act on the object
		 * rather than following its text. */
		if (S_ISLNK(child->inode->mode) && !child->inode->fops) {
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
				d = proc_root;
				p = work + 1;
				d_is_volatile = 0;
				/* Absolute symlink restart — free any volatile context */
				if (prev_volatile) { vfs_inode_free(prev_volatile); prev_volatile = NULL; }
			} else {
				p = work;
			}
			continue;
		}

		d = follow_mounts(child);
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

const char *vfs_fstype_name(int idx)
{
	vfs_fstype_t *ft = fstype_list;
	for (int i = 0; ft && i < idx; i++) ft = ft->next;
	return ft ? ft->name : NULL;
}

/* ── mount namespaces ──────────────────────────────────────── */

mnt_namespace_t init_mnt_ns = { .refcount = 1, .inum = 0xF0000004UL, .mounts = NULL };
static spinlock_t mntns_lock = SPINLOCK_INIT;

mnt_namespace_t *current_mnt_ns(void)
{
	process_t *p = task_current();
	return (p && p->mnt_ns) ? p->mnt_ns : &init_mnt_ns;
}

mnt_namespace_t *mnt_ns_get(mnt_namespace_t *ns)
{
	if (!ns) ns = &init_mnt_ns;
	if (ns != &init_mnt_ns) {
		uint64_t f = spin_lock_irq(&mntns_lock);
		ns->refcount++;
		spin_unlock_irq(&mntns_lock, f);
	}
	return ns;
}

mnt_namespace_t *mnt_ns_clone(mnt_namespace_t *old)
{
	if (!old) old = &init_mnt_ns;
	mnt_namespace_t *ns = kmalloc(sizeof(*ns));
	if (!ns) return NULL;
	ns->refcount = 1;
	ns->inum     = ns_inum_alloc();
	ns->mounts   = NULL;
	/* copy the mount entries; the underlying filesystems stay shared */
	uint64_t f = spin_lock_irq(&vfs_lock);
	for (vfs_mount_t *m = old->mounts; m; m = m->next) {
		vfs_mount_t *nm = kmalloc(sizeof(*nm));
		if (!nm) continue;
		*nm = *m;
		nm->fstype = nm->fstype_name;   /* re-point to this copy's own buffer */
		if (nm->get_sb) nm->get_sb(nm->priv);   /* shared sb gains a reference */
		nm->next   = ns->mounts;
		ns->mounts = nm;
	}
	spin_unlock_irq(&vfs_lock, f);
	return ns;
}

void mnt_ns_put(mnt_namespace_t *ns)
{
	if (!ns || ns == &init_mnt_ns) return;
	uint64_t f = spin_lock_irq(&mntns_lock);
	int last = (--ns->refcount == 0);
	spin_unlock_irq(&mntns_lock, f);
	if (last) {
		for (vfs_mount_t *m = ns->mounts, *nx; m; m = nx) { nx = m->next; kfree(m); }
		kfree(ns);
	}
}

/* If d is a mountpoint in the current namespace, return the mount's root
 * (following stacked mounts). Caller holds vfs_lock. */
static vfs_dentry_t *follow_mounts(vfs_dentry_t *d)
{
	mnt_namespace_t *ns = current_mnt_ns();
	int moved = 1, guard = 0;
	while (moved && guard++ < 64) {
		moved = 0;
		for (vfs_mount_t *m = ns->mounts; m; m = m->next)
			if (m->mountpoint == d) { d = m->root; moved = 1; break; }
	}
	return d;
}

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
	/* Own the fstype string: callers may pass a stack buffer (userspace mount(2)
	 * via syscall) as well as static literals (kernel mounts). */
	int _fl = 0; while (fstype && fstype[_fl] && _fl < 31) { mnt->fstype_name[_fl] = fstype[_fl]; _fl++; }
	mnt->fstype_name[_fl] = '\0';
	mnt->fstype     = mnt->fstype_name;
	mnt->priv       = data;
	mnt->kill_sb    = NULL;
	mnt->get_sb     = NULL;
	mnt->mountpoint = NULL;
	mnt->root       = NULL;
	mnt->next       = NULL;

	if (fill(mnt, data) != 0) { kfree(mnt); return -1; }

	mnt_namespace_t *mns = current_mnt_ns();
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
		/* No inode splice: path resolution follows mounts per-namespace
		 * (follow_mounts). Point the mounted root's parent at the
		 * mountpoint's parent so ".." crosses back out of the mount. */
		if (mnt->root) mnt->root->parent = mp->parent;
	}
	mnt->next   = mns->mounts;
	mns->mounts = mnt;
	spin_unlock_irq(&vfs_lock, f);
	return 0;
}

/* Reconstruct a dentry's absolute path by walking parents (caller holds
 * vfs_lock). Root (no parent, or self-parent) yields "/". */
static void dentry_abspath(vfs_dentry_t *d, char *out, size_t sz)
{
	vfs_dentry_t *chain[64];
	int n = 0;
	for (vfs_dentry_t *c = d; c && n < 64; c = c->parent) {
		if (!c->parent || c->parent == c) break;   /* reached root */
		chain[n++] = c;
	}
	if (n == 0) { if (sz > 1) { out[0] = '/'; out[1] = '\0'; } else if (sz) out[0] = '\0'; return; }
	size_t p = 0;
	for (int i = n - 1; i >= 0 && p + 1 < sz; i--) {
		out[p++] = '/';
		for (const char *nm = chain[i]->name; *nm && p + 1 < sz; nm++) out[p++] = *nm;
	}
	out[p < sz ? p : sz - 1] = '\0';
}

/* /proc/mounts: one line per mount in the caller's namespace —
 * "source mountpoint fstype rw 0 0". Source isn't tracked, so the fstype name
 * stands in (as it does for virtual filesystems on Linux). */
size_t vfs_proc_mounts(char *buf, size_t sz)
{
	mnt_namespace_t *mns = current_mnt_ns();
	uint64_t f = spin_lock_irq(&vfs_lock);
	size_t pos = 0;
	for (vfs_mount_t *m = mns->mounts; m; m = m->next) {
		char path[256];
		if (!m->mountpoint) { path[0] = '/'; path[1] = '\0'; }
		else                  dentry_abspath(m->mountpoint, path, sizeof(path));
		const char *fs = m->fstype ? m->fstype : "none";
		const char *cols[] = { fs, " ", path, " ", fs, " rw 0 0\n" };
		for (int ci = 0; ci < 6; ci++)
			for (const char *s = cols[ci]; *s && pos + 1 < sz; s++) buf[pos++] = *s;
	}
	spin_unlock_irq(&vfs_lock, f);
	return pos;
}

/* /proc/PID/mountinfo: the richer per-mount format libmount/findmnt parse —
 * "id parent major:minor root mountpoint opts - fstype source super_opts". */
size_t vfs_proc_mountinfo(char *buf, size_t sz)
{
	mnt_namespace_t *mns = current_mnt_ns();
	uint64_t f = spin_lock_irq(&vfs_lock);
	size_t pos = 0;
	int id = 36;
	for (vfs_mount_t *m = mns->mounts; m; m = m->next, id++) {
		char path[256];
		if (!m->mountpoint) { path[0] = '/'; path[1] = '\0'; }
		else                  dentry_abspath(m->mountpoint, path, sizeof(path));
		const char *fs = m->fstype ? m->fstype : "none";
		char idbuf[12]; int n = 0; int v = id;
		char tmp[12]; int t = 0;
		do { tmp[t++] = '0' + v % 10; v /= 10; } while (v);
		while (t) idbuf[n++] = tmp[--t];
		idbuf[n] = '\0';
		/* "<id> 1 0:<id> / <path> rw - <fs> <fs> rw\n" */
		const char *cols[] = { idbuf, " 1 0:", idbuf, " / ", path, " rw - ", fs, " ", fs, " rw\n" };
		for (int ci = 0; ci < (int)(sizeof(cols) / sizeof(cols[0])); ci++)
			for (const char *s = cols[ci]; *s && pos + 1 < sz; s++) buf[pos++] = *s;
	}
	spin_unlock_irq(&vfs_lock, f);
	return pos;
}

/* fstype of the mount covering an absolute path — the mount whose mountpoint is
 * the longest prefix of path (statfs(2)). Defaults to the root fs. */
const char *vfs_path_fstype(const char *path)
{
	if (!path || path[0] != '/') return "ramfs";
	mnt_namespace_t *mns = current_mnt_ns();
	uint64_t f = spin_lock_irq(&vfs_lock);
	const char *best = "ramfs";
	size_t bestlen = 0;
	for (vfs_mount_t *m = mns->mounts; m; m = m->next) {
		char mp[256];
		if (!m->mountpoint) { mp[0] = '/'; mp[1] = '\0'; }
		else                  dentry_abspath(m->mountpoint, mp, sizeof(mp));
		size_t l = strlen(mp);
		if (l == 0) continue;
		if (strncmp(path, mp, l) != 0) continue;
		/* mp must end at a path boundary (or be the root "/") */
		if (l > 1 && path[l] != '/' && path[l] != '\0') continue;
		if (l >= bestlen) { bestlen = l; best = m->fstype ? m->fstype : "ramfs"; }
	}
	spin_unlock_irq(&vfs_lock, f);
	return best;
}

/* Per-filesystem statfs: filesystems that track real block/inode usage
 * (tmpfs, ext2/4) supply iops->statfs; returns <0 if the fs has no op, so the
 * caller falls back to nominal counts. */
int vfs_statfs_inode(vfs_inode_t *in, vfs_statfs_t *out)
{
	if (!in || !in->iops || !in->iops->statfs) return -38; /* ENOSYS */
	return in->iops->statfs(in, out);
}

int vfs_statfs_path(const char *path, vfs_statfs_t *out)
{
	vfs_dentry_t *d = vfs_dentry_resolve(path);
	if (!d || !d->inode) return -2;
	return vfs_statfs_inode(d->inode, out);
}

int vfs_umount(const char *path)
{
	if (!path || path[0] != '/') return -22;

	/* resolve follows the mount, so this is the mounted root dentry */
	vfs_dentry_t *root = vfs_dentry_resolve(path);
	if (!root) return -2;

	mnt_namespace_t *mns = current_mnt_ns();
	uint64_t f = spin_lock_irq(&vfs_lock);
	vfs_mount_t *prev = NULL;
	vfs_mount_t *mnt  = mns->mounts;
	while (mnt) {
		if (mnt->root == root) break;
		prev = mnt;
		mnt  = mnt->next;
	}
	if (!mnt) { spin_unlock_irq(&vfs_lock, f); return -22; }

	if (prev) prev->next = mnt->next;
	else      mns->mounts = mnt->next;
	void (*kill_sb)(void *) = mnt->kill_sb;
	void *priv = mnt->priv;
	spin_unlock_irq(&vfs_lock, f);

	if (kill_sb) kill_sb(priv);
	kfree(mnt);
	return 0;
}

/* ── fd_table ────────────────────────────────────────────── */

/* The open/cloexec bitmaps are sized cap/64 words and growth doubles cap, so
 * the seed must be a multiple of 64 or the bitmaps come up short. */
_Static_assert(FD_INIT_CAP % 64 == 0, "CONFIG_FD_INIT_CAP must be a multiple of 64");

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
	t->lock     = (spinlock_t)SPINLOCK_INIT;
	return t;
}

fd_table_t *fd_table_get(fd_table_t *t)
{
	/* Atomic: threads share one fd table; concurrent thread create/exit on
	 * different CPUs must not race this refcount. */
	if (t) atomic_fetch_add32((volatile uint32_t*)&t->refcount, 1);
	return t;
}

void fd_table_put(fd_table_t *t)
{
	if (!t) return;
	if ((int32_t)atomic_sub_fetch32((volatile uint32_t*)&t->refcount, 1) <= 0) fd_table_destroy(t);
}

void fd_table_destroy(fd_table_t *t)
{
	if (!t) return;
	/* refcount hit 0: no other thread shares this table, so no lock needed. */
	for (int i = 0; i < t->cap; i++)
		if (t->files[i]) fput(t->files[i]);
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
	n->lock     = (spinlock_t)SPINLOCK_INIT;
	/* Snapshot the parent's table under its lock. n was sized to old->cap at
	 * entry, so bound the copy by n->cap in case a CLONE_FILES sibling grows
	 * old concurrently (the grow preserves the first n->cap entries). */
	uint64_t fl = spin_lock_irq(&old->lock);
	memset(n->files, 0, (size_t)n->cap * sizeof(vfs_file_t *));
	memcpy(n->open_bm, old->open_bm, (size_t)(n->cap/64) * sizeof(uint64_t));
	memcpy(n->cloexec, old->cloexec, (size_t)(n->cap/64) * sizeof(uint64_t));
	for (int i = 0; i < n->cap; i++) {
		if (old->files[i]) {
			n->files[i] = old->files[i];
			atomic_fetch_add32((volatile uint32_t *)&n->files[i]->refcount, 1);
		}
	}
	spin_unlock_irq(&old->lock, fl);
	return n;
}

int fd_alloc(fd_table_t *t, vfs_file_t *f)
{
	uint64_t fl = spin_lock_irq(&t->lock);
	for (;;) {
		for (int i = 0; i < t->cap; i++) {
			if (!BM_TST(t->open_bm, i)) {
				t->files[i] = f;
				BM_SET(t->open_bm, i);
				atomic_fetch_add32((volatile uint32_t *)&f->refcount, 1);
				spin_unlock_irq(&t->lock, fl);
				return i;
			}
		}
		if (fd_table_grow(t) != 0) { spin_unlock_irq(&t->lock, fl); return -1; }
	}
}

int fd_alloc_from(fd_table_t *t, vfs_file_t *f, int start)
{
	uint64_t fl = spin_lock_irq(&t->lock);
	for (;;) {
		for (int i = start; i < t->cap; i++) {
			if (!BM_TST(t->open_bm, i)) {
				t->files[i] = f;
				BM_SET(t->open_bm, i);
				atomic_fetch_add32((volatile uint32_t *)&f->refcount, 1);
				spin_unlock_irq(&t->lock, fl);
				return i;
			}
		}
		if (fd_table_grow(t) != 0) { spin_unlock_irq(&t->lock, fl); return -1; }
	}
}

int fd_alloc_at(fd_table_t *t, vfs_file_t *f, int fd)
{
	uint64_t fl = spin_lock_irq(&t->lock);
	while (fd >= t->cap)
		if (fd_table_grow(t) != 0) { spin_unlock_irq(&t->lock, fl); return -1; }
	t->files[fd] = f;
	BM_SET(t->open_bm, fd);
	atomic_fetch_add32((volatile uint32_t *)&f->refcount, 1);
	spin_unlock_irq(&t->lock, fl);
	return fd;
}

void fd_free(fd_table_t *t, int fd)
{
	if (!t || fd < 0 || fd >= t->cap) return;
	uint64_t fl = spin_lock_irq(&t->lock);
	t->files[fd] = NULL;
	BM_CLR(t->open_bm, fd);
	BM_CLR(t->cloexec, fd);
	spin_unlock_irq(&t->lock, fl);
}

vfs_file_t *fd_get(fd_table_t *t, int fd)
{
	if (!t || fd < 0 || fd >= t->cap) return NULL;
	uint64_t fl = spin_lock_irq(&t->lock);
	vfs_file_t *f = BM_TST(t->open_bm, fd) ? t->files[fd] : NULL;
	spin_unlock_irq(&t->lock, fl);
	return f;
}

/* Drop a file reference; the last one tears the file down. Canonical teardown
 * shared by vfs_close, fd_table_destroy and the fget() readers. */
void fput(vfs_file_t *f)
{
	if (!f) return;
	if (atomic_sub_fetch32((volatile uint32_t *)&f->refcount, 1) != 0) return;
	vfs_flock_release(f);
	if (f->inode && f->inode->fops && f->inode->fops->close)
		f->inode->fops->close(f);
	if (f->inode && !f->inode->cached)
		vfs_inode_free(f->inode);
	kfree(f);
}

/* fd_get + take a reference, atomically under the table lock, so a sibling
 * close() can't free the file while a blocking read/write holds it. */
vfs_file_t *fget(fd_table_t *t, int fd)
{
	if (!t) return NULL;
	uint64_t fl = spin_lock_irq(&t->lock);
	vfs_file_t *f = (fd >= 0 && fd < t->cap && BM_TST(t->open_bm, fd)) ? t->files[fd] : NULL;
	if (f) atomic_fetch_add32((volatile uint32_t *)&f->refcount, 1);
	spin_unlock_irq(&t->lock, fl);
	return f;
}

/* Atomically detach fd and return its file (the caller fputs it), so two
 * concurrent closes can't both drop the slot's reference. */
vfs_file_t *fd_remove(fd_table_t *t, int fd)
{
	if (!t || fd < 0 || fd >= t->cap) return NULL;
	uint64_t fl = spin_lock_irq(&t->lock);
	vfs_file_t *f = BM_TST(t->open_bm, fd) ? t->files[fd] : NULL;
	if (f) { t->files[fd] = NULL; BM_CLR(t->open_bm, fd); BM_CLR(t->cloexec, fd); }
	spin_unlock_irq(&t->lock, fl);
	return f;
}

/* ── file ops ────────────────────────────────────────────── */

int vfs_open(const char *path, int flags, mode_t mode)
{
	process_t *p = task_current();
	if (!p->fds) return -1;
	return vfs_open_fds(p->fds, p->euid, p->egid, p->umask, path, flags, mode);
}

/* lexical canonicalization of an absolute path (collapse "//", ".", "..").
 * Stored in f->path so /proc/self/fd/N (realpath), fchdir and fexecve see a
 * clean path. Symlinks are already resolved component-by-component by
 * vfs_dentry_resolve, so this matches the getcwd/chdir model. */
static void vfs_canon_path(const char *in, char *out, size_t outsz)
{
	if (!in || in[0] != '/' || outsz < 2) { if (outsz) out[0] = '\0'; return; }
	char *o = out, *oend = out + outsz - 1;
	const char *p = in;
	while (*p) {
		while (*p == '/') p++;
		if (!*p) break;
		const char *start = p;
		while (*p && *p != '/') p++;
		size_t len = (size_t)(p - start);
		if (len == 1 && start[0] == '.') continue;
		if (len == 2 && start[0] == '.' && start[1] == '.') {
			while (o > out && o[-1] != '/') o--;
			if (o > out) o--;
			continue;
		}
		if (o < oend) *o++ = '/';
		for (size_t i = 0; i < len && o < oend; i++) *o++ = start[i];
	}
	if (o == out) *o++ = '/';
	*o = '\0';
}

int vfs_open_fds(fd_table_t *fds, uid_t uid, gid_t gid, mode_t umask,
                 const char *path, int flags, mode_t mode)
{
	if (!fds) return -1;

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
		mode_t cmode = mode & ~umask;
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
	if (path[0] == '/') vfs_canon_path(path, f->path, sizeof(f->path));
	else { size_t pl = strlen(path); if (pl < sizeof(f->path)) memcpy(f->path, path, pl + 1); }

	if (inode->fops && inode->fops->open && inode->fops->open(inode, f, flags) != 0) {
		kfree(f);
		return -1;
	}

	int fd = fd_alloc(fds, f);
	if (fd < 0) { kfree(f); return -1; }
	if (flags & O_CLOEXEC) BM_SET(fds->cloexec, fd);
	return fd;
}

/* ── flock(2): BSD whole-file advisory locks, tied to the open file ──
 * LOCK_SH=1 LOCK_EX=2 LOCK_NB=4 LOCK_UN=8. The lock follows the open file
 * description (shared by dup/fork), released when the file is finally closed. */
static spinlock_t flock_lock = SPINLOCK_INIT;

void vfs_flock_release(vfs_file_t *f)
{
	if (!f || !f->inode || !f->flock_held) return;
	uint64_t fl = spin_lock_irq(&flock_lock);
	if (f->flock_held == 2 && f->inode->flock_ex == f) f->inode->flock_ex = NULL;
	else if (f->flock_held == 1 && f->inode->flock_sh > 0) f->inode->flock_sh--;
	f->flock_held = 0;
	spin_unlock_irq(&flock_lock, fl);
}

/* try to acquire LOCK_SH(1)/LOCK_EX(2); 0 = got it, -11 (EWOULDBLOCK) = conflict */
int vfs_flock_try(vfs_file_t *f, int cmd)
{
	if (!f || !f->inode) return -9;
	vfs_inode_t *in = f->inode;
	uint64_t fl = spin_lock_irq(&flock_lock);
	int other_ex = in->flock_ex && in->flock_ex != f;
	int other_sh = in->flock_sh - (f->flock_held == 1 ? 1 : 0);
	int conflict = (cmd == 2) ? (other_ex || other_sh > 0) : other_ex;
	if (conflict) { spin_unlock_irq(&flock_lock, fl); return -11; }
	if (f->flock_held == 2) in->flock_ex = NULL;          /* drop prior hold */
	else if (f->flock_held == 1) in->flock_sh--;
	if (cmd == 2) { in->flock_ex = f; f->flock_held = 2; }
	else          { in->flock_sh++;  f->flock_held = 1; }
	spin_unlock_irq(&flock_lock, fl);
	return 0;
}

/* ── POSIX advisory record locks (fcntl F_SETLK/F_GETLK) ─────────────
 * Owned by the process (pid); byte ranges; released on close of any fd to the
 * file and on process exit. A global list — fine for the small lock counts in
 * practice (sqlite, dpkg). type: 0=F_RDLCK 1=F_WRLCK. */
typedef struct posix_lock {
	void              *inode;
	int                pid;
	int                type;
	int64_t            start, end;
	struct posix_lock *next;
} posix_lock_t;

static posix_lock_t *g_plocks = NULL;
static spinlock_t    plock_lock = SPINLOCK_INIT;

static int pl_overlap(posix_lock_t *l, int64_t s, int64_t e)
{ return l->start <= e && s <= l->end; }

/* returns 0 if granted, -11 (EAGAIN) on conflict; F_UNLCK via vfs_posix_unlock */
int vfs_posix_setlk(void *inode, int pid, int type, int64_t start, int64_t end)
{
	uint64_t f = spin_lock_irq(&plock_lock);
	for (posix_lock_t *l = g_plocks; l; l = l->next) {
		if (l->inode != inode || l->pid == pid) continue;
		if (pl_overlap(l, start, end) && (type == 1 || l->type == 1)) {
			spin_unlock_irq(&plock_lock, f);
			return -11; /* EAGAIN: conflicting lock held by another process */
		}
	}
	/* drop this pid's existing records overlapping the range, then add fresh */
	posix_lock_t **pp = &g_plocks;
	while (*pp) {
		posix_lock_t *l = *pp;
		if (l->inode == inode && l->pid == pid && pl_overlap(l, start, end)) {
			*pp = l->next; kfree(l);
		} else pp = &l->next;
	}
	posix_lock_t *nl = kmalloc(sizeof(*nl));
	if (!nl) { spin_unlock_irq(&plock_lock, f); return -12; }
	nl->inode = inode; nl->pid = pid; nl->type = type;
	nl->start = start; nl->end = end;
	nl->next = g_plocks; g_plocks = nl;
	spin_unlock_irq(&plock_lock, f);
	return 0;
}

void vfs_posix_unlock(void *inode, int pid, int64_t start, int64_t end)
{
	uint64_t f = spin_lock_irq(&plock_lock);
	posix_lock_t **pp = &g_plocks;
	while (*pp) {
		posix_lock_t *l = *pp;
		if (l->inode == inode && l->pid == pid && pl_overlap(l, start, end)) {
			*pp = l->next; kfree(l);
		} else pp = &l->next;
	}
	spin_unlock_irq(&plock_lock, f);
}

/* find a lock conflicting with the (type,range) request; returns 1 + fills the
 * holder's type/range/pid, or 0 if the range is free. */
int vfs_posix_getlk(void *inode, int pid, int *type, int64_t *start, int64_t *end, int *holder_pid)
{
	uint64_t f = spin_lock_irq(&plock_lock);
	for (posix_lock_t *l = g_plocks; l; l = l->next) {
		if (l->inode != inode || l->pid == pid) continue;
		if (pl_overlap(l, *start, *end) && (*type == 1 || l->type == 1)) {
			*type = l->type; *start = l->start; *end = l->end; *holder_pid = l->pid;
			spin_unlock_irq(&plock_lock, f);
			return 1;
		}
	}
	spin_unlock_irq(&plock_lock, f);
	return 0;
}

static void plock_drop(void *inode, int pid, int all)
{
	uint64_t f = spin_lock_irq(&plock_lock);
	posix_lock_t **pp = &g_plocks;
	while (*pp) {
		posix_lock_t *l = *pp;
		if (l->pid == pid && (all || l->inode == inode)) { *pp = l->next; kfree(l); }
		else pp = &l->next;
	}
	spin_unlock_irq(&plock_lock, f);
}

void vfs_posix_drop_pid(int pid)              { plock_drop(NULL, pid, 1); }
void vfs_posix_drop_pid_inode(void *in, int pid) { plock_drop(in, pid, 0); }

/* drop one reference to an open file; close + free it when the last goes.
 * Used by SCM_RIGHTS fd-passing to manage in-flight file references. */
void vfs_file_put(vfs_file_t *f)
{
	if (!f) return;
	if (atomic_sub_fetch32((volatile uint32_t *)&f->refcount, 1) == 0) {
		vfs_flock_release(f);
		if (f->inode && f->inode->fops && f->inode->fops->close)
			f->inode->fops->close(f);
		if (f->inode && !f->inode->cached)
			vfs_inode_free(f->inode);
		kfree(f);
	}
}

int vfs_close(int fd)
{
	process_t  *p = task_current();
	vfs_file_t *f = fd_remove(p->fds, fd);   /* atomic detach: only one close wins */
	if (!f) return -1;
	if (f->inode && p) vfs_posix_drop_pid_inode(f->inode, (int)p->pid);
	fput(f);
	return 0;
}

ssize_t vfs_read(int fd, void *buf, size_t len)
{
	process_t  *p = task_current();
	vfs_file_t *f = fget(p->fds, fd);   /* hold a ref so a sibling close can't free it mid-read */
	if (!f) return -9; /* EBADF */
	ssize_t r = (f->inode->fops && f->inode->fops->read)
	          ? f->inode->fops->read(f, buf, len) : -22; /* EINVAL */
	fput(f);
	return r;
}

ssize_t vfs_write(int fd, const void *buf, size_t len)
{
	process_t  *p = task_current();
	vfs_file_t *f = fget(p->fds, fd);
	if (!f) return -9; /* EBADF */
	ssize_t r;
	if (!f->inode->fops || !f->inode->fops->write) {
		r = -22; /* EINVAL */
	} else {
		/* O_APPEND: each write lands at end of file, regardless of the seek pos. */
		if ((f->flags & O_APPEND) && S_ISREG(f->inode->mode))
			f->pos = (off_t)f->inode->size;
		r = f->inode->fops->write(f, buf, len);
	}
	fput(f);
	return r;
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

int vfs_mkfifo(const char *path, mode_t mode)
{
	if (vfs_dentry_resolve(path)) return -17; /* EEXIST */
	const char *slash = path + strlen(path) - 1;
	while (slash > path && *slash != '/') slash--;
	char dir_path[256];
	int dlen = (int)(slash - path);
	if (dlen == 0) { dir_path[0] = '/'; dir_path[1] = '\0'; }
	else           { memcpy(dir_path, path, dlen); dir_path[dlen] = '\0'; }

	vfs_dentry_t *parent = vfs_dentry_resolve(dir_path);
	if (!parent || !parent->inode || !parent->inode->iops) return -2;
	if (!parent->inode->iops->mknod) return -1; /* fs doesn't support mknod */
	process_t *p = task_current();
	int pr = vfs_check_perm(parent->inode, MAY_WRITE, p ? p->euid : 0, p ? p->egid : 0);
	if (pr) return pr;

	const char *fname = slash + 1;
	mode_t cmode = (mode & 07777 & ~(p ? p->umask : 022)) | S_IFIFO;
	int r = parent->inode->iops->mknod(parent->inode, fname, cmode, 0);
	if (r != 0) return r;
	vfs_inode_t *in = parent->inode->iops->lookup(parent->inode, fname);
	if (in) vfs_dentry_create(parent, fname, in);
	return 0;
}

/* mknod for character/block device nodes (S_IFCHR/S_IFBLK) — routes to the
 * driver registered for that major:minor (devfs registry). */
int vfs_mknod(const char *path, mode_t mode, dev_t dev)
{
	if (vfs_dentry_resolve(path)) return -17; /* EEXIST */
	const char *slash = path + strlen(path) - 1;
	while (slash > path && *slash != '/') slash--;
	char dir_path[256];
	int dlen = (int)(slash - path);
	if (dlen == 0) { dir_path[0] = '/'; dir_path[1] = '\0'; }
	else           { memcpy(dir_path, path, dlen); dir_path[dlen] = '\0'; }

	vfs_dentry_t *parent = vfs_dentry_resolve(dir_path);
	if (!parent || !parent->inode || !parent->inode->iops) return -2;
	if (!parent->inode->iops->mknod) return -1;
	process_t *p = task_current();
	if (p && p->euid != 0) return -1; /* EPERM: only root may create device nodes */
	int pr = vfs_check_perm(parent->inode, MAY_WRITE, p ? p->euid : 0, p ? p->egid : 0);
	if (pr) return pr;

	const char *fname = slash + 1;
	int r = parent->inode->iops->mknod(parent->inode, fname, mode, dev);
	if (r != 0) return r;
	vfs_inode_t *in = parent->inode->iops->lookup(parent->inode, fname);
	if (in) vfs_dentry_create(parent, fname, in);
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

/* Like vfs_stat, but does NOT follow a final symlink (lstat): resolve the parent
 * directory, then look up the last component directly so a symlink reports as
 * S_IFLNK instead of its target. */
int vfs_lstat(const char *path, vfs_stat_t *out)
{
	int len = (int)strlen(path);
	const char *slash = path + (len > 0 ? len - 1 : 0);
	while (slash > path && *slash != '/') slash--;
	if (slash[1] == '\0') return vfs_stat(path, out);   /* trailing slash / root */

	char parent_path[256];
	int plen = (int)(slash - path);
	if (plen == 0) { parent_path[0] = '/'; parent_path[1] = '\0'; }
	else           { memcpy(parent_path, path, (size_t)plen); parent_path[plen] = '\0'; }
	const char *fname = slash + 1;

	vfs_dentry_t *parent = vfs_dentry_resolve(parent_path);
	if (!parent || !parent->inode || !parent->inode->iops || !parent->inode->iops->lookup)
		return -1;
	vfs_inode_t *in = parent->inode->iops->lookup(parent->inode, fname);
	if (!in) return -1;

	int rc = 0;
	if (in->iops && in->iops->stat) {
		rc = in->iops->stat(in, out);
	} else {
		out->ino = in->ino; out->mode = in->mode; out->nlink = in->nlink;
		out->uid = in->uid; out->gid = in->gid; out->size = in->size; out->rdev = in->rdev;
	}
	if (!in->cached) kfree(in);
	return rc;
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

/* change owner/group of an inode (uid/gid == (uid_t)-1 leaves it unchanged).
 * Only root may change the uid; clears set-user/group-ID bits. */
int vfs_chown_inode(vfs_inode_t *in, uid_t uid, gid_t gid)
{
	if (!in) return -2;
	process_t *p = task_current();
	uid_t euid = p ? p->euid : 0;
	if (euid != 0) {
		/* non-root: only the owner, only the group, only to a group they're in */
		if (in->uid != euid) return -1;            /* EPERM */
		if (uid != (uid_t)-1 && uid != in->uid) return -1;
		if (gid != (gid_t)-1) {
			int ok = (p && gid == p->egid);
			for (int i = 0; p && !ok && i < p->ngroups; i++) ok = (p->groups[i] == gid);
			if (!ok) return -1;
		}
	}
	if (uid != (uid_t)-1) in->uid = uid;
	if (gid != (gid_t)-1) in->gid = gid;
	in->mode &= ~(mode_t)06000;                    /* drop setuid/setgid on chown */
	int64_t s, n; ranix_walltime(&s, &n);
	in->ctime = s; in->ctime_nsec = (uint32_t)n;
	return 0;
}

int vfs_chown(const char *path, uid_t uid, gid_t gid)
{
	vfs_dentry_t *d = vfs_dentry_resolve(path);
	if (!d || !d->inode) return -2;
	return vfs_chown_inode(d->inode, uid, gid);
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
#if IS_ENABLED(CONFIG_FS_TMPFS)
	extern void tmpfs_register(void);
	tmpfs_register();
#endif
#if IS_ENABLED(CONFIG_FS_OVERLAY)
	extern void overlay_register(void);
	overlay_register();
#endif
	procfs_register();
	devfs_register();
#if IS_ENABLED(CONFIG_FS_EXT2)
	extern void ext2_register(void);
	ext2_register();
#endif
#if IS_ENABLED(CONFIG_FS_FAT)
	extern void fat_register(void);
	fat_register();
#endif
#if IS_ENABLED(CONFIG_FS_ISO9660)
	extern void iso9660_register(void);
	iso9660_register();
#endif
	extern void sysfs_register(void);
	sysfs_register();
	extern void hugetlbfs_register(void);
	hugetlbfs_register();
	extern void cgroup_register(void);
	cgroup_register();
	kprintf("VFS: initialized\n");
}
