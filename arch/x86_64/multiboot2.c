#include <uapi/types.h>
#include <kernel/kprintf.h>
#include <asm/mmu.h>

#define MB2_TAG_TYPE_END   0
#define MB2_TAG_TYPE_MODULE 3
#define MB2_TAG_TYPE_MMAP  6

typedef struct {
    uint32_t type;
    uint32_t size;
} mb2_tag_t;

typedef struct {
    uint32_t mod_start;
    uint32_t mod_end;
    char     string[1];
} mb2_module_t;

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

extern uint64_t boot_initrd_start;
extern uint64_t boot_initrd_size;

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
        if (tag->size < 8) break;

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
        } else if (tag->type == MB2_TAG_TYPE_MODULE) {
            mb2_module_t *mod = (mb2_module_t *)(p + 8);
            boot_initrd_start = (uint64_t)mod->mod_start;
            boot_initrd_size  = (uint64_t)(mod->mod_end - mod->mod_start);
            kprintf("MB2: found module start=%x end=%x\n", mod->mod_start, mod->mod_end);
        }
        p += ((uint64_t)tag->size + 7) & ~(uint64_t)7;
    }

    if (max_ram > 0)
        *mem_size_out = max_ram;
}
