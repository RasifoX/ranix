/*
 * ISO 9660 (ECM-119) — the CD/DVD filesystem, read-only. Linux mounts this for
 * every optical medium and many install images; Ranix reads it so a data CD or
 * an ISO attached as a disk is browsable.
 *
 * Layout: 2048-byte logical sectors; the Primary Volume Descriptor sits at
 * sector 16 and points at the root directory record. Each directory is a packed
 * run of variable-length records; a file's data is one contiguous extent. We
 * decode plain 8.3 names (strip the ";1" version, lowercase) — enough to read a
 * standard image.
 */
#include <kernel/vfs.h>
#include <kernel/block.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>

#ifndef S_IFDIR
#define S_IFDIR 0040000
#define S_IFREG 0100000
#endif

#define ISO_SECTOR 2048

typedef struct { block_device_t *bdev; } iso_fs_t;
typedef struct { iso_fs_t *fs; uint32_t extent; uint32_t size; int is_dir; } iso_priv_t;

static iso_fs_t   g_iso;
static vfs_iops_t iso_dir_iops;
static vfs_fops_t iso_file_fops;

static uint32_t rd32le(const uint8_t *p) { return p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24); }

/* read one 2048-byte ISO sector (= 4 of the 512-byte block-device sectors) */
static int iso_rd(iso_fs_t *fs, uint32_t iso_sec, uint8_t *buf)
{
	return fs->bdev->read(fs->bdev, (uint64_t)iso_sec * 4, buf, 4);
}

static vfs_inode_t *iso_make_inode(iso_fs_t *fs, uint32_t extent, uint32_t size, int is_dir)
{
	vfs_inode_t *vi = kmalloc(sizeof(*vi));
	if (!vi) return NULL;
	iso_priv_t *pv = kmalloc(sizeof(*pv));
	if (!pv) { kfree(vi); return NULL; }
	memset(vi, 0, sizeof(*vi));
	pv->fs = fs; pv->extent = extent; pv->size = size; pv->is_dir = is_dir;
	vi->ino = extent; vi->size = size; vi->nlink = 1; vi->refcount = 1; vi->priv = pv;
	vi->mode = is_dir ? (S_IFDIR | 0555) : (S_IFREG | 0444);
	vi->iops = is_dir ? &iso_dir_iops : NULL;
	vi->fops = is_dir ? NULL : &iso_file_fops;
	return vi;
}

/* Normalise an ISO directory-record name into out: strip ";version", drop a
 * trailing '.', lowercase. Returns 0 for the "." (0x00) and ".." (0x01) self
 * entries, which the caller skips. */
static int iso_name(const uint8_t *rec, char *out)
{
	uint8_t nl = rec[32];
	const uint8_t *nm = rec + 33;
	if (nl == 1 && (nm[0] == 0x00 || nm[0] == 0x01)) return 0;   /* . / .. */
	int n = 0;
	for (int i = 0; i < nl && n < 64; i++) {
		char c = nm[i];
		if (c == ';') break;
		if (c >= 'A' && c <= 'Z') c += 32;
		out[n++] = c;
	}
	while (n > 0 && out[n-1] == '.') n--;   /* trailing dot */
	out[n] = '\0';
	return n;
}

/* Walk a directory extent, calling cb(name, record, arg) per real entry. */
static int iso_walk(iso_fs_t *fs, uint32_t extent, uint32_t size,
                    int (*cb)(const char *, const uint8_t *, void *), void *arg)
{
	uint8_t sec[ISO_SECTOR];
	uint32_t nsec = (size + ISO_SECTOR - 1) / ISO_SECTOR;
	for (uint32_t s = 0; s < nsec; s++) {
		if (iso_rd(fs, extent + s, sec) != 0) return 0;
		uint32_t off = 0;
		while (off < ISO_SECTOR) {
			const uint8_t *rec = sec + off;
			uint8_t len = rec[0];
			if (len == 0) break;                 /* rest of sector is padding */
			if (off + len > ISO_SECTOR) break;
			char name[72];
			if (iso_name(rec, name)) {
				if (cb(name, rec, arg)) return 1;
			}
			off += len;
		}
	}
	return 0;
}

struct iso_lookup_ctx { const char *want; vfs_inode_t *res; };
static int iso_lookup_cb(const char *name, const uint8_t *rec, void *arg)
{
	struct iso_lookup_ctx *c = arg;
	if (strcmp(name, c->want) != 0) return 0;
	c->res = iso_make_inode(&g_iso, rd32le(rec + 2), rd32le(rec + 10), (rec[25] & 0x02) != 0);
	return 1;
}
static vfs_inode_t *iso_lookup(vfs_inode_t *dir, const char *name)
{
	iso_priv_t *pv = dir->priv;
	struct iso_lookup_ctx c = { name, NULL };
	iso_walk(pv->fs, pv->extent, pv->size, iso_lookup_cb, &c);
	return c.res;
}

struct iso_rd_ctx { uint32_t want, cur; vfs_dirent_t *out; int found; };
static int iso_readdir_cb(const char *name, const uint8_t *rec, void *arg)
{
	struct iso_rd_ctx *c = arg;
	if (c->cur == c->want) {
		c->out->ino      = rd32le(rec + 2);
		c->out->mode     = (rec[25] & 0x02) ? (S_IFDIR | 0555) : (S_IFREG | 0444);
		c->out->next_off = c->want + 1;
		size_t l = strlen(name); if (l > 255) l = 255;
		memcpy(c->out->name, name, l); c->out->name[l] = '\0';
		c->found = 1; return 1;
	}
	c->cur++; return 0;
}
static int iso_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	iso_priv_t *pv = dir->priv;
	struct iso_rd_ctx c = { idx, 0, out, 0 };
	iso_walk(pv->fs, pv->extent, pv->size, iso_readdir_cb, &c);
	return c.found ? 0 : -1;
}

static ssize_t iso_read(vfs_file_t *f, void *buf, size_t len)
{
	iso_priv_t *pv = f->inode->priv;
	if ((uint64_t)f->pos >= pv->size) return 0;
	if (f->pos + len > pv->size) len = pv->size - f->pos;
	uint8_t sec[ISO_SECTOR];
	size_t done = 0;
	while (done < len) {
		uint64_t pos = f->pos + done;
		uint32_t s   = (uint32_t)(pos / ISO_SECTOR);
		uint32_t soff = (uint32_t)(pos % ISO_SECTOR);
		if (iso_rd(pv->fs, pv->extent + s, sec) != 0) break;
		uint32_t n = ISO_SECTOR - soff;
		if (n > len - done) n = (uint32_t)(len - done);
		memcpy((uint8_t *)buf + done, sec + soff, n);
		done += n;
	}
	f->pos += (off_t)done;
	return (ssize_t)done;
}

static vfs_iops_t iso_dir_iops  = { .lookup = iso_lookup, .readdir = iso_readdir };
static vfs_fops_t iso_file_fops = { .read = iso_read };

int iso9660_mount(vfs_mount_t *mnt, void *data)
{
	block_device_t *bdev = data;
	if (!bdev) return -1;
	uint8_t pvd[ISO_SECTOR];
	iso_fs_t *fs = &g_iso;
	fs->bdev = bdev;
	if (iso_rd(fs, 16, pvd) != 0) return -1;            /* PVD at sector 16 */
	if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) return -1;

	const uint8_t *root = pvd + 156;                    /* root directory record */
	uint32_t extent = rd32le(root + 2), size = rd32le(root + 10);
	vfs_inode_t *root_vi = iso_make_inode(fs, extent, size, 1);
	if (!root_vi) return -1;
	vfs_dentry_t *rd = vfs_dentry_create(NULL, "/", root_vi);
	if (!rd) { kfree(root_vi->priv); kfree(root_vi); return -1; }
	mnt->root = rd;
	kprintf("iso9660: mounted (root extent=%u size=%u)\n", extent, size);
	return 0;
}

void iso9660_register(void)
{
	extern void vfs_register_fs(const char *, int (*)(vfs_mount_t *, void *));
	vfs_register_fs("iso9660", iso9660_mount);
	vfs_register_fs("iso", iso9660_mount);
}
