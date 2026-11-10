/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The few kernel interfaces drivers/input/input_dev.c uses, for a host test:
 * one thread, so locks are counters and a sleep that would block is a failure. */
#ifndef HOST_INPUT_DEV_STUB_H
#define HOST_INPUT_DEV_STUB_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>

#define S_IFCHR 0020000
#define O_NONBLOCK 04000
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_ABS 0x03
#define DEVFS_GID_INPUT 23
#define TIMER_HZ 1000
#define ENOENT 2
#define EINTR 4
#define EAGAIN 11
#define ENOMEM 12
#define EFAULT 14
#define EBUSY 16
#define ENODEV 19
#define EINVAL 22

struct input_event { struct { int64_t tv_sec, tv_usec; } time; uint16_t type, code; int32_t value; };
typedef struct vfs_inode { void *priv; } vfs_inode_t;
typedef struct vfs_file { void *priv; int flags; } vfs_file_t;
typedef struct {
	int     (*open)(vfs_inode_t *, vfs_file_t *, int);
	ssize_t (*read)(vfs_file_t *, void *, size_t);
	int     (*ioctl)(vfs_file_t *, uint32_t, uint64_t);
	int     (*close)(vfs_file_t *);
	int     (*poll)(vfs_file_t *);
} vfs_fops_t;
typedef int spinlock_t;
#define SPINLOCK_INIT 0
typedef struct { int waiters; } wait_queue_t;

extern uint64_t kernel_ticks;
extern int stub_locks, stub_wakes, stub_sleeps, stub_hidden[16];
extern char stub_names[16][16];
extern void *stub_priv[16];
extern vfs_fops_t *stub_fops[16];
extern int stub_nnodes;

static inline uint64_t spin_lock_irq(spinlock_t *l) { (*l)++; stub_locks++; return 0; }
static inline void spin_unlock_irq(spinlock_t *l, uint64_t f) { (void)f; (*l)--; stub_locks--; }
static inline void wait_queue_init(wait_queue_t *q) { q->waiters = 0; }
static inline void wake_up(wait_queue_t *q) { (void)q; stub_wakes++; }
static inline void poll_notify(void) {}
static inline void sleep_on_locked(wait_queue_t *q, spinlock_t *l, uint64_t f) { (void)q; spin_unlock_irq(l, f); stub_sleeps++; }
static inline int task_has_actionable_signal(void) { return stub_sleeps > 0; }   /* a sleep "is interrupted" */
static inline long copy_to_user(void *d, const void *s, size_t n) { memcpy(d, s, n); return 0; }
static inline long copy_from_user(void *d, const void *s, size_t n) { memcpy(d, s, n); return 0; }
static inline long copy_fop_to_user(void *d, const void *s, size_t n) { memcpy(d, s, n); return 0; }
static inline void *kmalloc(size_t n) { return malloc(n); }
static inline void kfree(void *p) { free(p); }
void devfs_register_device_gid_priv(const char *name, mode_t mode, gid_t gid, dev_t rdev, vfs_fops_t *fops, void *priv);
void devfs_set_hidden(const char *name, int hidden);
#endif
