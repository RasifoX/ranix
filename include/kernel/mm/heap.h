/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef HEAP_H
#define HEAP_H

#include <uapi/types.h>

void heap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);
extern uint64_t heap_bad_frees;   /* kfree of a block that is not a live one ("kfree bad ptr") */

#endif
