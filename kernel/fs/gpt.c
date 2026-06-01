#include <kernel/fs/gpt.h>
#include <kernel/block.h>
#include <kernel/mm/buddy.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <asm/mmu.h>

/*
 * GUID Partition Table parser (UEFI spec). The header lives at LBA 1 and points
 * at the partition entry array; each entry carries 64-bit start/end LBAs, so
 * GPT is what lets a disk exceed the MBR's 2 TB ceiling. We trust the primary
 * header (no CRC verification / backup fallback yet) and register every entry
 * with a non-zero type GUID as a partition slice.
 */

#define GPT_SIG       "EFI PART"
#define GPT_MAX_ENTRIES 128

static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static int guid_is_zero(const uint8_t *g)
{
	for (int i = 0; i < 16; i++) if (g[i]) return 0;
	return 1;
}

int gpt_probe(block_device_t *dev)
{
	uint64_t sec_phys = (uint64_t)buddy_alloc(0);
	if (!sec_phys) return -1;
	uint8_t *sec = (uint8_t *)PHYS_TO_VIRT(sec_phys);   /* CPU-side buffer */

	if (dev->read(dev, 1, sec, 1) != 0) { buddy_free(sec_phys, 0); return -1; }
	if (memcmp(sec, GPT_SIG, 8) != 0)   { buddy_free(sec_phys, 0); return -1; }

	uint64_t entry_lba   = rd64(sec + 72);
	uint32_t num_entries = rd32(sec + 80);
	uint32_t entry_size  = rd32(sec + 84);

	if (entry_size < 128 || entry_size > 512) { buddy_free((uint64_t)sec, 0); return -1; }
	if (num_entries > GPT_MAX_ENTRIES) num_entries = GPT_MAX_ENTRIES;

	uint32_t per_sector = 512 / entry_size;
	int found = 0;

	for (uint32_t i = 0; i < num_entries; i++) {
		uint32_t sec_idx = i / per_sector;
		uint32_t in_sec  = i % per_sector;

		if (in_sec == 0) {
			if (dev->read(dev, entry_lba + sec_idx, sec, 1) != 0) break;
		}
		uint8_t *e = sec + in_sec * entry_size;

		if (guid_is_zero(e)) continue;           /* unused slot */
		uint64_t first = rd64(e + 32);
		uint64_t last  = rd64(e + 40);
		if (last < first) continue;

		block_device_t *pd = block_partition_add(dev, first, last - first + 1, found);
		if (!pd) break;
		kprintf("gpt: %s start=%lu size=%lu\n",
		        pd->name, (unsigned long)first, (unsigned long)(last - first + 1));
		found++;
	}

	buddy_free(sec_phys, 0);
	return found > 0 ? 0 : -1;
}
