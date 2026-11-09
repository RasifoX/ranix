/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef KERNEL_MM_VMALLOC_H
#define KERNEL_MM_VMALLOC_H

#include <uapi/types.h>

void *vmalloc(size_t size);
void *vzalloc(size_t size);
void  vfree(void *ptr);

#endif
