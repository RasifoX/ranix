/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* What drivers/sound/hda.c needs from the kernel, for the host model in
 * tests/hda_host/model.c. Every kernel header in stub/ is this one file. */
#ifndef HDA_MODEL_H
#define HDA_MODEL_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#define TIMER_HZ 1000
#define KERN_INFO ""
#define KERN_WARN ""
#define VM_READ 1
#define VM_WRITE 2
#define VM_NOCACHE 16
#define VM_DEVICE 32
#define DEVFS_GID_AUDIO 18
#define ENOTTY 25
#define PHYS_TO_VIRT(p) ((uint64_t)(uintptr_t)(p))
typedef struct { int x; } spinlock_t;
#define SPINLOCK_INIT { 0 }
typedef struct { int flags; } vfs_file_t;
typedef struct { int x; } vfs_inode_t;
typedef struct {
	int (*open)(vfs_inode_t *, vfs_file_t *, int);
	ssize_t (*write)(vfs_file_t *, const void *, size_t);
	int (*ioctl)(vfs_file_t *, uint32_t, uint64_t);
	int (*close)(vfs_file_t *);
	int (*poll)(vfs_file_t *);
} vfs_fops_t;
extern volatile uint64_t kernel_ticks;
static inline uint64_t spin_lock_irq(spinlock_t *l) { (void)l; return 0; }
static inline void spin_unlock_irq(spinlock_t *l, uint64_t f) { (void)l; (void)f; }
uint8_t inb(uint16_t port);
void kprintf(const char *fmt, ...);
int pci_find_class(uint8_t cls, uint8_t sub, uint8_t pi, uint8_t *b, uint8_t *d, uint8_t *f);
uint16_t pci_read16(uint8_t b, uint8_t d, uint8_t f, uint8_t reg);
uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t reg);
void pci_write16(uint8_t b, uint8_t d, uint8_t f, uint8_t reg, uint16_t v);
void *ioremap(uint64_t phys, size_t size, uint32_t flags);
void *buddy_alloc(int order);
int devfs_register_device_gid(const char *name, uint32_t mode, int gid, uint32_t dev, vfs_fops_t *f);
pid_t proc_create(const char *name, void (*fn)(void));
void task_sleep_until(uint64_t deadline);
int task_has_actionable_signal(void);
int copy_fop_from_user(void *dst, const void *src, size_t n);
int copy_from_user(void *dst, const void *src, size_t n);
int copy_to_user(void *dst, const void *src, size_t n);
typedef struct { char *p; size_t rem; } hw_out_t;
typedef void (*hwdiag_fn)(hw_out_t *);
void hwdiag_register(hwdiag_fn fn);
void hw_begin(hw_out_t *o, const char *cls, const char *name);
void hw_kv(hw_out_t *o, const char *key, uint64_t v);
void hw_end(hw_out_t *o);
const char *cmdline_get(const char *key, char *buf, size_t bufsz);
#endif
