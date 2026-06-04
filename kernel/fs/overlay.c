#include <kernel/fs/overlay.h>
#include <kernel/vfs.h>
#include <kernel/proc.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/sync.h>

/*
 * overlayfs — read-through union of a read-only lower and a writable upper.
 *
 * Lookups check upper first, then lower; the merged view shadows lower with
 * upper. The first write to a lower-only file copies it up to upper (copy-up),
 * after which all writes hit the upper copy. Deleting a lower file leaves a
 * whiteout (a 0/0 char-dev node) in upper so it vanishes from the merged view.
 *
 * Single lower dir, the common container-image case. Each overlay inode wraps
 * the matching upper and/or lower real inode and remembers the upper directory
 * it lives in, so copy-up can recreate it there.
 */

typedef struct {
	vfs_inode_t *upper_root;
	vfs_inode_t *lower_root;
	uint32_t     refcount;
	spinlock_t   lock;
} ovl_sb_t;

typedef struct {
	ovl_sb_t    *sb;
	vfs_inode_t *upper;        /* real inode in upper layer, or NULL */
	vfs_inode_t *lower;        /* real inode in lower layer, or NULL */
	vfs_inode_t *upper_parent; /* upper dir this entry lives in (for copy-up) */
	char         name[256];    /* entry name within the parent dir */
} ovl_inode_t;

static vfs_iops_t ovl_iops;
static vfs_fops_t ovl_fops;
static vfs_fops_t ovl_dir_fops;

/* a whiteout is a char device with rdev 0 (overlayfs convention) */
static int is_whiteout(vfs_inode_t *in)
{
	return in && S_ISCHR(in->mode) && in->rdev == 0;
}

static vfs_inode_t *ovl_make_inode(ovl_sb_t *sb, vfs_inode_t *upper, vfs_inode_t *lower,
                                   vfs_inode_t *upper_parent, const char *name)
{
	vfs_inode_t *real = upper ? upper : lower;
	if (!real) return NULL;

	vfs_inode_t *vi = kmalloc(sizeof(*vi));
	if (!vi) return NULL;
	memset(vi, 0, sizeof(*vi));

	ovl_inode_t *oi = kmalloc(sizeof(*oi));
	if (!oi) { kfree(vi); return NULL; }
	memset(oi, 0, sizeof(*oi));
	oi->sb = sb; oi->upper = upper; oi->lower = lower; oi->upper_parent = upper_parent;
	int n = 0; while (name && name[n] && n < 255) { oi->name[n] = name[n]; n++; }
	oi->name[n] = '\0';

	vi->ino      = real->ino;
	vi->mode     = real->mode;
	vi->uid      = real->uid;
	vi->gid      = real->gid;
	vi->size     = real->size;
	vi->nlink    = real->nlink ? real->nlink : 1;
	vi->rdev     = real->rdev;
	vi->atime = real->atime; vi->mtime = real->mtime; vi->ctime = real->ctime;
	vi->priv     = oi;
	vi->i_sb     = sb;
	vi->iops     = &ovl_iops;
	vi->fops     = S_ISDIR(real->mode) ? &ovl_dir_fops : &ovl_fops;
	vi->no_cache = 1;   /* merged view is computed per lookup, never cached */
	return vi;
}

/* ── copy-up ─────────────────────────────────────────────── */

/* Create dir `name` in upper_parent and return its inode (idempotent). */
static vfs_inode_t *upper_ensure_dir(vfs_inode_t *upper_parent, const char *name, mode_t mode)
{
	if (!upper_parent || !upper_parent->iops) return NULL;
	vfs_inode_t *ex = upper_parent->iops->lookup ? upper_parent->iops->lookup(upper_parent, name) : NULL;
	if (ex) return ex;
	if (!upper_parent->iops->mkdir) return NULL;
	if (upper_parent->iops->mkdir(upper_parent, name, mode & 07777) != 0) return NULL;
	return upper_parent->iops->lookup(upper_parent, name);
}

/* Materialize a lower-only file in the upper layer: create it under
 * upper_parent and copy the bytes. Sets oi->upper on success. */
static int copy_up_file(ovl_inode_t *oi)
{
	if (oi->upper) return 0;                 /* already in upper */
	if (!oi->lower || !oi->upper_parent) return -1;
	vfs_inode_t *up = oi->upper_parent;
	if (!up->iops || !up->iops->create || !up->iops->lookup) return -1;

	if (up->iops->create(up, oi->name, oi->lower->mode & 07777) != 0) return -1;
	vfs_inode_t *nu = up->iops->lookup(up, oi->name);
	if (!nu) return -1;

	/* copy lower → upper through their file ops */
	if (oi->lower->fops && oi->lower->fops->read && nu->fops && nu->fops->write) {
		vfs_file_t rf = {0}, wf = {0};
		rf.inode = oi->lower; wf.inode = nu;
		char buf[1024];
		ssize_t n;
		while ((n = oi->lower->fops->read(&rf, buf, sizeof(buf))) > 0)
			if (nu->fops->write(&wf, buf, (size_t)n) != n) return -1;
	}
	oi->upper = nu;
	return 0;
}

/* ── file ops (delegate to the active layer) ─────────────── */

static vfs_inode_t *active_inode(ovl_inode_t *oi) { return oi->upper ? oi->upper : oi->lower; }

static int ovl_open(vfs_inode_t *inode, vfs_file_t *file, int flags)
{
	ovl_inode_t *oi = inode->priv;
	/* a write-intent open of a lower-only file copies up first */
	int wr = (flags & (O_WRONLY | O_RDWR)) || (flags & O_TRUNC);
	if (wr && !oi->upper) { if (copy_up_file(oi) != 0) return -1; }
	vfs_inode_t *real = active_inode(oi);
	if (!real) return -1;
	file->priv = real;                          /* fast path for read/write */
	if (real->fops && real->fops->open) {
		vfs_file_t tmp = *file; tmp.inode = real;
		int r = real->fops->open(real, &tmp, flags);
		file->pos = tmp.pos;
		return r;
	}
	if (flags & O_APPEND) file->pos = (off_t)real->size;
	return 0;
}

static ssize_t ovl_read(vfs_file_t *f, void *buf, size_t len)
{
	vfs_inode_t *real = f->priv;
	if (!real || !real->fops || !real->fops->read) return -22;
	vfs_file_t tmp = *f; tmp.inode = real;
	ssize_t r = real->fops->read(&tmp, buf, len);
	f->pos = tmp.pos;
	return r;
}

static ssize_t ovl_write(vfs_file_t *f, const void *buf, size_t len)
{
	ovl_inode_t *oi = f->inode->priv;
	if (!oi->upper) { if (copy_up_file(oi) != 0) return -28; f->priv = oi->upper; }
	vfs_inode_t *real = oi->upper;
	if (!real || !real->fops || !real->fops->write) return -22;
	vfs_file_t tmp = *f; tmp.inode = real;
	ssize_t r = real->fops->write(&tmp, buf, len);
	f->pos = tmp.pos;
	if (r > 0) { f->inode->size = real->size; }
	return r;
}

static vfs_fops_t ovl_fops = { .open = ovl_open, .read = ovl_read, .write = ovl_write };
static vfs_fops_t ovl_dir_fops = { 0 };

/* ── inode ops ───────────────────────────────────────────── */

static vfs_inode_t *dir_lookup(vfs_inode_t *dir, const char *name)
{
	if (!dir || !dir->iops || !dir->iops->lookup) return NULL;
	return dir->iops->lookup(dir, name);
}

static vfs_inode_t *ovl_lookup(vfs_inode_t *dir, const char *name)
{
	ovl_inode_t *od = dir->priv;
	vfs_inode_t *u = od->upper ? dir_lookup(od->upper, name) : NULL;
	if (is_whiteout(u)) return NULL;            /* deleted in upper: hide lower */
	vfs_inode_t *l = od->lower ? dir_lookup(od->lower, name) : NULL;
	if (!u && !l) return NULL;
	return ovl_make_inode(od->sb, u, l, od->upper, name);
}

static int ovl_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	ovl_inode_t *od = dir->priv;
	if (idx == 0 || idx == 1) {
		out->ino = dir->ino; out->mode = dir->mode; out->next_off = idx + 1;
		out->name[0] = '.'; out->name[1] = (idx == 1) ? '.' : '\0'; out->name[2] = '\0';
		return 0;
	}
	uint32_t want = idx - 2;
	uint32_t seen = 0;

	/* Probe inodes from the underlying lookup are not freed: ramfs/tmpfs return
	 * their canonical (live) inode, so freeing would corrupt the layer. */

	/* upper entries first (skip "." ".." and whiteouts) */
	if (od->upper && od->upper->iops && od->upper->iops->readdir) {
		for (uint32_t i = 0; ; i++) {
			vfs_dirent_t d;
			if (od->upper->iops->readdir(od->upper, i, &d) != 0) break;
			if (d.name[0] == '.' && (d.name[1] == '\0' || (d.name[1] == '.' && d.name[2] == '\0'))) continue;
			if (is_whiteout(dir_lookup(od->upper, d.name))) continue;
			if (seen == want) { *out = d; out->next_off = idx + 1; return 0; }
			seen++;
		}
	}
	/* lower entries not shadowed/whited-out by upper */
	if (od->lower && od->lower->iops && od->lower->iops->readdir) {
		for (uint32_t i = 0; ; i++) {
			vfs_dirent_t d;
			if (od->lower->iops->readdir(od->lower, i, &d) != 0) break;
			if (d.name[0] == '.' && (d.name[1] == '\0' || (d.name[1] == '.' && d.name[2] == '\0'))) continue;
			if (od->upper && dir_lookup(od->upper, d.name)) continue;  /* upper (or whiteout) wins */
			if (seen == want) { *out = d; out->next_off = idx + 1; return 0; }
			seen++;
		}
	}
	return -1;
}

/* ensure dir has an upper counterpart, copying it up if it only exists in lower */
static int dir_copy_up(ovl_inode_t *od)
{
	if (od->upper) return 0;
	if (!od->upper_parent) return -1;
	vfs_inode_t *nu = upper_ensure_dir(od->upper_parent, od->name, od->lower ? od->lower->mode : 0755);
	if (!nu) return -1;
	od->upper = nu;
	return 0;
}

static int ovl_create(vfs_inode_t *dir, const char *name, mode_t mode)
{
	ovl_inode_t *od = dir->priv;
	if (dir_copy_up(od) != 0) return -1;
	if (!od->upper->iops || !od->upper->iops->create) return -1;
	return od->upper->iops->create(od->upper, name, mode);
}

static int ovl_mkdir(vfs_inode_t *dir, const char *name, mode_t mode)
{
	ovl_inode_t *od = dir->priv;
	if (dir_copy_up(od) != 0) return -1;
	if (!od->upper->iops || !od->upper->iops->mkdir) return -1;
	return od->upper->iops->mkdir(od->upper, name, mode);
}

/* create a whiteout (char dev 0/0) hiding `name` from the lower layer */
static int make_whiteout(vfs_inode_t *upper_dir, const char *name)
{
	if (!upper_dir->iops || !upper_dir->iops->mknod) return -1;
	return upper_dir->iops->mknod(upper_dir, name, S_IFCHR | 0000, 0);
}

static int ovl_unlink(vfs_inode_t *dir, const char *name)
{
	ovl_inode_t *od = dir->priv;
	int in_lower = od->lower && dir_lookup(od->lower, name);

	if (od->upper && dir_lookup(od->upper, name)) {
		if (od->upper->iops && od->upper->iops->unlink)
			od->upper->iops->unlink(od->upper, name);
	}
	/* a lower copy still shows through unless we lay down a whiteout */
	if (in_lower) {
		if (!od->upper && dir_copy_up(od) != 0) return -1;
		return make_whiteout(od->upper, name);
	}
	return 0;
}

static int ovl_stat(vfs_inode_t *inode, vfs_stat_t *out)
{
	ovl_inode_t *oi = inode->priv;
	vfs_inode_t *real = active_inode(oi);
	if (real && real->iops && real->iops->stat) return real->iops->stat(real, out);
	out->ino = inode->ino; out->mode = inode->mode; out->nlink = inode->nlink;
	out->uid = inode->uid; out->gid = inode->gid; out->size = inode->size; out->rdev = inode->rdev;
	return 0;
}

static int ovl_readlink(vfs_inode_t *inode, char *buf, size_t sz)
{
	ovl_inode_t *oi = inode->priv;
	vfs_inode_t *real = active_inode(oi);
	if (real && real->iops && real->iops->readlink) return real->iops->readlink(real, buf, sz);
	return -22;
}

static int ovl_truncate(vfs_inode_t *inode, off_t len)
{
	ovl_inode_t *oi = inode->priv;
	if (!oi->upper) { if (copy_up_file(oi) != 0) return -28; }
	vfs_inode_t *real = oi->upper;
	if (real && real->iops && real->iops->truncate) {
		int r = real->iops->truncate(real, len);
		if (r == 0) inode->size = real->size;
		return r;
	}
	return -22;
}

static vfs_iops_t ovl_iops = {
	.lookup   = ovl_lookup,
	.create   = ovl_create,
	.mkdir    = ovl_mkdir,
	.unlink   = ovl_unlink,
	.readdir  = ovl_readdir,
	.stat     = ovl_stat,
	.readlink = ovl_readlink,
	.truncate = ovl_truncate,
};

/* ── mount ───────────────────────────────────────────────── */

/* extract opt=VALUE (value terminated by ',' or end) into out */
static int get_opt(const char *opts, const char *key, char *out, size_t sz)
{
	if (!opts) return -1;
	size_t klen = strlen(key);
	const char *p = opts;
	while (*p) {
		if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
			p += klen + 1;
			size_t i = 0;
			while (*p && *p != ',' && i < sz - 1) out[i++] = *p++;
			out[i] = '\0';
			return 0;
		}
		while (*p && *p != ',') p++;
		while (*p == ',') p++;
	}
	return -1;
}

static void ovl_get_sb(void *priv)
{
	ovl_sb_t *sb = priv;
	if (!sb) return;
	uint64_t f = spin_lock_irq(&sb->lock);
	sb->refcount++;
	spin_unlock_irq(&sb->lock, f);
}

static void ovl_kill_sb(void *priv)
{
	ovl_sb_t *sb = priv;
	if (!sb) return;
	uint64_t f = spin_lock_irq(&sb->lock);
	int last = (--sb->refcount == 0);
	spin_unlock_irq(&sb->lock, f);
	if (last) kfree(sb);
}

int overlay_mount(vfs_mount_t *mnt, void *data)
{
	char lower[192], upper[192], work[192];
	if (get_opt((const char *)data, "lowerdir", lower, sizeof(lower)) != 0) return -1;
	int have_upper = (get_opt((const char *)data, "upperdir", upper, sizeof(upper)) == 0);
	(void)get_opt((const char *)data, "workdir", work, sizeof(work));   /* parsed; unused (no rename-exchange yet) */

	vfs_dentry_t *ld = vfs_dentry_resolve(lower);
	if (!ld || !ld->inode || !S_ISDIR(ld->inode->mode)) return -1;
	vfs_dentry_t *ud = NULL;
	if (have_upper) {
		ud = vfs_dentry_resolve(upper);
		if (!ud || !ud->inode || !S_ISDIR(ud->inode->mode)) return -1;
	}

	ovl_sb_t *sb = kmalloc(sizeof(*sb));
	if (!sb) return -1;
	memset(sb, 0, sizeof(*sb));
	sb->lock = (spinlock_t)SPINLOCK_INIT;
	sb->refcount = 1;
	sb->lower_root = ld->inode;
	sb->upper_root = ud ? ud->inode : NULL;

	vfs_inode_t *root = ovl_make_inode(sb, sb->upper_root, sb->lower_root, sb->upper_root, "");
	if (!root) { kfree(sb); return -1; }

	vfs_dentry_t *root_dentry = vfs_dentry_create(NULL, "/", root);
	if (!root_dentry) { kfree(root->priv); kfree(root); kfree(sb); return -1; }

	mnt->root    = root_dentry;
	mnt->priv    = sb;
	mnt->kill_sb = ovl_kill_sb;
	mnt->get_sb  = ovl_get_sb;
	return 0;
}

void overlay_register(void)
{
	extern void vfs_register_fs(const char *, int (*)(vfs_mount_t *, void *));
	vfs_register_fs("overlay", overlay_mount);
}
