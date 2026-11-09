/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
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
#include <kernel/block.h>
#include <kernel/fs/ramfs.h>
#include <kernel/fs/procfs.h>
#include <kernel/fs/devfs.h>
#include <kernel/fs/tmpfs.h>
#include <kernel/fs/overlay.h>
#include <kernel/fs/ext2.h>
#include <kernel/cgroup.h>
#include <kernel/errno.h>
#include <kernel/fscred.h>
#include <kernel/capabilities.h>

static vfs_dentry_t *root_dentry = NULL;
static vfs_dentry_t *follow_mounts(vfs_dentry_t *d);
static vfs_mount_t *mount_on(mnt_namespace_t *ns, vfs_dentry_t *d);

vfs_inotify_fn_t vfs_inotify_hook = NULL;

static spinlock_t vfs_lock = SPINLOCK_INIT;
static int dentry_count = 0;
static int dentry_max_val = DENTRY_MAX;

int vfs_dentry_count(void) { return dentry_count; }

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
	if (!in || !name || !name[0]) return -EINVAL;
	uint64_t f = spin_lock_irq(&xattr_lock);
	vfs_xattr_t *x = in->xattrs, *prev = NULL;
	while (x) { if (strcmp(x->name, name) == 0) break; prev = x; x = x->next; }
	(void)prev;
	if (x) {
		if (flags & XATTR_CREATE) { spin_unlock_irq(&xattr_lock, f); return -EEXIST; }
		void *nv = len ? kmalloc(len) : NULL;
		if (len && !nv) { spin_unlock_irq(&xattr_lock, f); return -ENOMEM; }
		if (len) memcpy(nv, val, len);
		if (x->value) kfree(x->value);
		x->value = nv; x->len = len;
		spin_unlock_irq(&xattr_lock, f);
		return 0;
	}
	if (flags & XATTR_REPLACE) { spin_unlock_irq(&xattr_lock, f); return -ENODATA; }
	vfs_xattr_t *nx = kmalloc(sizeof(*nx));
	if (!nx) { spin_unlock_irq(&xattr_lock, f); return -ENOMEM; }
	size_t nlen = strlen(name);
	nx->name = kmalloc(nlen + 1);
	nx->value = len ? kmalloc(len) : NULL;
	if (!nx->name || (len && !nx->value)) {
		if (nx->name) kfree(nx->name);
		if (nx->value) kfree(nx->value);
		kfree(nx);
		spin_unlock_irq(&xattr_lock, f);
		return -ENOMEM;
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
	if (!in || !name) return -EINVAL;
	uint64_t f = spin_lock_irq(&xattr_lock);
	for (vfs_xattr_t *x = in->xattrs; x; x = x->next) {
		if (strcmp(x->name, name) != 0) continue;
		ssize_t r = (ssize_t)x->len;
		if (size == 0) { spin_unlock_irq(&xattr_lock, f); return r; }
		if (size < x->len) { spin_unlock_irq(&xattr_lock, f); return -ERANGE; }
		if (x->len) memcpy(val, x->value, x->len);
		spin_unlock_irq(&xattr_lock, f);
		return r;
	}
	spin_unlock_irq(&xattr_lock, f);
	return -ENODATA;
}

ssize_t vfs_inode_listxattr(vfs_inode_t *in, char *list, size_t size)
{
	if (!in) return -EINVAL;
	uint64_t f = spin_lock_irq(&xattr_lock);
	size_t total = 0;
	for (vfs_xattr_t *x = in->xattrs; x; x = x->next) total += strlen(x->name) + 1;
	if (size == 0) { spin_unlock_irq(&xattr_lock, f); return (ssize_t)total; }
	if (size < total) { spin_unlock_irq(&xattr_lock, f); return -ERANGE; }
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
	if (!in || !name) return -EINVAL;
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
	return -ENODATA;
}

/* Free an in-core inode no dentry, pipe or static table owns. A filesystem
 * that tracks its copies hears of it through iops->release, so this is the one
 * way to free a lookup's result. */
void vfs_inode_free(vfs_inode_t *in)
{
	if (!in || in->cached) return; /* cached=1: owned by dentry tree, pipe, or static */
	if (in->iops && in->iops->release) in->iops->release(in);
	vfs_inode_free_xattrs(in);
	if (in->priv && !in->priv_static) kfree(in->priv);
	kfree(in);
}

/* ── deferred frees ──────────────────────────────────────────
 * A walk returns dentry and inode pointers it holds no reference on, and its
 * caller goes on using them while unlink on another CPU can drop the last name.
 * So a dentry or a nameless inode leaves only when every syscall that had walked a
 * path by then has finished. Each of those registers in the current generation.
 * What is retired waits in new, moves to old when the previous generation has
 * drained, and goes when the one after it has: two tasks taking turns in a walk
 * cannot hold it up, as they could with a single count. */
typedef struct { vfs_inode_t *in; vfs_dentry_t *de; vfs_deferred_t *call; vfs_inode_t *fr; } grave_t;

static spinlock_t grave_lock = SPINLOCK_INIT;

/* Where an open directory stands: its dentry (held, see dentry_hold) and the
 * mounts the walk that opened it came in through, innermost last, so that ".."
 * and its path leave a bind the way they entered (walk_t.mnt). A directory the
 * cache does not keep (procfs /proc/PID) has no dentry: it keeps instead the
 * dentry its f->path starts from (held too), the root it was opened under. */
#define WALK_MNT_MAX 16
typedef struct { vfs_dentry_t *root, *mp; } walk_mnt_t;
typedef struct { int n; vfs_dentry_t *start; walk_mnt_t mnt[WALK_MNT_MAX]; } walk_mnts_t;
struct vfs_dirpos {
	vfs_dentry_t *d;         /* NULL: not cached, see start */
	vfs_dentry_t *start;     /* where f->path starts, when d is NULL */
	int           nmnt;
	walk_mnt_t    mnt[];
};
static uint32_t   walk_gen, walkers[2];
static grave_t    grave_new, grave_old;

static void grave_free(grave_t g)
{
	while (g.in) {
		vfs_inode_t *in = g.in;
		g.in = in->grave_next;
		if (in->in_grave == 7) {
			kprintf(KERN_ERR "vfs: inode %p ino %lu mode 0x%x in the grave after eviction, retired by %p\n",
			        in, (unsigned long)in->ino, in->mode, in->retired_by);
			continue;
		}
		/* a walk that got this far opened it: the last close comes back here */
		uint64_t f = spin_lock_irq(&grave_lock);
		int opened = in->open_refs != 0;
		if (opened) in->in_grave = 0;
		spin_unlock_irq(&grave_lock, f);
		if (!opened) {
			in->in_grave = 7;        /* poison: a later retire of freed memory shows */
			
			in->iops->evict(in);
		}
	}
	while (g.de) {
		vfs_dentry_t *d = g.de;
		g.de = d->grave_next;
		kfree(d);
	}
	/* inodes of dentries the cache dropped (in_grave 2): a walk that got this far
	 * may have opened one, and then its last close frees it */
	while (g.fr) {
		vfs_inode_t *in = g.fr;
		g.fr = in->grave_next;
		uint64_t f = spin_lock_irq(&grave_lock);
		int opened = in->open_refs != 0;
		in->in_grave = 0;
		in->cached = 0;
		spin_unlock_irq(&grave_lock, f);
		if (!opened) vfs_inode_free(in);
	}
	/* last: a filesystem freed here may be what the inodes above evicted into */
	while (g.call) {
		vfs_deferred_t *c = g.call;
		g.call = c->next;
		c->fn(c);
	}
}

static void grave_advance(void)
{
	grave_t a = { 0 }, b = { 0 };
	uint64_t f = spin_lock_irq(&grave_lock);
	if (!walkers[0] && !walkers[1]) {
		a = grave_old; b = grave_new;
		grave_old = grave_new = (grave_t){ 0 };
	} else if (!walkers[(walk_gen & 1) ^ 1]) {
		a = grave_old;
		grave_old = grave_new;
		grave_new = (grave_t){ 0 };
		walk_gen++;
	}
	spin_unlock_irq(&grave_lock, f);
	grave_free(a);
	grave_free(b);
}

static void walk_begin(void)
{
	process_t *p = task_current();
	if (!p || !p->in_syscall || p->walking) return;
	uint64_t f = spin_lock_irq(&grave_lock);
	p->walking = 1;
	p->walk_gen = walk_gen;
	walkers[walk_gen & 1]++;
	spin_unlock_irq(&grave_lock, f);
}

void vfs_defer(vfs_deferred_t *d)
{
	walk_begin();   /* the caller, too, may use what fn frees until its syscall ends */
	uint64_t f = spin_lock_irq(&grave_lock);
	d->next = grave_new.call;
	grave_new.call = d;
	spin_unlock_irq(&grave_lock, f);
	grave_advance();
}

/* Paths are walked as strings from the root, but a task reaches its cwd and an
 * open directory by what it already holds: permission was checked when it got
 * them, and dropping privileges does not take them away. So a step that only
 * follows the path to one of those is not checked again; the anchor's own search
 * bit, and any step away from it, still are. An absolute path spelled through the
 * anchor gains nothing the relative one does not already have. */
static int on_anchor(const char *anchor, const char *start, size_t n)
{
	if (!anchor || strncmp(anchor, start, n) != 0) return 0;
	return anchor[n] == '/' || anchor[n] == '\0';
}

static int walk_anchored(const char *start, const char *end, int outside)
{
	process_t *p = task_current();
	size_t n = (size_t)(end - start);
	if (!p) return 0;
	if (outside == 2) return 0;   /* back at the root by a symlink: the anchors lie above */
	if (on_anchor(p->walk_anchor[0], start, n) || on_anchor(p->walk_anchor[1], start, n))
		return 1;
	if (outside) return 0;   /* the cwd is a path from the root, not from above it */
	fs_path_t *cwd = fs_cwd_get(p->fs);   /* held: a CLONE_FS sibling may chdir */
	int r = on_anchor(cwd->s, start, n);
	fs_path_put(cwd);
	return r;
}

/* for the rest of this syscall; without memory the walk just stays strict */
void vfs_walk_anchor(process_t *p, const char *path)
{
	int i = p->walk_anchor[0] ? 1 : 0;
	if (p->walk_anchor[i]) return;
	size_t n = strlen(path) + 1;
	char *c = kmalloc(n);
	if (c) memcpy(c, path, n);
	p->walk_anchor[i] = c;
}

/* Where this syscall's next path starts: at the task's root (top NULL), or, for
 * a directory held from before a chroot, at the top of the tree above it
 * (vfs_file_dirpath). A path walks from the top only when it lies on that
 * directory's anchor. One syscall's paths start at one of the two: mixing them
 * (renameat from such a directory to a path of the root) is -18 (EXDEV). */
int vfs_walk_from(process_t *p, vfs_dentry_t *top)
{
	int kind = top ? 2 : 1;
	if (top && p->walk_top && p->walk_top != top) return -EXDEV;
	if ((p->walk_kinds | kind) == 3) return -EXDEV;
	p->walk_kinds |= kind;
	if (top) p->walk_top = top;
	return 0;
}

/* The held directory's top, if path is on one of this syscall's anchors */
static vfs_dentry_t *walk_top_for(process_t *p, const char *path)
{
	if (!p || !p->walk_top) return NULL;
	for (int i = 0; i < 2; i++) {
		const char *a = p->walk_anchor[i];
		size_t n = a ? strlen(a) : 0;
		if (a && strncmp(path, a, n) == 0 && (n == 1 || path[n] == '/' || path[n] == '\0'))
			return p->walk_top;
	}
	return NULL;
}

/* At syscall exit and task exit, and before anything that may block for long. */
void vfs_walk_done(process_t *p)
{
	for (int i = 0; i < 2; i++) {
		kfree(p->walk_anchor[i]);
		p->walk_anchor[i] = NULL;
	}
	p->walk_top = NULL;
	p->walk_kinds = 0;
	if (!p->walking) return;
	uint64_t f = spin_lock_irq(&grave_lock);
	p->walking = 0;
	walkers[p->walk_gen & 1]--;
	spin_unlock_irq(&grave_lock, f);
	grave_advance();
}

/* Caller holds grave_lock. */
static void inode_retire_if_idle(vfs_inode_t *in)
{
	if (in->in_grave == 7) {
		kprintf(KERN_ERR "vfs: retire of an evicted inode %p from %p %p %p\n", in,
		        __builtin_return_address(0), __builtin_return_address(1), __builtin_return_address(2));
		return;
	}
	if (in->open_refs || in->in_grave) return;
	in->in_grave = 1;
	in->retired_by = __builtin_return_address(0);
	in->grave_next = grave_new.in;
	grave_new.in = in;
}

/* in has lost its last name: it goes once it is also out of every file and walk. */
void vfs_inode_orphan(vfs_inode_t *in)
{
	uint64_t f = spin_lock_irq(&grave_lock);
	if (in->deferred_unlink)
		kprintf(KERN_ERR "vfs: inode %lu orphaned twice, from %p grave=%d refs=%u\n",
		        (unsigned long)in->ino, __builtin_return_address(0), in->in_grave, in->open_refs);
	in->deferred_unlink = 1;
	inode_retire_if_idle(in);
	spin_unlock_irq(&grave_lock, f);
	grave_advance();
}

static void dentry_retire(vfs_dentry_t *d)
{
	uint64_t f = spin_lock_irq(&grave_lock);
	if (d->retired) {               /* twice would free it twice */
		spin_unlock_irq(&grave_lock, f);
		kprintf(KERN_ERR "vfs: dentry %s retired twice, from %p\n", d->name,
		        __builtin_return_address(0));
		return;
	}
	d->retired = 1;
	if (d->holds) {                 /* the last close of a directory here frees it */
		spin_unlock_irq(&grave_lock, f);
		return;
	}
	d->grave_next = grave_new.de;
	grave_new.de = d;
	spin_unlock_irq(&grave_lock, f);
}

/* An open directory holds its dentry: the cache keeps it, and if its name goes
 * (rmdir, a rename over it, umount) it stays readable, marked retired, until
 * the hold is dropped. 0: d is retired already, and nothing is held. */
static int dentry_hold(vfs_dentry_t *d)
{
	uint64_t f = spin_lock_irq(&grave_lock);
	int ok = !d->retired;
	if (ok) d->holds++;
	spin_unlock_irq(&grave_lock, f);
	return ok;
}

static void dentry_unhold(vfs_dentry_t *d)
{
	uint64_t f = spin_lock_irq(&grave_lock);
	if (--d->holds == 0 && d->retired) {
		d->grave_next = grave_new.de;
		grave_new.de = d;
	}
	spin_unlock_irq(&grave_lock, f);
}

/* Drop one open file description's hold on an inode. Filesystems supporting
 * open-unlinked semantics (evict hook) defer freeing a name-less inode until
 * its last fd closes here. Returns 1 if the inode is theirs to free (caller must
 * not touch it again), 0 otherwise. */
static int vfs_inode_drop_open(vfs_inode_t *in)
{
	if (!in || in->open_refs == 0) return 0;
	if (!in->iops || !in->iops->evict) {
		atomic_sub_fetch32((volatile uint32_t *)&in->open_refs, 1);
		return 0;
	}
	uint64_t f = spin_lock_irq(&grave_lock);
	int last = atomic_sub_fetch32((volatile uint32_t *)&in->open_refs, 1) == 0;
	int orphan = in->deferred_unlink;
	if (last && orphan) inode_retire_if_idle(in);
	spin_unlock_irq(&grave_lock, f);
	if (orphan) grave_advance();
	return orphan;
}

/* ── permission check ────────────────────────────────────── */
/* accmode: bitmask of MAY_READ=4, MAY_WRITE=2, MAY_EXEC=1   */
#define MAY_READ  4
#define MAY_WRITE 2
#define MAY_EXEC  1

/* uid/gid: the filesystem ids to check for (task_fsuid/task_fsgid); the
 * capabilities that may override the mode are the current task's. */
static int vfs_check_perm(vfs_inode_t *inode, int accmode, uid_t uid, gid_t gid)
{
	if (!inode) return -EACCES;
	fs_cred_t c;
	fs_cred_task(task_current(), &c);
	c.uid = uid;
	c.gid = gid;
	return fs_permission(&c, inode->mode, inode->uid, inode->gid, accmode);
}

/* May the current task look names up in dir? A task's first walks run before it
 * has an identity to check; kernel threads run as root. */
int vfs_may_search(vfs_inode_t *dir)
{
	process_t *p = task_current();
	if (!p || !dir || !vfs_check_perm(dir, MAY_EXEC, task_fsuid(p), task_fsgid(p))) return 1;
	p->walk_denied = 1;
	return 0;
}

/* Why a walk stopped, when not at a missing name: the syscall says so (ENOTDIR,
 * ELOOP, ENAMETOOLONG) instead of the ENOENT every caller reports. The first wins. */
static void walk_fail(int err)
{
	process_t *p = task_current();
	if (p && !p->walk_err) p->walk_err = err;
}

/* In a sticky directory a name goes only by its file's owner, the directory's,
 * or a holder of CAP_FOWNER over the file (Linux check_sticky). */
static int sticky_denied(vfs_inode_t *dir, const char *name)
{
	process_t *p = task_current();
	if (!(dir->mode & S_ISVTX) || !p || dir->uid == task_fsuid(p)) return 0;
	vfs_inode_t *in = dir->iops->lookup(dir, name);
	if (!in) return 0;
	int denied = !fs_owner_or_capable(in->uid, in->gid);
	if (!in->cached) vfs_inode_free(in);
	return denied;
}

/* May the current task add or drop names in dir? */
static int may_change(vfs_inode_t *dir)
{
	process_t *p = task_current();
	return vfs_check_perm(dir, MAY_WRITE | MAY_EXEC, task_fsuid(p), task_fsgid(p));
}

int vfs_may_write(vfs_inode_t *in)
{
	process_t *p = task_current();
	return vfs_check_perm(in, MAY_WRITE, task_fsuid(p), task_fsgid(p));
}

/* fchdir: entering an already open directory still needs search permission */
int vfs_may_enter(vfs_inode_t *dir)
{
	process_t *p = task_current();
	return vfs_check_perm(dir, MAY_EXEC, task_fsuid(p), task_fsgid(p));
}

/* ── dentry pool ─────────────────────────────────────────── */

/* A cached dentry whose inode lookup can make again (lookup_copy: a disk
 * filesystem's) is also on the cache order list, newest first. When the cache
 * passes its limit the oldest unheld leaves are dropped, and their inodes once no
 * walk can still hold them. tmpfs and the like keep their files in their inodes:
 * they are never on the list. Caller holds vfs_lock for all of this. */
static vfs_dentry_t *lru_head, *lru_tail;

static void lru_remove(vfs_dentry_t *d)
{
	if (d->lru_prev) d->lru_prev->lru_next = d->lru_next; else if (lru_head == d) lru_head = d->lru_next;
	if (d->lru_next) d->lru_next->lru_prev = d->lru_prev; else if (lru_tail == d) lru_tail = d->lru_prev;
	d->lru_prev = d->lru_next = NULL;
}

static void lru_push(vfs_dentry_t *d)
{
	d->lru_prev = NULL;
	d->lru_next = lru_head;
	if (lru_head) lru_head->lru_prev = d; else lru_tail = d;
	lru_head = d;
}

/* a cache hit: d moves to the young end (if it is on the list at all) */
static void lru_touch(vfs_dentry_t *d)
{
	if (!d->lru_prev) return;   /* at the head, or not on the list */
	lru_remove(d);
	lru_push(d);
}

static void dentry_link(vfs_dentry_t *parent, vfs_dentry_t *d)
{
	d->parent = parent;
	d->next   = parent->child;
	if (d->next) d->next->pprev = &d->next;
	parent->child = d;
	d->pprev  = &parent->child;
	if (d->inode && d->inode->lookup_copy) lru_push(d);
}

/* Out of its parent's child list and the cache order. next is left as it was:
 * the dentry goes to the grave, and nothing follows it from here. */
static void dentry_unlink(vfs_dentry_t *d)
{
	if (!d->pprev) return;
	*d->pprev = d->next;
	if (d->next) d->next->pprev = d->pprev;
	d->pprev = NULL;
	lru_remove(d);
}

/* A leaf no one holds: not a mountpoint or bind root (pins), not a task's root,
 * not open, and its inode a copy lookup can make again. A mount's own root is in
 * no child list (pprev NULL). */
static int dentry_evictable(vfs_dentry_t *d)
{
	vfs_inode_t *in = d->inode;
	return d->pprev && !d->child && !d->pins && !d->keep && !d->retired && !d->holds &&
	       in && in->lookup_copy && !in->open_refs && !in->in_grave && d != root_dentry;
}

static void dentry_evict(vfs_dentry_t *d)
{
	vfs_inode_t *in = d->inode;
	dentry_unlink(d);
	uint64_t f = spin_lock_irq(&grave_lock);
	in->in_grave = 2;
	in->grave_next = grave_new.fr;
	grave_new.fr = in;
	spin_unlock_irq(&grave_lock, f);
	dentry_retire(d);
	dentry_count--;
}

/* Drop up to want of the oldest evictable dentries, looking at no more than eight
 * times as many: one held for long goes to the young end and is not met again
 * soon. except is the directory a caller is about to cache into. */
static void dcache_evict(int want, vfs_dentry_t *except)
{
	int got = 0;
	for (int seen = 0; lru_tail && got < want && seen < want * 8; seen++) {
		vfs_dentry_t *d = lru_tail;
		if (d != except && dentry_evictable(d)) {
			dentry_evict(d);
			got++;
		} else {
			lru_remove(d);
			lru_push(d);
		}
	}
}

#define DCACHE_EVICT_BATCH 64

static vfs_dentry_t *_dentry_create(vfs_dentry_t *parent, const char *name, vfs_inode_t *inode)
{
	/* The lookup that returned inode ran unlocked, so unlink may have overtaken
	 * it. Its dentry sweep has run or waits for vfs_lock, so nothing would remove
	 * this one, and it would outlive the inode. */
	if (inode && inode->deferred_unlink) return NULL;

	vfs_dentry_t *d = kmalloc(sizeof(*d));
	if (!d) return NULL;

	int nlen = 0;
	while (name[nlen] && nlen < 255) nlen++;
	memcpy(d->name, name, nlen);
	d->name[nlen] = '\0';

	if (vfs_dentry_is_volatile(parent)) parent = NULL;   /* holds no child list */
	if (parent && parent->retired) parent = NULL;        /* dropped meanwhile: nothing would find it */

	d->inode  = inode;
	d->parent = parent;
	d->child  = NULL;
	d->next   = NULL;
	d->pins   = 0;
	d->retired = 0;
	d->grave_next = NULL;
	d->pprev  = NULL;
	d->lru_prev = d->lru_next = NULL;
	d->keep   = 0;
	d->holds  = 0;

	if (parent) {
		/* full: make room among the oldest first, never under the new name */
		if (dentry_count >= dentry_max_val) dcache_evict(DCACHE_EVICT_BATCH, parent);
		static int warned;
		if (dentry_count >= dentry_max_val && !warned) {   /* everything left is held */
			warned = 1;
			kprintf(KERN_WARN "vfs: %d dentries cached, past the limit (vfs_dentry_max)\n", dentry_count);
		}
		dentry_link(parent, d);
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
	/* A concurrent walk may have cached the name already, and unlink drops one. */
	vfs_dentry_t *d = parent && !vfs_dentry_is_volatile(parent) ? parent->child : NULL;
	while (d && (d->inode != inode || strcmp(d->name, name) != 0)) d = d->next;
	if (!d) d = _dentry_create(parent, name, inode);
	spin_unlock_irq(&vfs_lock, f);
	return d;
}

/* An empty directory nothing can be made in: the root a chroot into a generated
 * directory (procfs) gets. Such a directory has no dentry of its own to hold,
 * and its names belong to a task that may be gone; Linux leaves a dead, empty
 * directory there once the task exits, and Chromium's sandbox chroots into
 * /proc/self/fdinfo/ of a short-lived child for exactly that. One for all:
 * it never changes, is in no list and is never evicted. */
static int empty_create(vfs_inode_t *dir, const char *name, mode_t mode)
{
	(void)dir; (void)name; (void)mode;
	return -EACCES;
}
static int empty_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir; (void)idx; (void)out;
	return -1;
}
static vfs_iops_t empty_root_iops = { .create = empty_create, .readdir = empty_readdir };
static vfs_inode_t empty_root_inode = {
	.ino = 1, .mode = S_IFDIR | 0555, .nlink = 2, .refcount = 1, .cached = 1,
	.iops = &empty_root_iops,
};
static vfs_dentry_t empty_root = { .name = "/", .inode = &empty_root_inode, .keep = 1 };

vfs_dentry_t *vfs_empty_root(void)
{
	return &empty_root;
}

void vfs_dentry_keep(vfs_dentry_t *d)
{
	uint64_t f = spin_lock_irq(&vfs_lock);
	d->keep = 1;
	spin_unlock_irq(&vfs_lock, f);
}

void vfs_dentry_pin(vfs_dentry_t *d)
{
	uint64_t f = spin_lock_irq(&vfs_lock);
	d->pins++;
	spin_unlock_irq(&vfs_lock, f);
}

void vfs_dentry_unpin(vfs_dentry_t *d)
{
	uint64_t f = spin_lock_irq(&vfs_lock);
	d->pins--;
	spin_unlock_irq(&vfs_lock, f);
}

/* Caller holds vfs_lock. */
static int dentry_tree_pinned(vfs_dentry_t *root)
{
	vfs_dentry_t *d = root;
	for (;;) {
		if (d->pins) return 1;
		if (d->child) { d = d->child; continue; }
		while (d != root && !d->next) d = d->parent;
		if (d == root) return 0;
		d = d->next;
	}
}

/* Free the dentries of a dead mount, depth first but without recursion: a tree
 * can be as deep as PATH_MAX allows. root is the mount's, so its ->parent lies
 * outside and is never followed. An inode still open stays with its files, and
 * the last close frees it. */
void vfs_dentry_free_tree(vfs_dentry_t *root, int free_inodes)
{
	uint64_t f = spin_lock_irq(&vfs_lock);
	vfs_dentry_t *d = root;
	for (;;) {
		while (d->child) d = d->child;
		vfs_dentry_t *up = d == root ? NULL : d->parent;
		dentry_unlink(d);
		if (free_inodes && d->inode) {
			d->inode->cached = 0;
			if (!d->inode->open_refs) vfs_inode_free(d->inode);
		}
		dentry_retire(d);
		dentry_count--;
		if (!up) break;
		d = up;
	}
	spin_unlock_irq(&vfs_lock, f);
}

typedef struct {
	uint32_t      refcount;
	vfs_dentry_t *root;
	void        (*release)(vfs_inode_t *root);
} simple_sb_t;

static void simple_get_sb(void *priv)
{
	atomic_fetch_add32(&((simple_sb_t *)priv)->refcount, 1);
}

static void simple_kill_sb(void *priv)
{
	simple_sb_t *sb = priv;
	if (atomic_sub_fetch32(&sb->refcount, 1) != 0) return;
	vfs_inode_t *in = sb->root->inode;
	vfs_dentry_free_tree(sb->root, !sb->release);
	if (sb->release) sb->release(in);
	kfree(sb);
}

int vfs_simple_sb(vfs_mount_t *mnt, void (*release)(vfs_inode_t *root))
{
	simple_sb_t *sb = kmalloc(sizeof(*sb));
	if (!sb) return -1;
	sb->refcount = 1;
	sb->root     = mnt->root;
	sb->release  = release;
	mnt->priv    = sb;
	mnt->get_sb  = simple_get_sb;
	mnt->kill_sb = simple_kill_sb;
	return 0;
}

/* ── path resolution ─────────────────────────────────────── */

/* Flags of the mount whose tree holds d: a mount root is the first dentry above d
 * that any mount in this namespace claims. Caller holds vfs_lock. */
static unsigned long dentry_mnt_flags(vfs_dentry_t *d)
{
	mnt_namespace_t *ns = current_mnt_ns();
	for (; d; d = d->parent)
		for (vfs_mount_t *m = ns->mounts; m; m = m->next)
			if (m->root == d) return m->flags;
	return 0;
}

/* The vfs_path_mnt_flags checks ahead of each write run on an earlier walk than
 * the one the operation uses, and a task can swap a symlink in between. This asks
 * about the dentry actually reached. */
int vfs_dentry_readonly(vfs_dentry_t *d)
{
	uint64_t f = spin_lock_irq(&vfs_lock);
	unsigned long fl = dentry_mnt_flags(d);
	spin_unlock_irq(&vfs_lock, f);
	return !!(fl & MS_RDONLY);
}

/* Root of the mount holding d, or NULL for the part of the tree no mount claims. */
static vfs_dentry_t *dentry_mnt_root(vfs_dentry_t *d)
{
	mnt_namespace_t *ns = current_mnt_ns();
	uint64_t f = spin_lock_irq(&vfs_lock);
	vfs_dentry_t *root = NULL;
	for (; d && !root; d = d->parent)
		for (vfs_mount_t *m = ns->mounts; m; m = m->next)
			if (m->root == d) { root = d; break; }
	spin_unlock_irq(&vfs_lock, f);
	return root;
}

/* The block device under d's filesystem, held for the caller; NULL if none. An open
 * file keeps it after a lazy unmount, so the disk's name is not given to other
 * hardware while the old filesystem can still read. */
static void *dentry_bdev_get(vfs_dentry_t *d)
{
	mnt_namespace_t *ns = current_mnt_ns();
	void *bd = NULL;
	uint64_t f = spin_lock_irq(&vfs_lock);
	for (int found = 0; d && !found; d = d->parent)
		for (vfs_mount_t *m = ns->mounts; m; m = m->next)
			if (m->root == d) {
				found = 1;
				if (m->bdev) { bd = m->bdev; block_get(bd); }
				break;
			}
	spin_unlock_irq(&vfs_lock, f);
	return bd;
}

/* st_dev of the filesystem d is on: each mount numbers its own, a bind and a
 * namespace copy keep their source's, so find -xdev, du -x and tools reading
 * /proc/self/mountinfo see mount points. */
static uint32_t sdev_next = 16;

static uint32_t dentry_sdev(vfs_dentry_t *d)
{
	mnt_namespace_t *ns = current_mnt_ns();
	uint32_t dev = 0;
	uint64_t f = spin_lock_irq(&vfs_lock);
	for (int found = 0; d && !found; d = d->parent)
		for (vfs_mount_t *m = ns->mounts; m; m = m->next)
			if (m->root == d) { found = 1; dev = m->sdev; break; }
	spin_unlock_irq(&vfs_lock, f);
	return dev;
}

uint32_t vfs_path_sdev(const char *path)
{
	vfs_dentry_t *d = vfs_dentry_resolve(path);
	return d ? dentry_sdev(d) : 0;
}

static void file_free(vfs_file_t *f)
{
	if (f->at) { dentry_unhold(f->at->d ? f->at->d : f->at->start); kfree(f->at); }
	if (f->bdev) block_put(f->bdev);
	kfree(f->lpath);
	kfree(f);
}

/* boot code resolves before the first task exists */
static vfs_dentry_t boot_volatile;

static vfs_dentry_t *volatile_slot(void)
{
	process_t *p = task_current();
	return p ? &p->resolved : &boot_volatile;
}

int vfs_dentry_is_volatile(const vfs_dentry_t *d)
{
	return d && d == volatile_slot();
}

/* With mflags set, gather the flags of the mount a path ends in. A path that stops
 * at a missing name is charged to the deepest directory reached, which is where it
 * would be created. A final symlink counts both ways: unlink acts on the link's
 * mount, open on the target's. */
/* With rp set (PATH_MAX bytes), also write the path the walk really took: every
 * symlink it followed replaced by its target, "." and ".." applied after that,
 * so a name reached through /proc/self/fd/N or any other link reads back as
 * where it is (Linux d_path). Empty when that path did not fit. */
static vfs_dentry_t *resolve_walk_rp(const char *path, unsigned long *mflags, char *rp);
static vfs_dentry_t *resolve_walk(const char *path, unsigned long *mflags)
{
	return resolve_walk_rp(path, mflags, NULL);
}

/* rp text helpers: rp holds "" for the root, else "/a/b"; *rl is its length, -1 once it overflowed */
static void rp_push(char *rp, int *rl, const char *name, int len)
{
	if (!rp || *rl < 0) return;
	if (*rl + 1 + len >= PATH_MAX) { *rl = -1; return; }
	rp[(*rl)++] = '/';
	memcpy(rp + *rl, name, (size_t)len);
	*rl += len; rp[*rl] = '\0';
}
static void rp_pop(char *rp, int *rl)
{
	if (!rp || *rl <= 0) return;
	while (*rl > 0 && rp[*rl - 1] != '/') (*rl)--;
	if (*rl > 0) (*rl)--;
	rp[*rl] = '\0';
}

/* One path walk. d is where it stands. A name its directory keeps out of the
 * dentry cache stands in volatile_dentry, and the walk owns its inode
 * (prev_volatile) until the next step. f holds the irq flags of vfs_lock while
 * the walk has it. */

typedef struct {
	vfs_dentry_t *d;
	vfs_dentry_t *proc_root;   /* the task's root: ".." stops here */
	vfs_dentry_t *root_top;    /* what is mounted on proc_root, if anything */
	int           d_is_volatile;
	vfs_inode_t  *prev_volatile;
	vfs_dentry_t  volatile_dentry;
	char         *rp;
	int           rl;
	uint64_t      f;
	/* The mounts the walk came in through, innermost last: ".." at the root of
	 * one of them goes back out at its mountpoint. A bind's root keeps its real
	 * parent, so a walk that reached the same directory by its own path leaves
	 * by that parent instead. Dentries, not mounts: an umount meanwhile cannot
	 * free them under the walk. */
	walk_mnt_t    mnt[WALK_MNT_MAX];
	int           nmnt;
	int           outside;     /* started above proc_root, at the held directory's top */
	vfs_dentry_t *start;       /* where rp starts: that top, or proc_root */
} walk_t;

/* Step from d into whatever is mounted on it, as follow_mounts does, and note
 * each mount entered. Deeper than the stack, the oldest entry goes: a mount's
 * root still leads out through its ->parent, only a bind so far down would be
 * left the way of its source. Caller holds vfs_lock. */
static vfs_dentry_t *walk_enter_mounts(walk_t *w, vfs_dentry_t *d)
{
	mnt_namespace_t *ns = current_mnt_ns();
	vfs_mount_t *m;
	for (int guard = 0; guard < 64 && (m = mount_on(ns, d)); guard++) {
		if (w->nmnt == WALK_MNT_MAX) {
			memmove(&w->mnt[0], &w->mnt[1], sizeof(w->mnt[0]) * (WALK_MNT_MAX - 1));
			w->nmnt--;
		}
		w->mnt[w->nmnt].root = m->root;
		w->mnt[w->nmnt].mp   = d;
		w->nmnt++;
		d = m->root;
	}
	return d;
}

/* Step w to "..". Caller holds vfs_lock; this may drop and retake it. */
static void walk_dotdot(walk_t *w)
{
	if (w->d_is_volatile && w->d->inode && w->d->inode->dotdot_lookup) {
		/* below a no_cache directory the real parent can be many hops up */
		vfs_inode_t *dir = w->d->inode;
		spin_unlock_irq(&vfs_lock, w->f);
		vfs_inode_t *up = dir->iops->lookup(dir, "..");
		w->f = spin_lock_irq(&vfs_lock);
		if (up) {
			if (w->prev_volatile) vfs_inode_free(w->prev_volatile);
			w->prev_volatile = up;
			w->volatile_dentry.inode = up;
			rp_pop(w->rp, &w->rl);
			return;
		}
	}
	if (w->d_is_volatile) {
		/* step back to real parent */
		w->d = w->volatile_dentry.parent;
		w->d_is_volatile = 0;
		if (w->prev_volatile) { vfs_inode_free(w->prev_volatile); w->prev_volatile = NULL; }
		rp_pop(w->rp, &w->rl);
	} else if (w->d != w->proc_root && w->d != w->root_top) {
		/* out of each mount whose root this is, the way the walk came in */
		vfs_dentry_t *up = w->d;
		int n = w->nmnt;
		while (n && up == w->mnt[n - 1].root) up = w->mnt[--n].mp;
		if (up->parent) {
			w->d = up->parent;
			w->nmnt = n;
			rp_pop(w->rp, &w->rl);
		}
	}
}

/* The child called name of the directory w stands in: from the dentry cache, or
 * looked up and cached, or, where the directory allows no caching, in the
 * volatile slot. Caller holds vfs_lock. NULL: no such name, and the lock is
 * dropped. */
static vfs_dentry_t *walk_lookup(walk_t *w, const char *name)
{
	vfs_dentry_t *d = w->d;
	vfs_dentry_t *child = NULL;

	/* look in dentry children first (skip if parent marks children volatile) */
	if (!w->d_is_volatile && (!d->inode || !d->inode->no_cache)) {
		child = d->child;
		while (child && strcmp(child->name, name) != 0) child = child->next;
	}
	if (child) {
		lru_touch(child);
		/* Found in dentry cache; free any prior volatile inode */
		if (w->prev_volatile) { vfs_inode_free(w->prev_volatile); w->prev_volatile = NULL; }
		w->d_is_volatile = 0;
		return child;
	}

	if (!d->inode || !d->inode->iops || !d->inode->iops->lookup) {
		spin_unlock_irq(&vfs_lock, w->f);
		if (w->prev_volatile) { vfs_inode_free(w->prev_volatile); w->prev_volatile = NULL; }
		return NULL;
	}
	vfs_inode_t *dir_inode = d->inode;
	int parent_was_volatile = w->d_is_volatile;
	spin_unlock_irq(&vfs_lock, w->f);
	/* dir_inode may equal prev_volatile — do NOT free until after lookup */
	vfs_inode_t *in = dir_inode->iops->lookup(dir_inode, name);
	/* Now free the parent volatile inode (lookup is done with it) */
	if (w->prev_volatile) { vfs_inode_free(w->prev_volatile); w->prev_volatile = NULL; }
	w->f = spin_lock_irq(&vfs_lock);
	if (!in) {
		spin_unlock_irq(&vfs_lock, w->f);
		return NULL;
	}
	/* Cache only if parent is real (in dentry tree) and caching is allowed */
	/* Cached even past the limit: _dentry_create makes room by eviction, and
	 * the volatile slot (where chroot, bind and a chain of first-time names
	 * do not work) stays for directories that ask for it. */
	if (!parent_was_volatile && !dir_inode->no_cache) {
		/* Re-check: another task may have added this dentry while unlocked */
		child = d->child;
		while (child && strcmp(child->name, name) != 0) child = child->next;
		if (!child) {
			child = _dentry_create(d, name, in);
			if (!child) {
				vfs_inode_free(in);
				spin_unlock_irq(&vfs_lock, w->f);
				return NULL;
			}
		} else {
			vfs_inode_free(in);
		}
		w->d_is_volatile = 0;
		return child;
	}
	/* volatile: reuse the walk's one volatile dentry */
	/* in->cached: keep as-is; dynamic inodes are 0 via memset; static must stay 1 */
	w->volatile_dentry.inode   = in;
	/* a chain of volatile hops reuses this dentry: keep the real parent */
	w->volatile_dentry.parent  = d == &w->volatile_dentry ? w->volatile_dentry.parent : d;
	w->volatile_dentry.child   = NULL;
	w->volatile_dentry.next    = NULL;
	w->volatile_dentry.name[0] = '\0';
	w->prev_volatile = in;
	w->d_is_volatile = 1;
	return &w->volatile_dentry;
}

/* What an fd link of procfs names, as a copy of the open file's attributes for
 * stat. Strictly read-only: open (vfs_open_fds_rp) and link (vfs_link) refuse
 * it, and every attribute change through it is EPERM. */
static int snapshot_setattr(vfs_inode_t *in, const vfs_iattr_t *a)
{
	(void)in; (void)a;
	return -1;
}
static vfs_iops_t snapshot_iops = { .setattr = snapshot_setattr };

static vfs_inode_t *inode_snapshot(vfs_inode_t *src)
{
	vfs_stat_t st = {0};
	if (src->iops && src->iops->stat) {
		if (src->iops->stat(src, &st)) return NULL;
	} else {
		st.ino = src->ino; st.mode = src->mode; st.nlink = src->nlink;
		st.uid = src->uid; st.gid = src->gid; st.size = src->size; st.rdev = src->rdev;
	}
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino = st.ino; in->mode = st.mode; in->nlink = st.nlink;
	in->uid = st.uid; in->gid = st.gid; in->size = st.size; in->rdev = st.rdev;
	in->atime = src->atime; in->mtime = src->mtime; in->ctime = src->ctime;
	in->refcount = 1;
	in->iops = &snapshot_iops;
	return in;
}

/* child is an fd link of procfs (iops->link_file), met at the name ending at
 * end. Its text is walked as any symlink's (0) when it is the absolute path the
 * file was opened by under this same root. Otherwise, as the last name, the
 * walk ends on a copy of the file's attributes (1): Linux follows the link to
 * the open file itself, which a path from another root (opened before a
 * chroot), or a socket or pipe with no path, cannot name. Caller holds
 * vfs_lock; this drops and retakes it. -1: the walk fails, the lock dropped. */
static int walk_fdlink(walk_t *w, vfs_dentry_t *child, const char *end)
{
	const char *rest = end;
	while (*rest == '/') rest++;
	vfs_inode_t *lin = child->inode, *in = NULL;
	spin_unlock_irq(&vfs_lock, w->f);
	vfs_file_t *f = lin->iops->link_file(lin);
	int r = 0;
	if (f && !*rest && !(f->path[0] == '/' && f->opened_root == w->proc_root)) {
		in = f->inode ? inode_snapshot(f->inode) : NULL;
		r = in ? 1 : -1;
	}
	if (f) fput(f);
	if (r == -1) {
		if (w->prev_volatile) { vfs_inode_free(w->prev_volatile); w->prev_volatile = NULL; }
		return -1;
	}
	w->f = spin_lock_irq(&vfs_lock);
	if (r == 1) {
		/* in takes the link's place: in the volatile slot, under the link's directory */
		if (child != &w->volatile_dentry) w->volatile_dentry.parent = w->d;
		if (w->prev_volatile) vfs_inode_free(w->prev_volatile);
		w->volatile_dentry.inode = in;
		w->volatile_dentry.child = NULL;
		w->volatile_dentry.next  = NULL;
		w->prev_volatile = in;
		w->d_is_volatile = 1;
		w->d = &w->volatile_dentry;
	}
	return r;
}

/* child is a symlink met at the name ending at end, in work. Put its target in
 * place of the names walked so far and point *pp at where the walk goes on.
 * Caller holds vfs_lock. -1: the walk fails, and the lock is dropped. */
static int walk_symlink(walk_t *w, vfs_dentry_t *child, char *work, const char *end,
                        const char **pp, unsigned long *mflags, int *depth)
{
	if (++*depth > 40) {                    /* Linux's MAXSYMLINKS */
		spin_unlock_irq(&vfs_lock, w->f);
		walk_fail(ELOOP);
		return -1;
	}
	if (!child->inode->iops || !child->inode->iops->readlink) {
		spin_unlock_irq(&vfs_lock, w->f);
		return -1;
	}
	if (mflags) {
		const char *rest = end;
		while (*rest == '/') rest++;
		if (!*rest) *mflags |= dentry_mnt_flags(w->d);
	}
	char target[256];
	/* readlink may read the disk and sleep on its lock: not under vfs_lock.
	 * The inode stays (this walk holds its generation), as for lookup. */
	vfs_inode_t *lin = child->inode;
	spin_unlock_irq(&vfs_lock, w->f);
	int lrc = lin->iops->readlink(lin, target, sizeof(target));
	w->f = spin_lock_irq(&vfs_lock);
	if (lrc != 0) {
		spin_unlock_irq(&vfs_lock, w->f);
		return -1;
	}
	/* splice: work = target + remaining(end) */
	int tlen = strlen(target);
	int rlen = strlen(end);
	if (tlen + rlen + 2 >= PATH_MAX) {
		spin_unlock_irq(&vfs_lock, w->f);
		return -1;
	}
	/* end points into work: move the rest before the target lands */
	if (rlen > 0) {
		int gap = end[0] != '/';
		memmove(work + tlen + gap, end, rlen + 1);
		if (gap) work[tlen] = '/';
	} else {
		work[tlen] = '\0';
	}
	memcpy(work, target, tlen);
	if (target[0] == '/') {
		if (w->rp) { w->rl = 0; w->rp[0] = '\0'; }   /* an absolute target starts over at the root */
		w->d = w->proc_root;
		w->nmnt = 0;
		if (w->outside) w->outside = 2;
		w->start = w->proc_root;
		*pp = work + 1;
		w->d_is_volatile = 0;
		/* Absolute symlink restart — free any volatile context */
		if (w->prev_volatile) { vfs_inode_free(w->prev_volatile); w->prev_volatile = NULL; }
	} else {
		*pp = work;
		/* The walk goes on in the link's directory. A link that was not
		 * cached stood in the volatile slot, and its directory is real. */
		if (w->d != &w->volatile_dentry) {
			w->d_is_volatile = 0;
			if (w->prev_volatile) { vfs_inode_free(w->prev_volatile); w->prev_volatile = NULL; }
		}
	}
	return 0;
}

/* Set w up to walk path from the task's root. *workp gets the walk's own copy of
 * path, which symlinks rewrite. 0: vfs_lock is held (w->f). -1: no walk, and the
 * lock is not held. */
static int walk_init(walk_t *w, const char *path, char *rp, char **workp)
{
	w->rp = rp;
	w->rl = 0;
	if (rp) rp[0] = '\0';
	walk_begin();
	w->f = spin_lock_irq(&vfs_lock);
	if (!path || path[0] != '/' || !root_dentry) {
		spin_unlock_irq(&vfs_lock, w->f);
		return -1;
	}

	/* mutable work buffer so symlink splicing can rewrite the path. Heap-backed
	 * (PATH_MAX): a stack array this large would blow the kernel stack once
	 * overlayfs lookup re-enters this resolver. */
	char *work = kmalloc(PATH_MAX);
	if (!work) { spin_unlock_irq(&vfs_lock, w->f); return -1; }
	int wi = 0;
	while (wi < PATH_MAX - 1 && path[wi]) { work[wi] = path[wi]; wi++; }
	if (path[wi]) { kfree(work); spin_unlock_irq(&vfs_lock, w->f); return -1; }
	work[wi] = '\0';

	/* chroot: an absolute path is relative to the process's own root, not the
	 * global one. NULL ->root means it sees "/" as the global root. */
	process_t *_cur = task_current();
	vfs_dentry_t *_root = fs_root_of((struct process *)_cur);
	w->proc_root = _root ? _root : root_dentry;

	/* a mount's root has ->parent set to outside the mount, so a process root that
	 * is a mountpoint is left through this dentry, not through proc_root */
	w->root_top = follow_mounts(w->proc_root);
	vfs_dentry_t *top = walk_top_for(_cur, path);
	w->outside = top != NULL;
	w->d = top ? follow_mounts(top) : w->root_top;
	w->start = top ? top : w->proc_root;
	w->d_is_volatile = 0;
	w->prev_volatile = NULL;
	w->nmnt = 0;
	*workp = work;
	return 0;
}

/* The end of a walk: the mount flags and the path it took, for those who asked,
 * and the volatile dentry moved out of w, which dies with the caller's frame. */
static vfs_dentry_t *walk_finish(walk_t *w, vfs_dentry_t *reached, unsigned long *mflags, char *rp)
{
	if (mflags) {
		uint64_t f = spin_lock_irq(&vfs_lock);
		*mflags |= dentry_mnt_flags(w->d ? w->d : reached);
		spin_unlock_irq(&vfs_lock, f);
	}
	if (rp) {
		if (!w->d || w->rl < 0) rp[0] = '\0';
		else if (w->rl == 0) { rp[0] = '/'; rp[1] = '\0'; }
	}
	/* volatile_dentry dies with this frame; callers keep reading d->inode */
	if (w->d == &w->volatile_dentry) {
		vfs_dentry_t *slot = volatile_slot();
		slot->inode  = w->volatile_dentry.inode;
		slot->parent = w->volatile_dentry.parent;
		slot->child  = NULL;
		slot->next   = NULL;
		w->d = slot;
	}
	return w->d;
}

static vfs_dentry_t *resolve_walk_ms(const char *path, unsigned long *mflags, char *rp,
                                     walk_mnts_t *ms);
static vfs_dentry_t *resolve_walk_rp(const char *path, unsigned long *mflags, char *rp)
{
	return resolve_walk_ms(path, mflags, rp, NULL);
}

/* With ms set, also the mounts the walk ended inside (for vfs_dirpos). */
static vfs_dentry_t *resolve_walk_ms(const char *path, unsigned long *mflags, char *rp,
                                     walk_mnts_t *ms)
{
	walk_t w;
	char *work;
	if (ms) { ms->n = 0; ms->start = NULL; }
	if (walk_init(&w, path, rp, &work)) return NULL;
	const char   *p = work + 1;   /* skip leading '/' */
	int symlink_depth = 0;
	vfs_dentry_t *reached = w.d;

	while (*p) {
		reached = w.d;
		/* skip consecutive slashes */
		while (*p == '/') p++;
		if (!*p) break;

		/* extract next component (null-terminated in tmp) */
		char tmp[256];
		int i = 0;
		while (i < 255 && p[i] && p[i] != '/') { tmp[i] = p[i]; i++; }
		tmp[i] = '\0';
		const char *end = p + i;
		if (*end && *end != '/') {              /* a name over NAME_MAX */
			spin_unlock_irq(&vfs_lock, w.f);
			if (w.prev_volatile) { vfs_inode_free(w.prev_volatile); w.prev_volatile = NULL; }
			walk_fail(ENAMETOOLONG);
			w.d = NULL; goto out;
		}

		size_t len = (size_t)i;
		if (len == 0) break;

		/* handle . and .. */
		if (len == 1 && tmp[0] == '.') { p = end; continue; }
		if (len == 2 && tmp[0] == '.' && tmp[1] == '.') {
			walk_dotdot(&w);
			p = end; continue;
		}

		/* A path component can only be resolved against a directory. Descending
		 * into a non-directory (e.g. "/file/x") is ENOTDIR — and crucially, a
		 * filesystem's lookup op may reinterpret the inode's private data by type
		 * (ramfs reads it as a dir entry list), so calling it on a regular file
		 * walks file bytes as a pointer chain and faults. Reject here. */
		int notdir = w.d->inode && !S_ISDIR(w.d->inode->mode);
		if (notdir || (!walk_anchored(work, end, w.outside) && !vfs_may_search(w.d->inode))) {
			spin_unlock_irq(&vfs_lock, w.f);
			if (notdir) walk_fail(ENOTDIR);
			if (w.prev_volatile) { vfs_inode_free(w.prev_volatile); w.prev_volatile = NULL; }
			w.d = NULL; goto out;
		}

		vfs_dentry_t *child = walk_lookup(&w, tmp);
		if (!child) { w.d = NULL; goto out; }

		/* follow symlinks — except magic links that are also openable
		 * objects (nsfs-style, e.g. /proc/<pid>/ns/uts): those carry fops,
		 * so resolve to the link itself and let open() act on the object
		 * rather than following its text. */
		if (S_ISLNK(child->inode->mode) && !child->inode->fops) {
			int fr = child->inode->iops && child->inode->iops->link_file ?
			         walk_fdlink(&w, child, end) : 0;
			if (fr < 0) { w.d = NULL; goto out; }
			if (fr > 0) { p = end; continue; }
			if (walk_symlink(&w, child, work, end, &p, mflags, &symlink_depth) != 0) {
				w.d = NULL; goto out;
			}
			continue;
		}

		w.d = walk_enter_mounts(&w, child);
		rp_push(rp, &w.rl, tmp, (int)len);
		p = end;
	}

	/* prev_volatile (if set) is the FINAL component's inode — freed by vfs_close */
	spin_unlock_irq(&vfs_lock, w.f);
out:
	kfree(work);
	if (ms) {
		ms->n = w.d ? w.nmnt : 0;
		ms->start = w.start;
		memcpy(ms->mnt, w.mnt, sizeof(w.mnt[0]) * (size_t)ms->n);
	}
	return walk_finish(&w, reached, mflags, rp);
}

/* The cached dentry of name in dir, following neither symlinks nor mounts. NULL
 * if dir cannot hold cached children or the name is absent. */
vfs_dentry_t *vfs_dentry_child(vfs_dentry_t *dir, const char *name)
{
	vfs_inode_t *di = dir->inode;
	if (!di || di->no_cache || !di->iops || !di->iops->lookup) return NULL;
	uint64_t f = spin_lock_irq(&vfs_lock);
	vfs_dentry_t *c = dir->child;
	while (c && strcmp(c->name, name) != 0) c = c->next;
	spin_unlock_irq(&vfs_lock, f);
	if (c) return c;

	vfs_inode_t *in = di->iops->lookup(di, name);
	if (!in) return NULL;
	f = spin_lock_irq(&vfs_lock);
	for (c = dir->child; c && strcmp(c->name, name) != 0; c = c->next) ;
	if (c) vfs_inode_free(in);
	else if (!(c = _dentry_create(dir, name, in))) vfs_inode_free(in);   /* unlinked meanwhile */
	spin_unlock_irq(&vfs_lock, f);
	return c;
}

vfs_dentry_t *vfs_dentry_resolve(const char *path)
{
	return resolve_walk(path, NULL);
}

/* Split an absolute path into its parent directory and final component. Returns
 * a kmalloc'd copy of the parent dir (caller frees) and points *name at the last
 * component within the original path. NULL on OOM. The parent can be up to
 * PATH_MAX, so it cannot live in a fixed stack buffer (kernel-stack overflow). */
static char *vfs_parent_alloc(const char *path, const char **name)
{
	int len = (int)strlen(path);
	const char *slash = path + (len > 0 ? len - 1 : 0);
	while (slash > path && *slash != '/') slash--;
	int dlen = (int)(slash - path);
	char *dir = kmalloc(PATH_MAX);
	if (!dir) return NULL;
	if (dlen == 0) { dir[0] = '/'; dir[1] = '\0'; }
	else           { memcpy(dir, path, (size_t)dlen); dir[dlen] = '\0'; }
	if (name) *name = slash + 1;
	return dir;
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
	/* copy the mount entries; the underlying filesystems stay shared. Order is
	 * kept: stacked mounts resolve newest first, and teardown runs the same way. */
	vfs_mount_t **tail = &ns->mounts;
	uint64_t f = spin_lock_irq(&vfs_lock);
	for (vfs_mount_t *m = old->mounts; m; m = m->next) {
		vfs_mount_t *nm = kmalloc(sizeof(*nm));
		if (!nm) continue;
		*nm = *m;
		nm->fstype = nm->fstype_name;   /* re-point to this copy's own buffer */
		if (nm->get_sb) nm->get_sb(nm->priv);   /* shared sb gains a reference */
		if (nm->bdev) block_get(nm->bdev);
		if (nm->mountpoint) nm->mountpoint->pins++;
		if (nm->pin_root)   nm->root->pins++;
		nm->next = NULL;
		*tail    = nm;
		tail     = &nm->next;
	}
	spin_unlock_irq(&vfs_lock, f);
	return ns;
}

/* The entry itself; whatever filesystem it showed is the caller's business. */
static void mount_free(vfs_mount_t *m)
{
	if (m->bdev) block_put(m->bdev);
	kfree(m);
}

void mnt_ns_put(mnt_namespace_t *ns)
{
	if (!ns || ns == &init_mnt_ns) return;
	uint64_t f = spin_lock_irq(&mntns_lock);
	int last = (--ns->refcount == 0);
	spin_unlock_irq(&mntns_lock, f);
	if (!last) return;
	/* Unpin the whole table first: a mount here may sit on a dentry of another. */
	for (vfs_mount_t *m = ns->mounts; m; m = m->next) {
		if (m->mountpoint) vfs_dentry_unpin(m->mountpoint);
		if (m->pin_root)   vfs_dentry_unpin(m->root);
	}
	for (vfs_mount_t *m = ns->mounts, *nx; m; m = nx) {
		nx = m->next;
		/* with no get_sb the superblock is not counted, and other tables may hold it */
		if (m->get_sb && m->kill_sb) m->kill_sb(m->priv);
		mount_free(m);
	}
	kfree(ns);
}

/* The newest mount on d in ns, or NULL. Caller holds vfs_lock. */
static vfs_mount_t *mount_on(mnt_namespace_t *ns, vfs_dentry_t *d)
{
	for (vfs_mount_t *m = ns->mounts; m; m = m->next)
		if (m->mountpoint == d) return m;
	return NULL;
}

/* If d is a mountpoint in ns, return the mount's root (following stacked
 * mounts). Caller holds vfs_lock. */
static vfs_dentry_t *follow_mounts_in(mnt_namespace_t *ns, vfs_dentry_t *d)
{
	vfs_mount_t *m;
	for (int guard = 0; guard < 64 && (m = mount_on(ns, d)); guard++) d = m->root;
	return d;
}

static vfs_dentry_t *follow_mounts(vfs_dentry_t *d)
{
	return follow_mounts_in(current_mnt_ns(), d);
}

int vfs_mount(const char *path, const char *fstype, void *data)
{
	return vfs_mount_flags(path, fstype, data, 0);
}

/* A filled mount of fstype with no place in any tree yet; NULL on failure. */
static vfs_mount_t *mount_alloc(const char *path, const char *fstype, void *data, unsigned long flags)
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
	if (!fill) return NULL;

	vfs_mount_t *mnt = kmalloc(sizeof(*mnt));
	if (!mnt) return NULL;
	/* Own the fstype string: callers may pass a stack buffer (userspace mount(2)
	 * via syscall) as well as static literals (kernel mounts). */
	int _fl = 0; while (fstype && fstype[_fl] && _fl < 31) { mnt->fstype_name[_fl] = fstype[_fl]; _fl++; }
	mnt->fstype_name[_fl] = '\0';
	mnt->fstype     = mnt->fstype_name;
	mnt->flags      = flags;
	{ int _p = 0; while (path && path[_p] && _p < 255) { mnt->mnt_path[_p] = path[_p]; _p++; } mnt->mnt_path[_p] = '\0'; }
	mnt->priv       = data;
	mnt->kill_sb    = NULL;
	mnt->get_sb     = NULL;
	mnt->busy       = NULL;
	mnt->pin_root   = 0;
	mnt->ro_lock    = 0;
	mnt->mountpoint = NULL;
	mnt->root       = NULL;
	mnt->next       = NULL;
	mnt->bdev       = NULL;
	mnt->sdev       = __atomic_add_fetch(&sdev_next, 1, __ATOMIC_SEQ_CST);

	if (fill(mnt, data) != 0) { kfree(mnt); return NULL; }
	if (data && block_is_device(data)) {
		mnt->bdev = data;
		block_get(data);
	}
	return mnt;
}

int vfs_mount_flags(const char *path, const char *fstype, void *data, unsigned long flags)
{
	vfs_mount_t *mnt = mount_alloc(path, fstype, data, flags);
	if (!mnt) return -1;

	mnt_namespace_t *mns = current_mnt_ns();
	uint64_t f = spin_lock_irq(&vfs_lock);
	if (strcmp(path, "/") == 0) {
		root_dentry = mnt->root;
	} else {
		spin_unlock_irq(&vfs_lock, f);
		vfs_dentry_t *mp = vfs_dentry_resolve(path);
		f = spin_lock_irq(&vfs_lock);
		if (!mp || vfs_dentry_is_volatile(mp)) {
			spin_unlock_irq(&vfs_lock, f);
			mount_free(mnt);
			return -1;
		}
		mnt->mountpoint  = mp;
		mnt->orig_inode  = mp->inode;
		mp->pins++;
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

/* Mount on the dentry mp in ns, which need not be the caller's namespace. path
 * names mp as ns sees it, for /proc/mounts-style lookups only. */
vfs_mount_t *vfs_mount_at(mnt_namespace_t *ns, vfs_dentry_t *mp, const char *path,
                          const char *fstype, void *data, unsigned long flags)
{
	vfs_mount_t *mnt = mount_alloc(path, fstype, data, flags);
	if (!mnt) return NULL;
	uint64_t f = spin_lock_irq(&vfs_lock);
	mnt->mountpoint = mp;
	mnt->orig_inode = mp->inode;
	mp->pins++;
	if (mnt->root) mnt->root->parent = mp->parent;
	mnt->next  = ns->mounts;
	ns->mounts = mnt;
	spin_unlock_irq(&vfs_lock, f);
	return mnt;
}

/* Point m at dentry d of a tree the caller's namespace mounts: the dentry stays
 * while m does, and a superblock that counts references stays with it, so the
 * tree outlives its original mount. */
static void mount_hold_tree(vfs_mount_t *m, vfs_dentry_t *d)
{
	mnt_namespace_t *ns = current_mnt_ns();
	uint64_t f = spin_lock_irq(&vfs_lock);
	m->root = d;
	m->pin_root = 1;
	d->pins++;
	vfs_mount_t *src = NULL;
	for (vfs_dentry_t *c = d; c && !src; c = c->parent)
		for (vfs_mount_t *i = ns->mounts; i; i = i->next)
			if (i->root == c) { src = i; break; }
	if (src && src->get_sb) {
		m->priv    = src->priv;
		m->get_sb  = src->get_sb;
		m->kill_sb = src->kill_sb;
		m->busy    = src->busy;
		m->get_sb(m->priv);
	}
	m->sdev = src ? src->sdev : __atomic_add_fetch(&sdev_next, 1, __ATOMIC_SEQ_CST);
	if (src && src->bdev) {                /* a bind keeps the disk in use as well */
		m->bdev = src->bdev;
		block_get(m->bdev);
	}
	spin_unlock_irq(&vfs_lock, f);
}

/* Mount the existing directory dir, from the caller's view, in ns. mp is where:
 * NULL when dir is already there, and the entry then only claims the subtree for
 * flags, as mnt_ns_rooted does for the whole view. */
int vfs_bind(mnt_namespace_t *ns, vfs_dentry_t *mp, vfs_dentry_t *dir, const char *path,
             unsigned long flags)
{
	vfs_mount_t *m = kmalloc(sizeof(*m));
	if (!m) return -ENOMEM;
	memset(m, 0, sizeof(*m));
	memcpy(m->fstype_name, "bind", 5);
	m->fstype = m->fstype_name;
	m->flags  = flags;
	int l = 0;
	while (path[l] && l < 255) { m->mnt_path[l] = path[l]; l++; }
	mount_hold_tree(m, dir);
	uint64_t f = spin_lock_irq(&vfs_lock);
	if (mp) {
		m->mountpoint = mp;
		m->orig_inode = mp->inode;
		mp->pins++;
	}
	m->next = ns->mounts;
	ns->mounts = m;
	spin_unlock_irq(&vfs_lock, f);
	return 0;
}

vfs_dentry_t *vfs_follow_in(mnt_namespace_t *ns, vfs_dentry_t *d)
{
	uint64_t f = spin_lock_irq(&vfs_lock);
	d = follow_mounts_in(ns, d);
	spin_unlock_irq(&vfs_lock, f);
	return d;
}

/* A mount table holding only root. Claiming root as a mount with no mountpoint
 * adds no path, but makes flags reach everything below it through
 * dentry_mnt_flags, and leaving the creator's mounts out hides all of them. */
mnt_namespace_t *mnt_ns_rooted(vfs_dentry_t *root, unsigned long flags)
{
	mnt_namespace_t *ns = kmalloc(sizeof(*ns));
	vfs_mount_t *m = kmalloc(sizeof(*m));
	if (!ns || !m) { kfree(ns); kfree(m); return NULL; }
	memset(m, 0, sizeof(*m));
	memcpy(m->fstype_name, "rootfs", 7);
	m->fstype   = m->fstype_name;
	m->flags    = flags;
	mount_hold_tree(m, root);
	m->mnt_path[0] = '/';
	ns->refcount = 1;
	ns->inum     = ns_inum_alloc();
	ns->mounts   = m;
	return ns;
}

/* Reconstruct a dentry's absolute path by walking parents (caller holds
 * vfs_lock). Root (no parent, or self-parent) yields "/". */
static void dentry_abspath(vfs_dentry_t *d, char *out, size_t sz)
{
	vfs_dentry_t *chain[64];
	int n = 0;
	process_t *cur = task_current();
	for (vfs_dentry_t *c = d; c && n < 64; c = c->parent) {
		if (!c->parent || c->parent == c) break;   /* reached root */
		if (cur && c == fs_root_of((struct process *)cur)) break;   /* nothing above it is visible */
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
/* The root this task walks from: chroot and pivot_root move it off the boot root. */
static vfs_dentry_t *my_root(void)
{
	process_t *c = task_current();
	vfs_dentry_t *r = fs_root_of((struct process *)c);
	return r ? r : root_dentry;
}

/* The position of a directory f just opened, which the walk ended on at d */
static struct vfs_dirpos *dirpos_new(vfs_dentry_t *d, const walk_mnts_t *ms)
{
	if (!d) return NULL;
	if (vfs_dentry_is_volatile(d)) d = NULL;
	if (!d && !ms->start) return NULL;
	struct vfs_dirpos *pos = kmalloc(sizeof(*pos) + sizeof(walk_mnt_t) * (size_t)ms->n);
	if (!pos) return NULL;
	if (!dentry_hold(d ? d : ms->start)) { kfree(pos); return NULL; }
	pos->d = d;
	pos->start = ms->start;
	pos->nmnt = ms->n;
	memcpy(pos->mnt, ms->mnt, sizeof(walk_mnt_t) * (size_t)ms->n);
	return pos;
}

/* c is the root of a mount pos came in through, or of one in ns: its mountpoint
 * stands for it (name and parent). Only a mount still there counts; a stale
 * entry is compared, never followed. Caller holds vfs_lock. */
static vfs_dentry_t *dirpos_mountpoint(struct vfs_dirpos *pos, int *n,
                                       mnt_namespace_t *ns, vfs_dentry_t *c)
{
	while (*n && c == pos->mnt[*n - 1].root) {
		vfs_dentry_t *mp = pos->mnt[--*n].mp;
		for (vfs_mount_t *m = ns->mounts; m; m = m->next)
			if (m->mountpoint == mp && m->root == c) return mp;
	}
	if (c->pprev) return NULL;   /* in a child list: no mount's own root */
	for (vfs_mount_t *m = ns->mounts; m; m = m->next)
		if (m->root == c && m->mountpoint) return m->mountpoint;
	return NULL;
}

/* name in front of the path being built backwards at out + *at */
static int path_prepend(char *out, int *at, const char *name)
{
	int len = (int)strlen(name);
	if (*at - len - 1 < 0) return -ENAMETOOLONG;
	*at -= len;
	memcpy(out + *at, name, (size_t)len);
	out[--*at] = '/';
	return 0;
}

/* A directory not cached: its f->path, from the root it was opened under. That
 * is the caller's root as a rule (1: go by f->path); after a chroot it is the
 * start of the walk, above the new root. */
static int dirpos_by_path(vfs_file_t *f, vfs_dentry_t *root, char *out, vfs_dentry_t **top)
{
	vfs_dentry_t *s = f->at->start;
	if (s == root || f->path[0] != '/') return 1;
	if (s->retired) return -ENOENT;
	memcpy(out, f->path, strlen(f->path) + 1);
	*top = s;
	return 0;
}

/* Where the open directory f stands now (Linux: a dirfd lookup starts at the
 * directory, not at its name), written to out (PATH_MAX) as a path for this
 * task's walk: from its root when f lies below it; else (f was opened before a
 * chroot) from the top of the tree above f, which *top then names. 1: f has no
 * position (not a directory, or one the cache does not keep), go by f->path.
 * -2: removed. -36: too long. */
int vfs_file_dirpath(vfs_file_t *f, char *out, vfs_dentry_t **top)
{
	struct vfs_dirpos *pos = f->at;
	if (!pos) return 1;
	vfs_dentry_t *root = my_root();
	if (!pos->d) return dirpos_by_path(f, root, out, top);
	mnt_namespace_t *ns = current_mnt_ns();
	walk_begin();   /* the parents read below stay until this syscall ends */
	uint64_t fl = spin_lock_irq(&vfs_lock);
	vfs_dentry_t *root_top = follow_mounts(root), *c = pos->d, *mp;
	int n = pos->nmnt, at = PATH_MAX - 1, r = 0;
	out[at] = '\0';
	*top = NULL;
	for (int guard = 0; !r; guard++) {
		if (c->retired || guard > PATH_MAX / 2) r = -2;
		else if (c == root || c == root_top) break;
		else if ((mp = dirpos_mountpoint(pos, &n, ns, c))) c = mp;
		else if (!c->parent || c->parent == c) { *top = c; break; }
		else if (!(r = path_prepend(out, &at, c->name))) c = c->parent;
	}
	spin_unlock_irq(&vfs_lock, fl);
	if (r) return r;
	if (at == PATH_MAX - 1) out[--at] = '/';
	memmove(out, out + at, (size_t)(PATH_MAX - at));
	return 0;
}

size_t vfs_proc_mounts(char *buf, size_t sz)
{
	mnt_namespace_t *mns = current_mnt_ns();
	uint64_t f = spin_lock_irq(&vfs_lock);
	size_t pos = 0;
	for (vfs_mount_t *m = mns->mounts; m; m = m->next) {
		if (m->root == root_dentry && my_root() != root_dentry) continue;   /* outside this root */
		char path[256];
		if (m->root == my_root() || (m->mountpoint && m->mountpoint == my_root())) { path[0] = '/'; path[1] = '\0'; }   /* what pivot_root made the root */
		else if (!m->mountpoint) memcpy(path, m->mnt_path, sizeof(path));
		else                dentry_abspath(m->mountpoint, path, sizeof(path));
		if (path[0] == '/' && path[1] == '/') continue;   /* hangs from a root left behind */
		const char *fs = m->fstype ? m->fstype : "none";
		const char *cols[] = { fs, " ", path, " ", fs, m->flags & MS_RDONLY ? " ro 0 0\n" : " rw 0 0\n" };
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
		if (m->root == root_dentry && my_root() != root_dentry) continue;   /* outside this root */
		char path[256];
		if (m->root == my_root() || (m->mountpoint && m->mountpoint == my_root())) { path[0] = '/'; path[1] = '\0'; }   /* what pivot_root made the root */
		else if (!m->mountpoint) memcpy(path, m->mnt_path, sizeof(path));
		else                dentry_abspath(m->mountpoint, path, sizeof(path));
		if (path[0] == '/' && path[1] == '/') continue;   /* hangs from a root left behind */
		const char *fs = m->fstype ? m->fstype : "none";
		char idbuf[12]; int n = 0; int v = id;
		char tmp[12]; int t = 0;
		do { tmp[t++] = '0' + v % 10; v /= 10; } while (v);
		while (t) idbuf[n++] = tmp[--t];
		idbuf[n] = '\0';
		char devbuf[12]; n = 0; v = (int)m->sdev; t = 0;
		do { tmp[t++] = '0' + v % 10; v /= 10; } while (v);
		while (t) devbuf[n++] = tmp[--t];
		devbuf[n] = '\0';
		/* "<id> 1 0:<dev> / <path> rw - <fs> <fs> rw\n": the dev is the one stat reports */
		const char *cols[] = { idbuf, " 1 0:", devbuf, " / ", path, " rw - ", fs, " ", fs, " rw\n" };
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
		if (m->root == root_dentry && my_root() != root_dentry) continue;   /* outside this root */
		char mp[256];
		if (!m->mountpoint || m->root == my_root() || m->mountpoint == my_root()) { mp[0] = '/'; mp[1] = '\0'; }
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

/* The mount covering an absolute path: longest mountpoint prefix, same rule as
 * vfs_path_fstype. Caller holds vfs_lock. */
static vfs_mount_t *mount_for_path(const char *path)
{
	mnt_namespace_t *mns = current_mnt_ns();
	vfs_mount_t *best = NULL;
	size_t bestlen = 0;
	for (vfs_mount_t *m = mns->mounts; m; m = m->next) {
		const char *mp = m->mnt_path[0] ? m->mnt_path : "/";
		size_t l = strlen(mp);
		if (l == 0) continue;
		if (strncmp(path, mp, l) != 0) continue;
		if (l > 1 && path[l] != '/' && path[l] != '\0') continue;
		if (l >= bestlen) { bestlen = l; best = m; }
	}
	return best;
}

/* MS_* of the mount an absolute path reaches. */
unsigned long vfs_path_mnt_flags(const char *path)
{
	if (!path || path[0] != '/') return 0;
	unsigned long fl = 0;
	vfs_dentry_t *d = resolve_walk(path, &fl);
	if (vfs_dentry_is_volatile(d)) vfs_inode_free(d->inode);
	return fl;
}

int vfs_remount(const char *path, unsigned long flags)
{
	if (!path || path[0] != '/') return -EINVAL;
	uint64_t f = spin_lock_irq(&vfs_lock);
	vfs_mount_t *m = mount_for_path(path);
	if (m) m->flags = (flags & ~MS_REMOUNT) | (m->ro_lock ? MS_RDONLY : 0);
	spin_unlock_irq(&vfs_lock, f);
	return m ? 0 : -22;
}

/* Per-filesystem statfs: filesystems that track real block/inode usage
 * (tmpfs, ext2/4) supply iops->statfs; returns <0 if the fs has no op, so the
 * caller falls back to nominal counts. */
int vfs_statfs_inode(vfs_inode_t *in, vfs_statfs_t *out)
{
	if (!in || !in->iops || !in->iops->statfs) return -ENOSYS;
	return in->iops->statfs(in, out);
}

int vfs_statfs_path(const char *path, vfs_statfs_t *out)
{
	vfs_dentry_t *d = vfs_dentry_resolve(path);
	if (!d || !d->inode) return -ENOENT;
	return vfs_statfs_inode(d->inode, out);
}

int vfs_umount(const char *path)
{
	if (!path || path[0] != '/') return -EINVAL;

	/* resolve follows the mount, so this is the mounted root dentry */
	vfs_dentry_t *root = vfs_dentry_resolve(path);
	if (!root) return -ENOENT;
	if (proc_root_under(root)) return -EBUSY;   /* a chroot sits in this tree */

	mnt_namespace_t *mns = current_mnt_ns();
	uint64_t f = spin_lock_irq(&vfs_lock);
	vfs_mount_t *prev = NULL;
	vfs_mount_t *mnt  = mns->mounts;
	while (mnt) {
		if (mnt->root == root) break;
		prev = mnt;
		mnt  = mnt->next;
	}
	if (!mnt) { spin_unlock_irq(&vfs_lock, f); return -EINVAL; }
	/* a bind holds its own root pinned; that hold is not a user of the tree */
	if (mnt->pin_root) root->pins--;
	int busy = dentry_tree_pinned(root);
	if (mnt->pin_root) root->pins++;
	/* open files and mapped pages; the boot root's state outlives its entry */
	if (!busy && mnt->busy && root != root_dentry) busy = mnt->busy(mnt->priv);
	if (busy) { spin_unlock_irq(&vfs_lock, f); return -EBUSY; } /* EBUSY */

	if (prev) prev->next = mnt->next;
	else      mns->mounts = mnt->next;
	void (*kill_sb)(void *) = mnt->kill_sb;
	void *priv = mnt->priv;
	if (mnt->mountpoint) mnt->mountpoint->pins--;
	if (mnt->pin_root) root->pins--;       /* the bind's hold on its tree */
	/* after pivot_root the boot root leaves the table but stays the walk's default */
	if (root == root_dentry) kill_sb = NULL;
	spin_unlock_irq(&vfs_lock, f);

	if (kill_sb) kill_sb(priv);
	mount_free(mnt);
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
	t->rlim_nofile = FD_MAX;
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
	n->rlim_nofile = old->rlim_nofile;
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

/* Drop every fd of t that is not in keep[0..n). */
void fd_table_keep_only(fd_table_t *t, const int *keep, int n)
{
	for (int fd = 0; fd < t->cap; fd++) {
		if (!t->files[fd]) continue;
		int kept = 0;
		for (int i = 0; i < n && !kept; i++) kept = keep[i] == fd;
		if (kept) continue;
		vfs_file_t *f = t->files[fd];
		t->files[fd] = NULL;
		BM_CLR(t->open_bm, fd);
		BM_CLR(t->cloexec, fd);
		vfs_file_put(f);
	}
}

int fd_alloc_from(fd_table_t *t, vfs_file_t *f, int start)
{
	if (start < 0) return -EINVAL;
	uint64_t fl = spin_lock_irq(&t->lock);
	for (;;) {
		for (int i = start; i < t->cap; i++) {
			if (i >= t->rlim_nofile) { spin_unlock_irq(&t->lock, fl); return -EMFILE; } /* EMFILE */
			if (!BM_TST(t->open_bm, i)) {
				t->files[i] = f;
				BM_SET(t->open_bm, i);
				atomic_fetch_add32((volatile uint32_t *)&f->refcount, 1);
				spin_unlock_irq(&t->lock, fl);
				return i;
			}
		}
		if (t->cap >= t->rlim_nofile) { spin_unlock_irq(&t->lock, fl); return -EMFILE; }
		if (fd_table_grow(t) != 0) { spin_unlock_irq(&t->lock, fl); return -ENOMEM; }
	}
}

int fd_alloc(fd_table_t *t, vfs_file_t *f)
{
	return fd_alloc_from(t, f, 0);
}

int fd_alloc_at(fd_table_t *t, vfs_file_t *f, int fd)
{
	if (fd < 0) return -EBADF;
	uint64_t fl = spin_lock_irq(&t->lock);
	if (fd >= t->rlim_nofile) { spin_unlock_irq(&t->lock, fl); return -EMFILE; } /* EMFILE */
	while (fd >= t->cap)
		if (fd_table_grow(t) != 0) { spin_unlock_irq(&t->lock, fl); return -ENOMEM; }
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
	vfs_posix_drop_ofd(f);
	if (f->inode && f->inode->fops && f->inode->fops->close)
		f->inode->fops->close(f);
	if (!vfs_inode_drop_open(f->inode) && f->inode && !f->inode->cached)
		vfs_inode_free(f->inode);
	file_free(f);
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
	return vfs_open_fds(p->fds, task_fsuid(p), task_fsgid(p), fs_umask_of((struct process *)p), path, flags, mode);
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

static int vfs_open_fds_rp(fd_table_t *fds, uid_t uid, gid_t gid, mode_t umask,
                           const char *path, int flags, mode_t mode, unsigned long mflags, char *rp);
int vfs_open_fds(fd_table_t *fds, uid_t uid, gid_t gid, mode_t umask,
                 const char *path, int flags, mode_t mode)
{
	if (!fds) return -1;

	unsigned long mflags = vfs_path_mnt_flags(path);
	if (mflags & MS_RDONLY) {
		int acc = flags & 3;
		if (acc == O_WRONLY || acc == O_RDWR ||
		    (flags & (O_CREAT | O_TRUNC))) return -EROFS;
	}

	/* where the walk really went, for f->path: see resolve_walk_rp */
	char *rp = path[0] == '/' ? kmalloc(PATH_MAX) : NULL;
	int ret = vfs_open_fds_rp(fds, uid, gid, umask, path, flags, mode, mflags, rp);
	if (rp) kfree(rp);
	return ret;
}

/* O_CREAT of a name the walk did not find: make it in its parent directory.
 * 0 with *dp and *inp set; 1 when someone made it meanwhile (the caller walks
 * again, once); else -errno. */
static int open_create(const char *path, uid_t uid, gid_t gid, mode_t umask, mode_t mode,
                       char *rp, int raced, vfs_dentry_t **dp, vfs_inode_t **inp)
{
	/* find parent directory */
	const char *fname;
	char *dir_path = vfs_parent_alloc(path, &fname);
	if (!dir_path) return -ENOMEM;

	vfs_dentry_t *parent = resolve_walk_rp(dir_path, NULL, rp);
	/* the new name sits in the parent's real directory */
	if (rp && rp[0]) {
		size_t pl = strlen(rp), fl = strlen(fname);
		if (pl == 1) pl = 0;   /* parent is the root */
		if (pl + 1 + fl < PATH_MAX) { rp[pl] = '/'; memcpy(rp + pl + 1, fname, fl + 1); }
		else rp[0] = '\0';
	}
	if (!parent || !parent->inode || !parent->inode->iops) { kfree(dir_path); return -ENOENT; }
	if (vfs_dentry_readonly(parent)) { kfree(dir_path); return -EROFS; } /* EROFS */

	int pr = vfs_check_perm(parent->inode, MAY_WRITE | MAY_EXEC, uid, gid);
	if (pr) { kfree(dir_path); return pr; }

	mode_t cmode = mode & ~umask;
	/* /proc, /sys and friends have no create */
	if (!parent->inode->iops->create) { kfree(dir_path); return -EACCES; }
	int cr = parent->inode->iops->create(parent->inode, fname, cmode);
	if (cr != 0) {
		kfree(dir_path);
		/* someone created it since the walk: this is then a plain open */
		if (cr == -17 && !raced) return 1;
		return cr < -1 ? cr : -5;   /* -1 is a filesystem's bare failure, not EPERM */
	}

	vfs_inode_t *inode = parent->inode->iops->lookup(parent->inode, fname);
	if (!inode) { kfree(dir_path); return -EIO; }
	*inp = inode;
	*dp = vfs_dentry_create(parent, fname, inode);
	if (vfs_inotify_hook) vfs_inotify_hook(dir_path, 0x00000100 /*IN_CREATE*/, fname);
	kfree(dir_path);
	return 0;
}

/* f->path: where the walk went (rp), else path made canonical. */
static void file_set_path(vfs_file_t *f, const char *path, const char *rp)
{
	/* a truncated path would name some other file to openat and fchdir: too long
	 * stays empty, which they refuse */
	if (strlen(path) < sizeof(f->path)) {
		/* A path that does not fit is no path: cut short it would name another file
		 * for every openat against this descriptor, which now fails instead. */
		if (path[0] == '/' && rp && rp[0] && strlen(rp) < sizeof(f->path)) memcpy(f->path, rp, strlen(rp) + 1);
		else if (path[0] == '/' && strlen(path) < sizeof(f->path)) vfs_canon_path(path, f->path, sizeof(f->path));
		else if (path[0] != '/' && strlen(path) < sizeof(f->path)) memcpy(f->path, path, strlen(path) + 1);
	}
	/* too long for path[]: keep the walked absolute path aside for fchdir */
	if (!f->path[0] && path[0] == '/' && rp && rp[0] == '/') {
		size_t rl = strlen(rp);
		f->lpath = kmalloc(rl + 1);
		if (f->lpath) memcpy(f->lpath, rp, rl + 1);
	}
}

static int vfs_open_fds_rp(fd_table_t *fds, uid_t uid, gid_t gid, mode_t umask,
                           const char *path, int flags, mode_t mode, unsigned long mflags, char *rp)
{
	vfs_inode_t  *inode = NULL;
	walk_mnts_t ms;
	int raced = 0;
again:;
	vfs_dentry_t *d = resolve_walk_ms(path, NULL, rp, &ms);

	if (!d) {
		if (!(flags & O_CREAT)) return -ENOENT;

		int cr = open_create(path, uid, gid, umask, mode, rp, raced, &d, &inode);
		if (cr == 1) { raced = 1; goto again; }
		if (cr) return cr;
	} else {
		if ((flags & O_CREAT) && (flags & O_EXCL)) return -EEXIST;
		inode = d->inode;
		if (!inode) return -ENOENT; /* negative dentry — no backing inode, ENOENT */
		if (flags & O_NOFOLLOW) {
			/* note: vfs_lstat re-resolves and may mutate the dentry tree, so
			 * cache inode above before calling it. */
			vfs_stat_t ls = {0};
			if (vfs_lstat(path, &ls) == 0 && S_ISLNK(ls.mode)) return -ELOOP;
		}
		int acc = flags & 3; /* O_ACCMODE */
		if ((acc != 0 || (flags & O_TRUNC)) && vfs_dentry_readonly(d)) return -EROFS;
		if (S_ISDIR(inode->mode) && (acc == 1 || acc == 2)) return -EISDIR;
		/* check read/write permission on existing file */
		int accmode = 0;
		if (acc == 0 || acc == 2) accmode |= MAY_READ;
		if (acc == 1 || acc == 2) accmode |= MAY_WRITE;
		int pr = vfs_check_perm(inode, accmode, uid, gid);
		if (pr) return pr;
	}

	if (!inode) return -EIO;
	if (inode->iops == &snapshot_iops) return -ENXIO;   /* names no file to open */
	if ((mflags & MS_NODEV) && (S_ISCHR(inode->mode) || S_ISBLK(inode->mode)))
		return -EACCES;
	if ((flags & O_DIRECTORY) && !S_ISDIR(inode->mode)) return -ENOTDIR;
	if (!inode->fops && !S_ISDIR(inode->mode)) return -ENXIO;

	vfs_file_t *f = kmalloc(sizeof(*f));
	if (!f) return -ENOMEM;
	memset(f, 0, sizeof(*f));
	f->inode    = inode;
	f->flags    = flags & ~O_CLOEXEC; /* cloexec is per-fd, stored in fd_table */
	f->mnt_flags = mflags | (vfs_dentry_readonly(d) ? MS_RDONLY : 0);
	f->refcount = 0; /* incremented by fd_alloc */
	f->bdev     = dentry_bdev_get(d);
	f->sdev     = dentry_sdev(d);
	file_set_path(f, path, rp);
	if (S_ISDIR(inode->mode)) f->at = dirpos_new(d, &ms);
	f->opened_root = ms.start;

	/* held from here; only an open that may block for good (FIFO, device) leaves the walk */
	atomic_fetch_add32((volatile uint32_t *)&inode->open_refs, 1);
	if (!S_ISREG(inode->mode) && !S_ISDIR(inode->mode)) vfs_walk_done(task_current());

	int orc = (inode->fops && inode->fops->open) ? inode->fops->open(inode, f, flags) : 0;
	if (orc != 0) {
		vfs_inode_drop_open(inode);
		file_free(f);
		return orc;
	}

	int fd = fd_alloc(fds, f);
	if (fd < 0) {
		/* open ran, so its state (a pty pair, a fifo end) needs close */
		if (inode->fops && inode->fops->close) inode->fops->close(f);
		vfs_inode_drop_open(inode);
		file_free(f);
		return fd;
	}
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
	if (!f || !f->inode) return -EBADF;
	vfs_inode_t *in = f->inode;
	uint64_t fl = spin_lock_irq(&flock_lock);
	int other_ex = in->flock_ex && in->flock_ex != f;
	int other_sh = in->flock_sh - (f->flock_held == 1 ? 1 : 0);
	int conflict = (cmd == 2) ? (other_ex || other_sh > 0) : other_ex;
	if (conflict) { spin_unlock_irq(&flock_lock, fl); return -EAGAIN; }
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
 * practice (sqlite, dpkg). type: 0=F_RDLCK 1=F_WRLCK.
 * Open file description locks (F_OFD_*) share the table: their owner is the
 * vfs_file_t address, a kernel pointer and so negative as a long, never a pid.
 * They go when the last reference to that open file goes. */
typedef struct posix_lock {
	void              *inode;
	long               pid;   /* owner: pid, or (long)file for OFD locks */
	int                type;
	int64_t            start, end;
	struct posix_lock *next;
} posix_lock_t;

static posix_lock_t *g_plocks = NULL;
static spinlock_t    plock_lock = SPINLOCK_INIT;

static int pl_overlap(posix_lock_t *l, int64_t s, int64_t e)
{ return l->start <= e && s <= l->end; }

/* returns 0 if granted, -11 (EAGAIN) on conflict; F_UNLCK via vfs_posix_unlock */
int vfs_posix_setlk(void *inode, long pid, int type, int64_t start, int64_t end)
{
	uint64_t f = spin_lock_irq(&plock_lock);
	for (posix_lock_t *l = g_plocks; l; l = l->next) {
		if (l->inode != inode || l->pid == pid) continue;
		if (pl_overlap(l, start, end) && (type == 1 || l->type == 1)) {
			spin_unlock_irq(&plock_lock, f);
			return -EAGAIN;   /* conflicting lock held by another process */
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
	if (!nl) { spin_unlock_irq(&plock_lock, f); return -ENOMEM; }
	nl->inode = inode; nl->pid = pid; nl->type = type;
	nl->start = start; nl->end = end;
	nl->next = g_plocks; g_plocks = nl;
	spin_unlock_irq(&plock_lock, f);
	return 0;
}

void vfs_posix_unlock(void *inode, long pid, int64_t start, int64_t end)
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
int vfs_posix_getlk(void *inode, long pid, int *type, int64_t *start, int64_t *end, int *holder_pid)
{
	uint64_t f = spin_lock_irq(&plock_lock);
	for (posix_lock_t *l = g_plocks; l; l = l->next) {
		if (l->inode != inode || l->pid == pid) continue;
		if (pl_overlap(l, *start, *end) && (*type == 1 || l->type == 1)) {
			*type = l->type; *start = l->start; *end = l->end; *holder_pid = l->pid < 0 ? -1 : (int)l->pid;
			spin_unlock_irq(&plock_lock, f);
			return 1;
		}
	}
	spin_unlock_irq(&plock_lock, f);
	return 0;
}

static void plock_drop(void *inode, long pid, int all)
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
void vfs_posix_drop_ofd(vfs_file_t *f)        { plock_drop(NULL, (long)(uintptr_t)f, 1); }

/* drop one reference to an open file; close + free it when the last goes.
 * Used by SCM_RIGHTS fd-passing to manage in-flight file references. */
void vfs_file_put(vfs_file_t *f)
{
	if (!f) return;
	if (atomic_sub_fetch32((volatile uint32_t *)&f->refcount, 1) == 0) {
		vfs_flock_release(f);
		vfs_posix_drop_ofd(f);
		if (f->inode && f->inode->fops && f->inode->fops->close)
			f->inode->fops->close(f);
		if (!vfs_inode_drop_open(f->inode) && f->inode && !f->inode->cached)
			vfs_inode_free(f->inode);
		file_free(f);
	}
}

int vfs_close(int fd)
{
	process_t  *p = task_current();
	vfs_file_t *f = fd_remove(p->fds, fd);   /* atomic detach: only one close wins */
	if (!f) return -1;
	if (f->inode && p) vfs_posix_drop_pid_inode(f->inode, (int)p->tgid);
	fput(f);
	return 0;
}

ssize_t vfs_read(int fd, void *buf, size_t len)
{
	process_t  *p = task_current();
	vfs_file_t *f = fget(p->fds, fd);   /* hold a ref so a sibling close can't free it mid-read */
	if (!f) return -EBADF;
	ssize_t r;
	mode_t m = f->inode->mode;
	/* Only vfs_open files have these types, so their access mode is real. Without
	 * this a write-only open reads a file its opener has no read permission on. */
	if ((S_ISREG(m) || S_ISBLK(m) || S_ISCHR(m)) && (f->flags & 3) == O_WRONLY)
		r = -9; /* EBADF */
	else if (S_ISDIR(m))
		r = -21; /* EISDIR */
	else
		r = (f->inode->fops && f->inode->fops->read)
		  ? f->inode->fops->read(f, buf, len) : -22; /* EINVAL */
	fput(f);
	return r;
}

ssize_t vfs_write(int fd, const void *buf, size_t len)
{
	process_t  *p = task_current();
	vfs_file_t *f = fget(p->fds, fd);
	if (!f) return -EBADF;
	ssize_t r;
	mode_t m = f->inode->mode;
	if ((S_ISREG(m) || S_ISBLK(m) || S_ISCHR(m)) && (f->flags & 3) == O_RDONLY) {
		r = -9; /* EBADF */
	} else if (!f->inode->fops || !f->inode->fops->write) {
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
	if (!f) return -EBADF;

	/* pipes and character devices are not seekable */
	if (f->inode && (f->inode->mode & 0170000) == 0010000) return -ESPIPE;   /* S_IFIFO */
	if (f->inode && (f->inode->mode & 0170000) == 0020000) return -ESPIPE;   /* S_IFCHR */

	if (f->inode->fops && f->inode->fops->seek)
		return f->inode->fops->seek(f, off, whence);

	off_t newpos;
	switch (whence) {
	case SEEK_SET: newpos = off; break;
	case SEEK_CUR: newpos = f->pos + off; break;
	case SEEK_END: newpos = (off_t)f->inode->size + off; break;
	default:       return -EINVAL;
	}
	if (newpos < 0) return -EINVAL;
	f->pos = newpos;
	return newpos;
}

int vfs_mkdir(const char *path, mode_t mode)
{
	/* An existing target is EEXIST even on a read-only mount: mkdir -p walks every
	 * parent, and a session's read-only root must not stop it at the first one. */
	if (vfs_dentry_resolve(path)) return -EEXIST;
	if (vfs_path_mnt_flags(path) & MS_RDONLY) return -EROFS;

	const char *dname;
	char *dir_path = vfs_parent_alloc(path, &dname);
	if (!dir_path) return -ENOMEM;

	vfs_dentry_t *parent = vfs_dentry_resolve(dir_path);
	if (!parent || !parent->inode || !parent->inode->iops) { kfree(dir_path); return -ENOENT; }   /* mkdirp callers go by ENOENT */
	if (vfs_dentry_readonly(parent)) { kfree(dir_path); return -EROFS; } /* EROFS */

	process_t *p = task_current();
	int pr = may_change(parent->inode);
	if (pr) { kfree(dir_path); return pr; }
	if (!parent->inode->iops->mkdir) { kfree(dir_path); return -1; }   /* EPERM, as Linux */

	mode_t cmode = mode & ~fs_umask_of((struct process *)p);
	int mr = parent->inode->iops->mkdir(parent->inode, dname, cmode);
	if (mr != 0) { kfree(dir_path); return mr < -1 ? mr : -1; }   /* a racing mkdir -p needs EEXIST */

	vfs_inode_t *in = parent->inode->iops->lookup(parent->inode, dname);
	if (in) vfs_dentry_create(parent, dname, in);
	kfree(dir_path);
	return 0;
}

int vfs_mkfifo(const char *path, mode_t mode)
{
	/* An existing target is EEXIST even on a read-only mount: mkdir -p walks every
	 * parent, and a session's read-only root must not stop it at the first one. */
	if (vfs_dentry_resolve(path)) return -EEXIST;
	if (vfs_path_mnt_flags(path) & MS_RDONLY) return -EROFS;
	const char *fname;
	char *dir_path = vfs_parent_alloc(path, &fname);
	if (!dir_path) return -ENOMEM;

	vfs_dentry_t *parent = vfs_dentry_resolve(dir_path);
	if (!parent || !parent->inode || !parent->inode->iops) { kfree(dir_path); return -ENOENT; }
	if (vfs_dentry_readonly(parent)) { kfree(dir_path); return -EROFS; } /* EROFS */
	if (!parent->inode->iops->mknod) { kfree(dir_path); return -1; } /* fs doesn't support mknod */
	process_t *p = task_current();
	int pr = may_change(parent->inode);
	if (pr) { kfree(dir_path); return pr; }

	mode_t cmode = (mode & 07777 & ~fs_umask_of((struct process *)p)) | S_IFIFO;
	int r = parent->inode->iops->mknod(parent->inode, fname, cmode, 0);
	if (r != 0) { kfree(dir_path); return r; }
	vfs_inode_t *in = parent->inode->iops->lookup(parent->inode, fname);
	if (in) vfs_dentry_create(parent, fname, in);
	kfree(dir_path);
	return 0;
}

/* mknod for character/block device nodes (S_IFCHR/S_IFBLK) — routes to the
 * driver registered for that major:minor (devfs registry). */
int vfs_mknod(const char *path, mode_t mode, dev_t dev)
{
	/* An existing target is EEXIST even on a read-only mount: mkdir -p walks every
	 * parent, and a session's read-only root must not stop it at the first one. */
	if (vfs_dentry_resolve(path)) return -EEXIST;
	if (vfs_path_mnt_flags(path) & MS_RDONLY) return -EROFS;
	const char *fname;
	char *dir_path = vfs_parent_alloc(path, &fname);
	if (!dir_path) return -ENOMEM;

	vfs_dentry_t *parent = vfs_dentry_resolve(dir_path);
	if (!parent || !parent->inode || !parent->inode->iops) { kfree(dir_path); return -ENOENT; }
	if (vfs_dentry_readonly(parent)) { kfree(dir_path); return -EROFS; } /* EROFS */
	if (!parent->inode->iops->mknod) { kfree(dir_path); return -1; }
	process_t *p = task_current();
	/* EPERM: device nodes take CAP_MKNOD; anyone may leave a socket name */
	if (p && !capable_task((struct process *)p, CAP_MKNOD) && (mode & 0170000) != 0140000) {
		kfree(dir_path);
		return -EPERM;
	}
	int pr = vfs_check_perm(parent->inode, MAY_WRITE, task_fsuid(p), task_fsgid(p));
	if (pr) { kfree(dir_path); return pr; }

	int r = parent->inode->iops->mknod(parent->inode, fname, mode, dev);
	if (r != 0) { kfree(dir_path); return r; }
	vfs_inode_t *in = parent->inode->iops->lookup(parent->inode, fname);
	if (in) vfs_dentry_create(parent, fname, in);
	kfree(dir_path);
	return 0;
}

static int dentry_child_pinned(vfs_dentry_t *dir, const char *name)
{
	uint64_t f = spin_lock_irq(&vfs_lock);
	vfs_dentry_t *c = dir->child;
	while (c && strcmp(c->name, name) != 0) c = c->next;
	int pinned = c && c->pins;
	spin_unlock_irq(&vfs_lock, f);
	return pinned;
}

int vfs_unlink(const char *path)
{
	const char *fname;
	char *dir_path = vfs_parent_alloc(path, &fname);
	if (!dir_path) return -ENOMEM;

	vfs_dentry_t *parent = vfs_dentry_resolve(dir_path);
	if (parent && parent->inode && vfs_dentry_readonly(parent)) { kfree(dir_path); return -EROFS; } /* EROFS */
	if (!parent || !parent->inode || !parent->inode->iops || !parent->inode->iops->unlink) {
		kfree(dir_path); return -ENOENT;
	}

	int pr = may_change(parent->inode);
	if (pr) { kfree(dir_path); return pr; }
	if (dentry_child_pinned(parent, fname)) { kfree(dir_path); return -EBUSY; } /* EBUSY */
	if (sticky_denied(parent->inode, fname)) { kfree(dir_path); return -1; } /* EPERM */

	int r = parent->inode->iops->unlink(parent->inode, fname);
	if (r == -1) r = -2;   /* a filesystem's "no such name"; -1 above is EPERM */
	if (r == 0 && vfs_inotify_hook) vfs_inotify_hook(dir_path, 0x00000200 /*IN_DELETE*/, fname);
	if (r == 0) {
		uint64_t lf = spin_lock_irq(&vfs_lock);
		vfs_dentry_t **dp = &parent->child;
		while (*dp) {
			if (strcmp((*dp)->name, fname) == 0) {
				vfs_dentry_t *dead = *dp;
				dentry_unlink(dead);
				dentry_retire(dead);
				dentry_count--;
				break;
			}
			dp = &(*dp)->next;
		}
		spin_unlock_irq(&vfs_lock, lf);
	}
	kfree(dir_path);
	return r;
}

/* write a file from kernel data without going through the fd table */
int vfs_kernel_install(const char *path, const void *data, size_t len)
{
	const char *fname;
	char *dir_path = vfs_parent_alloc(path, &fname);
	if (!dir_path) return -ENOMEM;

	vfs_dentry_t *parent = vfs_dentry_resolve(dir_path);
	kfree(dir_path);
	if (!parent || !parent->inode || !parent->inode->iops) return -1;

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
	if (vfs_path_mnt_flags(linkpath) & MS_RDONLY) return -EROFS;
	const char *fname;
	char *dir_path = vfs_parent_alloc(linkpath, &fname);
	if (!dir_path) return -ENOMEM;

	vfs_dentry_t *parent = vfs_dentry_resolve(dir_path);
	kfree(dir_path);
	if (!parent || !parent->inode || !parent->inode->iops) return -1;
	if (!parent->inode->iops->symlink) return -ENOSYS;
	if (vfs_dentry_readonly(parent)) return -EROFS;
	int pr = may_change(parent->inode);
	if (pr) return pr;

	vfs_stat_t ls = {0};
	if (vfs_lstat(linkpath, &ls) == 0) return -EEXIST;   /* filesystems report it as a bare -1 */
	int sr = parent->inode->iops->symlink(parent->inode, fname, target);
	if (sr) return sr < -1 ? sr : -5;

	vfs_inode_t *in = parent->inode->iops->lookup(parent->inode, fname);
	if (in) vfs_dentry_create(parent, fname, in);
	return 0;
}

int vfs_stat(const char *path, vfs_stat_t *out)
{
	vfs_dentry_t *d = vfs_dentry_resolve(path);
	if (!d) return -1;
	/* For volatile (no_cache) nodes d is the task's slot, which the next resolve
	 * reuses: operate solely on the inode. */
	vfs_inode_t *in = d->inode;
	if (!in) return -1;
	int rc;
	if (in->iops && in->iops->stat) {
		rc = in->iops->stat(in, out);
	} else {
		out->ino   = in->ino;
		out->mode  = in->mode;
		out->nlink = in->nlink;
		out->uid   = in->uid;
		out->gid   = in->gid;
		out->size  = in->size;
		out->rdev  = in->rdev;
		rc = 0;
	}
	out->sdev = dentry_sdev(d);
	if (!in->cached) vfs_inode_free(in);
	return rc;
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

	const char *fname;
	char *parent_path = vfs_parent_alloc(path, &fname);
	if (!parent_path) return -ENOMEM;

	/* "." and ".." are not real directory entries (filesystems synthesize them
	 * only in readdir); they are resolved by the path walker. They can never be
	 * symlinks, so lstat == stat — defer to the full resolver instead of a
	 * doomed lookup() of a literal "." against the parent. */
	if ((fname[0] == '.' && fname[1] == '\0') ||
	    (fname[0] == '.' && fname[1] == '.' && fname[2] == '\0')) {
		kfree(parent_path);
		return vfs_stat(path, out);
	}

	vfs_dentry_t *parent = vfs_dentry_resolve(parent_path);
	/* A volatile parent (e.g. /proc/<pid> reached via /proc/self) comes back in the
	 * task's slot, which the next resolve reuses: drive the lookup from the inode. */
	vfs_inode_t *pin = parent ? parent->inode : NULL;
	uint32_t psdev = parent ? dentry_sdev(parent) : 0;
	kfree(parent_path);
	if (!parent) return -1;
	if (!pin || !pin->iops || !pin->iops->lookup || !vfs_may_search(pin)) {
		if (pin && !pin->cached) vfs_inode_free(pin);
		return -1;
	}
	vfs_inode_t *in = pin->iops->lookup(pin, fname);
	if (!pin->cached) vfs_inode_free(pin);
	if (!in) return -1;

	int rc = 0;
	if (in->iops && in->iops->stat) {
		rc = in->iops->stat(in, out);
	} else {
		out->ino = in->ino; out->mode = in->mode; out->nlink = in->nlink;
		out->uid = in->uid; out->gid = in->gid; out->size = in->size; out->rdev = in->rdev;
	}
	/* a symlink is on its directory's filesystem; anything else may be a mount
	 * point, whose stat is the mounted root's */
	out->sdev = S_ISLNK(out->mode) ? psdev : vfs_path_sdev(path);
	if (!out->sdev) out->sdev = psdev;
	if (!in->cached) vfs_inode_free(in);
	return rc;
}

/* For names kept outside the filesystem, which still sit behind its directories.
 * -2 and walk_denied when one refuses; a path leading nowhere is the caller's. */
int vfs_path_searchable(const char *path)
{
	const char *name;
	char *dir = vfs_parent_alloc(path, &name);
	if (!dir) return -ENOMEM;
	vfs_dentry_t *d = vfs_dentry_resolve(dir);
	kfree(dir);
	process_t *p = task_current();
	if (d ? (d->inode && !vfs_may_search(d->inode)) : p->walk_denied) return -ENOENT;
	return 0;
}

/* A walk that ends in a directory the dentry cache does not keep hands the
 * caller an inode of its own (cached 0); the caller frees it. */
static void put_walked(vfs_inode_t *in)
{
	if (in && !in->cached) vfs_inode_free(in);
}

int vfs_rename(const char *old_path, const char *new_path)
{
	const char *old_name, *new_name;
	char *old_dir = vfs_parent_alloc(old_path, &old_name);
	char *new_dir = vfs_parent_alloc(new_path, &new_name);
	vfs_inode_t *old_in = NULL, *new_in = NULL;
	int ret;
	if (!old_dir || !new_dir) { ret = -ENOMEM; goto out; }

	vfs_dentry_t *old_parent = vfs_dentry_resolve(old_dir);
	if (!old_parent || !old_parent->inode) { ret = -ENOENT; goto out; }
	old_in = old_parent->inode;   /* the next resolve may reuse a volatile dentry */
	int ro = vfs_dentry_readonly(old_parent);
	vfs_dentry_t *old_mnt = dentry_mnt_root(old_parent);
	/* two uncached directories both come back in the task's one volatile slot:
	 * equal pointers then say nothing about the directories being the same */
	int old_vol = vfs_dentry_is_volatile(old_parent);
	vfs_dentry_t *new_parent = vfs_dentry_resolve(new_dir);
	if (!new_parent || !new_parent->inode) { ret = -ENOENT; goto out; }
	new_in = new_parent->inode;
	if (ro || vfs_dentry_readonly(new_parent)) { ret = -EROFS; goto out; }
	if (old_mnt != dentry_mnt_root(new_parent)) { ret = -EXDEV; goto out; }
	ret = may_change(old_in);
	if (!ret) ret = may_change(new_in);
	if (ret) goto out;
	int same_dir = !old_vol && old_parent == new_parent;
	if (same_dir && strcmp(old_name, new_name) == 0) goto out;
	/* a directory moved under itself would detach a cycle from the tree */
	size_t ol = strlen(old_path);
	if (strncmp(old_path, new_path, ol) == 0 && new_path[ol] == '/') { ret = -EINVAL; goto out; }
	if (dentry_child_pinned(new_parent, new_name)) { ret = -EBUSY; goto out; }
	if (old_in->iops && new_in->iops &&
	    (sticky_denied(old_in, old_name) || sticky_denied(new_in, new_name))) {
		ret = -EPERM; goto out;
	}

	if (!old_in->iops || !old_in->iops->rename) { ret = -ENOSYS; goto out; }

	ret = old_in->iops->rename(old_in, old_name, new_in, new_name);
	if (ret == 0 && vfs_inotify_hook) {
		vfs_inotify_hook(old_dir, 0x00000040 /*IN_MOVED_FROM*/, old_name);
		vfs_inotify_hook(new_dir, 0x00000080 /*IN_MOVED_TO*/,   new_name);
	}
	if (ret == 0) {
		uint64_t f = spin_lock_irq(&vfs_lock);
		/* drop a cached dentry for an overwritten target: its inode went with it */
		vfs_dentry_t **tp = &new_parent->child;
		while (*tp) {
			if (strcmp((*tp)->name, new_name) == 0) {
				vfs_dentry_t *dead = *tp; dentry_unlink(dead); dentry_retire(dead); dentry_count--; break;
			}
			tp = &(*tp)->next;
		}
		/* detach the moved dentry from its old parent's child list (a volatile
		 * parent keeps none, and the slot may hold the new parent by now) */
		vfs_dentry_t **pp = old_vol ? NULL : &old_parent->child;
		while (pp && *pp && strcmp((*pp)->name, old_name) != 0) pp = &(*pp)->next;
		vfs_dentry_t *d = pp ? *pp : NULL;
		if (d) {
			int nlen = 0;
			while (new_name[nlen] && nlen < 254) nlen++;
			memcpy(d->name, new_name, nlen);
			d->name[nlen] = '\0';
			if (!same_dir) {
				dentry_unlink(d);
				dentry_link(new_parent, d);
			}
		}
		spin_unlock_irq(&vfs_lock, f);
	}
out:
	put_walked(old_in);
	if (new_in != old_in) put_walked(new_in);
	kfree(old_dir); kfree(new_dir);
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

int vfs_iattr_apply(vfs_inode_t *in, const vfs_iattr_t *a)
{
	if (a->valid & ATTR_MODE)  in->mode = (in->mode & 0xF000) | (a->mode & 07777);
	if (a->valid & ATTR_UID)   in->uid = a->uid;
	if (a->valid & ATTR_GID)   in->gid = a->gid;
	if (a->valid & ATTR_ATIME) { in->atime = a->atime; in->atime_nsec = a->atime_nsec; }
	if (a->valid & ATTR_MTIME) { in->mtime = a->mtime; in->mtime_nsec = a->mtime_nsec; }
	if (a->valid & ATTR_CTIME) { in->ctime = a->ctime; in->ctime_nsec = a->ctime_nsec; }
	if (a->valid & ATTR_XATTR_SET)
		return vfs_inode_setxattr(in, a->xname, a->xval, a->xlen, a->xflags);
	if (a->valid & ATTR_XATTR_REMOVE)
		return vfs_inode_removexattr(in, a->xname);
	return 0;
}

int vfs_inode_setattr(vfs_inode_t *in, const vfs_iattr_t *a)
{
	if (in->iops && in->iops->setattr) return in->iops->setattr(in, a);
	return vfs_iattr_apply(in, a);
}

static void iattr_stamp_ctime(vfs_iattr_t *a)
{
	int64_t s, n;
	ranix_walltime(&s, &n);
	a->valid |= ATTR_CTIME;
	a->ctime = s;
	a->ctime_nsec = (uint32_t)n;
}

static int current_owns(vfs_inode_t *in)
{
	return fs_owner_or_capable(in->uid, in->gid);
}

/* the owner, or CAP_FOWNER (Linux inode_owner_or_capable) */
int vfs_chmod_inode(vfs_inode_t *in, mode_t mode)
{
	if (!in) return -ENOENT;
	if (!current_owns(in)) return -EPERM;
	vfs_iattr_t a = { .valid = ATTR_MODE, .mode = mode };
	iattr_stamp_ctime(&a);
	return vfs_inode_setattr(in, &a);
}

/* utimensat: naming a time takes ownership; setting "now" only write access */
int vfs_utimes_inode(vfs_inode_t *in, vfs_iattr_t *a, int explicit_times)
{
	if (!current_owns(in)) {
		if (explicit_times) return -EPERM;
		process_t *p = task_current();
		if (vfs_check_perm(in, MAY_WRITE, task_fsuid(p), task_fsgid(p))) return -EACCES;
	}
	iattr_stamp_ctime(a);
	return vfs_inode_setattr(in, a);
}

int vfs_chmod(const char *path, mode_t mode)
{
	if (vfs_path_mnt_flags(path) & MS_RDONLY) return -EROFS;
	vfs_dentry_t *d = vfs_dentry_resolve(path);
	if (!d || !d->inode) return -ENOENT;
	if (vfs_dentry_readonly(d)) return -EROFS;
	vfs_inode_t *in = d->inode;
	int r = vfs_chmod_inode(in, mode);
	if (!in->cached) vfs_inode_free(in);
	return r;
}

/* change owner/group of an inode (uid/gid == (uid_t)-1 leaves it unchanged).
 * Linux chown_ok/chgrp_ok: CAP_CHOWN may do anything; the owner may only give
 * the file a group it is in. Clears set-user/group-ID bits. */
int vfs_chown_inode(vfs_inode_t *in, uid_t uid, gid_t gid)
{
	if (!in) return -ENOENT;
	fs_cred_t c;
	fs_cred_task(task_current(), &c);
	if (!fs_capable(&c, in->uid, in->gid, CAP_CHOWN)) {
		if (in->uid != c.uid) return -EPERM;
		if (uid != (uid_t)-1 && uid != in->uid) return -EPERM;
		if (gid != (gid_t)-1 && gid != in->gid && !fs_in_group(&c, gid)) return -EPERM;
	}
	vfs_iattr_t a = { .valid = ATTR_MODE, .uid = uid, .gid = gid };
	a.mode = in->mode & 07777 & ~(mode_t)06000;    /* drop setuid/setgid on chown */
	if (uid != (uid_t)-1) a.valid |= ATTR_UID;
	if (gid != (gid_t)-1) a.valid |= ATTR_GID;
	iattr_stamp_ctime(&a);
	return vfs_inode_setattr(in, &a);
}

int vfs_chown(const char *path, uid_t uid, gid_t gid)
{
	if (vfs_path_mnt_flags(path) & MS_RDONLY) return -EROFS;
	vfs_dentry_t *d = vfs_dentry_resolve(path);
	if (!d || !d->inode) return -ENOENT;
	if (vfs_dentry_readonly(d)) return -EROFS;
	vfs_inode_t *in = d->inode;
	int r = vfs_chown_inode(in, uid, gid);
	if (!in->cached) vfs_inode_free(in);
	return r;
}

/* resolve the final path component WITHOUT following a terminal symlink and
 * return its inode, so chmod/chown can act on the link itself. The inode is the
 * cached dentry's, the one every later walk checks: a private copy from lookup
 * would take the change while the walk kept the old mode and owner. Only below
 * a directory that caches nothing is it a fresh copy, freed by the caller. */
static vfs_inode_t *vfs_lresolve(const char *path, int *ro)
{
	int len = (int)strlen(path);
	const char *slash = path + (len > 0 ? len - 1 : 0);
	while (slash > path && *slash != '/') slash--;
	const char *last = slash + 1;
	if (!*last || !strcmp(last, ".") || !strcmp(last, "..")) {   /* a directory: nothing to not follow */
		vfs_dentry_t *d = vfs_dentry_resolve(path);
		if (d) *ro = vfs_dentry_readonly(d);
		return d ? d->inode : NULL;
	}
	const char *fname;
	char *parent_path = vfs_parent_alloc(path, &fname);
	if (!parent_path) return NULL;
	vfs_dentry_t *parent = vfs_dentry_resolve(parent_path);
	kfree(parent_path);
	if (!parent || !parent->inode || !parent->inode->iops || !parent->inode->iops->lookup ||
	    !vfs_may_search(parent->inode))
		return NULL;
	*ro = vfs_dentry_readonly(parent);
	vfs_inode_t *pin = parent->inode;   /* the next resolve may reuse a volatile dentry */
	if (!vfs_dentry_is_volatile(parent)) {
		vfs_dentry_t *c = vfs_dentry_child(parent, fname);
		if (c) return c->inode;
	}
	return pin->iops->lookup(pin, fname);
}

int vfs_lchown(const char *path, uid_t uid, gid_t gid)
{
	int ro = 0;
	vfs_inode_t *in = vfs_lresolve(path, &ro);
	if (ro) return -EROFS;
	if (!in) return -ENOENT;
	int rc = vfs_chown_inode(in, uid, gid);
	if (!in->cached) vfs_inode_free(in);
	return rc;
}

int vfs_lutimes(const char *path, vfs_iattr_t *a, int explicit_times)
{
	int ro = 0;
	vfs_inode_t *in = vfs_lresolve(path, &ro);
	if (ro) return -EROFS;
	if (!in) return -ENOENT;
	int rc = vfs_utimes_inode(in, a, explicit_times);
	if (!in->cached) vfs_inode_free(in);
	return rc;
}

int vfs_lchmod(const char *path, mode_t mode)
{
	int ro = 0;
	vfs_inode_t *in = vfs_lresolve(path, &ro);
	if (ro) return -EROFS;
	if (!in) return -ENOENT;
	int r = S_ISLNK(in->mode) ? -95 : vfs_chmod_inode(in, mode);   /* EOPNOTSUPP: a link has no mode */
	if (!in->cached) vfs_inode_free(in);
	return r;
}

int vfs_link(const char *oldpath, const char *newpath)
{
	if (vfs_path_mnt_flags(newpath) & MS_RDONLY) return -EROFS;
	vfs_dentry_t *od = vfs_dentry_resolve(oldpath);
	if (!od || !od->inode) return -ENOENT;
	if (S_ISDIR(od->inode->mode)) return -EISDIR;
	vfs_inode_t *old_in = od->inode;   /* the next resolve may reuse a volatile dentry */
	if (old_in->iops == &snapshot_iops) return -EXDEV;   /* a copy, not the file */
	vfs_dentry_t *old_mnt = dentry_mnt_root(od);

	const char *new_name;
	char *new_dir = vfs_parent_alloc(newpath, &new_name);
	if (!new_dir) return -ENOMEM;

	vfs_dentry_t *nd = vfs_dentry_resolve(new_dir);
	kfree(new_dir);
	if (!nd || !nd->inode) return -ENOENT;
	if (!nd->inode->iops || !nd->inode->iops->link) return -1;
	if (vfs_dentry_readonly(nd)) return -EROFS;
	int pr = may_change(nd->inode);
	if (pr) return pr;
	/* The new name's mount would decide the flags for the old inode: a link out
	 * of a read-only tree would turn it writable. */
	if (old_mnt != dentry_mnt_root(nd)) return -EXDEV;

	return nd->inode->iops->link(nd->inode, new_name, old_in);
}

/* ── init ────────────────────────────────────────────────── */


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
	tmpfs_register();
#endif
#if IS_ENABLED(CONFIG_FS_OVERLAY)
	overlay_register();
#endif
	procfs_register();
	devfs_register();
#if IS_ENABLED(CONFIG_FS_EXT2)
	ext2_register();
#endif
#if IS_ENABLED(CONFIG_FS_FAT)
	fat_register();
	exfat_register();
#endif
	ntfs_register();
#if IS_ENABLED(CONFIG_FS_ISO9660)
	iso9660_register();
#endif
	sysfs_register();
	hugetlbfs_register();
	cgroup_register();
	kprintf(KERN_INFO "vfs: initialized\n");
}
