#include <kernel/vfs.h>
#include <kernel/block.h>
#include <kernel/mm/buddy.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>

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

typedef struct {
	block_device_t *bdev;
	uint32_t        block_size;    /* bytes per block */
	uint32_t        sectors_per_block;
	uint32_t        blocks_per_group;
	uint32_t        inodes_per_group;
	uint16_t        inode_size;
	uint32_t        first_data_block;
	uint32_t        total_inodes;
	uint16_t        desc_size;
} ext2_fs_t;

/* ── block I/O helpers ───────────────────────────────────────── */

static int read_block(ext2_fs_t *fs, uint32_t blkno, void *buf)
{
	if (blkno == 0) { memset(buf, 0, fs->block_size); return 0; }
	uint64_t lba = (uint64_t)blkno * fs->sectors_per_block;
	return fs->bdev->read(fs->bdev, lba, buf, fs->sectors_per_block);
}

/* ── inode lookup ────────────────────────────────────────────── */

static int read_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *out)
{
	if (ino == 0 || ino > fs->total_inodes) return -1;

	uint32_t group   = (ino - 1) / fs->inodes_per_group;
	uint32_t idx     = (ino - 1) % fs->inodes_per_group;

	/* read block group descriptor */
	uint8_t *tmp = buddy_alloc(0);
	if (!tmp) return -1;

	/* BGD table starts at block first_data_block+1 */
	uint32_t bgd_block = fs->first_data_block + 1;
	/* offset of this group's descriptor within the BGD table */
	uint32_t bgd_off   = group * fs->desc_size;
	uint32_t bgd_blk   = bgd_block + bgd_off / fs->block_size;
	uint32_t bgd_boff  = bgd_off % fs->block_size;

	if (read_block(fs, bgd_blk, tmp) != 0) { buddy_free((uint64_t)tmp, 0); return -1; }

	ext2_bgd_t *bgd    = (ext2_bgd_t *)(tmp + bgd_boff);
	uint32_t inode_tbl = bgd->bg_inode_table_lo;

	/* inode table block + offset within it */
	uint32_t inodes_per_blk = fs->block_size / fs->inode_size;
	uint32_t tbl_blk  = inode_tbl + idx / inodes_per_blk;
	uint32_t tbl_off  = (idx % inodes_per_blk) * fs->inode_size;

	if (read_block(fs, tbl_blk, tmp) != 0) { buddy_free((uint64_t)tmp, 0); return -1; }

	memcpy(out, tmp + tbl_off,
	       (fs->inode_size < sizeof(ext2_inode_t)) ? fs->inode_size : sizeof(ext2_inode_t));

	buddy_free((uint64_t)tmp, 0);
	return 0;
}

/* ── extent tree block mapping ───────────────────────────────── */

static uint32_t ext4_extent_lookup(ext2_fs_t *fs, uint32_t *tree, uint32_t logical_blk)
{
	uint8_t *buf = (uint8_t *)tree;

	for (;;) {
		ext4_eh_t *eh = (ext4_eh_t *)buf;
		if (eh->eh_magic != 0xF30A) return 0;

		if (eh->eh_depth == 0) {
			/* leaf: search extents */
			ext4_extent_t *ext = (ext4_extent_t *)(buf + sizeof(ext4_eh_t));
			for (int i = 0; i < eh->eh_entries; i++) {
				uint32_t start = ext[i].ee_block;
				uint32_t len   = ext[i].ee_len & 0x7FFF;
				if (logical_blk >= start && logical_blk < start + len) {
					uint64_t phys = ((uint64_t)ext[i].ee_start_hi << 32) | ext[i].ee_start_lo;
					return (uint32_t)(phys + (logical_blk - start));
				}
			}
			return 0;
		}

		/* index node: find the right branch */
		ext4_extent_idx_t *idx = (ext4_extent_idx_t *)(buf + sizeof(ext4_eh_t));
		int best = -1;
		for (int i = 0; i < eh->eh_entries; i++) {
			if (idx[i].ei_block <= logical_blk)
				best = i;
		}
		if (best < 0) return 0;

		uint64_t leaf_blk = ((uint64_t)idx[best].ei_leaf_hi << 32) | idx[best].ei_leaf_lo;
		uint8_t *next_buf = buddy_alloc(0);
		if (!next_buf) return 0;
		if (read_block(fs, (uint32_t)leaf_blk, next_buf) != 0) {
			buddy_free((uint64_t)next_buf, 0);
			return 0;
		}
		/* copy into buf and iterate (static 4K buffer on the stack is too big) */
		memcpy(buf, next_buf, fs->block_size);
		buddy_free((uint64_t)next_buf, 0);
	}
}

/* Map logical block number → physical block number */
static uint32_t inode_map_block(ext2_fs_t *fs, ext2_inode_t *in, uint32_t logical)
{
	/* ext4 extent tree */
	if (in->i_flags & EXT4_EXTENTS_FL) {
		uint32_t tree[60];  /* 60×4 = 240 bytes: i_block[15] × 4 bytes */
		memcpy(tree, in->i_block, 60 * sizeof(uint32_t));
		return ext4_extent_lookup(fs, tree, logical);
	}

	uint32_t ptrs_per_blk = fs->block_size / 4;

	/* 12 direct blocks */
	if (logical < 12)
		return in->i_block[logical];

	logical -= 12;
	uint8_t *tmp = buddy_alloc(0);
	if (!tmp) return 0;
	uint32_t result = 0;

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

	/* triple indirect: not implemented — too large for kernel use */
done:
	buddy_free((uint64_t)tmp, 0);
	return result;
}

/* ── VFS inode type ──────────────────────────────────────────── */

typedef struct {
	ext2_fs_t   *fs;
	ext2_inode_t raw;
	uint32_t     ino;
} ext2_inode_priv_t;

static uint64_t ext2_ino_counter = 0;
static vfs_iops_t ext2_iops;

static vfs_inode_t *ext2_vfs_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *raw)
{
	vfs_inode_t *vi = buddy_alloc(0);
	if (!vi) return NULL;
	memset(vi, 0, sizeof(*vi));

	ext2_inode_priv_t *priv = buddy_alloc(0);
	if (!priv) { buddy_free((uint64_t)vi, 0); return NULL; }
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

	ext2_ino_counter++;
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

	uint8_t *tmp = buddy_alloc(0);
	if (!tmp) return -1;

	ssize_t done = 0;
	size_t  rem  = len;
	uint64_t pos = (uint64_t)f->pos;

	while (rem > 0) {
		uint32_t log_blk  = (uint32_t)(pos / fs->block_size);
		uint32_t blk_off  = (uint32_t)(pos % fs->block_size);
		uint32_t phys_blk = inode_map_block(fs, in, log_blk);

		if (read_block(fs, phys_blk, tmp) != 0) break;

		size_t copy = fs->block_size - blk_off;
		if (copy > rem) copy = rem;
		memcpy((uint8_t *)buf + done, tmp + blk_off, copy);
		done += (ssize_t)copy;
		rem  -= copy;
		pos  += copy;
	}

	buddy_free((uint64_t)tmp, 0);
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

static vfs_fops_t ext2_fops = {
	.read = ext2_read,
	.seek = ext2_seek,
};

/* ── ext2 iops ───────────────────────────────────────────────── */

static vfs_inode_t *ext2_lookup(vfs_inode_t *dir, const char *name)
{
	ext2_inode_priv_t *priv = dir->priv;
	ext2_fs_t         *fs   = priv->fs;
	ext2_inode_t      *in   = &priv->raw;
	uint64_t           size = dir->size;

	uint8_t *blk = buddy_alloc(0);
	if (!blk) return NULL;

	uint32_t log_blk = 0;
	uint64_t off     = 0;

	while (off < size) {
		uint32_t phys = inode_map_block(fs, in, log_blk++);
		if (read_block(fs, phys, blk) != 0) break;

		uint32_t blk_off = 0;
		while (blk_off < fs->block_size) {
			ext2_dirent_t *de = (ext2_dirent_t *)(blk + blk_off);
			if (de->rec_len == 0) break;
			if (de->inode != 0 && de->name_len == (uint8_t)strlen(name) &&
			    memcmp(de->name, name, de->name_len) == 0) {
				ext2_inode_t raw;
				if (read_inode(fs, de->inode, &raw) != 0) {
					buddy_free((uint64_t)blk, 0);
					return NULL;
				}
				vfs_inode_t *vi = ext2_vfs_inode(fs, de->inode, &raw);
				if (vi && S_ISREG(vi->mode)) vi->fops = &ext2_fops;
				buddy_free((uint64_t)blk, 0);
				return vi;
			}
			blk_off += de->rec_len;
		}
		off += fs->block_size;
	}

	buddy_free((uint64_t)blk, 0);
	return NULL;
}

static int ext2_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	ext2_inode_priv_t *priv = dir->priv;
	ext2_fs_t         *fs   = priv->fs;
	ext2_inode_t      *in   = &priv->raw;
	uint64_t           size = dir->size;

	uint8_t *blk = buddy_alloc(0);
	if (!blk) return -1;

	uint32_t count   = 0;
	uint32_t log_blk = 0;
	uint64_t off     = 0;

	while (off < size) {
		uint32_t phys = inode_map_block(fs, in, log_blk++);
		if (read_block(fs, phys, blk) != 0) break;

		uint32_t blk_off = 0;
		while (blk_off < fs->block_size) {
			ext2_dirent_t *de = (ext2_dirent_t *)(blk + blk_off);
			if (de->rec_len == 0) break;
			if (de->inode != 0) {
				if (count == idx) {
					out->ino  = de->inode;
					uint8_t nl = de->name_len;
					memcpy(out->name, de->name, nl);
					out->name[nl] = '\0';
					/* file_type: 1=reg, 2=dir, 7=symlink */
					switch (de->file_type) {
					case 1: out->mode = S_IFREG; break;
					case 2: out->mode = S_IFDIR; break;
					case 7: out->mode = S_IFLNK; break;
					default: out->mode = S_IFREG; break;
					}
					buddy_free((uint64_t)blk, 0);
					return 0;
				}
				count++;
			}
			blk_off += de->rec_len;
		}
		off += fs->block_size;
	}

	buddy_free((uint64_t)blk, 0);
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

static vfs_iops_t ext2_iops = {
	.lookup  = ext2_lookup,
	.readdir = ext2_readdir,
	.stat    = ext2_stat,
};

/* ── mount ───────────────────────────────────────────────────── */

static ext2_fs_t g_ext2_fs;

int ext2_mount(vfs_mount_t *mnt, void *data)
{
	block_device_t *bdev = data;
	if (!bdev) return -1;

	/* superblock at byte 1024, i.e. LBA 2 for 512-byte sectors */
	uint8_t *sb_buf = buddy_alloc(0);
	if (!sb_buf) return -1;

	/* read 2 sectors to cover byte 1024..2047 */
	if (bdev->read(bdev, 2, sb_buf, 2) != 0) {
		buddy_free((uint64_t)sb_buf, 0);
		return -1;
	}

	ext2_super_t *sb = (ext2_super_t *)sb_buf;
	if (sb->s_magic != EXT2_MAGIC) {
		kprintf("[ext2] bad magic: 0x%04x\n", sb->s_magic);
		buddy_free((uint64_t)sb_buf, 0);
		return -1;
	}

	uint32_t bs = 1024u << sb->s_log_block_size;
	if (bs > 4096) {
		kprintf("[ext2] block size %u not supported\n", bs);
		buddy_free((uint64_t)sb_buf, 0);
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
	fs->desc_size         = (sb->s_desc_size >= 32) ? sb->s_desc_size : 32;

	kprintf("[ext2] bs=%u ino_sz=%u groups=%u/%u\n",
	        bs, fs->inode_size,
	        (sb->s_blocks_count_lo + fs->blocks_per_group - 1) / fs->blocks_per_group,
	        fs->inodes_per_group);

	buddy_free((uint64_t)sb_buf, 0);

	/* read root inode */
	ext2_inode_t root_raw;
	if (read_inode(fs, EXT2_ROOT_INO, &root_raw) != 0) {
		kprintf("[ext2] cannot read root inode\n");
		return -1;
	}

	vfs_inode_t *root_vi = ext2_vfs_inode(fs, EXT2_ROOT_INO, &root_raw);
	if (!root_vi) return -1;

	vfs_dentry_t *root_dentry = vfs_dentry_create(NULL, "/", root_vi);
	if (!root_dentry) { buddy_free((uint64_t)root_vi->priv, 0); buddy_free((uint64_t)root_vi, 0); return -1; }

	mnt->root = root_dentry;
	kprintf("[ext2] mounted (ino root=%u)\n", EXT2_ROOT_INO);
	return 0;
}

void ext2_register(void)
{
	extern void vfs_register_fs(const char *, int (*)(vfs_mount_t *, void *));
	vfs_register_fs("ext2", ext2_mount);
	vfs_register_fs("ext4", ext2_mount);   /* ext4 is ext2-compatible */
}
