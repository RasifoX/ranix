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

	if (dev->read(dev, 0, sector, 1) != 0) {
		buddy_free(sec_phys, 0);
		return -1;
	}

	mbr_t *mbr = (mbr_t *)sector;
	if (mbr->signature != MBR_SIGNATURE) {
		buddy_free(sec_phys, 0);
		return -1;
	}

	int found = 0;
	for (int i = 0; i < MBR_PARTS; i++) {
		mbr_entry_t *e = &mbr->parts[i];
		if (e->lba_size == 0 || e->type == 0) continue;
		if (e->type == MBR_TYPE_GPT) continue;   /* protective MBR — GPT owns it */

		block_device_t *pd = block_partition_add(dev, e->lba_start, e->lba_size, found);
		if (!pd) break;
		kprintf("mbr: %s type=0x%02x start=%u size=%u\n",
		        pd->name, e->type, e->lba_start, e->lba_size);
		found++;
	}

	buddy_free(sec_phys, 0);
	return found > 0 ? 0 : -1;
}
