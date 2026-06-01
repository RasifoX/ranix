#include <uapi/types.h>
#include <kernel/kprintf.h>
#include <kernel/fb.h>
#include <kernel/cmdline.h>
#include <asm/mmu.h>

#define MB2_TAG_TYPE_END        0
#define MB2_TAG_TYPE_CMDLINE    1
#define MB2_TAG_TYPE_MODULE     3
#define MB2_TAG_TYPE_MMAP       6
#define MB2_TAG_TYPE_FRAMEBUFFER 8
#define MB2_TAG_TYPE_ACPI_OLD   14
#define MB2_TAG_TYPE_ACPI_NEW   15

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

typedef struct {
	uint64_t addr;
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
	uint8_t  bpp;
	uint8_t  type;
	uint16_t reserved;
	/* type==1 (direct RGB) color info follows */
	uint8_t  red_pos;   uint8_t red_size;
	uint8_t  green_pos; uint8_t green_size;
	uint8_t  blue_pos;  uint8_t blue_size;
} mb2_fb_tag_t;

static fb_info_t mb2_fb;
static int mb2_fb_valid = 0;

static uint64_t _mmap_base = 0;
static uint32_t _mmap_entry_sz = 0;
static uint32_t _mmap_count = 0;

int mb2_get_fb(fb_info_t *out)
{
	if (!mb2_fb_valid) return -1;
	*out = mb2_fb;
	return 0;
}

extern uint64_t boot_initrd_start;
extern uint64_t boot_initrd_size;

static uint64_t _mb2_base = 0;
static uint32_t _mb2_size = 0;

void mb2_get_reserved(uint64_t *base, uint32_t *size) { *base = _mb2_base; *size = _mb2_size; }

static uint64_t _rsdp_phys = 0;
uint64_t mb2_get_rsdp(void) { return _rsdp_phys; }

void multiboot2_parse(uint64_t ptr, uint64_t *mem_size_out)
{
    if (ptr == 0) return;

    mb2_info_t *info = (mb2_info_t *)ptr;
    uint32_t total_size = info->total_size;
    _mb2_base = ptr;
    _mb2_size = total_size;

    uint8_t *p   = (uint8_t *)(ptr + 8);
    uint8_t *end = (uint8_t *)(ptr + total_size);

    uint64_t max_ram = 0;

    while (p < end) {
        mb2_tag_t *tag = (mb2_tag_t *)p;
        if (tag->type == MB2_TAG_TYPE_END) break;
        if (tag->size < 8) break;

        if (tag->type == MB2_TAG_TYPE_CMDLINE) {
            /* cmdline string starts right after the 8-byte tag header */
            const char *cl = (const char *)(p + 8);
            cmdline_init(cl);
        } else if (tag->type == MB2_TAG_TYPE_MMAP) {
            uint32_t entry_size = *(uint32_t *)(p + 8);
            if (entry_size == 0) break;
            uint32_t count = (tag->size - 16) / entry_size;
            _mmap_base     = (uint64_t)(p + 16);
            _mmap_entry_sz = entry_size;
            _mmap_count    = count;
            for (uint32_t i = 0; i < count; i++) {
                mb2_mmap_entry_t *entry = (mb2_mmap_entry_t *)(_mmap_base + (uint64_t)i * entry_size);
                if (entry->type == 1 && entry->addr + entry->len > max_ram)
                    max_ram = entry->addr + entry->len;
            }
        } else if (tag->type == MB2_TAG_TYPE_MODULE) {
            mb2_module_t *mod = (mb2_module_t *)(p + 8);
            boot_initrd_start = (uint64_t)mod->mod_start;
            boot_initrd_size  = (uint64_t)(mod->mod_end - mod->mod_start);
            kprintf("MB2: found module start=%x end=%x\n", mod->mod_start, mod->mod_end);
        } else if (tag->type == MB2_TAG_TYPE_ACPI_OLD ||
                   tag->type == MB2_TAG_TYPE_ACPI_NEW) {
            /* The tag carries a copy of the RSDP; the boot identity map makes
             * its VA equal its PA, so this is the physical address acpi.c
             * re-reads through the direct map. New (v2) wins if both present. */
            if (_rsdp_phys == 0 || tag->type == MB2_TAG_TYPE_ACPI_NEW)
                _rsdp_phys = (uint64_t)(p + 8);
        } else if (tag->type == MB2_TAG_TYPE_FRAMEBUFFER) {
            mb2_fb_tag_t *fb = (mb2_fb_tag_t *)(p + 8);
            if (fb->type == 1 && fb->bpp == 32) {
                mb2_fb.fb          = (void *)fb->addr;
                mb2_fb.width       = fb->width;
                mb2_fb.height      = fb->height;
                mb2_fb.pitch       = fb->pitch;
                mb2_fb.depth       = fb->bpp;
                mb2_fb.fb_size     = fb->pitch * fb->height;
                mb2_fb.red_shift   = fb->red_pos;
                mb2_fb.green_shift = fb->green_pos;
                mb2_fb.blue_shift  = fb->blue_pos;
                mb2_fb_valid       = 1;
                kprintf("MB2: framebuffer %dx%d @%llx R=%d G=%d B=%d\n",
                        fb->width, fb->height, (unsigned long long)fb->addr,
                        fb->red_pos, fb->green_pos, fb->blue_pos);
            }
        }
        p += ((uint64_t)tag->size + 7) & ~(uint64_t)7;
    }

    if (max_ram > 0)
        *mem_size_out = max_ram;
}

void mb2_foreach_mem_region(void (*cb)(uint64_t, uint64_t, void *), void *ud)
{
    for (uint32_t i = 0; i < _mmap_count; i++) {
        mb2_mmap_entry_t *e = (mb2_mmap_entry_t *)(_mmap_base + (uint64_t)i * _mmap_entry_sz);
        if (e->type == 1 && e->len > 0)
            cb(e->addr, e->len, ud);
    }
}
