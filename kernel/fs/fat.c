/*
 * FAT filesystem — FAT12 / FAT16 / FAT32, read-only with long-filename (VFAT)
 * support. The one on-disk format Linux ships for every removable/EFI medium;
 * Ranix mounts it so USB sticks, SD cards and ESP partitions are readable.
 *
 * Layout: [reserved (incl. boot sector)] [FATs] [root dir (12/16 fixed; 32 in
 * the data area)] [data clusters]. A file/dir is a singly-linked chain of
 * clusters threaded through the FAT. Directory entries are 32 bytes; a VFAT long
 * name is a run of 0x0F-attribute entries (UTF-16, in reverse) preceding the 8.3
 * entry.
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

typedef struct {
	block_device_t *bdev;
	int       type;            /* 12, 16 or 32 */
	uint32_t  bytes_per_sec;
	uint32_t  sec_per_clus;
	uint32_t  rsvd_sec;
	uint32_t  num_fats;
	uint32_t  fat_sz;          /* sectors per FAT */
	uint32_t  root_ent_cnt;    /* FAT12/16 */
	uint32_t  root_clus;       /* FAT32 */
	uint64_t  fat_lba;         /* first FAT sector */
	uint64_t  root_lba;        /* FAT12/16 fixed root-dir sector */
	uint64_t  data_lba;        /* first data sector (cluster 2) */
	uint32_t  total_clus;
} fat_fs_t;

typedef struct {
	fat_fs_t *fs;
	uint32_t  first_clus;
	uint32_t  size;
	int       is_dir;
} fat_priv_t;

static fat_fs_t g_fat;
static vfs_iops_t fat_dir_iops;
static vfs_fops_t fat_file_fops;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24)); }

/* read one 512-byte sector */
static int fat_rd_sec(fat_fs_t *fs, uint64_t lba, uint8_t *buf)
{
	return fs->bdev->read(fs->bdev, lba, buf, 1);
}

/* next cluster in the chain, or >= EOC sentinel when the chain ends */
static uint32_t fat_next(fat_fs_t *fs, uint32_t clus)
{
	uint8_t sec[512];
	if (fs->type == 32) {
		uint64_t off = (uint64_t)clus * 4;
		if (fat_rd_sec(fs, fs->fat_lba + off / 512, sec) != 0) return 0x0FFFFFFF;
		return rd32(sec + (off % 512)) & 0x0FFFFFFF;
	} else if (fs->type == 16) {
		uint64_t off = (uint64_t)clus * 2;
		if (fat_rd_sec(fs, fs->fat_lba + off / 512, sec) != 0) return 0xFFFF;
		return rd16(sec + (off % 512));
	} else { /* FAT12: 1.5 bytes/entry, may straddle a sector boundary */
		uint64_t off = (uint64_t)clus + (clus / 2);
		uint8_t lo, hi;
		if (fat_rd_sec(fs, fs->fat_lba + off / 512, sec) != 0) return 0xFFF;
		lo = sec[off % 512];
		if ((off % 512) == 511) { if (fat_rd_sec(fs, fs->fat_lba + off / 512 + 1, sec) != 0) return 0xFFF; hi = sec[0]; }
		else hi = sec[(off % 512) + 1];
		uint16_t v = (uint16_t)(lo | (hi << 8));
		return (clus & 1) ? (v >> 4) : (v & 0x0FFF);
	}
}

static int fat_is_eoc(fat_fs_t *fs, uint32_t c)
{
	if (fs->type == 32) return c >= 0x0FFFFFF8;
	if (fs->type == 16) return c >= 0xFFF8;
	return c >= 0xFF8;
}

static uint64_t clus_to_lba(fat_fs_t *fs, uint32_t clus)
{
	return fs->data_lba + (uint64_t)(clus - 2) * fs->sec_per_clus;
}

static vfs_inode_t *fat_make_inode(fat_fs_t *fs, uint32_t first_clus, uint32_t size, int is_dir)
{
	vfs_inode_t *vi = kmalloc(sizeof(*vi));
	if (!vi) return NULL;
	fat_priv_t *pv = kmalloc(sizeof(*pv));
	if (!pv) { kfree(vi); return NULL; }
	memset(vi, 0, sizeof(*vi));
	pv->fs = fs; pv->first_clus = first_clus; pv->size = size; pv->is_dir = is_dir;
	vi->ino      = first_clus ? first_clus : (uint64_t)(uintptr_t)pv;
	vi->mode     = is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
	vi->size     = size;
	vi->nlink    = 1;
	vi->refcount = 1;
	vi->priv     = pv;
	vi->iops     = is_dir ? &fat_dir_iops : NULL;
	vi->fops     = is_dir ? NULL : &fat_file_fops;
	return vi;
}

/* Decode an 8.3 entry name into out (lowercased, with '.'). Returns 0 if the
 * slot is free/deleted/volume-label and should be skipped. */
static int fat_83_name(const uint8_t *de, char *out)
{
	if (de[0] == 0x00 || de[0] == 0xE5) return 0;       /* end / deleted */
	if (de[11] & 0x08) return 0;                         /* volume label */
	int n = 0;
	for (int i = 0; i < 8 && de[i] != ' '; i++) {
		char c = de[i]; if (c >= 'A' && c <= 'Z') c += 32;
		out[n++] = c;
	}
	if (de[8] != ' ') {
		out[n++] = '.';
		for (int i = 8; i < 11 && de[i] != ' '; i++) {
			char c = de[i]; if (c >= 'A' && c <= 'Z') c += 32;
			out[n++] = c;
		}
	}
	out[n] = '\0';
	return n;
}

/* Pull the 13 UTF-16 chars of one LFN slot into name[] at the slot's offset
 * (slots arrive in reverse order; seq is 1-based). ASCII-only, others → '_'. */
static void fat_lfn_chars(const uint8_t *de, char *name)
{
	int seq = (de[0] & 0x1F);
	if (seq < 1 || seq > 20) return;
	int base = (seq - 1) * 13;
	const int off[13] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };
	for (int i = 0; i < 13; i++) {
		uint16_t w = rd16(de + off[i]);
		if (w == 0 || w == 0xFFFF) { name[base + i] = '\0'; return; }
		name[base + i] = (w < 0x80) ? (char)w : '_';
	}
}

/* Walk a directory's entries. For each real entry call cb(name, de, arg); stop
 * when cb returns non-zero (lookup hit). dir_clus==0 with FAT12/16 means the
 * fixed root area. Returns 1 if cb stopped it, else 0. */
static int fat_walk_dir(fat_fs_t *fs, uint32_t dir_clus, int fixed_root,
                        int (*cb)(const char *, const uint8_t *, void *), void *arg)
{
	uint8_t sec[512];
	char lfn[261];
	lfn[0] = '\0';
	int have_lfn = 0;

	uint32_t clus = dir_clus;
	uint64_t fixed_lba = fs->root_lba;
	uint32_t fixed_secs = (fs->root_ent_cnt * 32 + 511) / 512;
	uint64_t fixed_done = 0;

	for (;;) {
		uint32_t secs_this;
		uint64_t base_lba;
		if (fixed_root) {
			if (fixed_done >= fixed_secs) break;
			base_lba = fixed_lba + fixed_done;
			secs_this = 1;
		} else {
			if (clus < 2 || fat_is_eoc(fs, clus)) break;
			base_lba = clus_to_lba(fs, clus);
			secs_this = fs->sec_per_clus;
		}
		for (uint32_t s = 0; s < secs_this; s++) {
			if (fat_rd_sec(fs, base_lba + s, sec) != 0) return 0;
			for (int e = 0; e < 512; e += 32) {
				const uint8_t *de = sec + e;
				if (de[0] == 0x00) return 0;                 /* end of directory */
				if (de[0] == 0xE5) { have_lfn = 0; continue; }
				if (de[11] == 0x0F) {                         /* LFN slot */
					fat_lfn_chars(de, lfn); have_lfn = 1; continue;
				}
				char n83[16];
				char *name;
				if (have_lfn && lfn[0]) name = lfn;
				else { if (!fat_83_name(de, n83)) { have_lfn = 0; continue; } name = n83; }
				int stop = cb(name, de, arg);
				have_lfn = 0; lfn[0] = '\0';
				if (stop) return 1;
			}
		}
		if (fixed_root) fixed_done++;
		else clus = fat_next(fs, clus);
	}
	return 0;
}

/* ── lookup ──────────────────────────────────────────────── */
struct lookup_ctx { const char *want; vfs_inode_t *result; };

static int lookup_cb(const char *name, const uint8_t *de, void *arg)
{
	struct lookup_ctx *c = arg;
	if (strcmp(name, c->want) != 0) return 0;
	uint32_t first = ((uint32_t)rd16(de + 20) << 16) | rd16(de + 26);
	int is_dir = (de[11] & 0x10) != 0;
	c->result = fat_make_inode(&g_fat, first, rd32(de + 28), is_dir);
	return 1;
}

static vfs_inode_t *fat_lookup(vfs_inode_t *dir, const char *name)
{
	fat_priv_t *pv = dir->priv;
	struct lookup_ctx c = { name, NULL };
	int fixed = (pv->fs->type != 32 && pv->first_clus == 0);
	fat_walk_dir(pv->fs, pv->first_clus, fixed, lookup_cb, &c);
	return c.result;
}

/* ── readdir ─────────────────────────────────────────────── */
struct rd_ctx { uint32_t want_idx, cur; vfs_dirent_t *out; int found; };

static int readdir_cb(const char *name, const uint8_t *de, void *arg)
{
	struct rd_ctx *c = arg;
	if (c->cur == c->want_idx) {
		uint32_t first = ((uint32_t)rd16(de + 20) << 16) | rd16(de + 26);
		c->out->ino      = first ? first : c->cur + 1;
		c->out->mode     = (de[11] & 0x10) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
		c->out->next_off = c->want_idx + 1;
		size_t l = strlen(name); if (l > 255) l = 255;
		memcpy(c->out->name, name, l); c->out->name[l] = '\0';
		c->found = 1;
		return 1;
	}
	c->cur++;
	return 0;
}

static int fat_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	fat_priv_t *pv = dir->priv;
	struct rd_ctx c = { idx, 0, out, 0 };
	int fixed = (pv->fs->type != 32 && pv->first_clus == 0);
	fat_walk_dir(pv->fs, pv->first_clus, fixed, readdir_cb, &c);
	return c.found ? 0 : -1;
}

/* ── file read ───────────────────────────────────────────── */
static ssize_t fat_read(vfs_file_t *f, void *buf, size_t len)
{
	fat_priv_t *pv = f->inode->priv;
	fat_fs_t   *fs = pv->fs;
	if ((uint64_t)f->pos >= pv->size) return 0;
	if (f->pos + len > pv->size) len = pv->size - f->pos;

	uint32_t clus_bytes = fs->sec_per_clus * 512;
	uint64_t pos = f->pos;
	size_t   done = 0;
	uint8_t  sec[512];

	/* skip to the cluster holding pos */
	uint32_t clus = pv->first_clus;
	uint64_t skip = pos / clus_bytes;
	while (skip-- > 0 && !fat_is_eoc(fs, clus)) clus = fat_next(fs, clus);

	uint64_t in_clus = pos % clus_bytes;
	while (done < len && clus >= 2 && !fat_is_eoc(fs, clus)) {
		uint64_t base = clus_to_lba(fs, clus);
		while (in_clus < clus_bytes && done < len) {
			uint32_t s = (uint32_t)(in_clus / 512);
			if (fat_rd_sec(fs, base + s, sec) != 0) return (ssize_t)done;
			uint32_t soff = in_clus % 512;
			uint32_t n = 512 - soff;
			if (n > len - done) n = (uint32_t)(len - done);
			memcpy((uint8_t *)buf + done, sec + soff, n);
			done += n; in_clus += n;
		}
		in_clus = 0;
		clus = fat_next(fs, clus);
	}
	f->pos += (off_t)done;
	return (ssize_t)done;
}

static vfs_iops_t fat_dir_iops  = { .lookup = fat_lookup, .readdir = fat_readdir };
static vfs_fops_t fat_file_fops = { .read = fat_read };

/* ── mount ───────────────────────────────────────────────── */
int fat_mount(vfs_mount_t *mnt, void *data)
{
	block_device_t *bdev = data;
	if (!bdev) return -1;
	uint8_t bpb[512];
	if (bdev->read(bdev, 0, bpb, 1) != 0) return -1;

	uint32_t bps = rd16(bpb + 11);
	if (bps != 512) return -1;                       /* we assume 512-byte sectors */
	if (bpb[510] != 0x55 || bpb[511] != 0xAA) return -1;

	fat_fs_t *fs = &g_fat;
	fs->bdev          = bdev;
	fs->bytes_per_sec = bps;
	fs->sec_per_clus  = bpb[13] ? bpb[13] : 1;
	fs->rsvd_sec      = rd16(bpb + 14);
	fs->num_fats      = bpb[16];
	fs->root_ent_cnt  = rd16(bpb + 17);
	uint32_t fat_sz16 = rd16(bpb + 22);
	uint32_t tot16    = rd16(bpb + 19);
	uint32_t tot32    = rd32(bpb + 32);
	uint32_t fat_sz32 = rd32(bpb + 36);
	fs->fat_sz   = fat_sz16 ? fat_sz16 : fat_sz32;
	fs->root_clus = rd32(bpb + 44);
	if (!fs->num_fats || !fs->fat_sz) return -1;

	uint32_t tot_sec   = tot16 ? tot16 : tot32;
	uint32_t root_secs = (fs->root_ent_cnt * 32 + bps - 1) / bps;
	fs->fat_lba  = fs->rsvd_sec;
	fs->root_lba = fs->rsvd_sec + fs->num_fats * fs->fat_sz;
	fs->data_lba = fs->root_lba + root_secs;
	uint32_t data_sec = tot_sec - (uint32_t)fs->data_lba;
	fs->total_clus = data_sec / fs->sec_per_clus;
	fs->type = (fs->total_clus < 4085) ? 12 : (fs->total_clus < 65525) ? 16 : 32;

	uint32_t root_first = (fs->type == 32) ? fs->root_clus : 0;
	vfs_inode_t *root_vi = fat_make_inode(fs, root_first, 0, 1);
	if (!root_vi) return -1;
	vfs_dentry_t *root_dentry = vfs_dentry_create(NULL, "/", root_vi);
	if (!root_dentry) { kfree(root_vi->priv); kfree(root_vi); return -1; }
	mnt->root = root_dentry;

	kprintf("fat: mounted FAT%d (clus=%u spc=%u)\n", fs->type, fs->total_clus, fs->sec_per_clus);
	return 0;
}

void fat_register(void)
{
	extern void vfs_register_fs(const char *, int (*)(vfs_mount_t *, void *));
	vfs_register_fs("vfat", fat_mount);
	vfs_register_fs("fat",  fat_mount);
	vfs_register_fs("msdos", fat_mount);
}
