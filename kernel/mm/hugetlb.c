#include <kernel/mm/hugetlb.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/mmu.h>
#include <kernel/sync.h>
#include <kernel/kprintf.h>

#define MAX_1G_BLOCKS  64

static struct {
	uint64_t base;       /* 1 GB-aligned physical base */
	int      refcount;   /* 0 = free */
} pool[MAX_1G_BLOCKS];

static int        pool_n = 0;
static spinlock_t pool_lock = SPINLOCK_INIT;

static long nr_huge2m = 0;   /* 2 MB user blocks currently mapped */

void huge2m_account(int delta)
{
	uint64_t f = spin_lock_irq(&pool_lock);
	nr_huge2m += delta;
	if (nr_huge2m < 0) nr_huge2m = 0;
	spin_unlock_irq(&pool_lock, f);
}

uint64_t huge2m_kb(void)
{
	return (uint64_t)nr_huge2m * (2UL * 1024);   /* 2 MB per block, in KiB */
}

void hugetlb_1g_reserve(uint64_t base, int nblocks)
{
	for (int i = 0; i < nblocks && pool_n < MAX_1G_BLOCKS; i++) {
		pool[pool_n].base     = base + (uint64_t)i * HUGE_1G_SIZE;
		pool[pool_n].refcount = 0;
		pool_n++;
	}
	if (nblocks > 0)
		kprintf("hugetlb: reserved %d x 1G @ %lu GB\n",
		        nblocks, (unsigned long)(base >> 30));
}

static int block_of(uint64_t pa)
{
	for (int i = 0; i < pool_n; i++)
		if (pa >= pool[i].base && pa < pool[i].base + HUGE_1G_SIZE)
			return i;
	return -1;
}

uint64_t huge1g_alloc(void)
{
	uint64_t f = spin_lock_irq(&pool_lock);
	for (int i = 0; i < pool_n; i++) {
		if (pool[i].refcount == 0) {
			pool[i].refcount = 1;
			uint64_t b = pool[i].base;
			spin_unlock_irq(&pool_lock, f);
			return b;
		}
	}
	spin_unlock_irq(&pool_lock, f);
	return 0;
}

void huge1g_get(uint64_t pa)
{
	uint64_t f = spin_lock_irq(&pool_lock);
	int i = block_of(pa);
	if (i >= 0 && pool[i].refcount > 0) pool[i].refcount++;
	spin_unlock_irq(&pool_lock, f);
}

void huge1g_put(uint64_t pa)
{
	uint64_t f = spin_lock_irq(&pool_lock);
	int i = block_of(pa);
	if (i >= 0 && pool[i].refcount > 0) pool[i].refcount--;
	spin_unlock_irq(&pool_lock, f);
}

int huge1g_refcount(uint64_t pa)
{
	uint64_t f = spin_lock_irq(&pool_lock);
	int i = block_of(pa);
	int r = (i >= 0) ? pool[i].refcount : 0;
	spin_unlock_irq(&pool_lock, f);
	return r;
}

int huge1g_owns(uint64_t pa)
{
	uint64_t f = spin_lock_irq(&pool_lock);
	int i = block_of(pa);
	spin_unlock_irq(&pool_lock, f);
	return i >= 0;
}

/* ── 2 MB pool ───────────────────────────────────────────────
 * The Linux nr_hugepages model, as a permit count over the buddy: nr_total is
 * the pool size (a ceiling), nr_free the uncommitted permits. A 2 MB block is
 * order 9, which the buddy serves directly, so the physical lifetime of a block
 * is left to the page refcount (like the anon MAP_HUGETLB path): huge2m_free
 * drops the owner's reference per page, and a block still mapped survives until
 * the last unmap pmm_frees it. The permit count is what nr_hugepages reserves
 * and what hugetlbfs spends, so the counter visibly tracks pool use. */
static long       pool2m_total = 0;
static long       pool2m_free  = 0;
static spinlock_t pool2m_lock = SPINLOCK_INIT;

uint64_t huge2m_alloc(void)
{
	uint64_t f = spin_lock_irq(&pool2m_lock);
	if (pool2m_free <= 0) { spin_unlock_irq(&pool2m_lock, f); return 0; }
	pool2m_free--;
	spin_unlock_irq(&pool2m_lock, f);

	void *blk = buddy_alloc(9);            /* 2 MB, 2 MB-aligned; pages refcount 1 */
	if (!blk) {
		f = spin_lock_irq(&pool2m_lock);
		pool2m_free++;
		spin_unlock_irq(&pool2m_lock, f);
		return 0;
	}
	return (uint64_t)blk;
}

void huge2m_free(uint64_t pa)
{
	if (!pa) return;
	for (int i = 0; i < 512; i++)
		pmm_free((void *)(pa + (uint64_t)i * PAGE_SIZE));
	uint64_t f = spin_lock_irq(&pool2m_lock);
	if (pool2m_free < pool2m_total) pool2m_free++;
	spin_unlock_irq(&pool2m_lock, f);
}

long huge2m_nr_set(long n)
{
	if (n < 0) n = 0;
	uint64_t f = spin_lock_irq(&pool2m_lock);
	long inuse = pool2m_total - pool2m_free;
	if (n < inuse) n = inuse;              /* can't reclaim committed pages */
	pool2m_free += n - pool2m_total;
	pool2m_total = n;
	spin_unlock_irq(&pool2m_lock, f);
	return n;
}

long huge2m_nr_total(void) { return pool2m_total; }
long huge2m_nr_free(void)  { return pool2m_free; }
