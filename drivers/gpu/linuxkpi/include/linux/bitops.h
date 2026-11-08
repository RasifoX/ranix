/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _LKPI_LINUX_BITOPS_H
#define _LKPI_LINUX_BITOPS_H
#include <linux/bits.h>
#define BITS_PER_TYPE(t) (sizeof(t) * 8)
static inline int fls(unsigned int x) { return x ? 32 - __builtin_clz(x) : 0; }
static inline unsigned long __ffs(unsigned long x) { return (unsigned long)__builtin_ctzl(x); }
static inline s64 sign_extend64(u64 value, int index)
{
	int shift = 63 - index;
	return (s64)(value << shift) >> shift;
}
static inline s32 sign_extend32(u32 value, int index)
{
	int shift = 31 - index;
	return (s32)(value << shift) >> shift;
}
#endif
