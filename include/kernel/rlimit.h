/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Resource limits (getrlimit/setrlimit/prlimit64). The numbers are Linux's.
 * Limits belong to the thread group: every thread holds the same copy, a change
 * is written to all of them (rlimit_store_group), a new thread copies its
 * creator's (copy_creds), so a thread cannot step around a limit its group set. */
#ifndef KERNEL_RLIMIT_H
#define KERNEL_RLIMIT_H
#include <uapi/types.h>

#define RLIMIT_NPROC   6
#define RLIMIT_NOFILE  7
#define RLIMIT_AS      9
#define RLIM_NLIMITS   16
#define RLIM_INFINITY  ((uint64_t)-1)

struct process;
/* copy p's limits to every other thread of its group (under the scheduler lock) */
void rlimit_store_group(struct process *p);
#endif
