#include <kernel/mm/buddy.h>
#include <kernel/mm/mmu.h>
#include <kernel/lib/string.h>
#include <asm/mmu.h>

struct node { struct node *next; };

static struct node *free_list[BUDDY_MAX_ORDER];
static uint8_t     *blk_order; /* order of the free/alloc block at this page */
static uint8_t     *free_bm;
static uint64_t    _base;
static uint64_t    _npages;

static void bm_set(uint64_t p) { free_bm[p >> 3] |=  (1 << (p & 7)); }
static void bm_clr(uint64_t p) { free_bm[p >> 3] &= ~(1 << (p & 7)); }
static int  bm_get(uint64_t p) { return  free_bm[p >> 3] &  (1 << (p & 7)); }

static uint64_t pidx(uint64_t addr) { return (addr - _base) >> PAGE_SHIFT; }
static uint64_t paddr(uint64_t p)   { return _base + (p << PAGE_SHIFT); }

static void push(int ord, uint64_t addr)
{
	struct node *n = (struct node *)PHYS_TO_VIRT(addr);
	n->next = free_list[ord];
	free_list[ord] = (struct node *)addr;
}

static uint64_t pop(int ord)
{
	uint64_t addr = (uint64_t)free_list[ord];
	struct node *n = (struct node *)PHYS_TO_VIRT(addr);
	free_list[ord] = n->next;
	return addr;
}

static void list_remove(int ord, uint64_t addr)
{
	struct node **p = &free_list[ord];
	while (*p && (uint64_t)*p != addr) {
		struct node *n = (struct node *)PHYS_TO_VIRT(*p);
		p = &n->next;
	}
	if (*p) {
		struct node *n = (struct node *)PHYS_TO_VIRT(*p);
		*p = n->next;
	}
}

void buddy_free(uint64_t addr, int ord)
{
	uint64_t p = pidx(addr);

	while (ord < BUDDY_MAX_ORDER - 1) {
		uint64_t buddy = p ^ (1ul << ord);
		if (buddy >= _npages || !bm_get(buddy) || blk_order[buddy] != (uint8_t)ord)
			break;
		bm_clr(buddy);
		list_remove(ord, paddr(buddy));
		p = (p < buddy) ? p : buddy;
		ord++;
	}

	bm_set(p);
	blk_order[p] = (uint8_t)ord;
	push(ord, paddr(p));
}

void *buddy_alloc(int ord)
{
	int o = ord;
	while (o < BUDDY_MAX_ORDER && !free_list[o])
		o++;
	if (o >= BUDDY_MAX_ORDER)
		return 0;

	uint64_t addr = pop(o);
	uint64_t p    = pidx(addr);
	bm_clr(p);

	while (o-- > ord) {
		uint64_t half   = addr + ((uint64_t)PAGE_SIZE << o);
		uint64_t half_p = pidx(half);
		bm_set(half_p);
		blk_order[half_p] = (uint8_t)o;
		push(o, half);
	}

	blk_order[p] = (uint8_t)ord;
	return (void *)addr;
}

void buddy_add_region(uint64_t addr, uint64_t size)
{
	uint64_t end = (addr + size) & ~(uint64_t)(PAGE_SIZE - 1);
	addr = (addr + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

	while (addr < end) {
		int o = BUDDY_MAX_ORDER - 1;
		while (o > 0) {
			uint64_t blk = (uint64_t)PAGE_SIZE << o;
			if ((addr & (blk - 1)) == 0 && addr + blk <= end)
				break;
			o--;
		}
		buddy_free(addr, o);
		addr += (uint64_t)PAGE_SIZE << o;
	}
}

uint64_t buddy_init(uint64_t base, uint64_t limit)
{
	uint64_t total_pages = limit >> PAGE_SHIFT;

	uint64_t blk_order_sz = total_pages * sizeof(uint8_t);
	uint64_t free_bm_sz   = (total_pages + 7) / 8;
	uint64_t meta_sz      = blk_order_sz + free_bm_sz;
	uint64_t meta_pages   = (meta_sz + PAGE_SIZE - 1) >> PAGE_SHIFT;

	if (meta_pages >= total_pages) return 0;

	blk_order = (uint8_t *)PHYS_TO_VIRT(base);
	free_bm   = (uint8_t *)PHYS_TO_VIRT(base + blk_order_sz);

	_base   = 0;
	_npages = total_pages;

	memset(free_list, 0, sizeof(free_list));
	memset(blk_order, 0, blk_order_sz);
	memset(free_bm,   0, free_bm_sz);

	return meta_pages << PAGE_SHIFT;
}

uint64_t buddy_free_pages(void)
{
	uint64_t n = 0;
	for (uint64_t i = 0; i < _npages; i++)
		if (bm_get(i)) n += 1ul << blk_order[i];
	return n;
}
