/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef KERNEL_EVENTFD_H
#define KERNEL_EVENTFD_H

/* eventfd(2) for kernel signallers (kernel/sys/syscall_event.c) */

#include <kernel/vfs.h>

/* 1 when f is an eventfd */
int eventfd_is(vfs_file_t *f);
/* Add n to the count, as write(2) would, waking readers. It never sleeps:
 * a count that would overflow stays where it is (the reader is far behind
 * anyway). Callable with interrupts off. */
void eventfd_signal_file(vfs_file_t *f, uint64_t n);

#endif
