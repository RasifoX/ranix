/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* What drivers/platform/casper_ec.c needs from the kernel, for the host model in
 * tests/casper_ec_host/model.c. */
#ifndef CASPER_EC_MODEL_H
#define CASPER_EC_MODEL_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#define PCI_VENDOR_ID 0x00
#define VM_READ    1
#define VM_WRITE   2
#define VM_NOCACHE 16
#define VM_DEVICE  32
#define TIMER_HZ   1000
#define KERN_INFO  ""
#define KERN_WARN  ""
typedef struct { int locked; } mutex_t;
#define MUTEX_INIT { 0 }
extern volatile uint64_t kernel_ticks;
uint32_t    pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
void       *ioremap(uint64_t phys, size_t size, uint32_t flags);
void        kprintf(const char *fmt, ...);
int         mutex_lock_killable(mutex_t *m);
void        mutex_unlock(mutex_t *m);
void        task_sleep_until(uint64_t deadline);
pid_t       proc_create(const char *name, void (*fn)(void));
const char *cmdline_get(const char *key, char *buf, size_t bufsz);
int         arch_fw_lock(uint32_t timeout_ms);
void        arch_fw_unlock(void);
int         arch_fw_oem(char oem[7], char table[9]);
void        casper_ec_init(void);
#endif
