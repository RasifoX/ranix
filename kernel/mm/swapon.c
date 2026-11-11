/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* swapon(2) and swapoff(2), and what they show in /proc.
 *
 * An area is a block device (a partition, /dev/zram0) or a regular file on
 * ext2/ext4, prepared by mkswap: page 0 carries the header Linux defines
 * (version 1, the last page, a bad-page list, and "SWAPSPACE2" at the end of
 * the page), so busybox mkswap/swapon/swapoff work unchanged. For a file each
 * page is turned into disk sectors once, here, and swap I/O then goes to the
 * disk directly, past the filesystem's cache; a page split across distant
 * blocks, or a hole, refuses the file. Both calls need CAP_SYS_ADMIN.
 *
 * swapoff stops new slots and reads every page of the area back: each address
 * space is walked for entries naming it. It refuses (ENOMEM) when RAM and the
 * other areas could not hold them. */
#include "swap_area.h"
#include <kernel/mm/pmm.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/vmalloc.h>
#include <kernel/proc.h>
#include <kernel/capabilities.h>
#include <kernel/errno.h>
#include <kernel/kprintf.h>
#include <kernel/config.h>
#include <kernel/lib/string.h>
#include <asm/mmu.h>

#ifdef ARCH_USER_VA_END
#define USER_END ARCH_USER_VA_END
#else
#define USER_END (1UL << 39)
#endif

#define SWAP_FLAG_PREFER     0x8000
#define SWAP_FLAG_PRIO_MASK  0x7fff
#define HDR_VERSION   1024      /* offsets in page 0 (Linux union swap_header) */
#define HDR_LAST_PAGE 1028
#define HDR_NR_BAD    1032
#define HDR_BADPAGES  1536
#define HDR_MAX_BAD   ((PAGE_SIZE - 10 - HDR_BADPAGES) / 4)

static mutex_t swapon_lock = MUTEX_INIT;   /* one swapon/swapoff at a time */
static int     auto_prio = -1;             /* areas without a priority: -2, -3, ... */

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* The sector of each page of a file. A hole refuses the file (-EINVAL); a
 * page whose blocks lie apart on the disk is left unused, as a bad page. */
static int file_map(swap_area_t *a, vfs_file_t *f)
{
	a->lba = vzalloc(a->pages * sizeof(uint64_t));
	if (!a->lba) return -ENOMEM;
	for (uint64_t s = 0; s < a->pages; s++) {
		void *dev = NULL;
		uint64_t first = f->inode->fops->bmap(f, s * PAGE_SIZE, &dev);
		if (!first || dev != a->bdev) return -EINVAL;
		int whole = 1;
		for (uint64_t k = 1; k < PAGE_SIZE / BLOCK_SIZE; k++) {
			uint64_t at = f->inode->fops->bmap(f, s * PAGE_SIZE + k * BLOCK_SIZE, &dev);
			if (!at) return -EINVAL;
			if (at != first + k) whole = 0;
		}
		if (!whole && s == 0) return -EINVAL;   /* the header must read as one page */
		if (!whole && !a->count[s]) { a->count[s] = 0xFFFFFFFFu; a->inuse++; a->bad++; }
		a->lba[s] = first;
	}
	return 0;
}

/* Where the area lives and how big it may be. */
static int area_device(swap_area_t *a, vfs_file_t *f, uint64_t *bytes)
{
	vfs_inode_t *in = f->inode;
	if (S_ISBLK(in->mode) && block_is_device(in->priv)) {
		a->bdev = in->priv;
		*bytes = a->bdev->size_blocks * BLOCK_SIZE;
		return 0;
	}
	if (!S_ISREG(in->mode) || !in->fops || !in->fops->bmap) return -EINVAL;
	void *dev = NULL;
	in->fops->bmap(f, 0, &dev);
	if (!dev) return -EINVAL;
	a->bdev = dev;
	a->is_file = 1;
	*bytes = (uint64_t)in->size;
#if IS_ENABLED(CONFIG_FS_EXT2)
	ext2_sync_all();   /* what mkswap left in the cache must reach the disk before we read it */
#endif
	return 0;
}

/* Read and check the mkswap header; sets a->pages and marks bad pages. */
static int area_header(swap_area_t *a, vfs_file_t *f, uint64_t bytes)
{
	uint8_t *h = (uint8_t *)PHYS_TO_VIRT((uint64_t)buddy_alloc(0));
	if ((uint64_t)h == PHYS_TO_VIRT(0)) return -ENOMEM;
	int r = -EINVAL;
	uint64_t lba = 0;
	if (a->is_file) { void *d; lba = f->inode->fops->bmap(f, 0, &d); }
	if ((a->is_file && !lba) || block_read(a->bdev, lba, h, PAGE_SIZE / BLOCK_SIZE) != 0) goto out;
	if (memcmp(h + PAGE_SIZE - 10, "SWAPSPACE2", 10) != 0 || rd32(h + HDR_VERSION) != 1) goto out;
	uint64_t pages = (uint64_t)rd32(h + HDR_LAST_PAGE) + 1;
	if (pages > bytes / PAGE_SIZE) pages = bytes / PAGE_SIZE;
	uint32_t nbad = rd32(h + HDR_NR_BAD);
	if (pages < 10 || nbad > HDR_MAX_BAD) goto out;
	a->pages = pages;
	r = -ENOMEM;
	a->count = vzalloc(pages * sizeof(uint32_t));
	a->cache = vzalloc(pages * sizeof(uint64_t));
	if (!a->count || !a->cache) goto out;
	for (uint32_t i = 0; i < nbad; i++) {
		uint32_t b = rd32(h + HDR_BADPAGES + 4 * i);
		if (b > 0 && b < pages && !a->count[b]) { a->count[b] = 0xFFFFFFFFu; a->inuse++; a->bad++; }
	}
	r = a->is_file ? file_map(a, f) : 0;
out:
	buddy_free(VIRT_TO_PHYS(h), 0);
	return r;
}

static void area_free_arrays(swap_area_t *a)
{
	vfree(a->count); vfree(a->cache); vfree(a->lba);
	a->count = NULL; a->cache = NULL; a->lba = NULL;
}

/* Is this file or disk already an area? Under swapon_lock. */
static int area_busy(vfs_file_t *f)
{
	for (int i = 0; i < SWAP_AREAS; i++) {
		swap_area_t *a = &swap_areas[i];
		if (!a->on) continue;
		if (a->file->inode == f->inode) return 1;
		if (!a->is_file && S_ISBLK(f->inode->mode) && a->bdev == f->inode->priv) return 1;
	}
	return 0;
}

static long area_open(vfs_file_t *f, const char *kpath, int flags)
{
	if (area_busy(f)) return -EBUSY;
	int idx = -1;
	for (int i = 0; i < SWAP_AREAS && idx < 0; i++) if (!swap_areas[i].on && !swap_areas[i].count) idx = i;
	if (idx < 0) return -EPERM;   /* Linux: too many areas */
	swap_area_t n;
	memset(&n, 0, sizeof(n));
	uint64_t bytes = 0;
	long r = area_device(&n, f, &bytes);
	if (!r && n.bdev->ro) r = -EROFS;
	if (!r) r = area_header(&n, f, bytes);
	if (r) { area_free_arrays(&n); return r; }
	n.file = f;
	n.prio = (flags & SWAP_FLAG_PREFER) ? (flags & SWAP_FLAG_PRIO_MASK) : --auto_prio;
	n.next = 1;
	n.inuse++;                         /* the header page is never a slot */
	n.on = 1;
	size_t pl = strlen(kpath);
	if (pl >= sizeof(n.path)) pl = sizeof(n.path) - 1;
	memcpy(n.path, kpath, pl);
	f->inode->swapfile = 1;   /* writes, truncate, fallocate: ETXTBSY until swapoff */
	uint64_t fl = spin_lock_irq(&swap_lock);
	swap_areas[idx] = n;
	spin_unlock_irq(&swap_lock, fl);
	swap_kswapd_start();
	kprintf(KERN_INFO "swap: %s on, %lu kB, priority %d\n", n.path,
	        (unsigned long)((n.pages - n.inuse) * (PAGE_SIZE / 1024)), n.prio);
	return 0;
}

/* The file at kpath, opened for the kernel's own use. */
static vfs_file_t *open_kernel(const char *kpath, long *err)
{
	process_t *p = task_current();
	int fd = vfs_open(kpath, O_RDONLY, 0);   /* swap I/O goes to the disk, not through the file */
	if (fd < 0) { *err = fd == -1 ? -ENOENT : fd; return NULL; }
	vfs_file_t *f = fget(p->fds, fd);
	vfs_close(fd);
	if (!f) *err = -EBADF;
	return f;
}

long swap_on(const char *kpath, int flags)
{
	if (!capable_task(task_current(), CAP_SYS_ADMIN)) return -EPERM;
	long r = 0;
	vfs_file_t *f = open_kernel(kpath, &r);
	if (!f) return r;
	mutex_lock(&swapon_lock);
	r = area_open(f, kpath, flags);
	mutex_unlock(&swapon_lock);
	if (r) fput(f);
	return r;
}

/* Read back every page of area idx that vs still has out. */
static int unuse_space(vm_space_t *vs, unsigned idx)
{
	uint64_t va = 0;
	for (;;) {
		uint64_t e = 0, at = 0, *pte;
		int budget = 4096;
		uint64_t lf = spin_lock_irq(&vs->fault_lock);
		while (budget-- > 0 && (pte = arch_pte_next(vs, &va, USER_END))) {
			if (swp_pte_is(*pte) && swp_area(*pte) == idx) { e = *pte; at = va; break; }
			va += PAGE_SIZE;
		}
		spin_unlock_irq(&vs->fault_lock, lf);
		if (va >= USER_END) return 0;
		if (fatal_signal_pending(task_current())) return -EINTR;
		/* a page that cannot come in (PROT_NONE, no memory) stays: swapoff ends EBUSY */
		if (e && swap_fault(vs, at, e, 0) != 0) va = at + PAGE_SIZE;
	}
}

/* Empty area idx: every page table, then the cache. 0 when no slot is used. */
static long area_drain(swap_area_t *a, unsigned idx)
{
	for (int pass = 0; pass < 4; pass++) {
		int n = task_count();
		for (int i = 0; i < n; i++) {
			vm_space_t *vs = proc_get_vm(i);
			if (!vs) continue;
			int r = unuse_space(vs, idx);
			vm_destroy(vs);
			if (r) return r;
		}
		if (a->inuse == 1 + a->bad) return 0;   /* the header and bad pages only */
	}
	return -EBUSY;
}

/* Could RAM and the other areas hold what area a has out? */
static int area_fits(swap_area_t *a)
{
	uint64_t need = a->inuse, room = buddy_free_pages();
	for (int i = 0; i < SWAP_AREAS; i++) {
		swap_area_t *o = &swap_areas[i];
		if (o != a && o->on && !o->draining) room += o->pages - o->inuse;
	}
	return need <= room;
}

static swap_area_t *area_find(vfs_file_t *f)
{
	for (int i = 0; i < SWAP_AREAS; i++) {
		swap_area_t *a = &swap_areas[i];
		if (a->on && (a->file->inode == f->inode ||
		              (!a->is_file && S_ISBLK(f->inode->mode) && a->bdev == f->inode->priv)))
			return a;
	}
	return NULL;
}

long swap_off(const char *kpath)
{
	if (!capable_task(task_current(), CAP_SYS_ADMIN)) return -EPERM;
	long r = 0;
	vfs_file_t *f = open_kernel(kpath, &r);
	if (!f) return r;
	mutex_lock(&swapon_lock);
	swap_area_t *a = area_find(f);
	fput(f);
	if (!a) { mutex_unlock(&swapon_lock); return -EINVAL; }
	if (!area_fits(a)) { mutex_unlock(&swapon_lock); return -ENOMEM; }
	a->draining = 1;
	r = area_drain(a, (unsigned)(a - swap_areas));
	uint64_t fl = spin_lock_irq(&swap_lock);
	if (!r && a->inuse != 1 + a->bad) r = -EBUSY;   /* a fork copied an entry at the last moment */
	if (r) {
		a->draining = 0;
		spin_unlock_irq(&swap_lock, fl);
		mutex_unlock(&swapon_lock);
		return r;
	}
	swap_area_t old = *a;
	memset(a, 0, sizeof(*a));
	spin_unlock_irq(&swap_lock, fl);
	area_free_arrays(&old);
	old.file->inode->swapfile = 0;
	fput(old.file);
	mutex_unlock(&swapon_lock);
	kprintf(KERN_INFO "swap: %s off\n", old.path);
	return 0;
}

/* ── what /proc shows ─────────────────────────────────────────────────── */

void swap_totals(uint64_t *total, uint64_t *free, uint64_t *cached)
{
	uint64_t t = 0, u = 0;
	uint64_t f = spin_lock_irq(&swap_lock);
	for (int i = 0; i < SWAP_AREAS; i++) {
		swap_area_t *a = &swap_areas[i];
		if (!a->on) continue;
		t += a->pages - 1 - a->bad;
		u += a->inuse - 1 - a->bad;
	}
	*cached = swap_nr_cached * PAGE_SIZE;
	spin_unlock_irq(&swap_lock, f);
	*total = t * PAGE_SIZE;
	*free = (t - u) * PAGE_SIZE;
}

void swap_counts(uint64_t *pswpin, uint64_t *pswpout)
{
	*pswpin = swap_nr_in;
	*pswpout = swap_nr_out;
}

static int put_num(char *o, int64_t v)
{
	char t[24]; int i = 0, n = 0;
	uint64_t u = v < 0 ? (uint64_t)-v : (uint64_t)v;
	if (v < 0) o[n++] = '-';
	do { t[i++] = (char)('0' + u % 10); u /= 10; } while (u);
	while (i) o[n++] = t[--i];
	return n;
}

/* Linux layout: Filename, Type, Size and Used in kB, Priority. */
int swap_proc_swaps(char *buf, size_t sz)
{
	const char *hdr = "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n";
	size_t n = strlen(hdr);
	if (n >= sz) return 0;
	memcpy(buf, hdr, n);
	uint64_t f = spin_lock_irq(&swap_lock);
	for (int i = 0; i < SWAP_AREAS && n + 256 < sz; i++) {
		swap_area_t *a = &swap_areas[i];
		if (!a->on) continue;
		size_t pl = strlen(a->path);
		memcpy(buf + n, a->path, pl); n += pl;
		do buf[n++] = ' '; while (++pl < 40);   /* the name in 40 columns */
		const char *type = a->is_file ? "file\t\t" : "partition\t";
		memcpy(buf + n, type, strlen(type)); n += strlen(type);
		n += put_num(buf + n, (int64_t)((a->pages - 1 - a->bad) * (PAGE_SIZE / 1024))); buf[n++] = '\t'; buf[n++] = '\t';
		n += put_num(buf + n, (int64_t)((a->inuse - 1 - a->bad) * (PAGE_SIZE / 1024))); buf[n++] = '\t'; buf[n++] = '\t';
		n += put_num(buf + n, a->prio); buf[n++] = '\n';
	}
	spin_unlock_irq(&swap_lock, f);
	return (int)n;
}

uint64_t swap_vm_bytes(vm_space_t *vs)
{
	uint64_t n = 0, va = 0, *pte;
	while (va < USER_END) {
		int budget = 4096;
		uint64_t lf = spin_lock_irq(&vs->fault_lock);
		while (budget-- > 0 && (pte = arch_pte_next(vs, &va, USER_END))) {
			if (swp_pte_is(*pte)) n++;
			va += PAGE_SIZE;
		}
		spin_unlock_irq(&vs->fault_lock, lf);
	}
	return n * PAGE_SIZE;
}
