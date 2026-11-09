/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef MMU_H
#define MMU_H

#include <uapi/types.h>
#include <asm/mmu.h>

#define PAGE_SHIFT  12
#define PAGE_SIZE   (1 << PAGE_SHIFT)
#define PAGE_MASK   (~(PAGE_SIZE - 1))

void mmu_init(void);
void mmu_null_guard_selftest(void);   /* x86_64: does a kernel NULL dereference fault? (boot log) */
void arch_map_high_memory(uint64_t max_phys);

#endif
