#include <kernel/vfs.h>
#include <kernel/hal.h>
#include <kernel/block.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/sync.h>
#include <kernel/proc.h>
#include <kernel/board.h>
#include <kernel/mm/pmm.h>
#include <kernel/cmdline.h>
#include <asm/mmu.h>

/*
 * ext2 / ext4 VFS driver. ext2 reads and writes; on ext4 a file with extents is
 * read-only, new files are ext2-style. A filesystem with an internal jbd2 journal
 * (ext3) commits metadata through it, in ordered mode, and is recovered at mount.
 *
 * ext4 additions handled:
 *   - Extent tree (EXT4_EXTENTS_FL) for inode block mapping
 *   - 64-bit file sizes (i_size_high)
 * Not supported: inline data, meta_bg, flex_bg with >1 meta block, htree writes.
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

#define EXT2_RA_MAX      CONFIG_BLOCK_READAHEAD /* max transfer, in fs blocks */
#define EXT2_HASH_BITS   16
#define EXT2_IHASH_SIZE  4096
#define EXT2_FLUSH_SECS  5
#define EXT2_LINK_MAX    32000

/* One disk block in the write-back cache. buf is a whole page from the buddy
 * allocator, so at a 4 KB block size a mapping can use it directly: the page
 * refcount then tells eviction it is in use. pins counts kernel holders, the
 * dirty list among them. */
typedef struct bc_ent {
	uint64_t        blkno;
	uint8_t        *buf;
	struct bc_ent  *hnext;
	struct bc_ent  *lprev, *lnext;
	struct bc_ent  *dnext;
	uint32_t        pins;
	volatile uint8_t valid;
	uint8_t         failed;
	uint8_t         dirty;
	uint8_t         ondirty;
	uint8_t         shared;   /* mapped MAP_SHARED writable: stays dirty while mapped */
	uint8_t         dead;     /* out of the cache, freed by the last unpin */
	uint8_t         data;     /* file contents: bypasses the journal, reaches disk first */
} bc_ent_t;

struct ext2_fs;

/* Blocks freed since the last commit. Reusing one earlier would let new contents
 * land on a block the on-disk metadata still gives to the old file. */
#define EXT2_PEND_MAX 4096
typedef struct { uint64_t start; uint32_t len; } ext2_pend_t;

struct ext2_vnode;

/* The in-core inode. A file has one of these however many vfs_inodes reach it
 * (hard links, volatile dentries), so size and block pointers cannot diverge.
 * raw is the truth; every vfs_inode made from it is on vnodes, and ino_write
 * copies a change to all of them, so a chmod through one name holds for every
 * path walk, whichever copy it finds. */
typedef struct ext2_inode_priv {
	struct ext2_fs *fs;
	ext2_inode_t    raw;
	uint32_t        ino;
	uint32_t        opens;    /* open files; the last close frees an unlinked file */
	int             orphan;
	struct ext2_inode_priv *hnext;
	struct ext2_vnode *vnodes;   /* live vfs_inodes of this file, under fs->vn_lock */
} ext2_inode_priv_t;

/* A vfs_inode of ours: the VFS frees it with kfree, so it comes first. */
typedef struct ext2_vnode {
	vfs_inode_t        vi;
	struct ext2_vnode *next;
} ext2_vnode_t;

typedef struct ext2_fs {
	block_device_t *bdev;
	uint32_t        block_size;
	uint32_t        sectors_per_block;
	uint32_t        blocks_per_group;
	uint32_t        inodes_per_group;
	uint16_t        inode_size;
	uint32_t        first_data_block;
	uint32_t        first_ino;
	uint32_t        total_inodes;
	uint64_t        total_blocks;
	uint32_t        num_groups;
	uint16_t        desc_size;
	uint64_t        free_blocks, free_inodes;  /* in core, written out by a flush */
	uint32_t        reserved_blocks;
	int             sb_dirty;
	uint64_t        goal;     /* wanted next block, keeps a file contiguous */
	/* block cache; cache_lock is never held across I/O */
	spinlock_t      cache_lock;
	bc_ent_t      **hash;
	bc_ent_t       *lru_head, *lru_tail;
	bc_ent_t       *dirty_head;  /* metadata */
	bc_ent_t       *data_head;
	uint64_t        bc_pages, bc_max, bc_dirty, bc_meta;
	ext2_pend_t    *pend;
	uint32_t        npend;
	uint32_t        last_orphan;
	uint32_t        free_ops;   /* blocks freed: every so many, a chance to yield */
	int             err;         /* first write error, reported by the next sync */
	int             rdonly;
	int             frozen;      /* marked clean: nothing more may reach the disk */
	int             aborted;     /* the log failed: disk stays at the last commit */
	/* jbd2 log; jmap is NULL without one */
	uint64_t       *jmap;        /* log block -> disk block */
	bc_ent_t      **jm;          /* the commit in progress */
	uint8_t        *jsb, *jbuf;
	uint32_t        jlen, jfirst, jhead, jseq;
	uint32_t        jtag;        /* bytes in a descriptor tag */
	uint8_t         juuid[16];
	/* namespace, allocation and inode metadata */
	mutex_t         lock;
	mutex_t         flush_lock;
	mutex_t         io_lock;    /* owns ra_buf */
	spinlock_t      vn_lock;    /* the vnodes lists; taken under vfs_lock as well */
	ext2_inode_priv_t *ihash[EXT2_IHASH_SIZE];
	ext2_inode_priv_t *ifree;
	uint8_t        *ra_buf;     /* one multi-block transfer, NULL if unavailable */
	int             ra_order;
	struct ext2_fs *next;
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

/* ── block cache ─────────────────────────────────────────────── */

static inline uint32_t bc_slot(uint64_t blkno)
{
	return ((uint32_t)blkno * 0x9E3779B1u) >> (32 - EXT2_HASH_BITS);
}

static bc_ent_t *bc_find(ext2_fs_t *fs, uint64_t blkno)
{
	for (bc_ent_t *e = fs->hash[bc_slot(blkno)]; e; e = e->hnext)
		if (e->blkno == blkno) return e;
	return NULL;
}

static void lru_unlink(ext2_fs_t *fs, bc_ent_t *e)
{
	if (e->lprev) e->lprev->lnext = e->lnext; else fs->lru_head = e->lnext;
	if (e->lnext) e->lnext->lprev = e->lprev; else fs->lru_tail = e->lprev;
	e->lprev = e->lnext = NULL;
}

static void lru_front(ext2_fs_t *fs, bc_ent_t *e)
{
	e->lnext = fs->lru_head;
	if (fs->lru_head) fs->lru_head->lprev = e; else fs->lru_tail = e;
	fs->lru_head = e;
}

static void bc_unhash(ext2_fs_t *fs, bc_ent_t *e)
{
	bc_ent_t **pp = &fs->hash[bc_slot(e->blkno)];
	while (*pp != e) pp = &(*pp)->hnext;
	*pp = e->hnext;
	lru_unlink(fs, e);
	fs->bc_pages--;
}

/* Drops the cache's reference: a page still mapped lives on with its mappers. */
static void bc_release(bc_ent_t *e)
{
	pmm_free((void *)VIRT_TO_PHYS((uint64_t)e->buf));
	kfree(e);
}

static uint64_t bc_evict(ext2_fs_t *fs, uint64_t want)
{
	bc_ent_t *gone = NULL;
	uint64_t scan = 0, limit = want * 64, got = 0;
	uint64_t f = spin_lock_irq(&fs->cache_lock);
	for (bc_ent_t *e = fs->lru_tail, *prev; e && got < want && scan < limit; e = prev) {
		prev = e->lprev;
		scan++;
		if (e->pins || !e->valid) continue;
		page_t *pg = paddr_to_page(VIRT_TO_PHYS((uint64_t)e->buf));
		if (pg && pg->refcount > 1) {   /* mapped, so in use */
			lru_unlink(fs, e);
			lru_front(fs, e);
			continue;
		}
		bc_unhash(fs, e);
		e->hnext = gone;
		gone = e;
		got++;
	}
	spin_unlock_irq(&fs->cache_lock, f);
	while (gone) {
		bc_ent_t *e = gone;
		gone = e->hnext;
		bc_release(e);
	}
	return got;
}

/* The entry for blkno, pinned, or NULL with no memory. *fresh: the caller must
 * fill it and set valid. */
static bc_ent_t *bc_lookup(ext2_fs_t *fs, uint64_t blkno, int *fresh)
{
	*fresh = 0;
	uint64_t f = spin_lock_irq(&fs->cache_lock);
	bc_ent_t *e = bc_find(fs, blkno);
	if (e) {
		e->pins++;
		lru_unlink(fs, e);
		lru_front(fs, e);
		spin_unlock_irq(&fs->cache_lock, f);
		return e;
	}
	spin_unlock_irq(&fs->cache_lock, f);

	uint8_t *pg = ext2_alloc_page();
	if (!pg) {
		bc_evict(fs, 256);
		pg = ext2_alloc_page();
		if (!pg) return NULL;
	}
	bc_ent_t *n = kmalloc(sizeof(*n));
	if (!n) { ext2_free_page(pg); return NULL; }
	memset(n, 0, sizeof(*n));
	n->blkno = blkno;
	n->buf   = pg;
	n->pins  = 1;

	f = spin_lock_irq(&fs->cache_lock);
	e = bc_find(fs, blkno);
	if (e) {
		e->pins++;
		spin_unlock_irq(&fs->cache_lock, f);
		bc_release(n);
		return e;
	}
	uint32_t s = bc_slot(blkno);
	n->hnext = fs->hash[s];
	fs->hash[s] = n;
	lru_front(fs, n);
	int over = ++fs->bc_pages > fs->bc_max;
	spin_unlock_irq(&fs->cache_lock, f);
	if (over) bc_evict(fs, 64);
	*fresh = 1;
	return n;
}

static void bput(ext2_fs_t *fs, bc_ent_t *e)
{
	uint64_t f = spin_lock_irq(&fs->cache_lock);
	int last = --e->pins == 0 && e->dead;
	spin_unlock_irq(&fs->cache_lock, f);
	if (last) bc_release(e);
}

/* Take the entry out of the cache. Holders keep what they pinned. */
static void bc_drop(ext2_fs_t *fs, bc_ent_t *e)
{
	bc_unhash(fs, e);
	e->dead  = 1;
	e->dirty = 0;
}

static void bc_forget(ext2_fs_t *fs, uint64_t blkno)
{
	bc_ent_t *e;
	int last = 0;
	uint64_t f = spin_lock_irq(&fs->cache_lock);
	e = bc_find(fs, blkno);
	if (e) {
		bc_drop(fs, e);
		last = e->pins == 0;
	}
	spin_unlock_irq(&fs->cache_lock, f);
	if (last) bc_release(e);
}

static bc_ent_t *bc_ready(ext2_fs_t *fs, bc_ent_t *e)
{
	while (!e->valid) yield();
	if (e->failed) { bput(fs, e); return NULL; }
	return e;
}

static bc_ent_t *bc_finish(ext2_fs_t *fs, bc_ent_t *e, int err)
{
	if (err) {
		uint64_t f = spin_lock_irq(&fs->cache_lock);
		e->failed = 1;
		bc_drop(fs, e);
		spin_unlock_irq(&fs->cache_lock, f);
	}
	__atomic_store_n(&e->valid, 1, __ATOMIC_RELEASE);
	if (err) { bput(fs, e); return NULL; }
	return e;
}

static bc_ent_t *bget(ext2_fs_t *fs, uint64_t blkno)
{
	int fresh;
	if (blkno >= fs->total_blocks) return NULL;
	bc_ent_t *e = bc_lookup(fs, blkno, &fresh);
	if (!e) return NULL;
	if (!fresh) return bc_ready(fs, e);
	return bc_finish(fs, e, block_read(fs->bdev, blkno * fs->sectors_per_block, e->buf,
	                                   fs->sectors_per_block));
}

/* For a caller about to replace the whole block: nothing is read. */
static bc_ent_t *bnew(ext2_fs_t *fs, uint64_t blkno)
{
	int fresh;
	if (blkno >= fs->total_blocks) return NULL;
	bc_ent_t *e = bc_lookup(fs, blkno, &fresh);
	if (!e) return NULL;
	if (!fresh) return bc_ready(fs, e);
	memset(e->buf, 0, PAGE_SIZE);
	return bc_finish(fs, e, 0);
}

static void bdirty(ext2_fs_t *fs, bc_ent_t *e)
{
	uint64_t f = spin_lock_irq(&fs->cache_lock);
	if (!e->dead) {
		e->dirty = 1;
		if (!e->ondirty) {
			e->ondirty = 1;
			e->pins++;
			if (e->data) {
				e->dnext = fs->data_head;
				fs->data_head = e;
				fs->bc_dirty++;
			} else {
				e->dnext = fs->dirty_head;
				fs->dirty_head = e;
				fs->bc_meta++;
			}
		}
	}
	spin_unlock_irq(&fs->cache_lock, f);
}

static int read_block(ext2_fs_t *fs, uint64_t blkno, void *buf)
{
	if (blkno == 0) { memset(buf, 0, fs->block_size); return 0; }
	bc_ent_t *e = bget(fs, blkno);
	if (!e) return -1;
	memcpy(buf, e->buf, fs->block_size);
	bput(fs, e);
	return 0;
}

static int write_block(ext2_fs_t *fs, uint64_t blkno, const void *buf)
{
	bc_ent_t *e = bnew(fs, blkno);
	if (!e) return -1;
	memcpy(e->buf, buf, fs->block_size);
	bdirty(fs, e);
	bput(fs, e);
	return 0;
}

static bc_ent_t *bc_sort(bc_ent_t *h)
{
	if (!h || !h->dnext) return h;
	bc_ent_t *slow = h, *fast = h->dnext;
	while (fast && fast->dnext) { slow = slow->dnext; fast = fast->dnext->dnext; }
	bc_ent_t *b = bc_sort(slow->dnext);
	slow->dnext = NULL;
	bc_ent_t *a = bc_sort(h), head, *t = &head;
	while (a && b) {
		if (a->blkno <= b->blkno) { t->dnext = a; a = a->dnext; }
		else                      { t->dnext = b; b = b->dnext; }
		t = t->dnext;
	}
	t->dnext = a ? a : b;
	return head.dnext;
}

static void flush_run(ext2_fs_t *fs, bc_ent_t **run, uint32_t n)
{
	uint64_t lba = run[0]->blkno * fs->sectors_per_block;
	int r;
	if (n == 1) {
		r = block_write(fs->bdev, lba, run[0]->buf, fs->sectors_per_block);
	} else {
		mutex_lock(&fs->io_lock);
		for (uint32_t i = 0; i < n; i++)
			memcpy(fs->ra_buf + (size_t)i * fs->block_size, run[i]->buf, fs->block_size);
		r = block_write(fs->bdev, lba, fs->ra_buf, (uint64_t)n * fs->sectors_per_block);
		mutex_unlock(&fs->io_lock);
	}
	if (r) {
		kprintf(KERN_ERR "ext2: write error at block %lu, %u blocks lost\n",
		        (unsigned long)run[0]->blkno, n);
		if (!fs->err) fs->err = -5;
	}
	for (uint32_t i = 0; i < n; i++) {
		bc_ent_t *e = run[i];
		if (e->shared) {
			page_t *pg = paddr_to_page(VIRT_TO_PHYS((uint64_t)e->buf));
			if (pg && pg->refcount > 1) bdirty(fs, e);
			else e->shared = 0;
		}
		bput(fs, e);
	}
}

static void sb_store(ext2_fs_t *fs)
{
	if (!fs->sb_dirty) return;
	fs->sb_dirty = 0;
	uint32_t off = fs->block_size == 1024 ? 0 : 1024;
	bc_ent_t *e = bget(fs, fs->block_size == 1024 ? 1 : 0);
	if (!e) return;
	ext2_super_t *sb = (ext2_super_t *)(e->buf + off);
	sb->s_free_blocks_count_lo = (uint32_t)fs->free_blocks;
	if (sb->s_feature_incompat & 0x80)
		sb->s_free_blocks_count_hi = (uint32_t)(fs->free_blocks >> 32);
	sb->s_free_inodes_count = (uint32_t)fs->free_inodes;
	sb->s_last_orphan = fs->last_orphan;
	bdirty(fs, e);
	bput(fs, e);
}

/* Write a list in disk order and in as few commands as possible. A block
 * redirtied meanwhile goes round again, as that write may have missed it. */
static void flush_list(ext2_fs_t *fs, bc_ent_t *l)
{
	bc_ent_t *run[EXT2_RA_MAX > 1 ? EXT2_RA_MAX : 1];
	uint32_t rmax = EXT2_RA_MAX > 1 && fs->ra_buf ? EXT2_RA_MAX : 1;
	uint32_t n = 0;
	l = bc_sort(l);
	while (l) {
		bc_ent_t *e = l;
		l = e->dnext;
		uint64_t f = spin_lock_irq(&fs->cache_lock);
		e->ondirty = 0;
		int dead = e->dead;
		e->dirty = 0;
		spin_unlock_irq(&fs->cache_lock, f);
		if (dead) { bput(fs, e); continue; }
		if (n && (n == rmax || run[n - 1]->blkno + 1 != e->blkno)) {
			flush_run(fs, run, n);
			n = 0;
		}
		run[n++] = e;
	}
	if (n) flush_run(fs, run, n);
}

static void flush_data(ext2_fs_t *fs)
{
	uint64_t f = spin_lock_irq(&fs->cache_lock);
	bc_ent_t *l = fs->data_head;
	fs->data_head = NULL;
	fs->bc_dirty = 0;
	spin_unlock_irq(&fs->cache_lock, f);
	flush_list(fs, l);
}

/* ── journal ─────────────────────────────────────────────────── */

static uint64_t bgd_block_bitmap(ext2_fs_t *fs, const ext2_bgd_t *b);
static int blk_meta(ext2_fs_t *fs, const ext2_bgd_t *b, uint64_t blk);
static ext2_bgd_t *bgd_get(ext2_fs_t *fs, uint32_t group, bc_ent_t **ep);

/* The jbd2 on-disk format, big-endian, so e2fsck and Linux can replay what we
 * commit. One commit at a time: its metadata is in place and durable before the
 * next begins, so nothing older than the last commit is ever replayed and no
 * revoke records are needed. */
#define JBD_MAGIC        0xC03B3998u
#define JBD_DESCRIPTOR   1
#define JBD_COMMIT       2
#define JBD_SB_V1        3
#define JBD_SB_V2        4
#define JBD_REVOKE       5
#define JBD_FLAG_ESCAPE  1
#define JBD_FLAG_SAME    2
#define JBD_FLAG_LAST    8
#define JBD_IN_REVOKE    1
#define JBD_IN_64BIT     2
#define JSB_SEQUENCE     0x18
#define JSB_START        0x1C

#define EXT2_FEAT_HAS_JOURNAL 0x4
#define EXT2_IN_RECOVER       0x4
#define EXT2_VALID_FS         1

static inline uint32_t be32(const void *p)
{
	const uint8_t *b = p;
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static inline void put_be32(void *p, uint32_t v)
{
	uint8_t *b = p;
	b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v;
}

static int jwrite(ext2_fs_t *fs, uint32_t pos, const void *buf)
{
	return block_write(fs->bdev, fs->jmap[pos] * fs->sectors_per_block, buf, fs->sectors_per_block);
}

static int jread(ext2_fs_t *fs, uint32_t pos, void *buf)
{
	return block_read(fs->bdev, fs->jmap[pos] * fs->sectors_per_block, buf, fs->sectors_per_block);
}

static int jsb_store(ext2_fs_t *fs, uint32_t start, uint32_t seq)
{
	put_be32(fs->jsb + JSB_START, start);
	put_be32(fs->jsb + JSB_SEQUENCE, seq);
	return jwrite(fs, 0, fs->jsb);
}

/* m[0..n) reach the log, durably, as one transaction. Returns 0, 1 if they cannot
 * fit in the log at all, or -5. Caller holds fs->lock, so they are one consistent
 * state of the metadata. */
static int jcommit(ext2_fs_t *fs, bc_ent_t **m, uint32_t n)
{
	uint32_t bs = fs->block_size;
	uint32_t per = (bs - 12 - 16) / fs->jtag;
	uint32_t need = (n + per - 1) / per + n + 1;
	if (need > fs->jlen - fs->jfirst) return 1;
	if (fs->jhead + need > fs->jlen) {
		/* the log restarts over old transactions: nothing may still point at them */
		if (jsb_store(fs, fs->jfirst, fs->jseq) || block_flush_all()) return -5;
		fs->jhead = fs->jfirst;
	}
	uint32_t pos = fs->jhead, i = 0;
	uint8_t *d = fs->jbuf;
	while (i < n) {
		uint32_t c = n - i < per ? n - i : per, off = 12;
		memset(d, 0, bs);
		put_be32(d, JBD_MAGIC);
		put_be32(d + 4, JBD_DESCRIPTOR);
		put_be32(d + 8, fs->jseq);
		for (uint32_t k = 0; k < c; k++) {
			bc_ent_t *e = m[i + k];
			uint32_t fl = (k ? JBD_FLAG_SAME : 0) | (k == c - 1 ? JBD_FLAG_LAST : 0);
			if (be32(e->buf) == JBD_MAGIC) fl |= JBD_FLAG_ESCAPE;
			put_be32(d + off, (uint32_t)e->blkno);
			d[off + 6] = (uint8_t)(fl >> 8);
			d[off + 7] = (uint8_t)fl;
			if (fs->jtag == 12) put_be32(d + off + 8, (uint32_t)(e->blkno >> 32));
			off += fs->jtag;
			if (!k) { memcpy(d + off, fs->juuid, 16); off += 16; }
		}
		if (jwrite(fs, pos++, d)) return -5;
		for (uint32_t k = 0; k < c; k++, i++) {
			bc_ent_t *e = m[i];
			if (be32(e->buf) == JBD_MAGIC) {
				memcpy(d, e->buf, bs);
				memset(d, 0, 4);
				if (jwrite(fs, pos++, d)) return -5;
			} else if (jwrite(fs, pos++, e->buf)) {
				return -5;
			}
		}
	}
	/* the commit block alone decides whether the transaction exists */
	if (block_flush_all()) return -5;
	memset(d, 0, bs);
	put_be32(d, JBD_MAGIC);
	put_be32(d + 4, JBD_COMMIT);
	put_be32(d + 8, fs->jseq);
	if (jwrite(fs, pos++, d) || block_flush_all()) return -5;
	fs->jhead = pos;
	fs->jseq++;
	return 0;
}

/* Return a freed block to its group. Caller holds fs->lock. */
static void block_clear(ext2_fs_t *fs, uint64_t blkno)
{
	uint64_t rel = blkno - fs->first_data_block;
	uint32_t g   = (uint32_t)(rel / fs->blocks_per_group);
	uint32_t bit = (uint32_t)(rel % fs->blocks_per_group);
	bc_ent_t *be;
	ext2_bgd_t *bgd = bgd_get(fs, g, &be);
	if (!bgd) return;
	bc_ent_t *me = bget(fs, bgd_block_bitmap(fs, bgd));
	if (me) {
		if (!(me->buf[bit / 8] & (1u << (bit & 7))) || blk_meta(fs, bgd, blkno)) {
			kprintf(KERN_ERR "ext2: bad free of block %lu, run e2fsck\n", (unsigned long)blkno);
			fs->free_blocks--;
		} else {
			me->buf[bit / 8] &= ~(1u << (bit & 7));
			bdirty(fs, me);
			bgd->bg_free_blocks_count_lo++;
			bdirty(fs, be);
		}
		bput(fs, me);
	}
	bput(fs, be);
}

static void pend_apply(ext2_fs_t *fs)
{
	for (uint32_t i = 0; i < fs->npend; i++)
		for (uint32_t k = 0; k < fs->pend[i].len; k++)
			block_clear(fs, fs->pend[i].start + k);
	fs->npend = 0;
}

/* ranix.fscrash=S, a test aid: reset S seconds after mount, inside a commit, which
 * a random reset almost never hits and which is the only case recovery is for. */
static uint64_t fscrash_tick;

/* Metadata reaches the disk in place, journalled if there is a log. */
static void flush_meta(ext2_fs_t *fs, bc_ent_t *l)
{
	uint32_t n = 0;
	if (!l) { mutex_unlock(&fs->lock); return; }
	if (fs->jmap) {
		l = bc_sort(l);
		uint32_t cnt = 0;
		for (bc_ent_t *e = l; e; e = e->dnext) cnt++;
		if (cnt <= fs->jlen) {
			/* off the list before fs->lock goes: a later change must queue again
			 * for the next commit, not slip into this one's in-place write */
			uint64_t f = spin_lock_irq(&fs->cache_lock);
			for (bc_ent_t *e = l; e; e = e->dnext) {
				fs->jm[n++] = e;
				e->ondirty = 0;
				e->dirty = 0;
			}
			spin_unlock_irq(&fs->cache_lock, f);
			int r = jcommit(fs, fs->jm, n);
			if (r < 0) {
				/* in-core state is now ahead of a log we cannot write */
				kprintf(KERN_ERR "ext2: journal write failed, no further writes\n");
				fs->aborted = 1;
				mutex_unlock(&fs->lock);
				for (uint32_t i = 0; i < n; i++) bput(fs, fs->jm[i]);
				return;
			}
			if (r == 0) {
				/* fs->lock stays until these are issued: once the log moves on, disk
				 * must hold exactly this commit, not the next one's half-done changes */
				bc_ent_t *run[EXT2_RA_MAX > 1 ? EXT2_RA_MAX : 1];
				uint32_t rmax = EXT2_RA_MAX > 1 && fs->ra_buf ? EXT2_RA_MAX : 1, c = 0;
				extern volatile uint64_t kernel_ticks;
				int crash = fscrash_tick && kernel_ticks >= fscrash_tick;
				for (uint32_t i = 0; i < n; i++) {
					bc_ent_t *e = fs->jm[i];
					if (crash && i == n / 2) board_reset();   /* committed, half home */
					if (e->dead) { bput(fs, e); continue; }
					if (c && (c == rmax || run[c - 1]->blkno + 1 != e->blkno)) {
						flush_run(fs, run, c);
						c = 0;
					}
					run[c++] = e;
				}
				if (c) flush_run(fs, run, c);
				mutex_unlock(&fs->lock);
				if (block_flush_all() && !fs->err) fs->err = -5;
				/* the log is spent; the next flush makes this durable too */
				if (jsb_store(fs, fs->jhead, fs->jseq) && !fs->err) fs->err = -5;
				return;
			}
			/* too big for the log: not atomic, say so and keep the data */
			for (uint32_t i = 0; i < n; i++) fs->jm[i]->ondirty = 1;
		}
		kprintf(KERN_WARN "ext2: %u metadata blocks exceed the log, written unjournalled\n", cnt);
	}
	mutex_unlock(&fs->lock);
	flush_list(fs, l);
}

/* Ordered mode: file data first, without blocking the namespace for what may be
 * hundreds of megabytes, then under fs->lock what slipped in, and the metadata
 * that points at it. jcommit's device flush covers the data too. Without a log,
 * everything is durable once a following block_flush_all returns. */
static void ext2_flush(ext2_fs_t *fs)
{
	mutex_lock(&fs->flush_lock);
	if (fs->frozen || fs->aborted) { mutex_unlock(&fs->flush_lock); return; }
	flush_data(fs);
	mutex_lock(&fs->lock);
	pend_apply(fs);
	sb_store(fs);
	uint64_t f = spin_lock_irq(&fs->cache_lock);
	bc_ent_t *l = fs->dirty_head;
	fs->dirty_head = NULL;
	fs->bc_meta = 0;
	spin_unlock_irq(&fs->cache_lock, f);
	flush_data(fs);
	flush_meta(fs, l);
	mutex_unlock(&fs->flush_lock);
}

/* A file's data block, with the contiguous blocks after it read in one command.
 * left: how many of the file's blocks come after this one. */
static uint64_t inode_map_block(ext2_fs_t *fs, ext2_inode_t *in, uint32_t logical);

static bc_ent_t *bget_file(ext2_fs_t *fs, ext2_inode_t *in, uint32_t lbn, uint64_t phys, uint64_t left)
{
	bc_ent_t *run[EXT2_RA_MAX > 1 ? EXT2_RA_MAX : 1];
	uint32_t n = 0, want = 1 + (left < EXT2_RA_MAX ? (uint32_t)left : EXT2_RA_MAX);
	int fresh;

	if (!fs->ra_buf || want < 2 || EXT2_RA_MAX < 2) return bget(fs, phys);
	if (phys >= fs->total_blocks) return NULL;
	if (want > EXT2_RA_MAX) want = EXT2_RA_MAX;
	if (want > fs->total_blocks - phys) want = (uint32_t)(fs->total_blocks - phys);
	bc_ent_t *first = bc_lookup(fs, phys, &fresh);
	if (!first) return NULL;
	if (!fresh) return bc_ready(fs, first);
	run[n++] = first;
	while (n < want) {
		if (inode_map_block(fs, in, lbn + n) != phys + n) break;
		bc_ent_t *e = bc_lookup(fs, phys + n, &fresh);
		if (!e) break;
		if (!fresh) { bput(fs, e); break; }
		run[n++] = e;
	}
	if (n == 1)
		return bc_finish(fs, first, block_read(fs->bdev, phys * fs->sectors_per_block,
		                                       first->buf, fs->sectors_per_block));
	mutex_lock(&fs->io_lock);
	int r = block_read(fs->bdev, phys * fs->sectors_per_block, fs->ra_buf,
	                   (uint64_t)n * fs->sectors_per_block);
	if (r == 0)
		for (uint32_t i = 0; i < n; i++)
			memcpy(run[i]->buf, fs->ra_buf + (size_t)i * fs->block_size, fs->block_size);
	mutex_unlock(&fs->io_lock);
	for (uint32_t i = 1; i < n; i++)
		if (bc_finish(fs, run[i], r)) bput(fs, run[i]);
	return bc_finish(fs, first, r);
}

/* ── inode lookup ────────────────────────────────────────────── */

static ext2_bgd_t *bgd_get(ext2_fs_t *fs, uint32_t group, bc_ent_t **ep)
{
	/* BGD table starts at block first_data_block+1 */
	uint32_t off = group * fs->desc_size;
	bc_ent_t *e = bget(fs, fs->first_data_block + 1 + off / fs->block_size);
	if (!e) return NULL;
	*ep = e;
	return (ext2_bgd_t *)(e->buf + off % fs->block_size);
}

static bc_ent_t *inode_block(ext2_fs_t *fs, uint32_t ino, uint32_t *offp)
{
	if (ino == 0 || ino > fs->total_inodes) return NULL;
	uint32_t group = (ino - 1) / fs->inodes_per_group;
	uint32_t idx   = (ino - 1) % fs->inodes_per_group;
	bc_ent_t *be;
	ext2_bgd_t *bgd = bgd_get(fs, group, &be);
	if (!bgd) return NULL;
	uint64_t inode_tbl = bgd->bg_inode_table_lo;
	if (fs->desc_size >= 64)
		inode_tbl |= (uint64_t)bgd->bg_inode_table_hi << 32;   /* ext4 64BIT */
	bput(fs, be);

	/* inode table block + offset within it */
	uint32_t inodes_per_blk = fs->block_size / fs->inode_size;
	*offp = (idx % inodes_per_blk) * fs->inode_size;
	return bget(fs, inode_tbl + idx / inodes_per_blk);
}

static int read_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *out)
{
	uint32_t off;
	bc_ent_t *e = inode_block(fs, ino, &off);
	if (!e) return -1;
	memset(out, 0, sizeof(*out));
	memcpy(out, e->buf + off,
	       (fs->inode_size < sizeof(ext2_inode_t)) ? fs->inode_size : sizeof(ext2_inode_t));
	bput(fs, e);
	return 0;
}

static int write_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *in)
{
	uint32_t off;
	bc_ent_t *e = inode_block(fs, ino, &off);
	if (!e) return -1;
	memcpy(e->buf + off, in,
	       (fs->inode_size < sizeof(ext2_inode_t)) ? fs->inode_size : sizeof(ext2_inode_t));
	bdirty(fs, e);
	bput(fs, e);
	return 0;
}

/* ── extent tree block mapping ───────────────────────────────── */

static uint64_t ext4_extent_lookup(ext2_fs_t *fs, uint32_t *tree, uint32_t logical_blk)
{
	uint8_t *buf = (uint8_t *)tree;

	for (int level = 0; level < 6; level++) {   /* a forged index can point at itself */
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
	return 0;
}

static uint32_t ind_entry(ext2_fs_t *fs, uint32_t blk, uint32_t idx)
{
	if (!blk) return 0;
	bc_ent_t *e = bget(fs, blk);
	if (!e) return 0;
	uint32_t v = ((uint32_t *)e->buf)[idx];
	bput(fs, e);
	return v;
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
	uint64_t ppp = ptrs_per_blk;
	if (12 + ppp + ppp * ppp + ppp * ppp * ppp <= logical) return 0;

	/* 12 direct blocks */
	if (logical < 12)
		return in->i_block[logical];

	logical -= 12;

	/* single indirect */
	if (logical < ptrs_per_blk)
		return ind_entry(fs, in->i_block[12], logical);
	logical -= ptrs_per_blk;

	/* double indirect */
	if (logical < ptrs_per_blk * ptrs_per_blk) {
		uint32_t l1 = ind_entry(fs, in->i_block[13], logical / ptrs_per_blk);
		return ind_entry(fs, l1, logical % ptrs_per_blk);
	}
	logical -= ptrs_per_blk * ptrs_per_blk;

	/* triple indirect */
	uint32_t l1 = ind_entry(fs, in->i_block[14], logical / (ptrs_per_blk * ptrs_per_blk));
	uint32_t l2 = ind_entry(fs, l1, (logical / ptrs_per_blk) % ptrs_per_blk);
	return ind_entry(fs, l2, logical % ptrs_per_blk);
}

/* ── VFS inode type ──────────────────────────────────────────── */

static uint64_t   ext2_ino_counter = 0;
static spinlock_t ext2_ino_lock    = SPINLOCK_INIT;
static vfs_iops_t ext2_iops, ext2_fifo_iops, ext2_dev_iops;
static vfs_fops_t ext2_fops, ext2_fops_paged;
extern void fifo_init_inode(vfs_inode_t *in);

/* Past the triple indirect tree an index would run off its page. */
static inline uint64_t ino_max_blocks(const ext2_fs_t *fs)
{
	uint64_t ppp = fs->block_size / 4;
	return 12 + ppp + ppp * ppp + ppp * ppp * ppp;
}

static inline uint64_t ino_size(const ext2_inode_t *in)
{
	return (uint64_t)in->i_size_lo | ((uint64_t)in->i_size_high << 32);
}

static inline int has_nsec(ext2_fs_t *fs, const ext2_inode_t *in)
{
	return fs->inode_size >= sizeof(ext2_inode_t) && in->i_extra_isize >= 16;
}

/* Caller holds fs->lock. */
static ext2_inode_priv_t *iget(ext2_fs_t *fs, uint32_t ino)
{
	ext2_inode_priv_t **slot = &fs->ihash[ino % EXT2_IHASH_SIZE];
	for (ext2_inode_priv_t *p = *slot; p; p = p->hnext)
		if (p->ino == ino) return p;
	ext2_inode_priv_t *p = fs->ifree;
	if (p) fs->ifree = p->hnext;
	else if (!(p = kmalloc(sizeof(*p)))) return NULL;
	memset(p, 0, sizeof(*p));
	p->fs  = fs;
	p->ino = ino;
	if (read_inode(fs, ino, &p->raw) != 0) {
		p->hnext = fs->ifree;
		fs->ifree = p;
		return NULL;
	}
	p->hnext = *slot;
	*slot = p;
	return p;
}

static void iforget(ext2_fs_t *fs, ext2_inode_priv_t *p)
{
	ext2_inode_priv_t **pp = &fs->ihash[p->ino % EXT2_IHASH_SIZE];
	while (*pp != p) pp = &(*pp)->hnext;
	*pp = p->hnext;
	/* a walk may still hold a vfs_inode pointing here, so it is reused, not freed;
	 * such a copy of a gone file takes no more updates */
	uint64_t f = spin_lock_irq(&fs->vn_lock);
	p->vnodes = NULL;
	spin_unlock_irq(&fs->vn_lock, f);
	p->hnext = fs->ifree;
	fs->ifree = p;
}

/* The attributes a vfs_inode copies from the disk inode. */
static void vn_fill(ext2_fs_t *fs, const ext2_inode_t *raw, vfs_inode_t *vi)
{
	vi->mode  = raw->i_mode;
	vi->uid   = raw->i_uid_lo;
	vi->gid   = raw->i_gid_lo;
	vi->size  = ino_size(raw);
	vi->nlink = raw->i_links_count;
	vi->atime = raw->i_atime;
	vi->mtime = raw->i_mtime;
	vi->ctime = raw->i_ctime;
	if (has_nsec(fs, raw)) {
		vi->atime_nsec = raw->i_atime_extra >> 2;
		vi->mtime_nsec = raw->i_mtime_extra >> 2;
		vi->ctime_nsec = raw->i_ctime_extra >> 2;
	}
}

/* Write p's disk inode and bring every vfs_inode of it up to date. Every
 * change to raw ends here. Caller holds fs->lock. */
static int ino_write(ext2_fs_t *fs, ext2_inode_priv_t *p)
{
	uint64_t f = spin_lock_irq(&fs->vn_lock);
	for (ext2_vnode_t *v = p->vnodes; v; v = v->next)
		vn_fill(fs, &p->raw, &v->vi);
	spin_unlock_irq(&fs->vn_lock, f);
	return write_inode(fs, p->ino, &p->raw);
}

/* iops->release: the VFS is freeing this copy. */
static void ext2_release(vfs_inode_t *vi)
{
	ext2_inode_priv_t *p = vi->priv;
	uint64_t f = spin_lock_irq(&p->fs->vn_lock);
	for (ext2_vnode_t **pp = &p->vnodes; *pp; pp = &(*pp)->next)
		if (&(*pp)->vi == vi) { *pp = (*pp)->next; break; }
	spin_unlock_irq(&p->fs->vn_lock, f);
}

static vfs_inode_t *ext2_vfs_inode(ext2_fs_t *fs, ext2_inode_priv_t *priv)
{
	ext2_inode_t *raw = &priv->raw;
	ext2_vnode_t *vn = kmalloc(sizeof(*vn));
	if (!vn) return NULL;
	memset(vn, 0, sizeof(*vn));
	vfs_inode_t *vi = &vn->vi;

	vi->ino      = priv->ino;
	vn_fill(fs, raw, vi);
	vi->refcount = 1;
	vi->priv     = priv;
	vi->priv_static = 1;
	vi->iops     = &ext2_iops;
	if (S_ISCHR(vi->mode) || S_ISBLK(vi->mode))
		vi->rdev = raw->i_block[0] ? raw->i_block[0] : raw->i_block[1];
	if (S_ISREG(vi->mode)) vi->fops = fs->block_size == PAGE_SIZE ? &ext2_fops_paged : &ext2_fops;
	if (S_ISFIFO(vi->mode)) {   /* priv is the pipe from here on, not ours */
		fifo_init_inode(vi);
		vi->priv_static = 0;
		vi->iops = &ext2_fifo_iops;
	}
	/* A device node opens its driver, found by number as devfs and tmpfs do; priv is
	 * then the driver's (never freed here), and stat answers from the copy above. */
	if (S_ISCHR(vi->mode) || S_ISBLK(vi->mode) || (vi->mode & 0170000) == 0140000) {
		if ((vi->mode & 0170000) != 0140000) {
			extern vfs_fops_t *devfs_fops_for_rdev(dev_t, void **);
			void *dpriv = NULL;
			vi->fops = devfs_fops_for_rdev(vi->rdev, &dpriv);
			vi->priv = dpriv;
		} else {
			vi->priv = NULL;
		}
		vi->priv_static = 1;
		vi->iops = &ext2_dev_iops;
	}
	/* FIFOs and device nodes keep no tie to priv, so they are not on the list */
	if (vi->iops == &ext2_iops) {
		uint64_t f = spin_lock_irq(&fs->vn_lock);
		vn->next = priv->vnodes;
		priv->vnodes = vn;
		spin_unlock_irq(&fs->vn_lock, f);
	}

	uint64_t f = spin_lock_irq(&ext2_ino_lock);
	ext2_ino_counter++;
	spin_unlock_irq(&ext2_ino_lock, f);
	return vi;
}

/* Stamp ctime, and mtime when asked. */
static void ino_stamp(ext2_fs_t *fs, ext2_inode_t *in, vfs_inode_t *vi, int mtime)
{
	int64_t s, ns;
	ranix_walltime(&s, &ns);
	in->i_ctime = (uint32_t)s;
	if (mtime) in->i_mtime = (uint32_t)s;
	if (has_nsec(fs, in)) {
		in->i_ctime_extra = (uint32_t)ns << 2;
		if (mtime) in->i_mtime_extra = (uint32_t)ns << 2;
	}
	if (vi) {
		vi->ctime = s; vi->ctime_nsec = (uint32_t)ns;
		if (mtime) { vi->mtime = s; vi->mtime_nsec = (uint32_t)ns; }
	}
}

/* ── ext2 fops: read ─────────────────────────────────────────── */

static ssize_t ext2_read(vfs_file_t *f, void *buf, size_t len)
{
	ext2_inode_priv_t *priv = f->inode->priv;
	ext2_fs_t         *fs   = priv->fs;
	ext2_inode_t      *in   = &priv->raw;
	uint64_t           size = ino_size(in);

	if (size > ino_max_blocks(fs) * fs->block_size) size = ino_max_blocks(fs) * fs->block_size;
	if (f->pos >= (off_t)size) return 0;
	if ((uint64_t)f->pos + len > size) len = (size_t)(size - (uint64_t)f->pos);

	ssize_t done = 0;
	size_t  rem  = len;
	uint64_t pos = (uint64_t)f->pos;
	uint8_t *zero = NULL;
	int err = -5;

	while (rem > 0) {
		uint32_t log_blk  = (uint32_t)(pos / fs->block_size);
		uint32_t blk_off  = (uint32_t)(pos % fs->block_size);
		uint64_t phys_blk = inode_map_block(fs, in, log_blk);
		size_t copy = fs->block_size - blk_off;
		if (copy > rem) copy = rem;

		if (!phys_blk) {   /* a hole */
			if (!zero && !(zero = ext2_alloc_page())) break;
			memset(zero, 0, copy);
			if (copy_fop_to_user((uint8_t *)buf + done, zero, copy) != 0) { err = -14; break; }
		} else {
			uint64_t left = (size - pos - 1) / fs->block_size;
			bc_ent_t *e = bget_file(fs, in, log_blk, phys_blk, left);
			if (!e) break;
			int r = copy_fop_to_user((uint8_t *)buf + done, e->buf + blk_off, copy);
			bput(fs, e);
			if (r != 0) { err = -14; break; }
		}
		done += (ssize_t)copy;
		rem  -= copy;
		pos  += copy;
	}

	ext2_free_page(zero);
	f->pos = (off_t)pos;
	return done || !len ? done : err;
}

static off_t ext2_seek(vfs_file_t *f, off_t off, int whence)
{
	ext2_inode_priv_t *priv = f->inode->priv;
	uint64_t size = ino_size(&priv->raw);
	off_t    new;
	if (whence == 0)      new = off;
	else if (whence == 1) new = f->pos + off;
	else if (whence == 2) new = (off_t)size + off;
	else return -22;
	if (new < 0) return -22;
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

static uint32_t first_clear(const uint8_t *bmp, uint32_t from, uint32_t lim)
{
	for (uint32_t i = from; i < lim; ) {
		if (!(i & 7) && bmp[i / 8] == 0xFF) { i += 8; continue; }
		if (!(bmp[i / 8] & (1u << (i & 7)))) return i;
		i++;
	}
	return lim;
}

static int blk_meta(ext2_fs_t *fs, const ext2_bgd_t *b, uint64_t blk)
{
	uint64_t itab = b->bg_inode_table_lo;
	if (fs->desc_size >= 64) itab |= (uint64_t)b->bg_inode_table_hi << 32;
	uint64_t itab_n = ((uint64_t)fs->inodes_per_group * fs->inode_size + fs->block_size - 1) / fs->block_size;
	uint64_t gdt_n = ((uint64_t)fs->num_groups * fs->desc_size + fs->block_size - 1) / fs->block_size;
	return blk == bgd_block_bitmap(fs, b) || blk == bgd_inode_bitmap(fs, b) ||
	       (blk >= itab && blk < itab + itab_n) || blk <= fs->first_data_block + gdt_n;
}

/* Take a free block in group g, at bit goal if free, else the next free one. */
static uint32_t block_take(ext2_fs_t *fs, uint32_t g, uint32_t goal)
{
	bc_ent_t *be;
	ext2_bgd_t *bgd = bgd_get(fs, g, &be);
	if (!bgd) return 0;
	if (bgd->bg_free_blocks_count_lo == 0) { bput(fs, be); return 0; }
	bc_ent_t *me = bget(fs, bgd_block_bitmap(fs, bgd));
	if (!me) { bput(fs, be); return 0; }

	uint32_t lim = fs->blocks_per_group;
	uint64_t past = fs->total_blocks - fs->first_data_block - (uint64_t)g * lim;
	if (past < lim) lim = (uint32_t)past;
	uint32_t i;
	for (;;) {
		i = first_clear(me->buf, goal, lim);
		if (i >= lim) i = first_clear(me->buf, 0, goal < lim ? goal : lim);
		if (i >= lim) break;
		if (!blk_meta(fs, bgd, (uint64_t)g * fs->blocks_per_group + fs->first_data_block + i)) break;
		/* a damaged bitmap must not hand out the inode table */
		kprintf(KERN_ERR "ext2: bitmap of group %u frees metadata, run e2fsck\n", g);
		me->buf[i / 8] |= (1u << (i & 7));
		bdirty(fs, me);
	}
	uint32_t got = 0;
	if (i < lim && !(me->buf[i / 8] & (1u << (i & 7)))) {
		me->buf[i / 8] |= (1u << (i & 7));
		bdirty(fs, me);
		bgd->bg_free_blocks_count_lo--;
		bdirty(fs, be);
		fs->free_blocks--;
		fs->sb_dirty = 1;
		got = g * fs->blocks_per_group + fs->first_data_block + i;
	}
	bput(fs, me);
	bput(fs, be);
	return got;
}

static uint32_t alloc_block(ext2_fs_t *fs, uint32_t pref_group)
{
	if (fs->goal > fs->first_data_block && fs->goal < fs->total_blocks) {
		uint64_t rel = fs->goal - fs->first_data_block;
		uint32_t b = block_take(fs, (uint32_t)(rel / fs->blocks_per_group),
		                        (uint32_t)(rel % fs->blocks_per_group));
		if (b) { fs->goal = (uint64_t)b + 1; return b; }
	}
	for (int retry = 0; retry < 2; retry++) {
		for (uint32_t gi = 0; gi < fs->num_groups; gi++) {
			uint32_t b = block_take(fs, (pref_group + gi) % fs->num_groups, 0);
			if (b) { fs->goal = (uint64_t)b + 1; return b; }
		}
		if (!fs->npend) break;
		pend_apply(fs);    /* a nearly full disk, where ENOSPC is the worse answer */
	}
	return 0;
}

static void free_block(ext2_fs_t *fs, uint64_t blkno)
{
	if (!blkno || blkno < fs->first_data_block) return;
	if (blkno >= fs->total_blocks) return;
	/* before the block can be handed out again: its old contents are garbage now,
	 * and a mapping that still has the page must not see the next file's data */
	bc_forget(fs, blkno);
	if (!(++fs->free_ops & 1023)) {
		extern void cond_resched(void);
		cond_resched();             /* a big file frees millions: let others run */
	}
	fs->free_blocks++;
	fs->sb_dirty = 1;
	if (fs->npend) {
		ext2_pend_t *l = &fs->pend[fs->npend - 1];
		if (l->start + l->len == blkno) { l->len++; return; }
	}
	if (fs->npend == EXT2_PEND_MAX) pend_apply(fs);
	fs->pend[fs->npend++] = (ext2_pend_t){ blkno, 1 };
}

static uint32_t alloc_inode(ext2_fs_t *fs, uint32_t pref_group, int isdir)
{
	for (uint32_t gi = 0; gi < fs->num_groups; gi++) {
		uint32_t g = (pref_group + gi) % fs->num_groups;
		bc_ent_t *be;
		ext2_bgd_t *bgd = bgd_get(fs, g, &be);
		if (!bgd) continue;
		if (bgd->bg_free_inodes_count_lo == 0) { bput(fs, be); continue; }
		bc_ent_t *me = bget(fs, bgd_inode_bitmap(fs, bgd));
		if (!me) { bput(fs, be); continue; }
		uint32_t i = first_clear(me->buf, 0, fs->inodes_per_group);
		while (i < fs->inodes_per_group && g == 0 && i + 1 < fs->first_ino) {
			/* never a reserved inode, such as the journal's */
			kprintf(KERN_ERR "ext2: inode bitmap frees reserved inode %u, run e2fsck\n", i + 1);
			me->buf[i / 8] |= (1u << (i & 7));
			bdirty(fs, me);
			i = first_clear(me->buf, i + 1, fs->inodes_per_group);
		}
		uint32_t ino = 0;
		if (i < fs->inodes_per_group) {
			me->buf[i / 8] |= (1u << (i & 7));
			bdirty(fs, me);
			bgd->bg_free_inodes_count_lo--;
			if (isdir) bgd->bg_used_dirs_count_lo++;
			bdirty(fs, be);
			fs->free_inodes--;
			fs->sb_dirty = 1;
			ino = g * fs->inodes_per_group + i + 1;
		}
		bput(fs, me);
		bput(fs, be);
		if (ino) return ino;
	}
	return 0;
}

static void free_inode(ext2_fs_t *fs, uint32_t ino, int isdir)
{
	if (ino < 2 || ino > fs->total_inodes) return;
	uint32_t g   = (ino - 1) / fs->inodes_per_group;
	uint32_t bit = (ino - 1) % fs->inodes_per_group;
	bc_ent_t *be;
	ext2_bgd_t *bgd = bgd_get(fs, g, &be);
	if (!bgd) return;
	bc_ent_t *me = bget(fs, bgd_inode_bitmap(fs, bgd));
	if (me) {
		me->buf[bit / 8] &= ~(1u << (bit & 7));
		bdirty(fs, me);
		bput(fs, me);
		bgd->bg_free_inodes_count_lo++;
		if (isdir && bgd->bg_used_dirs_count_lo) bgd->bg_used_dirs_count_lo--;
		bdirty(fs, be);
		fs->free_inodes++;
		fs->sb_dirty = 1;
	}
	bput(fs, be);
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
			write_block(fs, ib, tmp);   /* the next alloc may fail and leave it linked */
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
	uint64_t limit = ino_max_blocks(fs) * fs->block_size;
	if (pos >= limit) return -27;   /* EFBIG */
	if (len > limit - pos) len = (size_t)(limit - pos);
	size_t done = 0;
	int err = 0;

	while (done < len) {
		uint32_t lbn   = (uint32_t)(pos / fs->block_size);
		uint32_t boff  = (uint32_t)(pos % fs->block_size);
		uint32_t chunk = fs->block_size - boff;
		if (chunk > (uint32_t)(len - done)) chunk = (uint32_t)(len - done);

		bc_ent_t *e = NULL;
		mutex_lock(&fs->lock);
		uint64_t phys = inode_map_block(fs, in, lbn);
		int created = !phys;
		if (created) {
			if (lbn) {
				uint64_t prev = inode_map_block(fs, in, lbn - 1);
				if (prev) fs->goal = prev + 1;
			}
			phys = inode_alloc_block(fs, in, priv->ino, lbn);
			if (phys) {
				/* All one transaction, as a commit can fall anywhere in this loop:
				 * the pointer, a size reaching it (fsck rejects blocks past EOF)
				 * and zeros for the block, or the log would expose old contents. */
				e = bnew(fs, phys);
				if (e) { e->data = 1; bdirty(fs, e); }
				if (ino_size(in) < (uint64_t)lbn * fs->block_size) {
					uint64_t sz = (uint64_t)lbn * fs->block_size;
					in->i_size_lo   = (uint32_t)sz;
					in->i_size_high = (uint32_t)(sz >> 32);
					f->inode->size  = (off_t)sz;
				}
			}
			ino_write(fs, priv);   /* a failed alloc may have linked indirects */
		}
		mutex_unlock(&fs->lock);
		if (!phys) { err = -28; break; }

		if (!e) e = chunk == fs->block_size ? bnew(fs, phys) : bget(fs, phys);
		if (!e) { err = -5; break; }
		e->data = 1;
		int r = copy_fop_from_user(e->buf + boff, src + done, chunk);
		bdirty(fs, e);
		bput(fs, e);
		if (r != 0) { err = -14; break; }
		pos  += chunk;
		done += chunk;
	}

	mutex_lock(&fs->lock);
	if (pos > ino_size(in)) {
		in->i_size_lo   = (uint32_t)pos;
		in->i_size_high = (uint32_t)(pos >> 32);
		f->inode->size  = (off_t)pos;
	}
	f->pos = (off_t)pos;
	ino_stamp(fs, in, f->inode, 1);
	ino_write(fs, priv);
	mutex_unlock(&fs->lock);
	if (fs->bc_dirty > fs->bc_max / 4) ext2_flush(fs);
	return done ? (ssize_t)done : err;
}

/* A page for mmap, carrying one reference for the mapping. A file page is the
 * cached block itself, so every process mapping a library shares it, a private
 * mapping copying only what it writes. A hole or the space past EOF reads as
 * zeros, from a page of the mapping's own. */
static uint64_t ext2_mmap_page(vfs_file_t *f, off_t off, int shared_write)
{
	ext2_inode_priv_t *priv = f->inode->priv;
	ext2_fs_t *fs = priv->fs;
	ext2_inode_t *in = &priv->raw;
	uint64_t size = ino_size(in);

	if (off < 0 || (off & (PAGE_SIZE - 1))) return 0;
	uint64_t phys = 0;
	uint32_t lbn = (uint32_t)((uint64_t)off / PAGE_SIZE);
	int inside = (uint64_t)off < size, created = 0;
	if (inside) phys = inode_map_block(fs, in, lbn);
	if (inside && !phys && shared_write) {
		if (in->i_flags & EXT4_EXTENTS_FL) return 0;
		mutex_lock(&fs->lock);
		phys = inode_map_block(fs, in, lbn);
		if (!phys) {
			phys = inode_alloc_block(fs, in, priv->ino, lbn);
			created = 1;
		}
		if (phys) {
			if (created) {   /* with the pointer, as in ext2_write */
				bc_ent_t *z = bnew(fs, phys);
				if (z) { z->data = 1; bdirty(fs, z); bput(fs, z); }
			}
			ino_write(fs, priv);
		}
		mutex_unlock(&fs->lock);
		if (!phys) return 0;
	}
	if (!phys) {
		uint8_t *pg = ext2_alloc_page();
		if (!pg) return 0;
		memset(pg, 0, PAGE_SIZE);
		return VIRT_TO_PHYS((uint64_t)pg);
	}
	/* a block just allocated holds whatever was there, and must not be read */
	bc_ent_t *e = created ? bnew(fs, phys)
	                      : bget_file(fs, in, lbn, phys, (size - 1 - (uint64_t)off) / PAGE_SIZE);
	if (!e) return 0;
	uint64_t pa = VIRT_TO_PHYS((uint64_t)e->buf);
	pmm_hold((void *)pa);
	e->data = 1;
	if (shared_write) {
		e->shared = 1;
		bdirty(fs, e);
	}
	bput(fs, e);
	return pa;
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
			if (de->rec_len < 8 || off + de->rec_len > fs->block_size) break;
			uint16_t used = de->inode ? (uint16_t)(((uint32_t)8 + de->name_len + 3u) & ~3u) : 0;
			if (used > de->rec_len) break;
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
			if (de->rec_len < 8 || off + de->rec_len > fs->block_size) break;
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

/* Free the part of an indirect tree past keep logical blocks. *slot is a block
 * of pointers whose first entry maps logical block base. depth 1 points at data.
 * Caller holds fs->lock. */
static void trunc_tree(ext2_fs_t *fs, ext2_inode_t *in, uint32_t *slot, int depth,
                       uint64_t base, uint64_t keep)
{
	if (!*slot) return;
	uint32_t ppp = fs->block_size / 4;
	uint64_t span = 1;
	for (int i = 1; i < depth; i++) span *= ppp;
	uint32_t self = *slot;
	bc_ent_t *e = bget(fs, self);
	if (!e) return;
	uint32_t *p = (uint32_t *)e->buf;
	for (uint32_t i = 0; i < ppp; i++) {
		uint64_t b = base + i * span;
		if (b + span <= keep || !p[i]) continue;
		if (depth == 1) {
			free_block(fs, p[i]);
			in->i_blocks_lo -= fs->sectors_per_block;
			p[i] = 0;
		} else {
			trunc_tree(fs, in, &p[i], depth - 1, b, keep);
		}
		bdirty(fs, e);
	}
	bput(fs, e);
	if (keep <= base) {
		free_block(fs, self);
		in->i_blocks_lo -= fs->sectors_per_block;
		*slot = 0;
	}
}

/* Free every data block past the first keep. Caller holds fs->lock. */
static void inode_free_blocks(ext2_fs_t *fs, ext2_inode_t *in, uint64_t keep)
{
	if (in->i_flags & EXT4_EXTENTS_FL) return;
	uint64_t ppp = fs->block_size / 4;

	/* Direct */
	for (uint32_t i = keep < 12 ? (uint32_t)keep : 12; i < 12; i++)
		if (in->i_block[i]) {
			free_block(fs, in->i_block[i]);
			in->i_blocks_lo -= fs->sectors_per_block;
			in->i_block[i] = 0;
		}

	trunc_tree(fs, in, &in->i_block[12], 1, 12, keep);
	trunc_tree(fs, in, &in->i_block[13], 2, 12 + ppp, keep);
	trunc_tree(fs, in, &in->i_block[14], 3, 12 + ppp + ppp * ppp, keep);
}

static int do_truncate(ext2_fs_t *fs, ext2_inode_priv_t *priv, vfs_inode_t *vi, uint64_t len)
{
	ext2_inode_t *in = &priv->raw;
	if (in->i_flags & EXT4_EXTENTS_FL) return -30;
	if (len > ino_max_blocks(fs) * fs->block_size) return -27;   /* EFBIG */
	uint64_t old = ino_size(in);
	if (len < old) {
		uint32_t tail = (uint32_t)(len % fs->block_size);
		inode_free_blocks(fs, in, (len + fs->block_size - 1) / fs->block_size);
		if (tail) {   /* what a later extension must read as zeros */
			uint64_t phys = inode_map_block(fs, in, (uint32_t)(len / fs->block_size));
			bc_ent_t *e = phys ? bget(fs, phys) : NULL;
			if (e) {
				e->data = 1;
				memset(e->buf + tail, 0, fs->block_size - tail);
				bdirty(fs, e);
				bput(fs, e);
			}
		}
	}
	in->i_size_lo   = (uint32_t)len;
	in->i_size_high = (uint32_t)(len >> 32);
	if (vi) vi->size = len;
	ino_stamp(fs, in, vi, 1);
	return ino_write(fs, priv) ? -5 : 0;
}

/* The last link is gone. An open file keeps its blocks until the last close.
 * Caller holds fs->lock. */
static void ino_release(ext2_fs_t *fs, ext2_inode_priv_t *p)
{
	ext2_inode_t *in = &p->raw;
	if (p->opens) {
		if (p->orphan) {            /* on the list already: twice would make a ring */
			kprintf(KERN_ERR "ext2: inode %u released twice while open\n", p->ino);
			return;
		}
		/* on the superblock's list, for a crash before the last close */
		p->orphan = 1;
		in->i_dtime = fs->last_orphan;
		fs->last_orphan = p->ino;
		fs->sb_dirty = 1;
		ino_write(fs, p);
		return;
	}
	int fast_link = S_ISLNK(in->i_mode) && !in->i_blocks_lo;
	int dev = S_ISCHR(in->i_mode) || S_ISBLK(in->i_mode) || S_ISFIFO(in->i_mode);
	if (!fast_link && !dev) inode_free_blocks(fs, in, 0);
	int64_t s, ns;
	ranix_walltime(&s, &ns);
	in->i_dtime = (uint32_t)s;
	ino_write(fs, p);
	free_inode(fs, p->ino, S_ISDIR(in->i_mode));
	iforget(fs, p);
}

static void orphan_unlink(ext2_fs_t *fs, ext2_inode_priv_t *p)
{
	uint32_t next = p->raw.i_dtime;
	if (fs->last_orphan == p->ino) {
		fs->last_orphan = next;
		fs->sb_dirty = 1;
	} else {
		uint32_t steps = 0;
		for (uint32_t i = fs->last_orphan; i; ) {
			if (++steps > 100000) {   /* a ring in the list: stop walking it */
				kprintf(KERN_ERR "ext2: orphan list loops, inode %u not found in it\n", p->ino);
				break;
			}
			ext2_inode_priv_t *q = iget(fs, i);
			if (!q) break;
			if (q->raw.i_dtime == p->ino) {
				q->raw.i_dtime = next;
				ino_write(fs, q);
				break;
			}
			i = q->raw.i_dtime;
		}
	}
	p->orphan = 0;
	p->raw.i_dtime = 0;
}

static int ext2_open(vfs_inode_t *vi, vfs_file_t *f, int flags)
{
	ext2_inode_priv_t *priv = vi->priv;
	ext2_fs_t *fs = priv->fs;
	int r = 0;
	(void)f;
	mutex_lock(&fs->lock);
	priv->opens++;
	if ((flags & O_TRUNC) && (flags & 3) && ino_size(&priv->raw))
		r = do_truncate(fs, priv, vi, 0);
	if (r) priv->opens--;
	mutex_unlock(&fs->lock);
	return r;
}

static int ext2_close(vfs_file_t *f)
{
	ext2_inode_priv_t *priv = f->inode->priv;
	ext2_fs_t *fs = priv->fs;
	mutex_lock(&fs->lock);
	if (priv->opens && --priv->opens == 0 && priv->orphan) {
		orphan_unlink(fs, priv);
		ino_release(fs, priv);
	}
	mutex_unlock(&fs->lock);
	return 0;
}

static int ext2_truncate(vfs_inode_t *vi, off_t len)
{
	ext2_inode_priv_t *priv = vi->priv;
	ext2_fs_t *fs = priv->fs;
	if (len < 0) return -22;
	mutex_lock(&fs->lock);
	int r = do_truncate(fs, priv, vi, (uint64_t)len);
	mutex_unlock(&fs->lock);
	return r;
}

static uint32_t dir_find(ext2_fs_t *fs, ext2_inode_t *dir, const char *name, uint8_t *ftype)
{
	size_t len = strlen(name);
	uint8_t nlen = (uint8_t)len;
	uint64_t size = ino_size(dir);
	uint32_t found = 0;
	if (len > 255) return 0;

	for (uint32_t lbn = 0; !found && (uint64_t)lbn * fs->block_size < size; lbn++) {
		uint64_t phys = inode_map_block(fs, dir, lbn);
		if (!phys) continue;
		bc_ent_t *e = bget(fs, phys);
		if (!e) break;
		uint32_t off = 0;
		while (off + 8 <= fs->block_size) {
			ext2_dirent_t *de = (ext2_dirent_t *)(e->buf + off);
			/* rec_len/name_len come from disk; keep the header and the name
			 * inside the block so a corrupt directory can't read off the page. */
			if (de->rec_len < 8 || off + de->rec_len > fs->block_size) break;
			if (de->inode != 0 && off + 8u + de->name_len <= fs->block_size &&
			    de->name_len == nlen && memcmp(de->name, name, nlen) == 0) {
				found = de->inode;
				if (ftype) *ftype = de->file_type;
				break;
			}
			off += de->rec_len;
		}
		bput(fs, e);
	}
	return found;
}

static uint8_t mode_ftype(mode_t m)
{
	if (S_ISREG(m))  return 1;
	if (S_ISDIR(m))  return 2;
	if (S_ISCHR(m))  return 3;
	if (S_ISBLK(m))  return 4;
	if (S_ISFIFO(m)) return 5;
	if ((m & 0170000) == 0140000) return 6;
	if (S_ISLNK(m))  return 7;
	return 0;
}

/* A new inode, in the table and on disk. Caller holds fs->lock. */
static ext2_inode_priv_t *ino_new(ext2_fs_t *fs, ext2_inode_priv_t *dp, uint16_t mode, uint16_t links)
{
	uint32_t ino = alloc_inode(fs, (dp->ino - 1) / fs->inodes_per_group, S_ISDIR(mode));
	if (!ino) return NULL;
	ext2_inode_priv_t *p = iget(fs, ino);
	if (!p) { free_inode(fs, ino, S_ISDIR(mode)); return NULL; }
	memset(&p->raw, 0, sizeof(p->raw));
	p->raw.i_mode        = mode;
	p->raw.i_links_count = links;
	process_t *t = task_current();
	p->raw.i_uid_lo = t ? (uint16_t)t->euid : 0;
	p->raw.i_gid_lo = t ? (uint16_t)t->egid : 0;
	if (fs->inode_size >= sizeof(ext2_inode_t)) p->raw.i_extra_isize = 32;
	ino_stamp(fs, &p->raw, NULL, 1);
	p->raw.i_atime = p->raw.i_mtime;
	p->raw.i_atime_extra = p->raw.i_mtime_extra;
	return p;
}

static int ext2_create(vfs_inode_t *dir, const char *name, mode_t mode)
{
	ext2_inode_priv_t *dpriv = dir->priv;
	ext2_fs_t *fs = dpriv->fs;
	int r = -5;
	if (strlen(name) > 255) return -36;

	mutex_lock(&fs->lock);
	if (dir_find(fs, &dpriv->raw, name, NULL)) { r = -17; goto out; }
	ext2_inode_priv_t *p = ino_new(fs, dpriv, (uint16_t)(S_IFREG | (mode & 0xFFF)), 1);
	if (!p) { r = -28; goto out; }
	if (ino_write(fs, p) != 0 ||
	    (r = dir_add_entry(fs, dpriv->ino, &dpriv->raw, p->ino, name, 1 /*EXT2_FT_REG_FILE*/)) != 0) {
		p->raw.i_links_count = 0;
		ino_release(fs, p);
		goto out;
	}
	ino_stamp(fs, &dpriv->raw, dir, 1);
	ino_write(fs, dpriv);
	dir->size = dpriv->raw.i_size_lo;
	r = 0;
out:
	mutex_unlock(&fs->lock);
	return r;
}

static int ext2_mkdir(vfs_inode_t *dir, const char *name, mode_t mode)
{
	ext2_inode_priv_t *dpriv = dir->priv;
	ext2_fs_t *fs = dpriv->fs;
	int r = -5;
	if (strlen(name) > 255) return -36;

	mutex_lock(&fs->lock);
	if (dir_find(fs, &dpriv->raw, name, NULL)) { r = -17; goto out; }
	if (dpriv->raw.i_links_count >= EXT2_LINK_MAX) { r = -31; goto out; }
	ext2_inode_priv_t *p = ino_new(fs, dpriv, (uint16_t)(S_IFDIR | (mode & 0xFFF ? mode & 0xFFF : 0755)), 2);
	if (!p) { r = -28; goto out; }

	/* Allocate and write first block with . and .. */
	fs->goal = 0;
	uint32_t phys = inode_alloc_block(fs, &p->raw, p->ino, 0);
	bc_ent_t *e = phys ? bnew(fs, phys) : NULL;
	if (!e) { r = -28; p->raw.i_links_count = 0; ino_release(fs, p); goto out; }
	memset(e->buf, 0, fs->block_size);

	ext2_dirent_t *dot = (ext2_dirent_t *)e->buf;
	dot->inode     = p->ino;
	dot->rec_len   = 12;
	dot->name_len  = 1;
	dot->file_type = 2; /* EXT2_FT_DIR */
	dot->name[0]   = '.';

	ext2_dirent_t *dotdot = (ext2_dirent_t *)(e->buf + 12);
	dotdot->inode     = dpriv->ino;
	dotdot->rec_len   = (uint16_t)(fs->block_size - 12);
	dotdot->name_len  = 2;
	dotdot->file_type = 2;
	dotdot->name[0]   = '.';
	dotdot->name[1]   = '.';

	bdirty(fs, e);
	bput(fs, e);

	p->raw.i_size_lo = fs->block_size;
	r = -5;
	if (ino_write(fs, p) != 0 ||
	    (r = dir_add_entry(fs, dpriv->ino, &dpriv->raw, p->ino, name, 2 /*EXT2_FT_DIR*/)) != 0) {
		p->raw.i_links_count = 0;
		ino_release(fs, p);
		goto out;
	}
	dpriv->raw.i_links_count++;
	ino_stamp(fs, &dpriv->raw, dir, 1);
	ino_write(fs, dpriv);
	dir->size  = dpriv->raw.i_size_lo;
	dir->nlink = dpriv->raw.i_links_count;
	r = 0;
out:
	mutex_unlock(&fs->lock);
	return r;
}

/* Only . and .. left? A removed first entry leaves inode 0, so it can't be told
 * by position. */
static int dir_empty(ext2_fs_t *fs, ext2_inode_t *dir)
{
	uint64_t size = ino_size(dir);
	for (uint32_t lbn = 0; (uint64_t)lbn * fs->block_size < size; lbn++) {
		uint64_t phys = inode_map_block(fs, dir, lbn);
		if (!phys) continue;
		bc_ent_t *e = bget(fs, phys);
		if (!e) return 0;
		for (uint32_t off = 0; off + 8 <= fs->block_size; ) {
			ext2_dirent_t *de = (ext2_dirent_t *)(e->buf + off);
			if (de->rec_len < 8 || off + de->rec_len > fs->block_size) break;
			int dots = (de->name_len == 1 && de->name[0] == '.') ||
			           (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.');
			if (de->inode && !dots) { bput(fs, e); return 0; }
			off += de->rec_len;
		}
		bput(fs, e);
	}
	return 1;
}

/* A name of p is gone. Caller holds fs->lock. */
static void drop_link(ext2_fs_t *fs, ext2_inode_priv_t *p)
{
	if (S_ISDIR(p->raw.i_mode)) p->raw.i_links_count = 0;
	else if (p->raw.i_links_count > 0) p->raw.i_links_count--;
	ino_stamp(fs, &p->raw, NULL, 0);
	if (p->raw.i_links_count == 0) ino_release(fs, p);
	else ino_write(fs, p);
}

/* Point an existing name at another inode, which needs no room in the directory. */
static int dir_retarget(ext2_fs_t *fs, ext2_inode_t *dir, const char *name, uint32_t ino, uint8_t ftype)
{
	size_t nlen = strlen(name);
	uint64_t size = ino_size(dir);
	for (uint32_t lbn = 0; (uint64_t)lbn * fs->block_size < size; lbn++) {
		uint64_t phys = inode_map_block(fs, dir, lbn);
		bc_ent_t *e = phys ? bget(fs, phys) : NULL;
		if (!e) continue;
		for (uint32_t off = 0; off + 8 <= fs->block_size; ) {
			ext2_dirent_t *de = (ext2_dirent_t *)(e->buf + off);
			if (de->rec_len < 8 || off + de->rec_len > fs->block_size) break;
			if (de->inode && de->name_len == nlen && off + 8u + nlen <= fs->block_size &&
			    memcmp(de->name, name, nlen) == 0) {
				de->inode = ino;
				de->file_type = ftype;
				bdirty(fs, e);
				bput(fs, e);
				return 0;
			}
			off += de->rec_len;
		}
		bput(fs, e);
	}
	return -2;
}

/* Is ino dir itself or one of its ancestors? Caller holds fs->lock. */
static int dir_under(ext2_fs_t *fs, ext2_inode_priv_t *dir, uint32_t ino)
{
	for (int depth = 0; depth < 4096; depth++) {
		if (dir->ino == ino) return 1;
		if (dir->ino == EXT2_ROOT_INO) return 0;
		uint32_t up = dir_find(fs, &dir->raw, "..", NULL);
		if (!up || up == dir->ino || !(dir = iget(fs, up))) return 0;
	}
	return 1;   /* a loop already on disk: refuse */
}

/* Remove name from dpriv and drop the link it held. Caller holds fs->lock. */
static int ext2_remove(ext2_fs_t *fs, ext2_inode_priv_t *dpriv, vfs_inode_t *dir, const char *name)
{
	uint32_t ino = dir_find(fs, &dpriv->raw, name, NULL);
	if (!ino) return -2; /* ENOENT */
	ext2_inode_priv_t *p = iget(fs, ino);
	if (!p) return -5;
	int isdir = S_ISDIR(p->raw.i_mode);
	if (isdir && !dir_empty(fs, &p->raw)) return -39;

	dir_remove_entry(fs, &dpriv->raw, name);
	ino_stamp(fs, &dpriv->raw, dir, 1);
	if (isdir) dpriv->raw.i_links_count--;
	ino_write(fs, dpriv);
	if (dir) { dir->size = dpriv->raw.i_size_lo; dir->nlink = dpriv->raw.i_links_count; }
	drop_link(fs, p);
	return 0;
}

static int ext2_unlink(vfs_inode_t *dir, const char *name)
{
	ext2_inode_priv_t *dpriv = dir->priv;
	ext2_fs_t *fs = dpriv->fs;
	mutex_lock(&fs->lock);
	int r = ext2_remove(fs, dpriv, dir, name);
	mutex_unlock(&fs->lock);
	return r;
}

static int ext2_rename(vfs_inode_t *odir, const char *oname, vfs_inode_t *ndir, const char *nname)
{
	ext2_inode_priv_t *op = odir->priv, *np = ndir->priv;
	ext2_fs_t *fs = op->fs;
	int r = 0;
	uint8_t ft = 0;
	if (strlen(nname) > 255) return -36;

	mutex_lock(&fs->lock);
	uint32_t ino = dir_find(fs, &op->raw, oname, &ft);
	if (!ino) { r = -2; goto out; }
	ext2_inode_priv_t *p = iget(fs, ino);
	if (!p) { r = -5; goto out; }
	int isdir = S_ISDIR(p->raw.i_mode);
	if (isdir && op != np && dir_under(fs, np, ino)) { r = -22; goto out; }

	uint32_t tino = dir_find(fs, &np->raw, nname, NULL);
	if (tino == ino) goto out;   /* two names of one file: nothing to do */
	uint8_t nft = ft ? ft : mode_ftype(p->raw.i_mode);
	if (tino) {
		ext2_inode_priv_t *t = iget(fs, tino);
		if (!t) { r = -5; goto out; }
		int tdir = S_ISDIR(t->raw.i_mode);
		if (isdir && !tdir) { r = -20; goto out; }
		if (!isdir && tdir) { r = -21; goto out; }
		if (tdir && !dir_empty(fs, &t->raw)) { r = -39; goto out; }
		/* in place, so a full disk cannot leave the target gone and the file unnamed */
		r = dir_retarget(fs, &np->raw, nname, ino, nft);
		if (r) goto out;
		if (tdir) {
			np->raw.i_links_count--;
			ndir->nlink = np->raw.i_links_count;
		}
		drop_link(fs, t);
	} else {
		r = dir_add_entry(fs, np->ino, &np->raw, ino, nname, nft);
		if (r) goto out;
	}
	dir_remove_entry(fs, &op->raw, oname);
	ino_stamp(fs, &op->raw, odir, 1);
	ino_stamp(fs, &np->raw, ndir, 1);
	ino_stamp(fs, &p->raw, NULL, 0);
	ino_write(fs, p);

	if (isdir && op != np) {
		/* .. is the block's second entry and now names the new parent */
		uint64_t phys = inode_map_block(fs, &p->raw, 0);
		bc_ent_t *e = phys ? bget(fs, phys) : NULL;
		if (e) {
			uint32_t first = ((ext2_dirent_t *)e->buf)->rec_len;
			if (first >= 8 && first + 8 <= fs->block_size) {
				ext2_dirent_t *dd = (ext2_dirent_t *)(e->buf + first);
				dd->inode = np->ino;
				bdirty(fs, e);
			}
			bput(fs, e);
		}
		op->raw.i_links_count--;
		np->raw.i_links_count++;
		odir->nlink = op->raw.i_links_count;
		ndir->nlink = np->raw.i_links_count;
	}
	ino_write(fs, op);
	if (np != op) ino_write(fs, np);
	odir->size = op->raw.i_size_lo;
	ndir->size = np->raw.i_size_lo;
out:
	mutex_unlock(&fs->lock);
	return r;
}

static int ext2_link(vfs_inode_t *dir, const char *name, vfs_inode_t *target)
{
	ext2_inode_priv_t *dpriv = dir->priv, *p = target->priv;
	ext2_fs_t *fs = dpriv->fs;
	if (S_ISDIR(target->mode) || S_ISFIFO(target->mode)) return -1;
	if (strlen(name) > 255) return -36;
	mutex_lock(&fs->lock);
	/* a file whose last name is gone stays gone: a new name would put an orphan
	 * back in the tree, and its next unlink would release it twice */
	int r = p->raw.i_links_count == 0 || p->orphan ? -2
	      : dir_find(fs, &dpriv->raw, name, NULL) ? -17
	      : p->raw.i_links_count >= EXT2_LINK_MAX ? -31
	      : dir_add_entry(fs, dpriv->ino, &dpriv->raw, p->ino, name, mode_ftype(target->mode));
	if (r == 0) {
		p->raw.i_links_count++;
		target->nlink = p->raw.i_links_count;
		ino_stamp(fs, &p->raw, target, 0);
		ino_write(fs, p);
		ino_stamp(fs, &dpriv->raw, dir, 1);
		ino_write(fs, dpriv);
		dir->size = dpriv->raw.i_size_lo;
	}
	mutex_unlock(&fs->lock);
	return r;
}

/* FIFOs (make's jobserver), device nodes (a distribution's /dev in a chroot,
 * debootstrap's first check) and socket names. The device number goes where
 * Linux puts it: the old 8+8 bit form in i_block[0] when it fits, else the new one
 * in i_block[1]. */
static int ext2_mknod(vfs_inode_t *dir, const char *name, mode_t mode, dev_t dev)
{
	ext2_inode_priv_t *dpriv = dir->priv;
	ext2_fs_t *fs = dpriv->fs;
	int r = -5;
	int type = mode & 0170000;
	if (type != S_IFIFO && type != S_IFCHR && type != S_IFBLK && type != 0140000) return -1;
	if (strlen(name) > 255) return -36;
	uint8_t ft = type == S_IFIFO ? 5 : type == S_IFCHR ? 3 : type == S_IFBLK ? 4 : 6;

	mutex_lock(&fs->lock);
	if (dir_find(fs, &dpriv->raw, name, NULL)) { r = -17; goto out; }
	ext2_inode_priv_t *p = ino_new(fs, dpriv, (uint16_t)(type | (mode & 0xFFF)), 1);
	if (!p) { r = -28; goto out; }
	if (type == S_IFCHR || type == S_IFBLK) {
		uint32_t maj = (uint32_t)((dev >> 8) & 0xfff), min = (uint32_t)((dev & 0xff) | ((dev >> 12) & 0xfff00));
		if (maj < 256 && min < 256) p->raw.i_block[0] = maj << 8 | min;
		else p->raw.i_block[1] = (min & 0xff) | (maj << 8) | ((min & ~0xffu) << 12);
	}
	if (ino_write(fs, p) != 0 ||
	    (r = dir_add_entry(fs, dpriv->ino, &dpriv->raw, p->ino, name, ft)) != 0) {
		p->raw.i_links_count = 0;
		ino_release(fs, p);
		goto out;
	}
	ino_stamp(fs, &dpriv->raw, dir, 1);
	ino_write(fs, dpriv);
	dir->size = dpriv->raw.i_size_lo;
	r = 0;
out:
	mutex_unlock(&fs->lock);
	return r;
}

static int ext2_symlink(vfs_inode_t *dir, const char *name, const char *target)
{
	ext2_inode_priv_t *dpriv = dir->priv;
	ext2_fs_t *fs = dpriv->fs;
	size_t len = strlen(target);
	int r;
	if (len == 0 || len >= fs->block_size || strlen(name) > 255) return -36;

	mutex_lock(&fs->lock);
	if (dir_find(fs, &dpriv->raw, name, NULL)) { r = -17; goto out; }
	ext2_inode_priv_t *p = ino_new(fs, dpriv, (uint16_t)(S_IFLNK | 0777), 1);
	if (!p) { r = -28; goto out; }
	p->raw.i_size_lo = (uint32_t)len;
	if (len < 60) {
		memcpy(p->raw.i_block, target, len);
	} else {
		fs->goal = 0;
		uint32_t phys = inode_alloc_block(fs, &p->raw, p->ino, 0);
		bc_ent_t *e = phys ? bnew(fs, phys) : NULL;
		if (!e) { r = -28; p->raw.i_links_count = 0; ino_release(fs, p); goto out; }
		memset(e->buf, 0, fs->block_size);
		memcpy(e->buf, target, len);
		bdirty(fs, e);
		bput(fs, e);
	}
	r = -5;
	if (ino_write(fs, p) != 0 ||
	    (r = dir_add_entry(fs, dpriv->ino, &dpriv->raw, p->ino, name, 7 /*EXT2_FT_SYMLINK*/)) != 0) {
		p->raw.i_links_count = 0;
		ino_release(fs, p);
		goto out;
	}
	ino_stamp(fs, &dpriv->raw, dir, 1);
	ino_write(fs, dpriv);
	dir->size = dpriv->raw.i_size_lo;
	r = 0;
out:
	mutex_unlock(&fs->lock);
	return r;
}

static int ext2_setattr(vfs_inode_t *vi, const vfs_iattr_t *a)
{
	int r = vfs_iattr_apply(vi, a);
	if (r || !(a->valid & (ATTR_MODE | ATTR_UID | ATTR_GID | ATTR_ATIME | ATTR_MTIME | ATTR_CTIME)))
		return r;
	ext2_inode_priv_t *priv = vi->priv;
	ext2_fs_t *fs = priv->fs;
	ext2_inode_t *in = &priv->raw;
	int ns = has_nsec(fs, in);
	mutex_lock(&fs->lock);
	if (a->valid & ATTR_MODE) in->i_mode = (uint16_t)((in->i_mode & 0xF000) | (vi->mode & 0xFFF));
	if (a->valid & ATTR_UID)  in->i_uid_lo = (uint16_t)vi->uid;
	if (a->valid & ATTR_GID)  in->i_gid_lo = (uint16_t)vi->gid;
	if (a->valid & ATTR_ATIME) { in->i_atime = (uint32_t)vi->atime; if (ns) in->i_atime_extra = vi->atime_nsec << 2; }
	if (a->valid & ATTR_MTIME) { in->i_mtime = (uint32_t)vi->mtime; if (ns) in->i_mtime_extra = vi->mtime_nsec << 2; }
	if (a->valid & ATTR_CTIME) { in->i_ctime = (uint32_t)vi->ctime; if (ns) in->i_ctime_extra = vi->ctime_nsec << 2; }
	r = ino_write(fs, priv) ? -5 : 0;
	mutex_unlock(&fs->lock);
	return r;
}

static vfs_fops_t ext2_fops = {
	.open  = ext2_open,
	.close = ext2_close,
	.read  = ext2_read,
	.write = ext2_write,
	.seek  = ext2_seek,
};

/* Pages are blocks, so mapping needs no copy. */
static vfs_fops_t ext2_fops_paged = {
	.open  = ext2_open,
	.close = ext2_close,
	.read  = ext2_read,
	.write = ext2_write,
	.seek  = ext2_seek,
	.mmap_page = ext2_mmap_page,
};

/* ── ext2 iops ───────────────────────────────────────────────── */

static vfs_inode_t *ext2_lookup(vfs_inode_t *dir, const char *name)
{
	ext2_inode_priv_t *priv = dir->priv;
	ext2_fs_t         *fs   = priv->fs;
	vfs_inode_t       *vi   = NULL;

	mutex_lock(&fs->lock);
	uint32_t ino = dir_find(fs, &priv->raw, name, NULL);
	ext2_inode_priv_t *p = ino ? iget(fs, ino) : NULL;
	if (p) vi = ext2_vfs_inode(fs, p);
	mutex_unlock(&fs->lock);
	return vi;
}

/* idx is a byte offset into the directory data (0 = start).
   out->next_off is set to the byte offset of the next entry. */
static int ext2_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	ext2_inode_priv_t *priv = dir->priv;
	ext2_fs_t         *fs   = priv->fs;
	ext2_inode_t      *in   = &priv->raw;
	uint64_t           size = ino_size(in);
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
	ext2_inode_priv_t *priv = vi->priv;
	ext2_fs_t *fs = priv->fs;
	ext2_inode_t *in = &priv->raw;
	int ns = has_nsec(fs, in);
	out->ino   = vi->ino;
	out->mode  = in->i_mode;
	out->nlink = in->i_links_count;
	out->uid   = in->i_uid_lo;
	out->gid   = in->i_gid_lo;
	out->size  = ino_size(in);
	out->rdev  = vi->rdev;
	out->atime = in->i_atime; out->atime_nsec = ns ? in->i_atime_extra >> 2 : 0;
	out->mtime = in->i_mtime; out->mtime_nsec = ns ? in->i_mtime_extra >> 2 : 0;
	out->ctime = in->i_ctime; out->ctime_nsec = ns ? in->i_ctime_extra >> 2 : 0;
	return 0;
}

/* A FIFO's vfs_inode has no ext2 priv: everything comes from what lookup copied. */
static int ext2_fifo_stat(vfs_inode_t *vi, vfs_stat_t *out)
{
	out->ino   = vi->ino;
	out->mode  = vi->mode;
	out->nlink = vi->nlink;
	out->uid   = vi->uid;
	out->gid   = vi->gid;
	out->atime = vi->atime; out->atime_nsec = vi->atime_nsec;
	out->mtime = vi->mtime; out->mtime_nsec = vi->mtime_nsec;
	out->ctime = vi->ctime; out->ctime_nsec = vi->ctime_nsec;
	return 0;
}

static vfs_iops_t ext2_fifo_iops = {
	.stat = ext2_fifo_stat,
};

static int ext2_dev_stat(vfs_inode_t *vi, vfs_stat_t *out)
{
	ext2_fifo_stat(vi, out);
	out->rdev = vi->rdev;
	return 0;
}

static vfs_iops_t ext2_dev_iops = {
	.stat = ext2_dev_stat,
};

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

/* statfs: report real geometry and the in-core free counts. */
static int ext2_statfs(vfs_inode_t *inode, vfs_statfs_t *out)
{
	ext2_inode_priv_t *priv = inode->priv;
	if (!priv || !priv->fs) return -38;
	ext2_fs_t *fs = priv->fs;
	uint64_t rsvd = fs->reserved_blocks;

	out->bsize   = fs->block_size;
	out->blocks  = fs->total_blocks;
	out->bfree   = fs->free_blocks;
	out->bavail  = (fs->free_blocks > rsvd) ? fs->free_blocks - rsvd : 0;
	out->files   = fs->total_inodes;
	out->ffree   = fs->free_inodes;
	out->namelen = 255;
	return 0;
}

static vfs_iops_t ext2_iops = {
	.lookup   = ext2_lookup,
	.readdir  = ext2_readdir,
	.stat     = ext2_stat,
	.readlink = ext2_readlink,
	.create   = ext2_create,
	.mkdir    = ext2_mkdir,
	.mknod    = ext2_mknod,
	.unlink   = ext2_unlink,
	.statfs   = ext2_statfs,
	.rename   = ext2_rename,
	.link     = ext2_link,
	.symlink  = ext2_symlink,
	.truncate = ext2_truncate,
	.setattr  = ext2_setattr,
	.release  = ext2_release,
};

/* ── writeback ───────────────────────────────────────────────── */

static ext2_fs_t *ext2_all;
static mutex_t    ext2_all_lock = MUTEX_INIT;    /* held across a flush */
static spinlock_t ext2_all_spin = SPINLOCK_INIT; /* the list; the OOM path can't sleep */

/* Everything written so far reaches the devices; a write that failed since the
 * last call is reported once. An empty list proves nothing: ext2_write's own flush
 * may have taken it and still be writing, and ext2_flush waits for that. */
int ext2_sync_all(void)
{
	int r = 0;
	mutex_lock(&ext2_all_lock);
	for (ext2_fs_t *fs = ext2_all; fs; fs = fs->next) {
		ext2_flush(fs);
		if (fs->err) { if (!r) r = fs->err; fs->err = 0; }
		if (fs->aborted && !r) r = -5;
	}
	mutex_unlock(&ext2_all_lock);
	return r;
}

/* What the superblock says about how this mount ended. Direct, as the log is
 * being retired and a later cache flush must not undo it. */
static void sb_state(ext2_fs_t *fs, int clean)
{
	uint32_t off = fs->block_size == 1024 ? 0 : 1024;
	uint64_t blk = fs->block_size == 1024 ? 1 : 0;
	bc_ent_t *e = bget(fs, blk);
	if (!e) return;
	ext2_super_t *sb = (ext2_super_t *)(e->buf + off);
	int64_t s, ns;
	ranix_walltime(&s, &ns);
	if (clean) {
		sb->s_state |= EXT2_VALID_FS;
		sb->s_feature_incompat &= ~EXT2_IN_RECOVER;
		sb->s_wtime = (uint32_t)s;
	} else {
		sb->s_state &= (uint16_t)~EXT2_VALID_FS;
		if (fs->jmap) sb->s_feature_incompat |= EXT2_IN_RECOVER;
		sb->s_mnt_count++;
		sb->s_mtime = (uint32_t)s;
	}
	if (block_write(fs->bdev, blk * fs->sectors_per_block, e->buf, fs->sectors_per_block) && !fs->err)
		fs->err = -5;
	bput(fs, e);
}

/* The last flush of a mount: on disk it now reads as cleanly unmounted. */
static void ext2_seal(ext2_fs_t *fs)
{
	if (fs->rdonly) return;
	ext2_flush(fs);
	mutex_lock(&fs->flush_lock);
	if (!fs->frozen) {
		fs->frozen = 1;
		if (fs->aborted) { mutex_unlock(&fs->flush_lock); return; }
		/* empty log first: e2fsck refuses one with no recovery flag */
		if (fs->jmap) { jsb_store(fs, 0, fs->jseq); block_flush_all(); }
		sb_state(fs, 1);
		block_flush_all();
	}
	mutex_unlock(&fs->flush_lock);
}

/* Going down: whatever runs after this stays in memory. */
void ext2_shutdown_all(void)
{
	mutex_lock(&ext2_all_lock);
	for (ext2_fs_t *fs = ext2_all; fs; fs = fs->next) ext2_seal(fs);
	mutex_unlock(&ext2_all_lock);
}

/* Memory pressure: give back clean, unmapped blocks. */
uint64_t ext2_shrink(uint64_t want)
{
	uint64_t got = 0;
	uint64_t f = spin_lock_irq(&ext2_all_spin);
	for (ext2_fs_t *fs = ext2_all; fs && got < want; fs = fs->next)
		got += bc_evict(fs, want - got);
	spin_unlock_irq(&ext2_all_spin, f);
	return got;
}

static void ext2_flushd(void)
{
	extern volatile uint64_t kernel_ticks;
	/* frequent enough that metadata stays well within a log */
	for (uint32_t n = 1;; n++) {
		task_sleep_until(kernel_ticks + TIMER_HZ);
		int due = n % EXT2_FLUSH_SECS == 0;
		mutex_lock(&ext2_all_lock);
		for (ext2_fs_t *fs = ext2_all; fs; fs = fs->next)
			if (due || (fs->jmap && fs->bc_meta > fs->jlen / 4)) ext2_flush(fs);
		mutex_unlock(&ext2_all_lock);
	}
}

/* ── mount ───────────────────────────────────────────────────── */

static void ext2_kill_sb(void *priv)
{
	ext2_fs_t *fs = priv;
	if (!fs) return;
	mutex_lock(&ext2_all_lock);
	uint64_t f = spin_lock_irq(&ext2_all_spin);
	for (ext2_fs_t **pp = &ext2_all; *pp; pp = &(*pp)->next)
		if (*pp == fs) { *pp = fs->next; break; }
	spin_unlock_irq(&ext2_all_spin, f);
	mutex_unlock(&ext2_all_lock);
	ext2_seal(fs);
	while (fs->lru_head) {
		bc_ent_t *e = fs->lru_head;
		lru_unlink(fs, e);
		bc_release(e);
	}
	for (int i = 0; i < EXT2_IHASH_SIZE; i++)
		while (fs->ihash[i]) {
			ext2_inode_priv_t *p = fs->ihash[i];
			fs->ihash[i] = p->hnext;
			kfree(p);
		}
	while (fs->ifree) {
		ext2_inode_priv_t *p = fs->ifree;
		fs->ifree = p->hnext;
		kfree(p);
	}
	if (fs->ra_buf) buddy_free(VIRT_TO_PHYS((uint64_t)fs->ra_buf), fs->ra_order);
	ext2_free_page(fs->jsb);
	ext2_free_page(fs->jbuf);
	kfree(fs->jmap);
	kfree(fs->jm);
	kfree(fs->pend);
	kfree(fs->hash);
	kfree(fs);
}

/* Find the log. Returns 0 with fs->jmap set, 1 if there is none we can use. */
static int jload(ext2_fs_t *fs, uint32_t inum)
{
	ext2_inode_t in;
	if (read_inode(fs, inum, &in) != 0) return 1;
	uint32_t blocks = (uint32_t)(ino_size(&in) / fs->block_size);
	if (!blocks) return 1;
	fs->jsb  = ext2_alloc_page();
	fs->jbuf = ext2_alloc_page();
	fs->jmap = kmalloc((size_t)blocks * sizeof(*fs->jmap));
	fs->jm   = kmalloc((size_t)blocks * sizeof(*fs->jm));
	if (!fs->jsb || !fs->jbuf || !fs->jmap || !fs->jm) return 1;
	for (uint32_t i = 0; i < blocks; i++)
		if (!(fs->jmap[i] = inode_map_block(fs, &in, i))) return 1;
	if (jread(fs, 0, fs->jsb)) return 1;
	uint8_t *j = fs->jsb;
	uint32_t type = be32(j + 4), incompat = 0;
	if (be32(j) != JBD_MAGIC || (type != JBD_SB_V1 && type != JBD_SB_V2)) return 1;
	if (be32(j + 0xC) != fs->block_size) return 1;
	if (type == JBD_SB_V2) {
		incompat = be32(j + 0x28);
		/* checksummed log formats: not something we can write */
		if (incompat & ~(JBD_IN_REVOKE | JBD_IN_64BIT)) return 1;
		memcpy(fs->juuid, j + 0x30, 16);
	}
	fs->jlen   = be32(j + 0x10);
	fs->jfirst = be32(j + 0x14);
	if (fs->jlen > blocks || !fs->jfirst || fs->jfirst >= fs->jlen) return 1;
	fs->jtag   = incompat & JBD_IN_64BIT ? 12 : 8;
	fs->jseq   = be32(j + JSB_SEQUENCE);
	fs->jhead  = fs->jfirst;
	return 0;
}

static inline uint32_t jnext(ext2_fs_t *fs, uint32_t pos, uint32_t n)
{
	pos += n;
	return pos >= fs->jlen ? pos - fs->jlen + fs->jfirst : pos;
}

/* Blocks a descriptor maps; with apply, write them home too. */
static int jdesc(ext2_fs_t *fs, const uint8_t *d, uint32_t pos, int apply, uint8_t *tmp)
{
	uint32_t off = 12, n = 0, bs = fs->block_size;
	while (off + fs->jtag <= bs) {
		uint32_t fl = (uint32_t)d[off + 6] << 8 | d[off + 7];
		uint64_t blk = be32(d + off);
		if (fs->jtag == 12) blk |= (uint64_t)be32(d + off + 8) << 32;
		off += fs->jtag;
		if (!(fl & JBD_FLAG_SAME)) off += 16;
		n++;
		if (apply) {
			if (jread(fs, jnext(fs, pos, n), tmp)) return -1;
			if (fl & JBD_FLAG_ESCAPE) put_be32(tmp, JBD_MAGIC);
			if (write_block(fs, blk, tmp)) return -1;
		}
		if (fl & JBD_FLAG_LAST) break;
	}
	return (int)n;
}

/* Bring the metadata to the last transaction that committed. Our own commits
 * have no revokes; one from another kernel's crash needs e2fsck. */
static int jrecover(ext2_fs_t *fs)
{
	uint32_t start = be32(fs->jsb + JSB_START), seq = be32(fs->jsb + JSB_SEQUENCE);
	uint32_t end = seq, n = 0;
	if (!start) return 0;
	if (start < fs->jfirst || start >= fs->jlen) return -1;
	uint8_t *d = fs->jbuf, *tmp = ext2_alloc_page();
	if (!tmp) return -1;
	for (int apply = 0; apply < 2; apply++) {
		uint32_t pos = start, s = seq;
		for (uint32_t steps = 0; steps < fs->jlen && (apply ? s != end : 1); steps++) {
			if (jread(fs, pos, d)) goto bad;
			if (be32(d) != JBD_MAGIC || be32(d + 8) != s) break;
			uint32_t type = be32(d + 4);
			if (type == JBD_DESCRIPTOR) {
				int c = jdesc(fs, d, pos, apply, tmp);
				if (c < 0 || (uint32_t)c >= fs->jlen - fs->jfirst) goto bad;
				pos = jnext(fs, pos, (uint32_t)c + 1);
				if (apply) n += (uint32_t)c;
			} else if (type == JBD_COMMIT) {
				pos = jnext(fs, pos, 1);
				s++;
				if (!apply) end = s;
			} else if (type == JBD_REVOKE) {
				kprintf(KERN_ERR "ext2: log holds revoke records, run e2fsck\n");
				goto bad;
			} else {
				break;
			}
		}
	}
	ext2_free_page(tmp);
	uint64_t *jm = fs->jmap;
	fs->jmap = NULL;           /* straight home, not into the log being replayed */
	ext2_flush(fs);
	fs->jmap = jm;
	if (block_flush_all()) return -1;
	/* the next commits must not be mistaken for what was just replayed */
	fs->jseq = end + 1;
	if (jsb_store(fs, 0, fs->jseq) || block_flush_all()) return -1;
	kprintf(KERN_INFO "ext2: log replayed, %u blocks in %u transactions\n", n, end - seq);
	return 0;
bad:
	ext2_free_page(tmp);
	return -1;
}

/* The superblock on disk after recovery; in-core counts come from it. */
static void sb_reload(ext2_fs_t *fs)
{
	uint32_t off = fs->block_size == 1024 ? 0 : 1024;
	bc_ent_t *e = bget(fs, fs->block_size == 1024 ? 1 : 0);
	if (!e) return;
	ext2_super_t *sb = (ext2_super_t *)(e->buf + off);
	fs->free_blocks = sb->s_free_blocks_count_lo;
	if (sb->s_feature_incompat & 0x80)
		fs->free_blocks |= (uint64_t)sb->s_free_blocks_count_hi << 32;
	fs->free_inodes = sb->s_free_inodes_count;
	fs->last_orphan = sb->s_last_orphan;
	bput(fs, e);
}

/* Unlinked while open and then the machine went down: finish the job. */
static void orphans_reap(ext2_fs_t *fs)
{
	mutex_lock(&fs->lock);
	uint32_t ino = fs->last_orphan, n = 0;
	fs->last_orphan = 0;
	fs->sb_dirty = 1;
	while (ino && n < fs->total_inodes) {
		ext2_inode_priv_t *p = iget(fs, ino);
		if (!p) break;
		ino = p->raw.i_dtime;
		p->raw.i_links_count = 0;
		p->raw.i_size_lo = p->raw.i_size_high = 0;
		ino_release(fs, p);
		n++;
	}
	mutex_unlock(&fs->lock);
	if (n) kprintf(KERN_INFO "ext2: %u orphan inodes released\n", n);
}

int ext2_mount(vfs_mount_t *mnt, void *data)
{
	static int flushd_up;
	block_device_t *bdev = data;
	if (!bdev) return -1;

	/* superblock at byte 1024, i.e. LBA 2 for 512-byte sectors */
	uint8_t *sb_buf = ext2_alloc_page();
	if (!sb_buf) return -1;

	/* read 2 sectors to cover byte 1024..2047 */
	if (block_read(bdev, 2, sb_buf, 2) != 0) {
		ext2_free_page(sb_buf);
		return -1;
	}

	ext2_super_t *sb = (ext2_super_t *)sb_buf;
	if (sb->s_magic != EXT2_MAGIC) {
		ext2_free_page(sb_buf);
		return -1;
	}

	if (sb->s_log_block_size > 2) {
		kprintf(KERN_ERR "ext2: block size not supported\n");
		ext2_free_page(sb_buf);
		return -1;
	}
	uint32_t bs = 1024u << sb->s_log_block_size;
	uint16_t isz = sb->s_rev_level > 0 ? sb->s_inode_size : 128;
	if (!sb->s_blocks_per_group || sb->s_blocks_per_group > bs * 8 ||
	    !sb->s_inodes_per_group || sb->s_inodes_per_group > bs * 8 ||
	    isz < 128 || isz > bs || (isz & (isz - 1)) || sb->s_desc_size > bs ||
	    !sb->s_inodes_count || !sb->s_blocks_count_lo ||
	    sb->s_first_data_block >= sb->s_blocks_count_lo) {
		kprintf(KERN_ERR "ext2: superblock makes no sense\n");
		ext2_free_page(sb_buf);
		return -1;
	}

	ext2_fs_t *fs = kmalloc(sizeof(*fs));
	if (!fs) { ext2_free_page(sb_buf); return -1; }
	memset(fs, 0, sizeof(*fs));
	fs->bdev              = bdev;
	fs->block_size        = bs;
	fs->sectors_per_block = bs / 512;
	fs->blocks_per_group  = sb->s_blocks_per_group;
	fs->inodes_per_group  = sb->s_inodes_per_group;
	fs->inode_size        = (sb->s_rev_level > 0) ? sb->s_inode_size : 128;
	fs->first_data_block  = sb->s_first_data_block;
	fs->first_ino         = sb->s_rev_level > 0 ? sb->s_first_ino : 11;
	fs->total_inodes      = sb->s_inodes_count;
	fs->total_blocks      = sb->s_blocks_count_lo;
	if (sb->s_feature_incompat & 0x80)   /* INCOMPAT_64BIT */
		fs->total_blocks |= (uint64_t)sb->s_blocks_count_hi << 32;
	fs->num_groups        = (uint32_t)((fs->total_blocks + sb->s_blocks_per_group - 1) / sb->s_blocks_per_group);
	fs->desc_size         = (sb->s_desc_size >= 32) ? sb->s_desc_size : 32;
	fs->free_blocks       = sb->s_free_blocks_count_lo;
	fs->reserved_blocks   = sb->s_r_blocks_count_lo;
	if (sb->s_feature_incompat & 0x80)
		fs->free_blocks |= (uint64_t)sb->s_free_blocks_count_hi << 32;
	fs->free_inodes       = sb->s_free_inodes_count;
	fs->cache_lock  = (spinlock_t)SPINLOCK_INIT;
	fs->vn_lock     = (spinlock_t)SPINLOCK_INIT;
	fs->lock        = (mutex_t)MUTEX_INIT;
	fs->flush_lock  = (mutex_t)MUTEX_INIT;
	fs->io_lock     = (mutex_t)MUTEX_INIT;
	fs->hash = kmalloc(sizeof(*fs->hash) << EXT2_HASH_BITS);
	if (!fs->hash) { kfree(fs); ext2_free_page(sb_buf); return -1; }
	memset(fs->hash, 0, sizeof(*fs->hash) << EXT2_HASH_BITS);
	fs->bc_max = buddy_free_pages() / 2;

	/* one contiguous scratch big enough for a full transfer; if the allocation
	 * fails, reads and writes fall back to 1 block a command. */
	int ra_order = 0;
	uint64_t ra_bytes = (uint64_t)EXT2_RA_MAX * fs->block_size;
	while ((4096ULL << ra_order) < ra_bytes) ra_order++;
	void *rp = buddy_alloc(ra_order);
	fs->ra_buf   = rp ? (uint8_t *)PHYS_TO_VIRT((uint64_t)rp) : NULL;
	fs->ra_order = ra_order;

	kprintf(KERN_DEBUG "ext2: bs=%u ino_sz=%u groups=%u desc=%u %s%s\n",
	        bs, fs->inode_size, fs->num_groups, fs->desc_size,
	        (sb->s_feature_incompat & 0x80) ? "64bit" : "32bit",
	        (sb->s_feature_compat & 0x4) ? " journal" : "");

	/* Checksums we would not update: writing would corrupt the filesystem. It is
	 * also what keeps a host OS's ext4 safe from a stray rw mount. */
	uint32_t ro_compat = sb->s_feature_ro_compat, incompat = sb->s_feature_incompat;
	uint32_t compat = sb->s_feature_compat, jinum = sb->s_journal_inum;
	if (bdev->ro || (ro_compat & (0x400 | 0x10)) || (incompat & ~(0x2u | 0x4u | 0x40u | 0x80u | 0x200u)) ||
	    fs->total_blocks * fs->sectors_per_block > bdev->size_blocks)
		mnt->flags |= MS_RDONLY;
	fs->rdonly = !!(mnt->flags & MS_RDONLY);
	fs->pend = kmalloc(EXT2_PEND_MAX * sizeof(*fs->pend));
	if (!fs->pend) { ext2_free_page(sb_buf); kfree(fs->hash); kfree(fs); return -1; }
	ext2_free_page(sb_buf);

	int have_log = 0;
	if ((compat & EXT2_FEAT_HAS_JOURNAL) && jinum)
		have_log = jload(fs, jinum) == 0;
	if (!have_log) {
		ext2_free_page(fs->jsb); ext2_free_page(fs->jbuf);
		kfree(fs->jmap); kfree(fs->jm);
		fs->jsb = fs->jbuf = NULL; fs->jmap = NULL; fs->jm = NULL;
		/* a log that exists but can't be used: writing past it would lie to e2fsck */
		if ((compat & EXT2_FEAT_HAS_JOURNAL) && jinum) { mnt->flags |= MS_RDONLY; fs->rdonly = 1; }
	}
	if (incompat & EXT2_IN_RECOVER) {
		if (fs->rdonly || !have_log) {
			/* reads see the state before the crash, consistent but stale */
			kprintf(KERN_WARN "ext2: journal needs recovery, not replayed\n");
			if (!fs->rdonly) { mnt->flags |= MS_RDONLY; fs->rdonly = 1; }
		} else {
			if (jrecover(fs) != 0) {
				kprintf(KERN_ERR "ext2: recovery failed, mounting read-only\n");
				mnt->flags |= MS_RDONLY;
				fs->rdonly = 1;
			}
		}
	}
	sb_reload(fs);
	if (fs->rdonly) {
		ext2_free_page(fs->jsb); ext2_free_page(fs->jbuf);
		kfree(fs->jmap); kfree(fs->jm);
		fs->jsb = fs->jbuf = NULL; fs->jmap = NULL; fs->jm = NULL;
	} else {
		sb_state(fs, 0);
		/* the recovery flag first: e2fsck refuses a live log without it */
		if (block_flush_all()) kprintf(KERN_ERR "ext2: flush failed at mount\n");
		if (fs->jmap && jsb_store(fs, fs->jfirst, fs->jseq)) {
			kfree(fs->jmap);
			fs->jmap = NULL;
		}
		char arg[16];
		if (cmdline_get("ranix.fscrash", arg, sizeof(arg))) {
			extern volatile uint64_t kernel_ticks;
			uint64_t secs = 0;
			for (char *c = arg; *c >= '0' && *c <= '9'; c++) secs = secs * 10 + (uint64_t)(*c - '0');
			fscrash_tick = kernel_ticks + secs * TIMER_HZ;
		}
		if (fs->jmap) kprintf(KERN_INFO "ext2: log active, %u blocks\n", fs->jlen);
		orphans_reap(fs);
	}

	/* read root inode */
	mutex_lock(&fs->lock);
	ext2_inode_priv_t *root_priv = iget(fs, EXT2_ROOT_INO);
	mutex_unlock(&fs->lock);
	if (!root_priv) {
		kprintf(KERN_ERR "ext2: cannot read root inode\n");
		ext2_kill_sb(fs);
		return -1;
	}

	vfs_inode_t *root_vi = ext2_vfs_inode(fs, root_priv);
	if (!root_vi) { ext2_kill_sb(fs); return -1; }

	vfs_dentry_t *root_dentry = vfs_dentry_create(NULL, "/", root_vi);
	if (!root_dentry) { ext2_release(root_vi); kfree(root_vi); ext2_kill_sb(fs); return -1; }

	uint64_t f = spin_lock_irq(&ext2_all_spin);
	fs->next = ext2_all;
	ext2_all = fs;
	spin_unlock_irq(&ext2_all_spin, f);
	if (!__atomic_exchange_n(&flushd_up, 1, __ATOMIC_ACQ_REL))
		proc_create("ext2_flush", ext2_flushd);

	mnt->priv    = fs;
	mnt->kill_sb = ext2_kill_sb;
	mnt->root    = root_dentry;
	if (bdev->ro) {
		mnt->flags  |= MS_RDONLY;
		mnt->ro_lock = 1;
	}
	kprintf(KERN_DEBUG "ext2: mounted (ino root=%u)\n", EXT2_ROOT_INO);
	return 0;
}

void ext2_register(void)
{
	extern void vfs_register_fs(const char *, int (*)(vfs_mount_t *, void *));
	vfs_register_fs("ext2", ext2_mount);
	vfs_register_fs("ext4", ext2_mount);   /* ext4 is ext2-compatible */
}
