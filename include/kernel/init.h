/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef KERNEL_INIT_H
#define KERNEL_INIT_H

/* Boot entry points that have no subsystem header of their own, called once
 * from kernel_main() (kernel/main.c) or the board code, and the values the boot
 * code hands over. Read top to bottom, this is what boot starts. */

#include <uapi/types.h>

extern char     __kernel_phys_end;               /* linker script */
extern uint64_t _dtb_ptr;                        /* boot.S: device tree from firmware */
extern uint64_t boot_initrd_start;               /* initramfs found by the boot code */
extern uint64_t boot_initrd_size;

void initrd_init(void);
void fbdev_init(void);                           /* /dev/fb0 */
void drm_init(void);                             /* /dev/dri: KMS on the firmware's mode */
void thermal_guard_init(void);                   /* package temperature limit */
void casper_ec_init(void);                       /* Casper EC "OS present" flags, off by default */
void rescue_init(void);                          /* udp rescue channel + /dev/rescue */
void claim_init(void);                           /* /dev/claim: visible resource claims */
void pty_init(void);

/* platform drivers, started from board_drivers_register() */
void virtio_net_pci_init(void);
void ahci_init(void);
void nvme_init(void);
void xhci_init(void);
void rtl8168_init(void);
void rtw8723_init(void);
void bcmgenet_init(void);                        /* Pi 4 built-in gigabit ethernet */

#endif
