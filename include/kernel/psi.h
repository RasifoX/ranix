/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef KERNEL_PSI_H
#define KERNEL_PSI_H

#include <uapi/types.h>

/* Pressure stall information (kernel/psi.c): how much of the time work waited
 * for CPU, memory or I/O, as Linux's /proc/pressure/{cpu,memory,io}. */

#define PSI_MEM 0x1     /* in direct reclaim or a swap-in */
#define PSI_IO  0x2     /* waiting on a block device */

enum { PSI_RES_CPU, PSI_RES_MEM, PSI_RES_IO, PSI_RES_N };

/* Marks the current task stalled on what; returns the marks to give back to
 * psi_leave, so nested stalls (a swap-in that reads the disk) count once. */
unsigned psi_enter(unsigned what);
void     psi_leave(unsigned prev);

/* The BSP's tick: `ticks` kernel ticks went by since the last call. */
void psi_tick(uint64_t ticks);

/* The text of /proc/pressure/<res>: "some ..." and "full ..." lines. */
int psi_show(int res, char *buf, size_t sz);

#endif
