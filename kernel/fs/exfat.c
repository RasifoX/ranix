/*
 * exFAT, read-only. What most USB sticks and SD cards over 32 GB come formatted
 * with. Written from Microsoft's published exFAT specification.
 *
 * Layout: a boot sector names the FAT, the cluster heap and the root directory's
 * first cluster. A file is a run of clusters: either a chain through the 32-bit FAT,
 * or, when its stream extension says NoFatChain, simply contiguous. A directory is
 * a file of 32-byte entries; one file is an entry set: a File entry (0x85), a
 * Stream Extension (0xC0) with the first cluster and the sizes, and File Name
 * entries (0xC1) of 15 UTF-16 units each.
 *
 * Each mount has its own state, so two sticks can be in at once. Reads go to the
 * device a cluster run at a time, not a sector at a time.
 */
#include <kernel/vfs.h>
#include <kernel/hal.h>
#include <kernel/block.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>

#ifndef S_IFDIR
#define S_IFDIR 0040000
#define S_IFREG 0100000
#endif

#define EXFAT_EOC   0xFFFFFFFFu
#define EXFAT_BAD   0xFFFFFFF7u
#define RUN_MAX     (64 * 1024)          /* bytes read from the device in one go */

typedef struct {
	block_device_t *bdev;
	uint32_t sec_shift;               /* log2 bytes per sector, 9..12 */
	uint32_t clus_shift;              /* log2 sectors per cluster */
	uint64_t fat_lba;                 /* in 512-byte units */
	uint64_t heap_lba;
	uint32_t clus_count;
	uint32_t root_clus;
} exfat_fs_t;

typedef struct {
	exfat_fs_t *fs;
	uint32_t first;
	uint64_t size;                    /* DataLength */
	uint64_t valid;                   /* ValidDataLength: past it, zeros */
	int      nofat;                   /* contiguous, no FAT chain */
	int      is_dir;
} exfat_priv_t;

static vfs_iops_t exfat_dir_iops;
static vfs_fops_t exfat_file_fops;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static uint32_t clus_bytes(const exfat_fs_t *fs) { return 1u << (fs->sec_shift + fs->clus_shift); }

static uint64_t clus_lba(const exfat_fs_t *fs, uint32_t c)
{
	return fs->heap_lba + ((uint64_t)(c - 2) << (fs->sec_shift + fs->clus_shift - 9));
}

static int clus_ok(const exfat_fs_t *fs, uint32_t c) { return c >= 2 && c < fs->clus_count + 2; }

static uint32_t fat_next(exfat_fs_t *fs, uint32_t c)
{
	uint8_t sec[512];
	uint64_t off = (uint64_t)c * 4;
	if (fs->bdev->read(fs->bdev, fs->fat_lba + off / 512, sec, 1) != 0) return EXFAT_EOC;
	return rd32(sec + off % 512);
}

/* The cluster holding the n-th cluster-sized piece of a file, 0 past the end. */
static uint32_t nth_clus(exfat_priv_t *pv, uint64_t n)
{
	if (pv->nofat) {
		uint64_t c = (uint64_t)pv->first + n;
		return c < (uint64_t)pv->fs->clus_count + 2 ? (uint32_t)c : 0;
	}
	uint32_t c = pv->first;
	while (n-- && clus_ok(pv->fs, c)) c = fat_next(pv->fs, c);
	return clus_ok(pv->fs, c) ? c : 0;
}

static vfs_inode_t *make_inode(exfat_fs_t *fs, uint32_t first, uint64_t size, uint64_t valid,
                               int nofat, int is_dir)
{
	vfs_inode_t *vi = kmalloc(sizeof(*vi));
	exfat_priv_t *pv = kmalloc(sizeof(*pv));
	if (!vi || !pv) { kfree(vi); kfree(pv); return NULL; }
	memset(vi, 0, sizeof(*vi));
	pv->fs = fs; pv->first = first; pv->size = size; pv->valid = valid > size ? size : valid;
	pv->nofat = nofat; pv->is_dir = is_dir;
	vi->ino      = first ? first : (uint64_t)(uintptr_t)pv;
	vi->mode     = is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
	vi->size     = size;
	vi->nlink    = 1;
	vi->refcount = 1;
	vi->priv     = pv;
	vi->iops     = is_dir ? &exfat_dir_iops : NULL;
	vi->fops     = is_dir ? NULL : &exfat_file_fops;
	return vi;
}

/* ── directories ─────────────────────────────────────────────────────────── */

/* Walks a directory's entries in order. The sector holding entry i is kept, so a
 * set crossing a sector costs one more read, not one per entry. */
typedef struct {
	exfat_priv_t *dir;
	uint8_t  sec[512];
	uint64_t at;                     /* entry index of sec[0], or ~0 */
} dirwalk_t;

static const uint8_t *dir_entry(dirwalk_t *w, uint64_t i)
{
	uint64_t byte = i * 32;
	exfat_fs_t *fs = w->dir->fs;
	if (w->dir->size && byte >= w->dir->size) return NULL;   /* the root has no size: its chain ends it */
	uint64_t first = byte / 512 * 16;
	if (w->at != first) {
		uint32_t cb = clus_bytes(fs);
		uint32_t c = nth_clus(w->dir, byte / cb);
		if (!c) return NULL;
		uint64_t lba = clus_lba(fs, c) + (byte % cb) / 512;
		if (fs->bdev->read(fs->bdev, lba, w->sec, 1) != 0) return NULL;
		w->at = first;
	}
	return w->sec + (byte % 512);
}

typedef struct {
	char     name[256];
	int      is_dir, nofat;
	uint32_t first;
	uint64_t size, valid;
} exfat_ent_t;

static void put_utf8(char *out, size_t *n, size_t cap, uint32_t c)
{
	if (c < 0x80) { if (*n + 1 < cap) out[(*n)++] = (char)c; }
	else if (c < 0x800) { if (*n + 2 < cap) { out[(*n)++] = (char)(0xC0 | c >> 6); out[(*n)++] = (char)(0x80 | (c & 0x3F)); } }
	else if (c < 0x10000) { if (*n + 3 < cap) { out[(*n)++] = (char)(0xE0 | c >> 12); out[(*n)++] = (char)(0x80 | (c >> 6 & 0x3F)); out[(*n)++] = (char)(0x80 | (c & 0x3F)); } }
	else if (*n + 4 < cap) { out[(*n)++] = (char)(0xF0 | c >> 18); out[(*n)++] = (char)(0x80 | (c >> 12 & 0x3F)); out[(*n)++] = (char)(0x80 | (c >> 6 & 0x3F)); out[(*n)++] = (char)(0x80 | (c & 0x3F)); }
}

/* The next file's entry set at or after *i. 1 found, 0 end of directory. */
static int next_file(dirwalk_t *w, uint64_t *i, exfat_ent_t *e)
{
	for (;;) {
		const uint8_t *d = dir_entry(w, *i);
		if (!d || d[0] == 0x00) return 0;             /* end of directory */
		if (d[0] != 0x85) { (*i)++; continue; }       /* not a file: bitmap, upcase, label, unused */
		int count = d[1];
		int attr = rd16(d + 4);
		uint64_t at = *i;
		*i += 1 + (uint64_t)count;
		if (count < 2) continue;
		const uint8_t *s = dir_entry(w, at + 1);
		if (!s || s[0] != 0xC0) continue;
		int nlen = s[3];
		e->is_dir = (attr & 0x10) != 0;
		e->nofat  = (s[1] & 0x02) != 0;
		e->valid  = rd64(s + 8);
		e->first  = rd32(s + 20);
		e->size   = rd64(s + 24);
		size_t n = 0;
		uint32_t hi = 0;
		for (int k = 2; k <= count && nlen > 0; k++) {
			const uint8_t *f = dir_entry(w, at + (uint64_t)k);
			if (!f || f[0] != 0xC1) break;
			for (int j = 0; j < 15 && nlen > 0; j++, nlen--) {
				uint32_t u = rd16(f + 2 + 2 * j);
				if (u >= 0xD800 && u < 0xDC00) { hi = u; continue; }
				if (u >= 0xDC00 && u < 0xE000 && hi) { u = 0x10000 + ((hi - 0xD800) << 10) + (u - 0xDC00); hi = 0; }
				put_utf8(e->name, &n, sizeof e->name, u);
			}
		}
		e->name[n] = '\0';
		if (!n) continue;
		return 1;
	}
}

static int name_eq(const char *a, const char *b)
{
	if (!strcmp(a, b)) return 1;
	for (; *a && *b; a++, b++) {                  /* exFAT names are case-blind: ASCII here */
		char x = *a, y = *b;
		if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
		if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
		if (x != y) return 0;
	}
	return *a == *b;
}

static vfs_inode_t *exfat_lookup(vfs_inode_t *dir, const char *name)
{
	exfat_priv_t *pv = dir->priv;
	dirwalk_t *w = kmalloc(sizeof(*w));
	exfat_ent_t *e = kmalloc(sizeof(*e));
	vfs_inode_t *r = NULL;
	if (w && e) {
		w->dir = pv; w->at = ~0ULL;
		for (uint64_t i = 0; next_file(w, &i, e); )
			if (name_eq(e->name, name)) {
				r = make_inode(pv->fs, e->first, e->size, e->valid, e->nofat, e->is_dir);
				break;
			}
	}
	kfree(w); kfree(e);
	return r;
}

/* idx is the cookie: the entry index to go on from */
static int exfat_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	exfat_priv_t *pv = dir->priv;
	dirwalk_t *w = kmalloc(sizeof(*w));
	exfat_ent_t *e = kmalloc(sizeof(*e));
	int rc = -1;
	if (w && e) {
		w->dir = pv; w->at = ~0ULL;
		uint64_t i = idx;
		if (next_file(w, &i, e)) {
			out->ino = e->first ? e->first : i;
			out->mode = e->is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
			out->next_off = (uint32_t)i;
			size_t l = strlen(e->name);
			memcpy(out->name, e->name, l + 1);
			rc = 0;
		}
	}
	kfree(w); kfree(e);
	return rc;
}

/* ── files ───────────────────────────────────────────────────────────────── */

static ssize_t exfat_read(vfs_file_t *f, void *buf, size_t len)
{
	exfat_priv_t *pv = f->inode->priv;
	exfat_fs_t *fs = pv->fs;
	uint64_t pos = (uint64_t)f->pos;
	if (pos >= pv->size) return 0;
	if (len > pv->size - pos) len = (size_t)(pv->size - pos);
	uint32_t cb = clus_bytes(fs);
	uint8_t *run = kmalloc(RUN_MAX);
	if (!run) return -12;
	size_t done = 0;
	while (done < len) {
		uint64_t p = pos + done;
		size_t want = len - done;
		if (p >= pv->valid) {                          /* allocated, never written: zeros */
			size_t z = want > RUN_MAX ? RUN_MAX : want;
			memset(run, 0, z);
			if (copy_fop_to_user((uint8_t *)buf + done, run, z)) break;
			done += z;
			continue;
		}
		if (want > pv->valid - p) want = (size_t)(pv->valid - p);
		uint32_t c = nth_clus(pv, p / cb);
		if (!c) break;
		/* the rest of this cluster, and the following ones while they are contiguous */
		uint64_t in = p % cb;
		uint64_t span = cb - in;
		if (pv->nofat)
			span += (uint64_t)cb * (uint64_t)((RUN_MAX / cb) ? RUN_MAX / cb : 0);
		if (span > want) span = want;
		if (span > RUN_MAX - (in % 512)) span = RUN_MAX - (in % 512);
		uint64_t lba = clus_lba(fs, c) + in / 512;
		uint64_t secs = (in % 512 + span + 511) / 512;
		if (fs->bdev->read(fs->bdev, lba, run, secs) != 0) break;
		if (copy_fop_to_user((uint8_t *)buf + done, run + in % 512, (size_t)span)) break;
		done += (size_t)span;
	}
	kfree(run);
	f->pos += (off_t)done;
	return done ? (ssize_t)done : (pos < pv->size ? -5 : 0);
}

static vfs_iops_t exfat_dir_iops  = { .lookup = exfat_lookup, .readdir = exfat_readdir };
static vfs_fops_t exfat_file_fops = { .read = exfat_read };

/* ── mount ───────────────────────────────────────────────────────────────── */

int exfat_mount(vfs_mount_t *mnt, void *data)
{
	block_device_t *bdev = data;
	uint8_t bs[512];
	if (!bdev || bdev->read(bdev, 0, bs, 1) != 0) return -1;
	if (memcmp(bs + 3, "EXFAT   ", 8) != 0 || bs[510] != 0x55 || bs[511] != 0xAA) return -1;
	uint32_t sec_shift = bs[108], clus_shift = bs[109];
	if (sec_shift < 9 || sec_shift > 12 || sec_shift + clus_shift > 25) return -1;
	exfat_fs_t *fs = kmalloc(sizeof(*fs));
	if (!fs) return -1;
	fs->bdev       = bdev;
	fs->sec_shift  = sec_shift;
	fs->clus_shift = clus_shift;
	fs->fat_lba    = (uint64_t)rd32(bs + 80) << (sec_shift - 9);
	fs->heap_lba   = (uint64_t)rd32(bs + 88) << (sec_shift - 9);
	fs->clus_count = rd32(bs + 92);
	fs->root_clus  = rd32(bs + 96);
	if (!clus_ok(fs, fs->root_clus)) { kfree(fs); return -1; }
	vfs_inode_t *root = make_inode(fs, fs->root_clus, 0, 0, 0, 1);
	if (!root) { kfree(fs); return -1; }
	vfs_dentry_t *rd = vfs_dentry_create(NULL, "/", root);
	if (!rd) { kfree(root->priv); kfree(root); kfree(fs); return -1; }
	mnt->root = rd;
	kprintf(KERN_INFO "exfat: %u clusters of %u bytes\n", fs->clus_count, clus_bytes(fs));
	return 0;
}

void exfat_register(void)
{
	extern void vfs_register_fs(const char *, int (*)(vfs_mount_t *, void *));
	vfs_register_fs("exfat", exfat_mount);
}
