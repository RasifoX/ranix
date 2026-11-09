/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * Virtio over PCI, the "modern" (virtio 1.x) transport: the device's registers
 * live in memory BARs that vendor capabilities point at. Devices that exist
 * only in this form (virtio-gpu) use it; virtio-net and virtio-blk still use
 * the legacy I/O-port transport. Spec: OASIS virtio 1.2, 4.1.4 and 2.7.
 *
 * A driver finds its device, starts it with the features it wants, sets up its
 * queues, says ready, and then owns the descriptors itself: the transport only
 * puts a chain head on the available ring, rings the doorbell and hands back
 * what the device finished.
 */
#ifndef DRIVERS_VIRTIO_PCI_H
#define DRIVERS_VIRTIO_PCI_H

#include <uapi/types.h>

#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2       /* the device writes this buffer */

typedef struct {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
} __attribute__((packed)) virtq_desc_t;

typedef struct {
	uint16_t size;                 /* entries, power of two */
	uint16_t index;                /* queue number on the device */
	uint16_t last_used;            /* used entries handed back so far */
	virtq_desc_t      *desc;
	volatile uint16_t *avail;      /* flags, idx, ring[size] */
	volatile uint8_t  *used;       /* flags, idx, {id, len}[size] */
	volatile uint16_t *doorbell;
} virtq_t;

typedef struct {
	uint8_t bus, dev, fn;
	volatile uint8_t *common;      /* common configuration */
	volatile uint8_t *device;      /* device-specific configuration */
	volatile uint8_t *notify;      /* doorbells */
	uint32_t notify_mul;
	uint64_t features;             /* what both sides agreed on */
} virtio_pci_t;

/* Find the first modern PCI function of virtio device type `type` (16 = GPU)
 * and map its registers. 0 on success. */
int  virtio_pci_find(uint16_t type, virtio_pci_t *vp);
/* Reset, then offer `want` (VIRTIO_F_VERSION_1 is always added). 0 when the
 * device accepted; vp->features holds the result. */
int  virtio_pci_start(virtio_pci_t *vp, uint64_t want);
/* Queue `index` with at most `size` entries in one zeroed page. 0 on success. */
int  virtio_pci_queue(virtio_pci_t *vp, uint16_t index, uint16_t size, virtq_t *q);
void virtio_pci_ready(virtio_pci_t *vp);      /* DRIVER_OK: the device may run */
void virtio_pci_fail(virtio_pci_t *vp);       /* FAILED: we gave up on it */

/* Offer the chain starting at descriptor `head` and ring the doorbell. */
void virtq_kick(virtq_t *q, uint16_t head);
/* The next chain the device finished: 1 and its head (and bytes written), or 0. */
int  virtq_pop(virtq_t *q, uint16_t *head, uint32_t *len);

#endif
