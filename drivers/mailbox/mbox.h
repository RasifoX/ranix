#ifndef MBOX_H
#define MBOX_H

#include <uapi/types.h>
#include <kernel/fb.h>

extern uint64_t _mbox_base;
#define MBOX_READ    (_mbox_base + 0x00)
#define MBOX_STATUS  (_mbox_base + 0x18)
#define MBOX_WRITE   (_mbox_base + 0x20)

#define MBOX_FULL       0x80000000U
#define MBOX_EMPTY      0x40000000U
#define MBOX_RESPONSE   0x80000000U
#define MBOX_CH_PROP    8

#define MBOX_TAG_GET_PHYS_WH    0x00040003
#define MBOX_TAG_SET_PHYS_WH    0x00048003
#define MBOX_TAG_SET_VIRT_WH    0x00048004
#define MBOX_TAG_SET_VIRT_OFF   0x00048009
#define MBOX_TAG_SET_DEPTH      0x00048005
#define MBOX_TAG_SET_PIXEL_ORD  0x00048006
#define MBOX_TAG_ALLOC_FB       0x00040001
#define MBOX_TAG_GET_PITCH      0x00040008
#define MBOX_TAG_END            0x00000000

void mbox_init(uint64_t base, uint32_t bus_offset);
int  mbox_call(u8 channel, volatile u32 *msg);
int  mbox_get_display_size(u32 *w, u32 *h);
int  mbox_fb_init(u32 width, u32 height, fb_info_t *out);
int  mbox_set_virt_offset(u32 x, u32 y);

#endif
