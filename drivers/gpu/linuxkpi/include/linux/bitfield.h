/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _LKPI_LINUX_BITFIELD_H
#define _LKPI_LINUX_BITFIELD_H
#include <linux/bits.h>
#define __bf_shf(x) (__builtin_ffsll(x) - 1)
#define FIELD_PREP(mask, val) (((typeof(mask))(val) << __bf_shf(mask)) & (mask))
#define FIELD_GET(mask, reg)  ((typeof(mask))(((reg) & (mask)) >> __bf_shf(mask)))
#endif
