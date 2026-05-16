#include <uapi/types.h>
#include <kernel/kprintf.h>
#include <kernel/fb.h>
#include <asm/mmu.h>

#define MB2_TAG_TYPE_END        0
#define MB2_TAG_TYPE_MODULE     3
#define MB2_TAG_TYPE_MMAP       6
#define MB2_TAG_TYPE_FRAMEBUFFER 8

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

int mb2_get_fb(fb_info_t *out)
{
	if (!mb2_fb_valid) return -1;
	*out = mb2_fb;
	return 0;
}

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
