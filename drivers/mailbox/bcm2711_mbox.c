#include <drivers/mailbox/mbox.h>
#include <kernel/kprintf.h>
#include <asm/mmu.h>

uint64_t _mbox_base;
static uint32_t _bus_offset;

void mbox_init(uint64_t base, uint32_t bus_offset) {
    _mbox_base  = base;
    _bus_offset = bus_offset;
}

static inline u32 mmio_read(u64 addr) {
    return *(volatile u32 *)addr;
}
static inline void mmio_write(u64 addr, u32 val) {
    *(volatile u32 *)addr = val;
}

static volatile u32 __attribute__((aligned(16), section(".mbox_buf"))) mbox_buf[36];

int mbox_call(u8 channel, volatile u32 *msg) {
    u32 phys = (u32)(u64)msg;
    u32 val  = ((phys + _bus_offset) & ~0xFU) | (channel & 0xF);

    u32 size = msg[0];
    uintptr_t start = (uintptr_t)msg & ~63UL;
    uintptr_t end   = ((uintptr_t)msg + size + 63) & ~63UL;
    for (uintptr_t l = start; l < end; l += 64)
        __asm__ volatile("dc civac, %0" :: "r"(l) : "memory");

    __asm__ volatile("dsb sy" ::: "memory");

    while (mmio_read(MBOX_STATUS) & MBOX_FULL)
        __asm__ volatile("nop");

    mmio_write(MBOX_WRITE, val);

    while (1) {
        while (mmio_read(MBOX_STATUS) & MBOX_EMPTY)
            __asm__ volatile("nop");

        u32 r = mmio_read(MBOX_READ);

        if (r == val) {
            for (uintptr_t l = start; l < end; l += 64)
                __asm__ volatile("dc civac, %0" :: "r"(l) : "memory");
            __asm__ volatile("dsb sy" ::: "memory");
            return (msg[1] == MBOX_RESPONSE) ? 0 : -1;
        }
    }
}

int mbox_get_display_size(u32 *w, u32 *h)
{
    u32 i = 0;
    mbox_buf[i++] = 0;
    mbox_buf[i++] = 0;
    mbox_buf[i++] = MBOX_TAG_GET_PHYS_WH;
    mbox_buf[i++] = 8;
    mbox_buf[i++] = 0;
    mbox_buf[i++] = 0;
    mbox_buf[i++] = 0;
    mbox_buf[i++] = MBOX_TAG_END;
    mbox_buf[0] = i * sizeof(u32);

    if (mbox_call(MBOX_CH_PROP, mbox_buf) != 0) return -1;
    if (mbox_buf[5] == 0 || mbox_buf[6] == 0) return -1;
    *w = mbox_buf[5];
    *h = mbox_buf[6];
    return 0;
}

int mbox_fb_init(u32 width, u32 height, fb_info_t *out) {
    u32 i = 0;

    mbox_buf[i++] = 0;
    mbox_buf[i++] = 0;

    mbox_buf[i++] = MBOX_TAG_SET_PHYS_WH;
    mbox_buf[i++] = 8;
    mbox_buf[i++] = 0;
    mbox_buf[i++] = width;
    mbox_buf[i++] = height;

    mbox_buf[i++] = MBOX_TAG_SET_VIRT_WH;
    mbox_buf[i++] = 8;
    mbox_buf[i++] = 0;
    mbox_buf[i++] = width;
    mbox_buf[i++] = height * 2;

    mbox_buf[i++] = MBOX_TAG_SET_VIRT_OFF;
    mbox_buf[i++] = 8;
    mbox_buf[i++] = 0;
    mbox_buf[i++] = 0;
    mbox_buf[i++] = 0;

    mbox_buf[i++] = MBOX_TAG_SET_DEPTH;
    mbox_buf[i++] = 4;
    mbox_buf[i++] = 0;
    mbox_buf[i++] = 32;

    mbox_buf[i++] = MBOX_TAG_SET_PIXEL_ORD;
    mbox_buf[i++] = 4;
    mbox_buf[i++] = 0;
    mbox_buf[i++] = 1;

    mbox_buf[i++] = MBOX_TAG_ALLOC_FB;
    mbox_buf[i++] = 8;
    mbox_buf[i++] = 0;
    mbox_buf[i++] = 16;
    mbox_buf[i++] = 0;

    mbox_buf[i++] = MBOX_TAG_GET_PITCH;
    mbox_buf[i++] = 4;
    mbox_buf[i++] = 0;
    mbox_buf[i++] = 0;

    mbox_buf[i++] = MBOX_TAG_END;

    mbox_buf[0] = i * sizeof(u32);

    if (mbox_call(MBOX_CH_PROP, mbox_buf) != 0) {
        kprintf("[  mem] fb: init failed\n");
        return -1;
    }

    u32 fb_bus_addr = mbox_buf[28];
    u32 fb_size     = mbox_buf[29];
    u32 pitch       = mbox_buf[33];

    if (fb_bus_addr == 0) {
        kprintf("[  mem] fb: alloc failed\n");
        return -1;
    }

    out->width   = width;
    out->height  = height;
    out->virt_height = height * 2;
    out->depth   = 32;
    out->pitch   = pitch;
    out->fb      = (void *)(u64)(fb_bus_addr & 0x3FFFFFFFU);
    out->fb_size = fb_size;

    kprintf("[  mem] fb: %dx%d, pitch %d, addr 0x%lx\n",
            width, height, pitch, (uint64_t)out->fb);

    return 0;
}

int mbox_set_virt_offset(u32 x, u32 y)
{
    u32 i = 0;
    mbox_buf[i++] = 8 * 4;
    mbox_buf[i++] = 0;
    mbox_buf[i++] = MBOX_TAG_SET_VIRT_OFF;
    mbox_buf[i++] = 8;
    mbox_buf[i++] = 0;
    mbox_buf[i++] = x;
    mbox_buf[i++] = y;
    mbox_buf[i++] = MBOX_TAG_END;
    mbox_buf[0] = i * sizeof(u32);
    return mbox_call(MBOX_CH_PROP, mbox_buf);
}

int mbox_set_clock(u32 clock_id, u32 rate_hz)
{
    u32 i = 0;
    mbox_buf[i++] = 0;           /* total size — filled below */
    mbox_buf[i++] = 0;           /* request code */
    mbox_buf[i++] = MBOX_TAG_SET_CLOCK_RATE;
    mbox_buf[i++] = 12;          /* value buffer size (3×4 bytes) */
    mbox_buf[i++] = 0;           /* request/response indicator */
    mbox_buf[i++] = clock_id;
    mbox_buf[i++] = rate_hz;
    mbox_buf[i++] = 0;           /* skip_turbo = 0 */
    mbox_buf[i++] = MBOX_TAG_END;
    mbox_buf[0] = i * sizeof(u32);
    return mbox_call(MBOX_CH_PROP, mbox_buf);
}

u32 mbox_get_clock(u32 clock_id)
{
    u32 i = 0;
    mbox_buf[i++] = 0;
    mbox_buf[i++] = 0;
    mbox_buf[i++] = MBOX_TAG_GET_CLOCK_RATE;
    mbox_buf[i++] = 8;
    mbox_buf[i++] = 0;
    mbox_buf[i++] = clock_id;
    mbox_buf[i++] = 0;           /* rate returned here */
    mbox_buf[i++] = MBOX_TAG_END;
    mbox_buf[0] = i * sizeof(u32);
    if (mbox_call(MBOX_CH_PROP, mbox_buf) != 0) return 0;
    return mbox_buf[6];          /* returned rate_hz */
}