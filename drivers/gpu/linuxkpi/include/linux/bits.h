/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _LKPI_LINUX_BITS_H
#define _LKPI_LINUX_BITS_H
#include <linux/types.h>
#include <linux/build_bug.h>
#define BIT(n)      (1UL << (n))
#define BIT_ULL(n)  (1ULL << (n))
#define BITS_PER_LONG 64
#define GENMASK(h, l)     ((~0UL << (l)) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#define GENMASK_ULL(h, l) ((~0ULL << (l)) & (~0ULL >> (63 - (h))))
#endif
