/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/fs/mbr.h>
#include <kernel/block.h>
#include <kernel/mm/buddy.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <asm/mmu.h>

/*
 * MBR partition table parser.
 * Reads sector 0, validates 0x55AA signature, registers a partition slice for
 * each used entry. A lone 0xEE protective entry means the disk is GPT — leave
 * it to the GPT prober.
 */

#define MBR_SIGNATURE  0xAA55
#define MBR_PARTS      4
#define MBR_TYPE_GPT   0xEE

typedef struct __attribute__((packed)) {
	uint8_t  status;
	uint8_t  chs_first[3];
	uint8_t  type;
	uint8_t  chs_last[3];
	uint32_t lba_start;
	uint32_t lba_size;
} mbr_entry_t;

typedef struct __attribute__((packed)) {
	uint8_t   boot_code[446];
	mbr_entry_t parts[4];
	uint16_t  signature;
} mbr_t;

int mbr_probe(block_device_t *dev)
{
	uint64_t sec_phys = (uint64_t)buddy_alloc(0);
	if (!sec_phys) return -1;
	uint8_t *sector = (uint8_t *)PHYS_TO_VIRT(sec_phys);   /* CPU-side buffer */

	if (block_read(dev, 0, sector, 1) != 0) {
		buddy_free(sec_phys, 0);
		return -1;
	}

	mbr_t *mbr = (mbr_t *)sector;
	/* A volume boot record ends in the same signature, and its code would read as
	 * partition entries: an NTFS or BitLocker filesystem without a table. */
	if (mbr->signature != MBR_SIGNATURE ||
	    memcmp(sector + 3, "NTFS    ", 8) == 0 || memcmp(sector + 3, "-FVE-FS-", 8) == 0) {
		buddy_free(sec_phys, 0);
		return -1;
	}
	/* A FAT or exFAT stick without a table (a "superfloppy") also ends in 55AA, its
	 * boot code where the entries would be. A real entry's first byte is a boot
	 * flag, 0 or 0x80; code almost never gives four of those. */
	int flags_ok = 1;
	for (int i = 0; i < MBR_PARTS; i++)
		if (mbr->parts[i].status & 0x7F) flags_ok = 0;
	if (!flags_ok || memcmp(sector + 3, "EXFAT   ", 8) == 0) {
		buddy_free(sec_phys, 0);
		return -1;
	}
	uint32_t scale = dev->lba_size > BLOCK_SIZE ? dev->lba_size / BLOCK_SIZE : 1;

	int found = 0;
	for (int i = 0; i < MBR_PARTS; i++) {
		mbr_entry_t *e = &mbr->parts[i];
		if (e->lba_size == 0 || e->type == 0) continue;
		if (e->type == MBR_TYPE_GPT) continue;   /* protective MBR — GPT owns it */

		uint64_t start = (uint64_t)e->lba_start * scale, size = (uint64_t)e->lba_size * scale;
		if (!e->lba_start || start >= dev->size_blocks || size > dev->size_blocks - start) {
			kprintf(KERN_WARN "mbr: %s entry %d outside the disk, skipped\n", dev->name, i);
			continue;
		}
		block_device_t *pd = block_partition_add(dev, start, size, found, NULL);
		if (!pd) break;
		kprintf(KERN_INFO "mbr: %s type=0x%02x start=%u size=%u\n",
		        pd->name, e->type, e->lba_start, e->lba_size);
		found++;
	}

	buddy_free(sec_phys, 0);
	return found > 0 ? 0 : -1;
}
