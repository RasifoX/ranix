/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef KERNEL_VDSO_H
#define KERNEL_VDSO_H

/* The vDSO image mapped into every process, and its time page (vvar). */

#include <uapi/types.h>
#include <kernel/mm/vm.h>

extern uint32_t vdso_timens_used;   /* once set, monotonic clocks take the syscall */

void vdso_init(void);
void vdso_map(vm_space_t *vs);
void vdso_time_changed(void);       /* refresh the vvar after a clock step */

#endif
