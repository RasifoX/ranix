#include <kernel/mm/hugetlb.h>
#include <kernel/sync.h>
#include <kernel/kprintf.h>

#define MAX_1G_BLOCKS  64

static struct {
	uint64_t base;       /* 1 GB-aligned physical base */
	int      refcount;   /* 0 = free */
} pool[MAX_1G_BLOCKS];

static int        pool_n = 0;
static spinlock_t pool_lock = SPINLOCK_INIT;

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
