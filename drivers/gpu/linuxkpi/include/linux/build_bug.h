/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _LKPI_LINUX_BUILD_BUG_H
#define _LKPI_LINUX_BUILD_BUG_H
/* 0 when c is false; a compile error (negative bit-field) when it is true */
#define BUILD_BUG_ON_ZERO(c) ((int)(sizeof(struct { int:(-!!(c)); })))
#define BUILD_BUG_ON(c) ((void)BUILD_BUG_ON_ZERO(c))
#define static_assert(e, ...) _Static_assert(e, #e)
/* true for an integer constant expression (the Linux trick: a null pointer constant) */
#define __is_constexpr(x) (sizeof(int) == sizeof(*(8 ? ((void *)((long)(x) * 0l)) : (int *)8)))
#endif
