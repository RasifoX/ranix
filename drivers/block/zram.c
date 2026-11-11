/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* zram: /dev/zram0, a disk whose pages live compressed in RAM - the first home
 * of swap, where a page that compresses well costs a fraction of itself.
 *
 * Empty at boot (size 0). Root sizes it by writing bytes to
 * /sys/block/zram0/disksize (K, M, G suffixes, as Linux); a size change empties
 * it and is refused while it is open. Each 4 KB page is one of:
 *   empty  never written, or discarded by swap: reads as zeros, costs nothing
 *   same   one 8-byte word repeated (mostly zero pages): the word is all we keep
 *   lz     compressed by kernel/lib/lzr.c into a heap block of up to 1 KB
 *   raw    anything that would not fit in 1 KB: the page as it is
 * /sys/block/zram0/mm_stat shows the sizes in Linux's column order. */
#include <kernel/block.h>
#include <kernel/mm/swap.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/vmalloc.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/lib/lzr.h>
#include <kernel/lib/string.h>
#include <kernel/errno.h>
#include <kernel/kprintf.h>
#include <asm/mmu.h>

#define SECTORS   (PAGE_SIZE / BLOCK_SIZE)
#define LZ_MAX    1000      /* + the heap header fits the 1 KB class, three to a page */

enum { ZP_EMPTY, ZP_SAME, ZP_LZ, ZP_RAW };

typedef struct {
	union {
		void    *data;    /* lz: heap block; raw: the page */
		uint64_t word;    /* same: the repeated word */
	};
	uint16_t len;         /* lz: compressed bytes */
	uint8_t  kind;
} zpage_t;

static block_device_t zram_dev;
static zpage_t   *zpages;
static uint64_t   znpages;
static spinlock_t zlock = SPINLOCK_INIT;            /* zpages[] entries and the counters */
static uint64_t   z_orig, z_compr, z_used, z_used_max, z_same, z_raw;

/* scratch for the one transfer at a time the block layer lets through */
static uint8_t  zcomp[PAGE_SIZE];
static uint8_t  zpage_buf[PAGE_SIZE];
static uint16_t ztable[LZR_TABLE_SIZE];

int zram_owns(block_device_t *dev) { return dev == &zram_dev; }

/* Heap memory a stored page costs: its slab class, or the whole raw page. */
static uint64_t zcost(const zpage_t *z)
{
	if (z->kind == ZP_RAW) return PAGE_SIZE;
	if (z->kind != ZP_LZ) return 0;
	uint64_t c = 32;
	while (c < (uint64_t)z->len + 16) c <<= 1;
	return c;
}

/* Under zlock: count a page in or out of the totals. */
static void zaccount(const zpage_t *z, int sign)
{
	if (z->kind == ZP_EMPTY) return;
	uint64_t compr = z->kind == ZP_LZ ? z->len : z->kind == ZP_RAW ? PAGE_SIZE : 0;
	if (sign > 0) {
		z_orig += PAGE_SIZE; z_compr += compr; z_used += zcost(z);
		if (z->kind == ZP_SAME) z_same++;
		if (z->kind == ZP_RAW) z_raw++;
		if (z_used > z_used_max) z_used_max = z_used;
	} else {
		z_orig -= PAGE_SIZE; z_compr -= compr; z_used -= zcost(z);
		if (z->kind == ZP_SAME) z_same--;
		if (z->kind == ZP_RAW) z_raw--;
	}
}

static void zrelease(zpage_t *z)
{
	if (z->kind == ZP_LZ) kfree(z->data);
	if (z->kind == ZP_RAW) buddy_free(VIRT_TO_PHYS(z->data), 0);
	z->kind = ZP_EMPTY;
}

/* Swap gave a slot up: its page is not needed any more. */
void zram_discard(block_device_t *dev, uint64_t lba)
{
	uint64_t idx = lba / SECTORS;
	if (dev != &zram_dev) return;
	uint64_t f = spin_lock_irq(&zlock);
	zpage_t old = { .kind = ZP_EMPTY };
	if (idx < znpages) { old = zpages[idx]; zaccount(&old, -1); zpages[idx].kind = ZP_EMPTY; }
	spin_unlock_irq(&zlock, f);
	zrelease(&old);
}

static int same_filled(const uint8_t *src, uint64_t *word)
{
	const uint64_t *w = (const uint64_t *)src;
	for (unsigned i = 1; i < PAGE_SIZE / 8; i++) if (w[i] != w[0]) return 0;
	*word = w[0];
	return 1;
}

/* Keep page idx as src says, in the smallest form that holds it. */
static int zstore(uint64_t idx, const uint8_t *src)
{
	zpage_t z = { .kind = ZP_SAME };
	if (!same_filled(src, &z.word)) {
		long n = lzr_compress(src, PAGE_SIZE, zcomp, LZ_MAX, ztable);
		if (n > 0 && (z.data = kmalloc((size_t)n)) != NULL) {
			memcpy(z.data, zcomp, (size_t)n);
			z.len = (uint16_t)n;
			z.kind = ZP_LZ;
		} else {
			void *pg = buddy_alloc(0);
			if (!pg) return -ENOMEM;
			z.data = (void *)PHYS_TO_VIRT(pg);
			memcpy(z.data, src, PAGE_SIZE);
			z.kind = ZP_RAW;
		}
	}
	uint64_t f = spin_lock_irq(&zlock);
	zpage_t old = zpages[idx];
	zaccount(&old, -1);
	zpages[idx] = z;
	zaccount(&z, 1);
	spin_unlock_irq(&zlock, f);
	zrelease(&old);
	return 0;
}

/* Page idx into dst. Reads under zlock: a discard cannot free it meanwhile. */
static int zload(uint64_t idx, uint8_t *dst)
{
	int r = 0;
	uint64_t f = spin_lock_irq(&zlock);
	zpage_t *z = &zpages[idx];
	switch (z->kind) {
	case ZP_EMPTY: memset(dst, 0, PAGE_SIZE); break;
	case ZP_SAME:  for (unsigned i = 0; i < PAGE_SIZE / 8; i++) ((uint64_t *)dst)[i] = z->word; break;
	case ZP_RAW:   memcpy(dst, z->data, PAGE_SIZE); break;
	default:       r = lzr_decompress(z->data, z->len, dst, PAGE_SIZE) == PAGE_SIZE ? 0 : -EIO; break;
	}
	spin_unlock_irq(&zlock, f);
	return r;
}

/* Block driver entries. Whole pages go straight through; a part of a page is
 * read, changed and stored again. */
static int zram_read(block_device_t *dev, uint64_t lba, void *buf, uint64_t count)
{
	(void)dev;
	uint8_t *out = buf;
	while (count > 0) {
		uint64_t idx = lba / SECTORS, off = lba % SECTORS;
		uint64_t n = SECTORS - off < count ? SECTORS - off : count;
		if (idx >= znpages) return -EIO;
		if (n == SECTORS) {
			if (zload(idx, out)) return -EIO;
		} else {
			if (zload(idx, zpage_buf)) return -EIO;
			memcpy(out, zpage_buf + off * BLOCK_SIZE, n * BLOCK_SIZE);
		}
		out += n * BLOCK_SIZE; lba += n; count -= n;
	}
	return 0;
}

static int zram_write(block_device_t *dev, uint64_t lba, const void *buf, uint64_t count)
{
	(void)dev;
	const uint8_t *in = buf;
	while (count > 0) {
		uint64_t idx = lba / SECTORS, off = lba % SECTORS;
		uint64_t n = SECTORS - off < count ? SECTORS - off : count;
		if (idx >= znpages) return -EIO;
		const uint8_t *src = in;
		if (n != SECTORS) {
			if (zload(idx, zpage_buf)) return -EIO;
			memcpy(zpage_buf + off * BLOCK_SIZE, in, n * BLOCK_SIZE);
			src = zpage_buf;
		}
		if (zstore(idx, src)) return -ENOMEM;
		in += n * BLOCK_SIZE; lba += n; count -= n;
	}
	return 0;
}

/* /sys/block/zram0/disksize. 0 empties the device. */
int zram_set_disksize(uint64_t bytes)
{
	uint64_t pages = bytes / PAGE_SIZE;
	if (zram_dev.users > 0) return -EBUSY;
	zpage_t *nz = NULL;
	if (pages && !(nz = vzalloc(pages * sizeof(zpage_t)))) return -ENOMEM;
	mutex_lock(&zram_dev.lock);
	uint64_t f = spin_lock_irq(&zlock);
	zpage_t *old = zpages;
	uint64_t oldn = znpages;
	zpages = nz;
	znpages = pages;
	zram_dev.size_blocks = pages * SECTORS;
	z_orig = z_compr = z_used = z_used_max = z_same = z_raw = 0;
	spin_unlock_irq(&zlock, f);
	mutex_unlock(&zram_dev.lock);
	for (uint64_t i = 0; i < oldn; i++) zrelease(&old[i]);
	vfree(old);
	if (pages) kprintf(KERN_INFO "zram: zram0 %lu kB\n", (unsigned long)(bytes / 1024));
	return 0;
}

uint64_t zram_disksize(void) { return znpages * PAGE_SIZE; }

/* orig_data_size compr_data_size mem_used_total mem_limit mem_used_max
 * same_pages pages_compacted huge_pages, in bytes and pages as Linux */
void zram_mm_stat(uint64_t v[8])
{
	uint64_t f = spin_lock_irq(&zlock);
	v[0] = z_orig; v[1] = z_compr; v[2] = z_used; v[3] = 0;
	v[4] = z_used_max; v[5] = z_same; v[6] = 0; v[7] = z_raw;
	spin_unlock_irq(&zlock, f);
}

void zram_init(void)
{
	memcpy(zram_dev.name, "zram0", 6);
	zram_dev.read = zram_read;
	zram_dev.write = zram_write;
	if (block_device_register(&zram_dev) != 0) return;
	zram_dev.ro = 0;   /* RAM, not a disk: nothing on it to protect from a write */
}
