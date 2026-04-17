#include <kernel/kprintf.h>
#include <kernel/mm/pmm.h>

static uint8_t bitmap[32768];
static uint64_t total_pages;
static uint64_t base_addr;

static void bitmap_set(uint64_t page) { bitmap[page / 8] |= (1 << (page % 8)); }
static void bitmap_clear(uint64_t page) { bitmap[page / 8] &= ~(1 << (page % 8)); }
static int bitmap_test(uint64_t page) { return bitmap[page / 8] & (1 << (page % 8)); }

void pmm_init(uint64_t mem_start, uint64_t mem_size) {
  base_addr = mem_start;
  total_pages = mem_size / PAGE_SIZE;
  for (uint64_t i = 0; i < total_pages / 8; i++)
    bitmap[i] = 0xFF;
}

void pmm_free_region(uint64_t addr, uint64_t size) {
  uint64_t page = (addr - base_addr) / PAGE_SIZE;
  uint64_t count = size / PAGE_SIZE;
  for (uint64_t i = 0; i < count; i++)
    bitmap_clear(page + i);
}

void *pmm_alloc(void) {
  for (uint64_t i = 0; i < total_pages; i++) {
    if (!bitmap_test(i)) {
      bitmap_set(i);
      return (void *)(base_addr + i * PAGE_SIZE);
    }
  }
  kpanic("pmm_alloc: out of memory");
  return 0;
}

void pmm_free(void *page) {
  uint64_t idx = ((uint64_t)page - base_addr) / PAGE_SIZE;
  bitmap_clear(idx);
}

uint64_t pmm_free_pages(void) {
  uint64_t count = 0;
  for (uint64_t i = 0; i < total_pages; i++)
    if (!bitmap_test(i))
      count++;
  return count;
}

