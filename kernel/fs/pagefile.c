/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/kprintf.h>
#include <kernel/fs/pagefile.h>
#include <kernel/hal.h>
#include <kernel/proc.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/pmm.h>
#include <kernel/lib/string.h>
#include <kernel/cgroup.h>
#include <asm/mmu.h>
#include <kernel/errno.h>

#define FANOUT_BITS 9
#define FANOUT      (1U << FANOUT_BITS)
#define MAX_SIZE    (1ULL << 47)

/* root is a data page at height 0 and an index page above it, so a one-page file
 * costs one page. Index pages count against the cgroup, since sparse writes could
 * otherwise pin unaccounted memory, but not against limit, which is file data.
 *
 * busy serializes everything that changes the file. It sleeps, because copying
 * from user memory can fault. lock only guards the index against readers, who
 * take a page reference under it and copy outside it. */
struct pagefile {
	uint64_t      root;
	int           height;
	int           busy;
	uint64_t      charged;      /* with the cgroup */
	uint64_t      data_bytes;   /* with limit */
	void         *cg;
	const pagefile_limit_t *limit;
	void         *limit_ctx;
	spinlock_t    lock;
	wait_queue_t  waiters;
};

static uint8_t zeroes[PAGE_SIZE];

static void pf_enter(pagefile_t *pf)
{
	uint64_t f = spin_lock_irq(&pf->lock);
	while (pf->busy) {
		sleep_on_locked(&pf->waiters, &pf->lock, f);
		f = spin_lock_irq(&pf->lock);
	}
	pf->busy = 1;
	spin_unlock_irq(&pf->lock, f);
}

static void pf_leave(pagefile_t *pf)
{
	uint64_t f = spin_lock_irq(&pf->lock);
	pf->busy = 0;
	wake_up(&pf->waiters);
	spin_unlock_irq(&pf->lock, f);
}

static uint64_t *slots(uint64_t pa)
{
	return (uint64_t *)PHYS_TO_VIRT(pa);
}

static uint64_t pf_find(pagefile_t *pf, uint64_t idx)
{
	if (idx >> (FANOUT_BITS * pf->height)) return 0;
	uint64_t e = pf->root;
	for (int l = pf->height; l > 0 && e; l--)
		e = slots(e)[(idx >> (FANOUT_BITS * (l - 1))) & (FANOUT - 1)];
	return e;
}

static void pf_uncharge(pagefile_t *pf, uint64_t n, uint64_t data)
{
	if (pf->limit && data) pf->limit->uncharge(pf->limit_ctx, data);
	pf->data_bytes -= data;
	if (!n) return;
	cgroup_file_uncharge(pf->cg, (long)n);
	pf->charged -= n;
}

/* a zeroed, charged page; 0 with *err set when it cannot be had */
static uint64_t pf_new_page(pagefile_t *pf, int *err, int is_data)
{
	uint64_t data = is_data ? PAGE_SIZE : 0;
	*err = -28; /* ENOSPC */
	if (pf->limit && data && pf->limit->charge(pf->limit_ctx, data) != 0) return 0;
	pf->data_bytes += data;
	if (cgroup_charge_file(&pf->cg, (long)pf->charged, PAGE_SIZE) != 0) {
		pf_uncharge(pf, 0, data);
		return 0;
	}
	pf->charged += PAGE_SIZE;
	uint64_t pa = (uint64_t)buddy_alloc(0);
	if (!pa) { pf_uncharge(pf, PAGE_SIZE, data); return 0; }
	memset((void *)PHYS_TO_VIRT(pa), 0, PAGE_SIZE);
	return pa;
}

static void pf_publish(pagefile_t *pf, uint64_t *slot, uint64_t pa)
{
	uint64_t f = spin_lock_irq(&pf->lock);
	*slot = pa;
	spin_unlock_irq(&pf->lock, f);
}

/* Index pages left behind by a failure stay in the tree and go with it. */
static uint64_t pf_get_page(pagefile_t *pf, uint64_t idx, int *err)
{
	while (idx >> (FANOUT_BITS * pf->height)) {
		uint64_t n = 0;
		if (pf->root) {
			n = pf_new_page(pf, err, 0);
			if (!n) return 0;
			slots(n)[0] = pf->root;
		}
		uint64_t f = spin_lock_irq(&pf->lock);
		pf->root = n;
		pf->height++;
		spin_unlock_irq(&pf->lock, f);
	}

	uint64_t *slot = &pf->root;
	for (int l = pf->height; l > 0; l--) {
		if (!*slot) {
			uint64_t n = pf_new_page(pf, err, 0);
			if (!n) return 0;
			pf_publish(pf, slot, n);
		}
		slot = &slots(*slot)[(idx >> (FANOUT_BITS * (l - 1))) & (FANOUT - 1)];
	}
	if (!*slot) {
		uint64_t n = pf_new_page(pf, err, 1);
		if (!n) return 0;
		pf_publish(pf, slot, n);
	}
	return *slot;
}

/* Bytes gone from the tree at or past page first. A data page someone still maps
 * stays in place, zeroed: the mapping must keep seeing the file if it grows back.
 * With all set everything goes and the result counts only what reached the
 * allocator, since a mapped page lives on and is paid for until it is unmapped. */
static uint64_t pf_prune(uint64_t *slot, int level, uint64_t base, uint64_t first, int all,
                         uint64_t *data)
{
	uint64_t e = *slot;
	if (!e) return 0;
	if (level == 0) {
		if (!all && paddr_to_page(e)->refcount != 1) {
			memset((void *)PHYS_TO_VIRT(e), 0, PAGE_SIZE);
			return 0;
		}
		*slot = 0;
		*data += PAGE_SIZE;
		return pmm_free((void *)e) || !all ? PAGE_SIZE : 0;
	}

	uint64_t span = 1ULL << (FANOUT_BITS * (level - 1)), gone = 0;
	uint64_t *n = slots(e);
	int empty = 1;
	for (uint32_t i = 0; i < FANOUT; i++) {
		if (n[i] && base + (i + 1) * span > first)
			gone += pf_prune(&n[i], level - 1, base + i * span, first, all, data);
		if (n[i]) empty = 0;
	}
	if (empty) {
		*slot = 0;
		pmm_free((void *)e);
		gone += PAGE_SIZE;
	}
	return gone;
}

/* a store through a mapping can reach past EOF inside the last page */
static void pf_clear_tail(pagefile_t *pf, uint64_t size)
{
	uint32_t off = (uint32_t)(size & (PAGE_SIZE - 1));
	if (!off) return;
	uint64_t pa = pf_find(pf, size >> PAGE_SHIFT);
	if (pa) memset((uint8_t *)PHYS_TO_VIRT(pa) + off, 0, PAGE_SIZE - off);
}

pagefile_t *pagefile_new(const pagefile_limit_t *limit, void *ctx)
{
	pagefile_t *pf = kmalloc(sizeof(*pf));
	if (!pf) return NULL;
	memset(pf, 0, sizeof(*pf));
	pf->limit = limit;
	pf->limit_ctx = ctx;
	pf->lock = (spinlock_t)SPINLOCK_INIT;
	wait_queue_init(&pf->waiters);
	return pf;
}

void pagefile_evict(pagefile_t *pf)
{
	if (pf->height == 0xDEAD) {             /* freed already: say by whom it came again */
		kprintf(KERN_ERR "pagefile: %p evicted twice, via %p %p %p\n", pf, __builtin_return_address(0),
		        __builtin_return_address(1), __builtin_return_address(2));
		return;
	}
	uint64_t data = 0, freed = pf_prune(&pf->root, pf->height, 0, 0, 1, &data);
	if (pf->limit) pf->limit->uncharge(pf->limit_ctx, pf->data_bytes);
	cgroup_file_release(pf->cg, (long)freed);
	pf->height = 0xDEAD;
	kfree(pf);
}

ssize_t pagefile_read(vfs_file_t *f, void *buf, size_t len)
{
	pagefile_t *pf = f->inode->priv;
	if (!pf) return 0;
	if (f->pos < 0) return -EINVAL;
	uint64_t pos = (uint64_t)f->pos;
	size_t done = 0;

	while (done < len) {
		uint64_t l = spin_lock_irq(&pf->lock);
		uint64_t size = f->inode->size;
		if (pos >= size) { spin_unlock_irq(&pf->lock, l); break; }
		uint64_t pa = pf_find(pf, pos >> PAGE_SHIFT);
		if (pa) pmm_hold((void *)pa);
		spin_unlock_irq(&pf->lock, l);

		uint32_t off = (uint32_t)(pos & (PAGE_SIZE - 1));
		size_t n = PAGE_SIZE - off;
		if (n > len - done) n = len - done;
		if (n > size - pos) n = (size_t)(size - pos);
		const uint8_t *src = pa ? (uint8_t *)PHYS_TO_VIRT(pa) + off : zeroes;
		long rc = copy_fop_to_user((uint8_t *)buf + done, src, n);
		if (pa) pmm_free((void *)pa);
		if (rc != 0) {
			if (!done) return -EFAULT;
			break;
		}
		pos  += n;
		done += n;
	}
	f->pos = (off_t)pos;
	return (ssize_t)done;
}

ssize_t pagefile_write(vfs_file_t *f, const void *buf, size_t len)
{
	vfs_inode_t *in = f->inode;
	pagefile_t *pf = in->priv;
	if (!pf) return -1;
	if (!len) return 0;

	pf_enter(pf);
	int err = 0;
	size_t done = 0;
	uint64_t pos = (f->flags & O_APPEND) ? in->size : (uint64_t)f->pos;
	if (f->pos < 0 && !(f->flags & O_APPEND)) { err = -22; goto out; }
	if (pos >= MAX_SIZE || len > MAX_SIZE - pos) { err = -27; goto out; } /* EFBIG */
	if (pos > in->size) pf_clear_tail(pf, in->size);

	while (done < len) {
		uint64_t pa = pf_find(pf, pos >> PAGE_SHIFT);
		if (!pa) pa = pf_get_page(pf, pos >> PAGE_SHIFT, &err);
		if (!pa) break;

		uint32_t off = (uint32_t)(pos & (PAGE_SIZE - 1));
		size_t n = PAGE_SIZE - off;
		if (n > len - done) n = len - done;
		if (copy_fop_from_user((uint8_t *)PHYS_TO_VIRT(pa) + off, (const uint8_t *)buf + done, n) != 0) {
			err = -14;
			break;
		}
		pos  += n;
		done += n;
		if (pos > in->size) {
			uint64_t l = spin_lock_irq(&pf->lock);
			in->size = pos;
			spin_unlock_irq(&pf->lock, l);
		}
	}
	if (done) {
		f->pos = (off_t)pos;
		int64_t s, ns; ranix_walltime(&s, &ns);
		in->mtime = in->ctime = s;
		in->mtime_nsec = in->ctime_nsec = (uint32_t)ns;
	}
out:
	pf_leave(pf);
	return done ? (ssize_t)done : err;
}

/* Hand the generic mmap path the file's own page covering off, with a reference
 * for the mapping. A hole gets its page now: mappings are built eagerly and stores
 * through them must land in the file. Past EOF returns 0 (EINVAL). */
uint64_t pagefile_mmap(vfs_file_t *f, off_t off)
{
	vfs_inode_t *in = f->inode;
	pagefile_t *pf = in->priv;
	if (!pf || off < 0) return 0;

	pf_enter(pf);
	uint64_t pa = 0;
	int err;
	if ((uint64_t)off < in->size) {
		uint64_t idx = (uint64_t)off >> PAGE_SHIFT;
		pa = pf_find(pf, idx);
		if (!pa) pa = pf_get_page(pf, idx, &err);
		if (pa) pmm_hold((void *)pa);
	}
	pf_leave(pf);
	return pa;
}

int pagefile_truncate(vfs_inode_t *in, off_t len)
{
	pagefile_t *pf = in->priv;
	if (!pf || len < 0) return -EINVAL;
	if ((uint64_t)len > MAX_SIZE) return -EFBIG;
	uint64_t newlen = (uint64_t)len;

	pf_enter(pf);
	if (newlen > in->size) {
		pf_clear_tail(pf, in->size);
		uint64_t l = spin_lock_irq(&pf->lock);
		in->size = newlen;
		spin_unlock_irq(&pf->lock, l);
	} else if (newlen < in->size) {
		uint64_t first = (newlen + PAGE_SIZE - 1) >> PAGE_SHIFT, gone = 0, data = 0;
		uint64_t l = spin_lock_irq(&pf->lock);
		in->size = newlen;
		if (first < 1ULL << (FANOUT_BITS * pf->height))
			gone = pf_prune(&pf->root, pf->height, 0, first, 0, &data);
		if (!pf->root) pf->height = 0;
		spin_unlock_irq(&pf->lock, l);
		pf_uncharge(pf, gone, data);
		pf_clear_tail(pf, newlen);
	}
	pf_leave(pf);
	return 0;
}

int pagefile_open(vfs_inode_t *inode, vfs_file_t *file, int flags)
{
	if ((flags & O_TRUNC) && inode->priv) {
		int r = pagefile_truncate(inode, 0);
		if (r) return r;
	}
	if (flags & O_APPEND) file->pos = (off_t)inode->size;
	return 0;
}
