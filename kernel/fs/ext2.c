#include <kernel/vfs.h>
#include <kernel/block.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/sync.h>
#include <asm/mmu.h>

/*
 * ext2 / ext4 read-only VFS driver.
 *
 * ext4 additions handled:
 *   - Extent tree (EXT4_EXTENTS_FL) for inode block mapping
 *   - 64-bit file sizes (i_size_high)
 * Not supported: inline data, meta_bg, flex_bg with >1 meta block.
 */

/* ── on-disk structures ──────────────────────────────────────── */

#define EXT2_MAGIC       0xEF53
#define EXT2_ROOT_INO    2
#define EXT4_EXTENTS_FL  0x00080000

typedef struct __attribute__((packed)) {
	uint32_t s_inodes_count;
	uint32_t s_blocks_count_lo;
	uint32_t s_r_blocks_count_lo;
	uint32_t s_free_blocks_count_lo;
	uint32_t s_free_inodes_count;
	uint32_t s_first_data_block;    /* 1 if block_size=1024, else 0 */
	uint32_t s_log_block_size;      /* block_size = 1024 << s_log_block_size */
	uint32_t s_log_cluster_size;
	uint32_t s_blocks_per_group;
	uint32_t s_clusters_per_group;
	uint32_t s_inodes_per_group;
	uint32_t s_mtime;
	uint32_t s_wtime;
	uint16_t s_mnt_count;
	uint16_t s_max_mnt_count;
	uint16_t s_magic;               /* 0xEF53 */
	uint16_t s_state;
	uint16_t s_errors;
	uint16_t s_minor_rev_level;
	uint32_t s_lastcheck;
	uint32_t s_checkinterval;
	uint32_t s_creator_os;
	uint32_t s_rev_level;
	uint16_t s_def_resuid;
	uint16_t s_def_resgid;
	/* EXT2_DYNAMIC_REV fields */
	uint32_t s_first_ino;
	uint16_t s_inode_size;
	uint16_t s_block_group_nr;
	uint32_t s_feature_compat;
	uint32_t s_feature_incompat;
	uint32_t s_feature_ro_compat;
	uint8_t  s_uuid[16];
	char     s_volume_name[16];
	char     s_last_mounted[64];
	uint32_t s_algorithm_usage_bitmap;
	/* performance hints */
	uint8_t  s_prealloc_blocks;
	uint8_t  s_prealloc_dir_blocks;
	uint16_t s_reserved_gdt_blocks;
	/* journaling */
	uint8_t  s_journal_uuid[16];
	uint32_t s_journal_inum;
	uint32_t s_journal_dev;
	uint32_t s_last_orphan;
	uint32_t s_hash_seed[4];
	uint8_t  s_def_hash_version;
	uint8_t  s_jnl_backup_type;
	uint16_t s_desc_size;           /* size of block group descriptor (ext4: 64) */
	uint32_t s_default_mount_opts;
	uint32_t s_first_meta_bg;
	uint32_t s_mkfs_time;
	uint32_t s_jnl_blocks[17];
	/* 64-bit fields */
	uint32_t s_blocks_count_hi;
	uint32_t s_r_blocks_count_hi;
	uint32_t s_free_blocks_count_hi;
	uint16_t s_min_extra_isize;
	uint16_t s_want_extra_isize;
	uint32_t s_flags;
	uint16_t s_raid_stride;
	uint16_t s_mmp_update_interval;
	uint64_t s_mmp_block;
	uint32_t s_raid_stripe_width;
	uint8_t  s_log_groups_per_flex;
} ext2_super_t;

typedef struct __attribute__((packed)) {
	uint32_t bg_block_bitmap_lo;
	uint32_t bg_inode_bitmap_lo;
	uint32_t bg_inode_table_lo;
	uint16_t bg_free_blocks_count_lo;
	uint16_t bg_free_inodes_count_lo;
	uint16_t bg_used_dirs_count_lo;
	uint16_t bg_flags;
	uint32_t bg_exclude_bitmap_lo;
	uint16_t bg_block_bitmap_csum_lo;
	uint16_t bg_inode_bitmap_csum_lo;
	uint16_t bg_itable_unused_lo;
	uint16_t bg_checksum;
	/* 64-bit ext4 fields (only if s_desc_size >= 64) */
	uint32_t bg_block_bitmap_hi;
	uint32_t bg_inode_bitmap_hi;
	uint32_t bg_inode_table_hi;
	uint16_t bg_free_blocks_count_hi;
	uint16_t bg_free_inodes_count_hi;
	uint16_t bg_used_dirs_count_hi;
	uint16_t bg_itable_unused_hi;
	uint32_t bg_exclude_bitmap_hi;
	uint16_t bg_block_bitmap_csum_hi;
	uint16_t bg_inode_bitmap_csum_hi;
	uint32_t bg_reserved;
} ext2_bgd_t;

typedef struct __attribute__((packed)) {
	uint16_t i_mode;
	uint16_t i_uid_lo;
	uint32_t i_size_lo;
	uint32_t i_atime;
	uint32_t i_ctime;
	uint32_t i_mtime;
	uint32_t i_dtime;
	uint16_t i_gid_lo;
	uint16_t i_links_count;
	uint32_t i_blocks_lo;   /* in 512-byte units */
	uint32_t i_flags;
	uint32_t i_osd1;
	uint32_t i_block[15];   /* 12 direct + 1 ind + 1 dind + 1 tind */
	uint32_t i_generation;
	uint32_t i_file_acl_lo;
	uint32_t i_size_high;
	uint32_t i_obso_faddr;
	uint8_t  i_osd2[12];
	uint16_t i_extra_isize;
	uint16_t i_checksum_hi;
	uint32_t i_ctime_extra;
	uint32_t i_mtime_extra;
	uint32_t i_atime_extra;
	uint32_t i_crtime;
	uint32_t i_crtime_extra;
	uint32_t i_version_hi;
	uint32_t i_projid;
} ext2_inode_t;

typedef struct __attribute__((packed)) {
	uint32_t inode;
	uint16_t rec_len;
	uint8_t  name_len;
	uint8_t  file_type;
	char     name[256];
} ext2_dirent_t;

/* ext4 extent tree */
typedef struct __attribute__((packed)) {
	uint16_t eh_magic;    /* 0xF30A */
	uint16_t eh_entries;
	uint16_t eh_max;
	uint16_t eh_depth;
	uint32_t eh_generation;
} ext4_eh_t;

typedef struct __attribute__((packed)) {
	uint32_t ee_block;    /* first logical block */
	uint16_t ee_len;      /* number of blocks (uninitialized if bit 15 set) */
	uint16_t ee_start_hi;
	uint32_t ee_start_lo;
} ext4_extent_t;

typedef struct __attribute__((packed)) {
	uint32_t ei_block;
	uint32_t ei_leaf_lo;
	uint16_t ei_leaf_hi;
	uint16_t ei_unused;
} ext4_extent_idx_t;

/* ── filesystem metadata ─────────────────────────────────────── */

#define EXT2_BCACHE_N  16   /* LRU block cache entries per fs */

typedef struct {
	uint64_t blkno;   /* ~0ULL = empty */
	uint8_t *buf;     /* NULL until slot first allocated */
	uint32_t lru_ts;  /* monotonically increasing access stamp */
} bcache_entry_t;

typedef struct {
	block_device_t *bdev;
	uint32_t        block_size;
	uint32_t        sectors_per_block;
	uint32_t        blocks_per_group;
	uint32_t        inodes_per_group;
	uint16_t        inode_size;
	uint32_t        first_data_block;
	uint32_t        total_inodes;
	uint64_t        total_blocks;
	uint32_t        num_groups;
	uint16_t        desc_size;
	/* LRU block cache */
	bcache_entry_t  cache[EXT2_BCACHE_N];
	uint32_t        cache_clock;
} ext2_fs_t;

static inline uint8_t *ext2_alloc_page(void)
{
	void *p = buddy_alloc(0);
	return p ? (uint8_t *)PHYS_TO_VIRT((uint64_t)p) : NULL;
}

static inline void ext2_free_page(uint8_t *p)
{
	if (p) buddy_free(VIRT_TO_PHYS((uint64_t)p), 0);
}

/* ── block I/O helpers ───────────────────────────────────────── */

static int read_block(ext2_fs_t *fs, uint64_t blkno, void *buf)
{
	if (blkno == 0) { memset(buf, 0, fs->block_size); return 0; }

	/* search cache */
	for (int i = 0; i < EXT2_BCACHE_N; i++) {
		if (fs->cache[i].blkno == blkno && fs->cache[i].buf) {
			fs->cache[i].lru_ts = ++fs->cache_clock;
			memcpy(buf, fs->cache[i].buf, fs->block_size);
			return 0;
		}
	}

	/* read from device */
	uint64_t lba = (uint64_t)blkno * fs->sectors_per_block;
	if (fs->bdev->read(fs->bdev, lba, buf, fs->sectors_per_block) != 0) return -1;

	/* find victim: prefer empty slot, else oldest */
	int victim = 0;
	uint32_t oldest = (uint32_t)-1;
	for (int i = 0; i < EXT2_BCACHE_N; i++) {
		if (!fs->cache[i].buf || fs->cache[i].blkno == ~0ULL) {
			victim = i;
			oldest = 0;
			break;
		}
		if (fs->cache[i].lru_ts < oldest) {
			oldest = fs->cache[i].lru_ts;
			victim = i;
		}
	}

	if (!fs->cache[victim].buf)
		fs->cache[victim].buf = ext2_alloc_page();
	if (fs->cache[victim].buf) {
		memcpy(fs->cache[victim].buf, buf, fs->block_size);
		fs->cache[victim].blkno  = blkno;
		fs->cache[victim].lru_ts = ++fs->cache_clock;
	}
	return 0;
}

/* ── inode lookup ────────────────────────────────────────────── */

static int read_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *out)
{
	if (ino == 0 || ino > fs->total_inodes) return -1;

	uint32_t group   = (ino - 1) / fs->inodes_per_group;
	uint32_t idx     = (ino - 1) % fs->inodes_per_group;

	/* read block group descriptor */
	uint8_t *tmp = ext2_alloc_page();
	if (!tmp) return -1;

	/* BGD table starts at block first_data_block+1 */
	uint32_t bgd_block = fs->first_data_block + 1;
	/* offset of this group's descriptor within the BGD table */
	uint32_t bgd_off   = group * fs->desc_size;
	uint32_t bgd_blk   = bgd_block + bgd_off / fs->block_size;
	uint32_t bgd_boff  = bgd_off % fs->block_size;

	if (read_block(fs, bgd_blk, tmp) != 0) { ext2_free_page(tmp); return -1; }

	ext2_bgd_t *bgd    = (ext2_bgd_t *)(tmp + bgd_boff);
	uint64_t inode_tbl = bgd->bg_inode_table_lo;
	if (fs->desc_size >= 64)
		inode_tbl |= (uint64_t)bgd->bg_inode_table_hi << 32;   /* ext4 64BIT */

	/* inode table block + offset within it */
	uint32_t inodes_per_blk = fs->block_size / fs->inode_size;
	uint64_t tbl_blk  = inode_tbl + idx / inodes_per_blk;
	uint32_t tbl_off  = (idx % inodes_per_blk) * fs->inode_size;

	if (read_block(fs, tbl_blk, tmp) != 0) { ext2_free_page(tmp); return -1; }

	memcpy(out, tmp + tbl_off,
	       (fs->inode_size < sizeof(ext2_inode_t)) ? fs->inode_size : sizeof(ext2_inode_t));

	ext2_free_page(tmp);
	return 0;
}

/* ── extent tree block mapping ───────────────────────────────── */

static uint64_t ext4_extent_lookup(ext2_fs_t *fs, uint32_t *tree, uint32_t logical_blk)
{
	uint8_t *buf = (uint8_t *)tree;

	for (;;) {
		ext4_eh_t *eh = (ext4_eh_t *)buf;
		if (eh->eh_magic != 0xF30A) return 0;

		/* eh_entries/eh_max are read from disk; clamp the iteration to what the
		 * block buffer actually holds so a forged header can't walk the entry
		 * array off the end of the page (extent and index entries are both 12
		 * bytes, so one bound covers leaf and index nodes). */
		int n = eh->eh_entries;
		if (eh->eh_max && n > eh->eh_max) n = eh->eh_max;
		int maxn = (int)((fs->block_size - sizeof(ext4_eh_t)) / sizeof(ext4_extent_t));
		if (n > maxn) n = maxn;
		if (n < 0) n = 0;

		if (eh->eh_depth == 0) {
			/* leaf: search extents */
			ext4_extent_t *ext = (ext4_extent_t *)(buf + sizeof(ext4_eh_t));
			for (int i = 0; i < n; i++) {
				uint32_t start = ext[i].ee_block;
				uint32_t len   = ext[i].ee_len & 0x7FFF;
				if (logical_blk >= start && logical_blk < start + len) {
					uint64_t phys = ((uint64_t)ext[i].ee_start_hi << 32) | ext[i].ee_start_lo;
					return phys + (logical_blk - start);
				}
			}
			return 0;
		}

		/* index node: find the right branch */
		ext4_extent_idx_t *idx = (ext4_extent_idx_t *)(buf + sizeof(ext4_eh_t));
		int best = -1;
		for (int i = 0; i < n; i++) {
			if (idx[i].ei_block <= logical_blk)
				best = i;
		}
		if (best < 0) return 0;

		uint64_t leaf_blk = ((uint64_t)idx[best].ei_leaf_hi << 32) | idx[best].ei_leaf_lo;
		uint8_t *next_buf = ext2_alloc_page();
		if (!next_buf) return 0;
		if (read_block(fs, leaf_blk, next_buf) != 0) {
			ext2_free_page(next_buf);
			return 0;
		}
		/* copy into buf and iterate (static 4K buffer on the stack is too big) */
		memcpy(buf, next_buf, fs->block_size);
		ext2_free_page(next_buf);
	}
}

/* Map logical block number → physical block number. 64-bit return so an ext4
   64BIT (>16 TB) filesystem's extent-mapped blocks are addressable; the legacy
   indirect-block path stays 32-bit on disk, which is inherent to its format. */
static uint64_t inode_map_block(ext2_fs_t *fs, ext2_inode_t *in, uint32_t logical)
{
	/* ext4 extent tree — use heap buffer so extent traversal can overwrite
	   it with a full block (1024 B) without corrupting the stack */
	if (in->i_flags & EXT4_EXTENTS_FL) {
		uint8_t *tree = ext2_alloc_page();
		if (!tree) return 0;
		memcpy(tree, in->i_block, 15 * sizeof(uint32_t)); /* 60 bytes inline */
		uint64_t r = ext4_extent_lookup(fs, (uint32_t *)tree, logical);
		ext2_free_page(tree);
		return r;
	}

	uint32_t ptrs_per_blk = fs->block_size / 4;

	/* 12 direct blocks */
	if (logical < 12)
		return in->i_block[logical];

	logical -= 12;
	uint8_t *tmp = ext2_alloc_page();
	if (!tmp) return 0;
	uint64_t result = 0;

	/* single indirect */
	if (logical < ptrs_per_blk) {
		if (read_block(fs, in->i_block[12], tmp) == 0)
			result = ((uint32_t *)tmp)[logical];
		goto done;
	}
	logical -= ptrs_per_blk;

	/* double indirect */
	if (logical < ptrs_per_blk * ptrs_per_blk) {
		if (read_block(fs, in->i_block[13], tmp) != 0) goto done;
		uint32_t l1 = ((uint32_t *)tmp)[logical / ptrs_per_blk];
		if (read_block(fs, l1, tmp) != 0) goto done;
		result = ((uint32_t *)tmp)[logical % ptrs_per_blk];
		goto done;
	}

	logical -= ptrs_per_blk * ptrs_per_blk;

	/* triple indirect */
	if (in->i_block[14]) {
		uint8_t *t2 = ext2_alloc_page();
		if (!t2) goto done;
		if (read_block(fs, in->i_block[14], tmp) != 0) { ext2_free_page(t2); goto done; }
		uint32_t l1 = ((uint32_t *)tmp)[logical / (ptrs_per_blk * ptrs_per_blk)];
		if (!l1) { ext2_free_page(t2); goto done; }
		if (read_block(fs, l1, tmp) != 0) { ext2_free_page(t2); goto done; }
		uint32_t l2 = ((uint32_t *)tmp)[(logical / ptrs_per_blk) % ptrs_per_blk];
		if (!l2) { ext2_free_page(t2); goto done; }
		if (read_block(fs, l2, t2) != 0) { ext2_free_page(t2); goto done; }
		result = ((uint32_t *)t2)[logical % ptrs_per_blk];
		ext2_free_page(t2);
	}
done:
	ext2_free_page(tmp);
	return result;
}

/* ── VFS inode type ──────────────────────────────────────────── */

typedef struct {
	ext2_fs_t   *fs;
	ext2_inode_t raw;
	uint32_t     ino;
} ext2_inode_priv_t;

static uint64_t   ext2_ino_counter = 0;
static spinlock_t ext2_ino_lock    = SPINLOCK_INIT;
static vfs_iops_t ext2_iops;

static vfs_inode_t *ext2_vfs_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *raw)
{
	vfs_inode_t *vi = kmalloc(sizeof(*vi));
	if (!vi) return NULL;
	memset(vi, 0, sizeof(*vi));

	ext2_inode_priv_t *priv = kmalloc(sizeof(*priv));
	if (!priv) { kfree(vi); return NULL; }
	priv->fs  = fs;
	priv->ino = ino;
	memcpy(&priv->raw, raw, sizeof(*raw));

	vi->ino      = ino;
	vi->mode     = raw->i_mode;
	vi->uid      = raw->i_uid_lo;
	vi->gid      = raw->i_gid_lo;
	vi->size     = (uint64_t)raw->i_size_lo | ((uint64_t)raw->i_size_high << 32);
	vi->nlink    = raw->i_links_count;
	vi->refcount = 1;
	vi->priv     = priv;
	vi->iops     = &ext2_iops;

	spin_lock(&ext2_ino_lock);
	ext2_ino_counter++;
	spin_unlock(&ext2_ino_lock);
	return vi;
}

/* ── ext2 fops: read ─────────────────────────────────────────── */

static ssize_t ext2_read(vfs_file_t *f, void *buf, size_t len)
{
	ext2_inode_priv_t *priv = f->inode->priv;
	ext2_fs_t         *fs   = priv->fs;
	ext2_inode_t      *in   = &priv->raw;
	uint64_t           size = f->inode->size;

	if (f->pos >= (off_t)size) return 0;
	if ((uint64_t)f->pos + len > size) len = (size_t)(size - (uint64_t)f->pos);

	uint8_t *tmp = ext2_alloc_page();
	if (!tmp) return -1;

	ssize_t done = 0;
	size_t  rem  = len;
	uint64_t pos = (uint64_t)f->pos;

	while (rem > 0) {
		uint32_t log_blk  = (uint32_t)(pos / fs->block_size);
		uint32_t blk_off  = (uint32_t)(pos % fs->block_size);
		uint64_t phys_blk = inode_map_block(fs, in, log_blk);

		if (read_block(fs, phys_blk, tmp) != 0) break;

		size_t copy = fs->block_size - blk_off;
		if (copy > rem) copy = rem;
		memcpy((uint8_t *)buf + done, tmp + blk_off, copy);
		done += (ssize_t)copy;
		rem  -= copy;
		pos  += copy;
	}

	ext2_free_page(tmp);
	f->pos = (off_t)pos;
	return done;
}

static off_t ext2_seek(vfs_file_t *f, off_t off, int whence)
{
	uint64_t size = f->inode->size;
	off_t    new;
	if (whence == 0)      new = off;
	else if (whence == 1) new = f->pos + off;
	else                  new = (off_t)size + off;
	if (new < 0) new = 0;
	f->pos = new;
	return new;
}

/* ── write support ───────────────────────────────────────────── */

/* Block/inode bitmap addresses, combining the ext4 64BIT high halves. */
static uint64_t bgd_block_bitmap(ext2_fs_t *fs, const ext2_bgd_t *b)
{
	uint64_t a = b->bg_block_bitmap_lo;
	if (fs->desc_size >= 64) a |= (uint64_t)b->bg_block_bitmap_hi << 32;
	return a;
}
static uint64_t bgd_inode_bitmap(ext2_fs_t *fs, const ext2_bgd_t *b)
{
	uint64_t a = b->bg_inode_bitmap_lo;
	if (fs->desc_size >= 64) a |= (uint64_t)b->bg_inode_bitmap_hi << 32;
	return a;
}

static int write_block(ext2_fs_t *fs, uint64_t blkno, const void *buf)
{
	uint64_t lba = (uint64_t)blkno * fs->sectors_per_block;
	if (fs->bdev->write(fs->bdev, lba, buf, fs->sectors_per_block) != 0) return -1;
	for (int i = 0; i < EXT2_BCACHE_N; i++) {
		if (fs->cache[i].blkno == blkno && fs->cache[i].buf) {
			memcpy(fs->cache[i].buf, buf, fs->block_size);
			return 0;
		}
	}
	return 0;
}

static int write_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *in)
{
	uint32_t group = (ino - 1) / fs->inodes_per_group;
	uint32_t idx   = (ino - 1) % fs->inodes_per_group;
	uint8_t *tmp   = ext2_alloc_page();
	if (!tmp) return -1;

	uint32_t bgd_blk  = (fs->first_data_block + 1) + (group * fs->desc_size) / fs->block_size;
	uint32_t bgd_boff = (group * fs->desc_size) % fs->block_size;
	if (read_block(fs, bgd_blk, tmp) != 0) { ext2_free_page(tmp); return -1; }
	ext2_bgd_t *wbgd = (ext2_bgd_t *)(tmp + bgd_boff);
	uint64_t inode_tbl = wbgd->bg_inode_table_lo;
	if (fs->desc_size >= 64)
		inode_tbl |= (uint64_t)wbgd->bg_inode_table_hi << 32;   /* ext4 64BIT */
	ext2_free_page(tmp);

	tmp = ext2_alloc_page();
	if (!tmp) return -1;
	uint32_t inodes_per_blk = fs->block_size / fs->inode_size;
	uint64_t tbl_blk = inode_tbl + idx / inodes_per_blk;
	uint32_t tbl_off = (idx % inodes_per_blk) * fs->inode_size;
	if (read_block(fs, tbl_blk, tmp) != 0) { ext2_free_page(tmp); return -1; }
	uint32_t csz = fs->inode_size < sizeof(ext2_inode_t) ? fs->inode_size : sizeof(ext2_inode_t);
	memcpy(tmp + tbl_off, in, csz);
	int r = write_block(fs, tbl_blk, tmp);
	ext2_free_page(tmp);
	return r;
}

static int read_bgd(ext2_fs_t *fs, uint32_t group, ext2_bgd_t *out)
{
	uint8_t *tmp = ext2_alloc_page();
	if (!tmp) return -1;
	uint32_t bgd_blk  = (fs->first_data_block + 1) + (group * fs->desc_size) / fs->block_size;
	uint32_t bgd_boff = (group * fs->desc_size) % fs->block_size;
	if (read_block(fs, bgd_blk, tmp) != 0) { ext2_free_page(tmp); return -1; }
	*out = *(ext2_bgd_t *)(tmp + bgd_boff);
	ext2_free_page(tmp);
	return 0;
}

static int write_bgd(ext2_fs_t *fs, uint32_t group, ext2_bgd_t *bgd)
{
	uint8_t *tmp = ext2_alloc_page();
	if (!tmp) return -1;
	uint32_t bgd_blk  = (fs->first_data_block + 1) + (group * fs->desc_size) / fs->block_size;
	uint32_t bgd_boff = (group * fs->desc_size) % fs->block_size;
	if (read_block(fs, bgd_blk, tmp) != 0) { ext2_free_page(tmp); return -1; }
	*(ext2_bgd_t *)(tmp + bgd_boff) = *bgd;
	int r = write_block(fs, bgd_blk, tmp);
	ext2_free_page(tmp);
	return r;
}

static uint32_t alloc_block(ext2_fs_t *fs, uint32_t pref_group)
{
	uint8_t *bmp = ext2_alloc_page();
	if (!bmp) return 0;
	for (uint32_t gi = 0; gi < fs->num_groups; gi++) {
		uint32_t g = (pref_group + gi) % fs->num_groups;
		ext2_bgd_t bgd;
		if (read_bgd(fs, g, &bgd) != 0 || bgd.bg_free_blocks_count_lo == 0) continue;
		uint64_t bbm = bgd_block_bitmap(fs, &bgd);
		if (read_block(fs, bbm, bmp) != 0) continue;
		uint32_t lim = fs->blocks_per_group;
		for (uint32_t i = 0; i < lim; i++) {
			if (!(bmp[i / 8] & (1u << (i & 7)))) {
				bmp[i / 8] |= (1u << (i & 7));
				write_block(fs, bbm, bmp);
				bgd.bg_free_blocks_count_lo--;
				write_bgd(fs, g, &bgd);
				ext2_free_page(bmp);
				return g * fs->blocks_per_group + fs->first_data_block + i;
			}
		}
	}
	ext2_free_page(bmp);
	return 0;
}

static void free_block(ext2_fs_t *fs, uint64_t blkno)
{
	if (!blkno || blkno < fs->first_data_block) return;
	uint64_t rel = blkno - fs->first_data_block;
	uint32_t g   = (uint32_t)(rel / fs->blocks_per_group);
	uint32_t bit = (uint32_t)(rel % fs->blocks_per_group);
	if (g >= fs->num_groups) return;
	ext2_bgd_t bgd;
	if (read_bgd(fs, g, &bgd) != 0) return;
	uint8_t *bmp = ext2_alloc_page();
	if (!bmp) return;
	uint64_t bbm = bgd_block_bitmap(fs, &bgd);
	if (read_block(fs, bbm, bmp) == 0) {
		bmp[bit / 8] &= ~(1u << (bit & 7));
		write_block(fs, bbm, bmp);
		bgd.bg_free_blocks_count_lo++;
		write_bgd(fs, g, &bgd);
	}
	ext2_free_page(bmp);
}

static uint32_t alloc_inode(ext2_fs_t *fs, uint32_t pref_group)
{
	uint8_t *bmp = ext2_alloc_page();
	if (!bmp) return 0;
	for (uint32_t gi = 0; gi < fs->num_groups; gi++) {
		uint32_t g = (pref_group + gi) % fs->num_groups;
		ext2_bgd_t bgd;
		if (read_bgd(fs, g, &bgd) != 0 || bgd.bg_free_inodes_count_lo == 0) continue;
		uint64_t ibm = bgd_inode_bitmap(fs, &bgd);
		if (read_block(fs, ibm, bmp) != 0) continue;
		for (uint32_t i = 0; i < fs->inodes_per_group; i++) {
			if (!(bmp[i / 8] & (1u << (i & 7)))) {
				bmp[i / 8] |= (1u << (i & 7));
				write_block(fs, ibm, bmp);
				bgd.bg_free_inodes_count_lo--;
				write_bgd(fs, g, &bgd);
				ext2_free_page(bmp);
				return g * fs->inodes_per_group + i + 1;
			}
		}
	}
	ext2_free_page(bmp);
	return 0;
}

static void free_inode(ext2_fs_t *fs, uint32_t ino)
{
	if (ino < 2 || ino > fs->total_inodes) return;
	uint32_t g   = (ino - 1) / fs->inodes_per_group;
	uint32_t bit = (ino - 1) % fs->inodes_per_group;
	ext2_bgd_t bgd;
	if (read_bgd(fs, g, &bgd) != 0) return;
	uint8_t *bmp = ext2_alloc_page();
	if (!bmp) return;
	uint64_t ibm = bgd_inode_bitmap(fs, &bgd);
	if (read_block(fs, ibm, bmp) == 0) {
		bmp[bit / 8] &= ~(1u << (bit & 7));
		write_block(fs, ibm, bmp);
		bgd.bg_free_inodes_count_lo++;
		write_bgd(fs, g, &bgd);
	}
	ext2_free_page(bmp);
}

/* Allocate and link a physical block at logical position lbn in inode.
 * Modifies *in in memory; caller must write_inode afterwards. */
static uint32_t inode_alloc_block(ext2_fs_t *fs, ext2_inode_t *in,
                                   uint32_t ino, uint32_t lbn)
{
	uint32_t pg = (ino - 1) / fs->inodes_per_group;
	uint32_t ppp = fs->block_size / 4; /* pointers per block */

	/* Direct */
	if (lbn < 12) {
		if (in->i_block[lbn]) return in->i_block[lbn];
		uint32_t b = alloc_block(fs, pg);
		if (!b) return 0;
		in->i_block[lbn] = b;
		in->i_blocks_lo += (uint32_t)fs->sectors_per_block;
		return b;
	}
	lbn -= 12;

	/* Single indirect */
	if (lbn < ppp) {
		uint8_t *tmp = ext2_alloc_page();
		if (!tmp) return 0;
		if (!in->i_block[12]) {
			uint32_t ib = alloc_block(fs, pg);
			if (!ib) { ext2_free_page(tmp); return 0; }
			memset(tmp, 0, fs->block_size);
			in->i_block[12] = ib;
			in->i_blocks_lo += (uint32_t)fs->sectors_per_block;
		} else if (read_block(fs, in->i_block[12], tmp) != 0) {
			ext2_free_page(tmp); return 0;
		}
		uint32_t *ptrs = (uint32_t *)tmp;
		if (!ptrs[lbn]) {
			uint32_t b = alloc_block(fs, pg);
			if (!b) { ext2_free_page(tmp); return 0; }
			ptrs[lbn] = b;
			in->i_blocks_lo += (uint32_t)fs->sectors_per_block;
			write_block(fs, in->i_block[12], tmp);
		}
		uint32_t r = ptrs[lbn];
		ext2_free_page(tmp);
		return r;
	}
	lbn -= ppp;

	/* Double indirect */
	if (lbn < ppp * ppp) {
		uint8_t *t1 = ext2_alloc_page(), *t2 = ext2_alloc_page();
		if (!t1 || !t2) { ext2_free_page(t1); ext2_free_page(t2); return 0; }
		if (!in->i_block[13]) {
			uint32_t ib = alloc_block(fs, pg);
			if (!ib) { ext2_free_page(t1); ext2_free_page(t2); return 0; }
			memset(t1, 0, fs->block_size);
			in->i_block[13] = ib;
			in->i_blocks_lo += (uint32_t)fs->sectors_per_block;
			write_block(fs, ib, t1);
		} else if (read_block(fs, in->i_block[13], t1) != 0) {
			ext2_free_page(t1); ext2_free_page(t2); return 0;
		}
		uint32_t *l1 = (uint32_t *)t1;
		uint32_t l1i = lbn / ppp, l2i = lbn % ppp;
		if (!l1[l1i]) {
			uint32_t ib = alloc_block(fs, pg);
			if (!ib) { ext2_free_page(t1); ext2_free_page(t2); return 0; }
			memset(t2, 0, fs->block_size);
			l1[l1i] = ib;
			in->i_blocks_lo += (uint32_t)fs->sectors_per_block;
			write_block(fs, in->i_block[13], t1);
			write_block(fs, ib, t2);
		} else if (read_block(fs, l1[l1i], t2) != 0) {
			ext2_free_page(t1); ext2_free_page(t2); return 0;
		}
		uint32_t *l2 = (uint32_t *)t2;
		if (!l2[l2i]) {
			uint32_t b = alloc_block(fs, pg);
			if (!b) { ext2_free_page(t1); ext2_free_page(t2); return 0; }
			l2[l2i] = b;
			in->i_blocks_lo += (uint32_t)fs->sectors_per_block;
			write_block(fs, l1[l1i], t2);
		}
		uint32_t r = l2[l2i];
		ext2_free_page(t1); ext2_free_page(t2);
		return r;
	}
	lbn -= ppp * ppp;

	/* Triple indirect */
	{
		uint8_t *t1 = ext2_alloc_page(), *t2 = ext2_alloc_page(), *t3 = ext2_alloc_page();
		if (!t1 || !t2 || !t3) {
			ext2_free_page(t1); ext2_free_page(t2); ext2_free_page(t3); return 0;
		}
		if (!in->i_block[14]) {
			uint32_t ib = alloc_block(fs, pg);
			if (!ib) { ext2_free_page(t1); ext2_free_page(t2); ext2_free_page(t3); return 0; }
			memset(t1, 0, fs->block_size);
			in->i_block[14] = ib;
			in->i_blocks_lo += (uint32_t)fs->sectors_per_block;
			write_block(fs, ib, t1);
		} else if (read_block(fs, in->i_block[14], t1) != 0) {
			ext2_free_page(t1); ext2_free_page(t2); ext2_free_page(t3); return 0;
		}
		uint32_t *l1 = (uint32_t *)t1;
		uint32_t l1i = lbn / (ppp * ppp);
		uint32_t l2i = (lbn / ppp) % ppp;
		uint32_t l3i = lbn % ppp;
		if (!l1[l1i]) {
			uint32_t ib = alloc_block(fs, pg);
			if (!ib) { ext2_free_page(t1); ext2_free_page(t2); ext2_free_page(t3); return 0; }
			memset(t2, 0, fs->block_size);
			l1[l1i] = ib;
			in->i_blocks_lo += (uint32_t)fs->sectors_per_block;
			write_block(fs, in->i_block[14], t1);
			write_block(fs, ib, t2);
		} else if (read_block(fs, l1[l1i], t2) != 0) {
			ext2_free_page(t1); ext2_free_page(t2); ext2_free_page(t3); return 0;
		}
		uint32_t *l2 = (uint32_t *)t2;
		if (!l2[l2i]) {
			uint32_t ib = alloc_block(fs, pg);
			if (!ib) { ext2_free_page(t1); ext2_free_page(t2); ext2_free_page(t3); return 0; }
			memset(t3, 0, fs->block_size);
			l2[l2i] = ib;
			in->i_blocks_lo += (uint32_t)fs->sectors_per_block;
			write_block(fs, l1[l1i], t2);
			write_block(fs, ib, t3);
		} else if (read_block(fs, l2[l2i], t3) != 0) {
			ext2_free_page(t1); ext2_free_page(t2); ext2_free_page(t3); return 0;
		}
		uint32_t *l3 = (uint32_t *)t3;
		if (!l3[l3i]) {
			uint32_t b = alloc_block(fs, pg);
			if (!b) { ext2_free_page(t1); ext2_free_page(t2); ext2_free_page(t3); return 0; }
			l3[l3i] = b;
			in->i_blocks_lo += (uint32_t)fs->sectors_per_block;
			write_block(fs, l2[l2i], t3);
		}
		uint32_t r = l3[l3i];
		ext2_free_page(t1); ext2_free_page(t2); ext2_free_page(t3);
		return r;
	}
}

static ssize_t ext2_write(vfs_file_t *f, const void *buf, size_t len)
{
	if (!len) return 0;
	ext2_inode_priv_t *priv = f->inode->priv;
	ext2_fs_t  *fs  = priv->fs;
	ext2_inode_t *in = &priv->raw;
	if (in->i_flags & EXT4_EXTENTS_FL) return -1;

	const uint8_t *src = buf;
	uint64_t pos = (uint64_t)f->pos;
	size_t done = 0;

	uint8_t *blkbuf = ext2_alloc_page();
	if (!blkbuf) return -12;

	while (done < len) {
		uint32_t lbn   = (uint32_t)(pos / fs->block_size);
		uint32_t boff  = (uint32_t)(pos % fs->block_size);
		uint32_t chunk = fs->block_size - boff;
		if (chunk > (uint32_t)(len - done)) chunk = (uint32_t)(len - done);

		uint32_t phys = inode_alloc_block(fs, in, priv->ino, lbn);
		if (!phys) break;

		if (boff || chunk < fs->block_size) {
			if (read_block(fs, phys, blkbuf) != 0) break;
		} else {
			memset(blkbuf, 0, fs->block_size);
		}
		memcpy(blkbuf + boff, src + done, chunk);
		if (write_block(fs, phys, blkbuf) != 0) break;
		pos  += chunk;
		done += chunk;
	}
	ext2_free_page(blkbuf);

	if (pos > (uint64_t)f->inode->size) {
		in->i_size_lo   = (uint32_t)pos;
		in->i_size_high = (uint32_t)(pos >> 32);
		f->inode->size  = (off_t)pos;
	}
	f->pos = (off_t)pos;
	write_inode(fs, priv->ino, in);
	return (ssize_t)done;
}

/* Add a dirent to a directory. Modifies dir_in in memory; caller writes inode. */
static int dir_add_entry(ext2_fs_t *fs, uint32_t dir_ino, ext2_inode_t *dir_in,
                         uint32_t ino, const char *name, uint8_t ftype)
{
	uint8_t nlen = 0; while (name[nlen]) nlen++;
	uint16_t need = (uint16_t)(((uint32_t)8 + nlen + 3u) & ~3u);
	uint8_t *blk = ext2_alloc_page();
	if (!blk) return -12;

	uint64_t dsz = (uint64_t)dir_in->i_size_lo | ((uint64_t)dir_in->i_size_high << 32);
	uint32_t nblk = (uint32_t)((dsz + fs->block_size - 1) / fs->block_size);

	for (uint32_t lbn = 0; lbn < nblk; lbn++) {
		uint64_t phys = inode_map_block(fs, dir_in, lbn);
		if (!phys || read_block(fs, phys, blk) != 0) continue;
		uint32_t off = 0;
		while (off + 8 <= fs->block_size) {
			ext2_dirent_t *de = (ext2_dirent_t *)(blk + off);
			if (!de->rec_len || off + de->rec_len > fs->block_size) break;
			uint16_t used = de->inode ? (uint16_t)(((uint32_t)8 + de->name_len + 3u) & ~3u) : 0;
			uint16_t avail = de->rec_len - used;
			if (avail >= need) {
				if (de->inode) { de->rec_len = used; off += used; de = (ext2_dirent_t *)(blk + off); de->rec_len = avail; }
				de->inode     = ino;
				de->name_len  = nlen;
				de->file_type = ftype;
				memcpy(de->name, name, nlen);
				write_block(fs, phys, blk);
				ext2_free_page(blk);
				return 0;
			}
			off += de->rec_len;
		}
	}

	/* Allocate new block for directory */
	uint32_t phys = inode_alloc_block(fs, dir_in, dir_ino, nblk);
	if (!phys) { ext2_free_page(blk); return -28; }

	memset(blk, 0, fs->block_size);
	ext2_dirent_t *de = (ext2_dirent_t *)blk;
	de->inode     = ino;
	de->rec_len   = (uint16_t)fs->block_size;
	de->name_len  = nlen;
	de->file_type = ftype;
	memcpy(de->name, name, nlen);
	write_block(fs, phys, blk);

	dir_in->i_size_lo = (uint32_t)(dsz + fs->block_size);
	ext2_free_page(blk);
	return 0;
}

/* Remove a dirent from a directory. Returns inode number of removed entry or 0. */
static uint32_t dir_remove_entry(ext2_fs_t *fs, ext2_inode_t *dir_in, const char *name)
{
	uint8_t nlen = 0; while (name[nlen]) nlen++;
	uint8_t *blk = ext2_alloc_page();
	if (!blk) return 0;

	uint64_t dsz = (uint64_t)dir_in->i_size_lo | ((uint64_t)dir_in->i_size_high << 32);
	uint32_t nblk = (uint32_t)((dsz + fs->block_size - 1) / fs->block_size);

	for (uint32_t lbn = 0; lbn < nblk; lbn++) {
		uint64_t phys = inode_map_block(fs, dir_in, lbn);
		if (!phys || read_block(fs, phys, blk) != 0) continue;
		uint32_t off = 0;
		ext2_dirent_t *prev = NULL;
		while (off + 8 <= fs->block_size) {
			ext2_dirent_t *de = (ext2_dirent_t *)(blk + off);
			if (!de->rec_len || off + de->rec_len > fs->block_size) break;
			if (de->inode && de->name_len == nlen && memcmp(de->name, name, nlen) == 0) {
				uint32_t found_ino = de->inode;
				if (prev) {
					prev->rec_len = (uint16_t)(prev->rec_len + de->rec_len);
				} else {
					de->inode = 0; /* mark as deleted */
				}
				write_block(fs, phys, blk);
				ext2_free_page(blk);
				return found_ino;
			}
			prev = de;
			off += de->rec_len;
		}
	}
	ext2_free_page(blk);
	return 0;
}

/* Free all data blocks of an inode (non-extent only) */
static void inode_free_blocks(ext2_fs_t *fs, ext2_inode_t *in)
{
	if (in->i_flags & EXT4_EXTENTS_FL) return;
	uint32_t ppp = fs->block_size / 4;
	uint8_t *tmp = ext2_alloc_page();
	if (!tmp) return;

	/* Direct */
	for (int i = 0; i < 12; i++) if (in->i_block[i]) free_block(fs, in->i_block[i]);

	/* Single indirect */
	if (in->i_block[12]) {
		if (read_block(fs, in->i_block[12], tmp) == 0) {
			uint32_t *p = (uint32_t *)tmp;
			for (uint32_t i = 0; i < ppp; i++) if (p[i]) free_block(fs, p[i]);
		}
		free_block(fs, in->i_block[12]);
	}

	/* Double indirect */
	if (in->i_block[13]) {
		uint8_t *t2 = ext2_alloc_page();
		if (t2 && read_block(fs, in->i_block[13], tmp) == 0) {
			uint32_t *l1 = (uint32_t *)tmp;
			for (uint32_t i = 0; i < ppp; i++) {
				if (!l1[i]) continue;
				if (read_block(fs, l1[i], t2) == 0) {
					uint32_t *l2 = (uint32_t *)t2;
					for (uint32_t j = 0; j < ppp; j++) if (l2[j]) free_block(fs, l2[j]);
				}
				free_block(fs, l1[i]);
			}
		}
		ext2_free_page(t2);
		free_block(fs, in->i_block[13]);
	}
	ext2_free_page(tmp);
}

static int ext2_create(vfs_inode_t *dir, const char *name, mode_t mode)
{
	ext2_inode_priv_t *dpriv = dir->priv;
	ext2_fs_t *fs = dpriv->fs;
	uint32_t pref = (dpriv->ino - 1) / fs->inodes_per_group;

	uint32_t new_ino = alloc_inode(fs, pref);
	if (!new_ino) return -28;

	ext2_inode_t in;
	memset(&in, 0, sizeof(in));
	in.i_mode       = (uint16_t)(S_IFREG | (mode & 0xFFF));
	in.i_links_count = 1;

	if (write_inode(fs, new_ino, &in) != 0) { free_inode(fs, new_ino); return -5; }

	if (dir_add_entry(fs, dpriv->ino, &dpriv->raw, new_ino, name, 1 /*EXT2_FT_REG_FILE*/) != 0) {
		free_inode(fs, new_ino); return -5;
	}
	write_inode(fs, dpriv->ino, &dpriv->raw);
	dir->size = dpriv->raw.i_size_lo;
	return 0;
}

static int ext2_mkdir(vfs_inode_t *dir, const char *name, mode_t mode)
{
	ext2_inode_priv_t *dpriv = dir->priv;
	ext2_fs_t *fs = dpriv->fs;
	uint32_t pref = (dpriv->ino - 1) / fs->inodes_per_group;

	uint32_t new_ino = alloc_inode(fs, pref);
	if (!new_ino) return -28;

	ext2_inode_t in;
	memset(&in, 0, sizeof(in));
	in.i_mode        = (uint16_t)(S_IFDIR | (mode & 0xFFF ? mode & 0xFFF : 0755));
	in.i_links_count = 2; /* . and parent */

	/* Allocate and write first block with . and .. */
	uint32_t phys = inode_alloc_block(fs, &in, new_ino, 0);
	if (!phys) { free_inode(fs, new_ino); return -28; }

	uint8_t *blk = ext2_alloc_page();
	if (!blk) { free_block(fs, phys); free_inode(fs, new_ino); return -12; }
	memset(blk, 0, fs->block_size);

	ext2_dirent_t *dot = (ext2_dirent_t *)blk;
	dot->inode     = new_ino;
	dot->rec_len   = 12;
	dot->name_len  = 1;
	dot->file_type = 2; /* EXT2_FT_DIR */
	dot->name[0]   = '.';

	ext2_dirent_t *dotdot = (ext2_dirent_t *)(blk + 12);
	dotdot->inode     = dpriv->ino;
	dotdot->rec_len   = (uint16_t)(fs->block_size - 12);
	dotdot->name_len  = 2;
	dotdot->file_type = 2;
	dotdot->name[0]   = '.';
	dotdot->name[1]   = '.';

	write_block(fs, phys, blk);
	ext2_free_page(blk);

	in.i_size_lo = fs->block_size;
	if (write_inode(fs, new_ino, &in) != 0) { inode_free_blocks(fs, &in); free_inode(fs, new_ino); return -5; }

	if (dir_add_entry(fs, dpriv->ino, &dpriv->raw, new_ino, name, 2 /*EXT2_FT_DIR*/) != 0) {
		inode_free_blocks(fs, &in); free_inode(fs, new_ino); return -5;
	}
	dpriv->raw.i_links_count++;
	write_inode(fs, dpriv->ino, &dpriv->raw);
	dir->size  = dpriv->raw.i_size_lo;
	dir->nlink = dpriv->raw.i_links_count;
	return 0;
}

static int ext2_unlink(vfs_inode_t *dir, const char *name)
{
	ext2_inode_priv_t *dpriv = dir->priv;
	ext2_fs_t *fs = dpriv->fs;

	uint32_t target_ino = dir_remove_entry(fs, &dpriv->raw, name);
	if (!target_ino) return -2; /* ENOENT */
	write_inode(fs, dpriv->ino, &dpriv->raw);
	dir->size = dpriv->raw.i_size_lo;

	ext2_inode_t tin;
	if (read_inode(fs, target_ino, &tin) != 0) return 0;
	if (tin.i_links_count > 0) tin.i_links_count--;
	if (tin.i_links_count == 0) {
		tin.i_dtime = 1;
		inode_free_blocks(fs, &tin);
		write_inode(fs, target_ino, &tin);
		free_inode(fs, target_ino);
	} else {
		write_inode(fs, target_ino, &tin);
	}
	return 0;
}

static vfs_fops_t ext2_fops = {
	.read  = ext2_read,
	.write = ext2_write,
	.seek  = ext2_seek,
};

/* ── ext2 iops ───────────────────────────────────────────────── */

static vfs_inode_t *ext2_lookup(vfs_inode_t *dir, const char *name)
{
	ext2_inode_priv_t *priv = dir->priv;
	ext2_fs_t         *fs   = priv->fs;
	ext2_inode_t      *in   = &priv->raw;
	uint64_t           size = dir->size;

	uint8_t *blk = ext2_alloc_page();
	if (!blk) return NULL;

	uint32_t log_blk = 0;
	uint64_t off     = 0;

	while (off < size) {
		uint64_t phys = inode_map_block(fs, in, log_blk++);
		if (read_block(fs, phys, blk) != 0) break;

		uint32_t blk_off = 0;
		while (blk_off < fs->block_size) {
			ext2_dirent_t *de = (ext2_dirent_t *)(blk + blk_off);
			/* rec_len/name_len come from disk; keep the header and the name
			 * inside the block so a corrupt directory can't read off the page. */
			if (de->rec_len < 8 || blk_off + de->rec_len > fs->block_size) break;
			if (de->inode != 0 &&
			    blk_off + 8u + de->name_len <= fs->block_size &&
			    de->name_len == (uint8_t)strlen(name) &&
			    memcmp(de->name, name, de->name_len) == 0) {
				ext2_inode_t raw;
				if (read_inode(fs, de->inode, &raw) != 0) {
					ext2_free_page(blk);
					return NULL;
				}
				vfs_inode_t *vi = ext2_vfs_inode(fs, de->inode, &raw);
				if (vi && S_ISREG(vi->mode)) vi->fops = &ext2_fops;
				ext2_free_page(blk);
				return vi;
			}
			blk_off += de->rec_len;
		}
		off += fs->block_size;
	}

	ext2_free_page(blk);
	return NULL;
}

/* idx is a byte offset into the directory data (0 = start).
   out->next_off is set to the byte offset of the next entry. */
static int ext2_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	ext2_inode_priv_t *priv = dir->priv;
	ext2_fs_t         *fs   = priv->fs;
	ext2_inode_t      *in   = &priv->raw;
	uint64_t           size = dir->size;

	uint8_t *blk = ext2_alloc_page();
	if (!blk) return -1;

	uint32_t byte_off = idx;
	int loaded_blk = -1;

	while ((uint64_t)byte_off < size) {
		uint32_t log_blk = byte_off / fs->block_size;
		uint32_t blk_off = byte_off % fs->block_size;

		if ((int)log_blk != loaded_blk) {
			uint64_t phys = inode_map_block(fs, in, log_blk);
			if (read_block(fs, phys, blk) != 0) break;
			loaded_blk = (int)log_blk;
		}

		ext2_dirent_t *de = (ext2_dirent_t *)(blk + blk_off);
		/* rec_len/name_len come from disk; keep the header and the name inside
		 * the block so a corrupt directory can't read off the page. */
		if (de->rec_len < 8 || blk_off + de->rec_len > fs->block_size) break;

		byte_off += de->rec_len;

		if (de->inode == 0) continue; /* deleted entry, skip */

		out->ino      = de->inode;
		out->next_off = byte_off;
		uint32_t nl = de->name_len;
		if (blk_off + 8u + nl > fs->block_size) nl = fs->block_size - blk_off - 8u;
		memcpy(out->name, de->name, nl);
		out->name[nl] = '\0';
		switch (de->file_type) {
		case 1: out->mode = S_IFREG; break;
		case 2: out->mode = S_IFDIR; break;
		case 7: out->mode = S_IFLNK; break;
		default: out->mode = S_IFREG; break;
		}
		ext2_free_page(blk);
		return 0;
	}

	ext2_free_page(blk);
	return -1;
}

static int ext2_stat(vfs_inode_t *vi, vfs_stat_t *out)
{
	out->ino   = vi->ino;
	out->mode  = vi->mode;
	out->nlink = vi->nlink;
	out->uid   = vi->uid;
	out->gid   = vi->gid;
	out->size  = vi->size;
	return 0;
}

static int ext2_readlink(vfs_inode_t *vi, char *buf, size_t bufsize)
{
	ext2_inode_priv_t *priv = vi->priv;
	ext2_inode_t      *in   = &priv->raw;
	uint64_t           len  = vi->size;

	if (len == 0 || len >= bufsize) return -1;

	if (len < 60) {
		memcpy(buf, in->i_block, (size_t)len);
		buf[len] = '\0';
		return 0;
	}

	ext2_fs_t *fs = priv->fs;
	uint8_t *blk = ext2_alloc_page();
	if (!blk) return -1;
	uint64_t phys = inode_map_block(fs, in, 0);
	if (read_block(fs, phys, blk) != 0) {
		ext2_free_page(blk);
		return -1;
	}
	size_t copy = len < bufsize - 1 ? (size_t)len : bufsize - 1;
	memcpy(buf, blk, copy);
	buf[copy] = '\0';
	ext2_free_page(blk);
	return 0;
}

static vfs_iops_t ext2_iops = {
	.lookup   = ext2_lookup,
	.readdir  = ext2_readdir,
	.stat     = ext2_stat,
	.readlink = ext2_readlink,
	.create   = ext2_create,
	.mkdir    = ext2_mkdir,
	.unlink   = ext2_unlink,
};

/* ── mount ───────────────────────────────────────────────────── */

static ext2_fs_t g_ext2_fs;

int ext2_mount(vfs_mount_t *mnt, void *data)
{
	block_device_t *bdev = data;
	if (!bdev) return -1;

	/* superblock at byte 1024, i.e. LBA 2 for 512-byte sectors */
	uint8_t *sb_buf = ext2_alloc_page();
	if (!sb_buf) return -1;

	/* read 2 sectors to cover byte 1024..2047 */
	if (bdev->read(bdev, 2, sb_buf, 2) != 0) {
		ext2_free_page(sb_buf);
		return -1;
	}

	ext2_super_t *sb = (ext2_super_t *)sb_buf;
	if (sb->s_magic != EXT2_MAGIC) {
		ext2_free_page(sb_buf);
		return -1;
	}

	uint32_t bs = 1024u << sb->s_log_block_size;
	if (bs > 4096) {
		kprintf("ext2: block size %u not supported\n", bs);
		ext2_free_page(sb_buf);
		return -1;
	}

	ext2_fs_t *fs = &g_ext2_fs;
	fs->bdev              = bdev;
	fs->block_size        = bs;
	fs->sectors_per_block = bs / 512;
	fs->blocks_per_group  = sb->s_blocks_per_group;
	fs->inodes_per_group  = sb->s_inodes_per_group;
	fs->inode_size        = (sb->s_rev_level > 0) ? sb->s_inode_size : 128;
	fs->first_data_block  = sb->s_first_data_block;
	fs->total_inodes      = sb->s_inodes_count;
	fs->total_blocks      = sb->s_blocks_count_lo;
	if (sb->s_feature_incompat & 0x80)   /* INCOMPAT_64BIT */
		fs->total_blocks |= (uint64_t)sb->s_blocks_count_hi << 32;
	fs->num_groups        = (uint32_t)((fs->total_blocks + sb->s_blocks_per_group - 1) / sb->s_blocks_per_group);
	fs->desc_size         = (sb->s_desc_size >= 32) ? sb->s_desc_size : 32;
	fs->cache_clock = 0;
	for (int i = 0; i < EXT2_BCACHE_N; i++) {
		fs->cache[i].blkno  = ~0ULL;
		fs->cache[i].buf    = NULL;
		fs->cache[i].lru_ts = 0;
	}

	kprintf("ext2: bs=%u ino_sz=%u groups=%u desc=%u %s%s\n",
	        bs, fs->inode_size, fs->num_groups, fs->desc_size,
	        (sb->s_feature_incompat & 0x80) ? "64bit" : "32bit",
	        (sb->s_feature_compat & 0x4) ? " journal" : "");

	/* A journal needing recovery (INCOMPAT_RECOVER) means committed
	 * transactions haven't been checkpointed to their final blocks. We don't
	 * replay jbd2 yet, so reads see the pre-crash (consistent but possibly
	 * stale) state; warn rather than silently treat it as clean. */
	if (sb->s_feature_incompat & 0x4)
		kprintf("ext2: WARNING journal needs recovery (jbd2 replay not implemented)\n");

	ext2_free_page(sb_buf);

	/* read root inode */
	ext2_inode_t root_raw;
	if (read_inode(fs, EXT2_ROOT_INO, &root_raw) != 0) {
		kprintf("ext2: cannot read root inode\n");
		return -1;
	}

	vfs_inode_t *root_vi = ext2_vfs_inode(fs, EXT2_ROOT_INO, &root_raw);
	if (!root_vi) return -1;

	vfs_dentry_t *root_dentry = vfs_dentry_create(NULL, "/", root_vi);
	if (!root_dentry) { kfree(root_vi->priv); kfree(root_vi); return -1; }

	mnt->root = root_dentry;
	kprintf("ext2: mounted (ino root=%u)\n", EXT2_ROOT_INO);
	return 0;
}

void ext2_register(void)
{
	extern void vfs_register_fs(const char *, int (*)(vfs_mount_t *, void *));
	vfs_register_fs("ext2", ext2_mount);
	vfs_register_fs("ext4", ext2_mount);   /* ext4 is ext2-compatible */
}
