#include <kernel/fs/gpt.h>
#include <kernel/block.h>
#include <kernel/mm/buddy.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <asm/mmu.h>

/*
 * GUID Partition Table parser (UEFI spec). The header lives at LBA 1 and points
 * at the partition entry array; each entry carries 64-bit start/end LBAs, so
 * GPT is what lets a disk exceed the MBR's 2 TB ceiling. The primary header is
 * CRC32-verified; if it fails we fall back to the backup header at the last LBA
 * (which self-describes its own entry array). Every entry with a non-zero type
 * GUID becomes a partition slice.
 */

#define GPT_SIG       "EFI PART"
#define GPT_MAX_ENTRIES 128

static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* CRC-32 (IEEE 802.3, reflected) — the variant GPT mandates. */
static uint32_t crc32(const uint8_t *p, uint32_t len)
{
	uint32_t crc = 0xFFFFFFFFu;
	for (uint32_t i = 0; i < len; i++) {
		crc ^= p[i];
		for (int b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
	}
	return ~crc;
}

static int guid_is_zero(const uint8_t *g)
{
	for (int i = 0; i < 16; i++) if (g[i]) return 0;
	return 1;
}

/* Read the GPT header at lba into buf, validate signature + header CRC32. */
static int read_valid_header(block_device_t *dev, uint64_t lba, uint8_t *buf)
{
	if (dev->read(dev, lba, buf, 1) != 0) return -1;
	if (memcmp(buf, GPT_SIG, 8) != 0) return -1;

	uint32_t hsize = rd32(buf + 12);
	if (hsize < 92 || hsize > 512) return -1;

	uint32_t stored = rd32(buf + 16);
	memset(buf + 16, 0, 4);                 /* CRC field reads as zero */
	uint32_t calc = crc32(buf, hsize);
	if (calc != stored) return -1;
	return 0;
}

int gpt_probe(block_device_t *dev)
{
	uint64_t sec_phys = (uint64_t)buddy_alloc(0);
	if (!sec_phys) return -1;
	uint8_t *sec = (uint8_t *)PHYS_TO_VIRT(sec_phys);   /* CPU-side buffer */

	int from_backup = 0;
	if (read_valid_header(dev, 1, sec) != 0) {
		uint64_t last = dev->size_blocks ? dev->size_blocks - 1 : 0;
		if (!last || read_valid_header(dev, last, sec) != 0) {
			buddy_free(sec_phys, 0);
			return -1;
		}
		from_backup = 1;
	}

	uint64_t entry_lba   = rd64(sec + 72);
	uint32_t num_entries = rd32(sec + 80);
	uint32_t entry_size  = rd32(sec + 84);
	uint32_t stored_crc  = rd32(sec + 88);
	buddy_free(sec_phys, 0);

	if (entry_size < 128 || entry_size > 512) return -1;
	if (from_backup)
		kprintf("gpt: primary header invalid, using backup at LBA %lu\n",
		        (unsigned long)(dev->size_blocks - 1));

	/* Read the whole entry array into one buffer so we can CRC-verify it and
	 * parse without re-reading. Cap the buffer; arrays beyond it skip the CRC. */
	uint64_t bytes  = (uint64_t)num_entries * entry_size;
	uint64_t sectors = (bytes + 511) / 512;
	uint64_t pages   = (sectors * 512 + 4095) / 4096;
	int order = 0;
	while (((uint64_t)1 << order) < pages) order++;
	if (order > 6) return -1;                       /* > 256 KB array: unsupported */
	uint64_t buf_phys = (uint64_t)buddy_alloc(order);
	if (!buf_phys) return -1;
	uint8_t *arr = (uint8_t *)PHYS_TO_VIRT(buf_phys);

	for (uint64_t s = 0; s < sectors; s++) {
		if (dev->read(dev, entry_lba + s, arr + s * 512, 1) != 0) {
			buddy_free(buf_phys, order);
			return -1;
		}
	}
	if (crc32(arr, (uint32_t)bytes) != stored_crc) {  /* corrupt entry array */
		buddy_free(buf_phys, order);
		return -1;
	}

	if (num_entries > GPT_MAX_ENTRIES) num_entries = GPT_MAX_ENTRIES;
	int found = 0;
	for (uint32_t i = 0; i < num_entries; i++) {
		uint8_t *e = arr + (uint64_t)i * entry_size;
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

	buddy_free(buf_phys, order);
	return found > 0 ? 0 : -1;
}
