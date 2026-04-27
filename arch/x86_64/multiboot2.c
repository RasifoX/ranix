#include <uapi/types.h>
#include <kernel/kprintf.h>
#include <asm/mmu.h>

#define MB2_TAG_TYPE_END   0
#define MB2_TAG_TYPE_MMAP  6

typedef struct {
    uint32_t type;
    uint32_t size;
} mb2_tag_t;

typedef struct {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
} mb2_mmap_entry_t;

typedef struct {
    uint32_t total_size;
    uint32_t reserved;
} mb2_info_t;

void multiboot2_parse(uint64_t ptr, uint64_t *mem_size_out)
{
    if (ptr == 0) return;

    mb2_info_t *info = (mb2_info_t *)ptr;
    uint32_t total_size = info->total_size;

    uint8_t *p   = (uint8_t *)(ptr + 8);
    uint8_t *end = (uint8_t *)(ptr + total_size);

    uint64_t max_ram = 0;

    while (p < end) {
        mb2_tag_t *tag = (mb2_tag_t *)p;
        if (tag->type == MB2_TAG_TYPE_END) break;
        if (tag->size == 0) break;

        if (tag->type == MB2_TAG_TYPE_MMAP) {
            uint32_t entry_size = *(uint32_t *)(p + 8);
            if (entry_size == 0) break;
            mb2_mmap_entry_t *entry = (mb2_mmap_entry_t *)(p + 16);
            int count = (tag->size - 16) / entry_size;

            for (int i = 0; i < count; i++) {
                if (entry->type == 1) {
                    if (entry->addr + entry->len > max_ram)
                        max_ram = entry->addr + entry->len;
                }
                entry = (mb2_mmap_entry_t *)((uint8_t *)entry + entry_size);
            }
        }
        p += (tag->size + 7) & ~7;
    }

    if (max_ram > 0)
        *mem_size_out = max_ram;
}
