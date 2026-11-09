/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * NTFS mount for the VFS, over the parser in ntfs_core.c. Read-only without a switch
 * to change that: the operation tables below hold no create, write, truncate, rename
 * or setattr, the mount is locked read-only, and no call reaches block_write.
 *
 * Inodes are built per lookup and every directory is no_cache. A Windows tree has
 * more names than the dentry cache should hold, and dentry children are a list, so
 * one directory of a hundred thousand files would walk it on every miss. Names are
 * found through the table ntfs_core keeps per directory.
 *
 * Paths: the VFS walks absolute strings of at most PATH_MAX, and NTFS paths can be
 * longer. /<mount>/.ntfs-id/<record> reaches any file by MFT record number, so a tree
 * walker can go by the numbers getdents gives and never build the long string. It is
 * not listed by readdir.
 *
 * Mode bits say root only: nothing on the volume maps to a Ranix user, and a Windows
 * account's files should not become readable by every session on this machine.
 */
#include <kernel/vfs.h>
#include <kernel/block.h>
#include <kernel/sync.h>
#include <kernel/hal.h>
#include <kernel/kprintf.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>
#include <kernel/fs/ntfs_core.h>
#include <kernel/errno.h>

#define NTFS_BOUNCE   (128u * 1024)
#define NTFS_ID_DIR   ".ntfs-id"
#define NTFS_ID_INO   (1ULL << 60)
#define MFT_MASK      0x0000FFFFFFFFFFFFULL

#define FILE_MODE     (S_IFREG | 0400)
#define DIR_MODE      (S_IFDIR | 0500)
#define LINK_MODE     (S_IFLNK | 0400)

typedef struct {
	ntfs_vol_t     *v;
	block_device_t *bd;
	mutex_t         lock;       /* the parser keeps one set of scratch buffers */
	uint32_t        refs;       /* mounts and open files */
	uint32_t        mounts;
	vfs_dentry_t   *root;
	uint8_t        *bounce;     /* drivers may not fault on a user buffer */
	ntfs_info_t    *info;       /* big, one per mount under lock */
} ntfs_sb_t;

vfs_iops_t ntfs_dir_iops, ntfs_file_iops, ntfs_link_iops, ntfs_id_iops;   /* defined below */
vfs_fops_t ntfs_fops;

static int ntfs_dev_read(void *dev, uint64_t lba, void *buf, uint32_t n)
{
	return block_read(dev, lba, buf, n) == 0 ? 0 : -1;
}

static void sb_put(ntfs_sb_t *sb)
{
	if (atomic_sub_fetch32(&sb->refs, 1) != 0) return;
	ntfs_close(sb->v);
	kfree(sb->bounce);
	kfree(sb->info);
	kfree(sb);
}

static void sb_get(void *priv)
{
	ntfs_sb_t *sb = priv;
	atomic_fetch_add32(&sb->refs, 1);
	atomic_fetch_add32(&sb->mounts, 1);
}

static void ntfs_kill_sb(void *priv)
{
	ntfs_sb_t *sb = priv;
	if (atomic_sub_fetch32(&sb->mounts, 1) == 0)
		vfs_dentry_free_tree(sb->root, 1);
	sb_put(sb);
}

/* FILETIME: 100 ns units since 1601. Times before 1970 stop at the epoch. */
static void ft_split(uint64_t ft, int64_t *sec, uint32_t *nsec)
{
	const uint64_t epoch = 116444736000000000ULL;
	if (ft < epoch) { *sec = 0; *nsec = 0; return; }
	ft -= epoch;
	*sec = (int64_t)(ft / 10000000ULL);
	*nsec = (uint32_t)(ft % 10000000ULL) * 100;
}

static int fmt_hex(char *o, uint32_t v)
{
	static const char hex[] = "0123456789abcdef";
	for (int i = 0; i < 8; i++) o[i] = hex[(v >> (28 - 4 * i)) & 15];
	return 8;
}

static void xattr_str(vfs_inode_t *in, const char *name, const char *val)
{
	vfs_inode_setxattr(in, name, val, strlen(val), 0);
}

/* What this file is beyond its mode, for a copier to report: lives in memory on the
 * inode, never on the disk. */
static void inode_notes(vfs_inode_t *in, const ntfs_info_t *o, const char *longname)
{
	static const char *const why[] = { "", "encrypted", "wof", "cloud", "dedup", "reparse" };
	if (o->unreadable) xattr_str(in, "user.ntfs.unreadable", why[o->unreadable]);
	if (o->reparse_tag) {
		char t[11] = "0x";
		fmt_hex(t + 2, o->reparse_tag);
		t[10] = '\0';
		xattr_str(in, "user.ntfs.reparse_tag", t);
	}
	if (o->target[0]) xattr_str(in, "user.ntfs.target", o->target);
	if (longname) xattr_str(in, "user.ntfs.longname", longname);
	if (o->named_streams) {
		char t[12];
		int n = 0;
		uint32_t v = o->named_streams;
		char r[12];
		int k = 0;
		do { r[k++] = (char)('0' + v % 10); v /= 10; } while (v);
		while (k) t[n++] = r[--k];
		t[n] = '\0';
		xattr_str(in, "user.ntfs.streams", t);
	}
}

static vfs_inode_t *inode_new(ntfs_sb_t *sb, const ntfs_info_t *o, const char *longname)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino = o->mft;
	in->nlink = o->links ? o->links : 1;
	in->i_sb = sb;
	ft_split(o->atime, &in->atime, &in->atime_nsec);
	ft_split(o->mtime, &in->mtime, &in->mtime_nsec);
	ft_split(o->ctime, &in->ctime, &in->ctime_nsec);
	if (o->is_link) {
		size_t n = strlen(o->target);
		char *t = kmalloc(n + 1);
		if (!t) { kfree(in); return NULL; }
		memcpy(t, o->target, n + 1);
		in->mode = LINK_MODE;
		in->size = n;
		in->priv = t;
		in->iops = &ntfs_link_iops;
		/* no fops: the walk treats a symlink with fops as a magic link */
	} else if (o->is_dir) {
		in->mode = DIR_MODE;
		in->iops = &ntfs_dir_iops;
		in->fops = &ntfs_fops;
		in->no_cache = 1;
		in->dotdot_lookup = 1;
	} else {
		in->mode = FILE_MODE;
		in->size = o->size;
		in->iops = &ntfs_file_iops;
		in->fops = &ntfs_fops;
	}
	inode_notes(in, o, longname);
	return in;
}

/* Caller holds sb->lock. */
static vfs_inode_t *inode_by_ref(ntfs_sb_t *sb, uint64_t mref, const char *longname)
{
	if (ntfs_get_info(sb->v, mref, sb->info) != 0) return NULL;
	return inode_new(sb, sb->info, longname);
}

static vfs_inode_t *id_dir_inode(ntfs_sb_t *sb)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino = NTFS_ID_INO;
	in->mode = S_IFDIR | 0500;
	in->nlink = 2;
	in->i_sb = sb;
	in->iops = &ntfs_id_iops;
	in->fops = &ntfs_fops;
	in->no_cache = 1;
	return in;
}

static vfs_inode_t *ntfs_lookup(vfs_inode_t *dir, const char *name)
{
	ntfs_sb_t *sb = dir->i_sb;
	vfs_inode_t *in = NULL;
	mutex_lock(&sb->lock);
	if (strcmp(name, "..") == 0) {
		uint64_t up;
		if (dir->ino != NTFS_MFT_ROOT && ntfs_parent(sb->v, dir->ino, &up) == 0 &&
		    up != NTFS_MFT_ROOT)
			in = inode_by_ref(sb, up, NULL);
	} else if (dir->ino == NTFS_MFT_ROOT && strcmp(name, NTFS_ID_DIR) == 0) {
		in = id_dir_inode(sb);
	} else {
		ntfs_dir_t *d;
		ntfs_dirent_t e;
		if (ntfs_dir_load(sb->v, dir->ino, &d) == 0 && ntfs_dir_find(d, name, &e) == 0) {
			/* the table may go on a load, the long name with it: keep a copy */
			char *ln = NULL;
			if (e.longname) {
				size_t n = strlen(e.longname);
				ln = kmalloc(n + 1);
				if (ln) memcpy(ln, e.longname, n + 1);
			}
			in = inode_by_ref(sb, e.mref, ln);
			kfree(ln);
		}
	}
	mutex_unlock(&sb->lock);
	return in;
}

static int ntfs_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	ntfs_sb_t *sb = dir->i_sb;
	ntfs_dir_t *d;
	ntfs_dirent_t e;
	mutex_lock(&sb->lock);
	int rc = ntfs_dir_load(sb->v, dir->ino, &d);
	if (rc) {
		mutex_unlock(&sb->lock);
		return rc == -1 ? -5 : rc;
	}
	rc = ntfs_dir_entry(d, idx, &e);
	if (rc) {
		/* the end, unless part of the directory could not be read */
		rc = ntfs_dir_damaged(d) ? -5 : -1;
		mutex_unlock(&sb->lock);
		return rc;
	}
	size_t n = strlen(e.name);
	memcpy(out->name, e.name, n + 1);
	out->ino = e.mref & MFT_MASK;
	out->mode = e.is_reparse ? 0 : e.is_dir ? S_IFDIR : S_IFREG;
	out->next_off = idx + 1;
	mutex_unlock(&sb->lock);
	return 0;
}

static vfs_inode_t *ntfs_id_lookup(vfs_inode_t *dir, const char *name)
{
	ntfs_sb_t *sb = dir->i_sb;
	uint64_t no = 0;
	if (!*name) return NULL;
	for (const char *p = name; *p; p++) {
		if (*p < '0' || *p > '9' || no > MFT_MASK / 10) return NULL;
		no = no * 10 + (uint64_t)(*p - '0');
	}
	mutex_lock(&sb->lock);
	vfs_inode_t *in = no <= MFT_MASK ? inode_by_ref(sb, no, NULL) : NULL;
	mutex_unlock(&sb->lock);
	return in;
}

static int ntfs_id_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	return -1;
}

static int ntfs_stat(vfs_inode_t *in, vfs_stat_t *out)
{
	out->ino   = in->ino;
	out->mode  = in->mode;
	out->nlink = in->nlink;
	out->uid   = 0;
	out->gid   = 0;
	out->size  = in->size;
	out->rdev  = 0;
	out->atime = in->atime; out->atime_nsec = in->atime_nsec;
	out->mtime = in->mtime; out->mtime_nsec = in->mtime_nsec;
	out->ctime = in->ctime; out->ctime_nsec = in->ctime_nsec;
	return 0;
}

/* Called from the walk under the VFS spinlock, so it uses only the inode's copy. */
static int ntfs_readlink(vfs_inode_t *in, char *buf, size_t bufsize)
{
	size_t n = (size_t)in->size;
	if (!in->priv || n >= bufsize) return -1;
	memcpy(buf, in->priv, n + 1);
	return 0;
}

static int ntfs_statfs(vfs_inode_t *in, vfs_statfs_t *out)
{
	ntfs_sb_t *sb = in->i_sb;
	ntfs_volinfo_t vi;
	mutex_lock(&sb->lock);
	ntfs_volinfo(sb->v, &vi);
	mutex_unlock(&sb->lock);
	memset(out, 0, sizeof(*out));
	out->bsize   = vi.cluster_size;
	out->blocks  = vi.total_clusters;
	out->namelen = NTFS_NAME_MAX;
	return 0;
}

static int ntfs_open_file(vfs_inode_t *in, vfs_file_t *f, int flags)
{
	if ((flags & 3) != O_RDONLY) return -EROFS;
	atomic_fetch_add32(&((ntfs_sb_t *)in->i_sb)->refs, 1);
	return 0;
}

static int ntfs_close_file(vfs_file_t *f)
{
	sb_put(f->inode->i_sb);
	return 0;
}

static ssize_t ntfs_read(vfs_file_t *f, void *buf, size_t len)
{
	vfs_inode_t *in = f->inode;
	ntfs_sb_t *sb = in->i_sb;
	uint8_t *out = buf;
	size_t done = 0;
	if (f->pos < 0) return -EINVAL;
	while (done < len) {
		uint32_t n = len - done > NTFS_BOUNCE ? NTFS_BOUNCE : (uint32_t)(len - done);
		/* one chunk under the lock, so a long read leaves room for lookups */
		mutex_lock(&sb->lock);
		long r = ntfs_file_read(sb->v, in->ino, (uint64_t)f->pos, sb->bounce, n);
		if (r > 0 && copy_fop_to_user(out + done, sb->bounce, (size_t)r) != 0) r = -14;
		mutex_unlock(&sb->lock);
		if (r < 0) return done ? (ssize_t)done : (ssize_t)r;
		if (!r) break;
		f->pos += (off_t)r;
		done += (size_t)r;
	}
	return (ssize_t)done;
}

static off_t ntfs_seek(vfs_file_t *f, off_t off, int whence)
{
	off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? f->pos :
	             whence == SEEK_END ? (off_t)f->inode->size : -1;
	if (base < 0 || base + off < 0) return -EINVAL;
	f->pos = base + off;
	return f->pos;
}

vfs_iops_t ntfs_dir_iops  = { .lookup = ntfs_lookup, .readdir = ntfs_readdir,
                              .stat = ntfs_stat, .statfs = ntfs_statfs };
vfs_iops_t ntfs_file_iops = { .stat = ntfs_stat, .statfs = ntfs_statfs };
vfs_iops_t ntfs_link_iops = { .stat = ntfs_stat, .readlink = ntfs_readlink };
vfs_iops_t ntfs_id_iops   = { .lookup = ntfs_id_lookup, .readdir = ntfs_id_readdir,
                              .stat = ntfs_stat };
vfs_fops_t ntfs_fops      = { .open = ntfs_open_file, .close = ntfs_close_file,
                              .read = ntfs_read, .seek = ntfs_seek };

static void volume_notes(vfs_inode_t *root, const ntfs_volinfo_t *vi)
{
	char t[32], *o = t;
	*o++ = (char)('0' + vi->major % 10); *o++ = '.'; *o++ = (char)('0' + vi->minor % 10);
	*o = '\0';
	xattr_str(root, "user.ntfs.version", t);
	xattr_str(root, "user.ntfs.dirty", vi->dirty ? "1" : "0");
	xattr_str(root, "user.ntfs.hibernated", vi->hibernated ? "1" : "0");
	xattr_str(root, "user.ntfs.label", vi->label);
}

static int ntfs_mount(vfs_mount_t *mnt, void *data)
{
	block_device_t *bd = data;
	if (!bd) return -1;
	ntfs_sb_t *sb = kmalloc(sizeof(*sb));
	if (!sb) return -1;
	memset(sb, 0, sizeof(*sb));
	sb->bd = bd;
	sb->lock = (mutex_t)MUTEX_INIT;
	sb->refs = 1;
	sb->mounts = 1;
	sb->bounce = kmalloc(NTFS_BOUNCE);
	sb->info = kmalloc(sizeof(*sb->info));
	if (!sb->bounce || !sb->info) goto fail;

	int rc = ntfs_open(ntfs_dev_read, bd, bd->size_blocks, &sb->v);
	if (rc == NTFS_EBITLOCKER) {
		kprintf(KERN_WARN "ntfs: %s is BitLocker encrypted (-FVE-FS-): nothing here can "
		        "read it, unlock it on Windows or with its recovery key elsewhere\n", bd->name);
		goto fail;
	}
	if (rc) {
		kprintf(KERN_WARN "ntfs: %s not mounted, error %d\n", bd->name, rc);
		goto fail;
	}

	ntfs_volinfo_t vi;
	ntfs_volinfo(sb->v, &vi);
	if (vi.dirty)
		kprintf(KERN_WARN "ntfs: %s is dirty, $LogFile not replayed: recent changes may be "
		        "missing or half written\n", bd->name);
	if (vi.hibernated)
		kprintf(KERN_WARN "ntfs: %s has a live hibernation image: Windows did not shut down "
		        "and the disk may be behind what it last showed\n", bd->name);

	vfs_inode_t *root = inode_by_ref(sb, NTFS_MFT_ROOT, NULL);
	if (!root) goto fail_vol;
	volume_notes(root, &vi);
	sb->root = vfs_dentry_create(NULL, "/", root);
	if (!sb->root) { kfree(root); goto fail_vol; }

	mnt->priv    = sb;
	mnt->kill_sb = ntfs_kill_sb;
	mnt->get_sb  = sb_get;
	mnt->root    = sb->root;
	mnt->flags  |= MS_RDONLY;
	mnt->ro_lock = 1;
	kprintf(KERN_INFO "ntfs: mounted %s read-only, NTFS %u.%u, %u byte clusters\n", bd->name,
	        vi.major, vi.minor, vi.cluster_size);
	return 0;

fail_vol:
	ntfs_close(sb->v);
fail:
	kfree(sb->bounce);
	kfree(sb->info);
	kfree(sb);
	return -1;
}

void ntfs_register(void)
{
	vfs_register_fs("ntfs", ntfs_mount);
}
