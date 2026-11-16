/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef KERNEL_EVENTFD_H
#define KERNEL_EVENTFD_H

/* eventfd(2) for kernel signallers (kernel/sys/syscall_event.c) */

#include <kernel/vfs.h>

/* A kernel listener on an eventfd (KVM's irqfd): fn runs on every signal, from
 * the signaller's context with the eventfd's lock held and interrupts off, so
 * it must not sleep. It returns 1 to take the count (the eventfd reads empty
 * after), 0 to leave it. Registered when the count is already up, it runs at
 * once. */
typedef struct eventfd_hook {
	int  (*fn)(void *arg);
	void  *arg;
	struct eventfd_hook *next;
} eventfd_hook_t;
int  eventfd_hook_add(vfs_file_t *f, eventfd_hook_t *h);   /* -EINVAL: not an eventfd */
void eventfd_hook_del(vfs_file_t *f, eventfd_hook_t *h);

/* 1 when f is an eventfd */
int eventfd_is(vfs_file_t *f);
/* Add n to the count, as write(2) would, waking readers. It never sleeps:
 * a count that would overflow stays where it is (the reader is far behind
 * anyway). Callable with interrupts off. */
void eventfd_signal_file(vfs_file_t *f, uint64_t n);

#endif
