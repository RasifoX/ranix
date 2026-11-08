/* SPDX-License-Identifier: BSD-2-Clause */
/* Ranix LinuxKPI: Linux kernel types over Ranix's own. Written for Ranix. */
#ifndef _LKPI_LINUX_TYPES_H
#define _LKPI_LINUX_TYPES_H
#include <uapi/types.h>
typedef _Bool bool;
#define true  1
#define false 0
typedef u8  __u8;
typedef u16 __u16;
typedef u32 __u32;
typedef u64 __u64;
typedef s8  __s8;
typedef s16 __s16;
typedef s32 __s32;
typedef s64 __s64;
typedef u16 __le16;
typedef u32 __le32;
typedef u64 __le64;
typedef u64 dma_addr_t;
typedef u64 phys_addr_t;
typedef u64 resource_size_t;
typedef unsigned int gfp_t;
#define U8_MAX  ((u8)~0U)
#define U16_MAX ((u16)~0U)
#define U32_MAX ((u32)~0U)
#define U64_MAX ((u64)~0ULL)
#define S32_MAX ((s32)(U32_MAX >> 1))
#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif
#endif
