/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef KERNEL_CLAIM_H
#define KERNEL_CLAIM_H

/* Kernel side of /dev/claim (kernel/claim.c); the ioctl ABI is uapi/claim.h. */

#include <kernel/proc.h>
#include <kernel/kout.h>

int claim_reboot_check(process_t *caller);       /* reboot(2): 0, or -EBUSY under a claim */
int claim_report(kout_t *o, int json);

#endif
