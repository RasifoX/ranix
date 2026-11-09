/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/fs/gpt.h>
#include <kernel/block.h>
#include <kernel/mm/buddy.h>
#include <kernel/lib/string.h>
#include <kernel/lib/crc32.h>
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

/* Nothing here writes: a bad primary header is read around, never repaired. */

static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* The log partition the kernel keeps for itself: no device node, root included. */
#define GPT_KERNEL_ONLY_NAME "ranix-sesslog"

/* Entry name is UTF-16LE. Only ASCII labels are matched, so the rest may degrade. */
static void gpt_name(const uint8_t *e, char *out)
{
	const uint8_t *u = e + 56;
	int i = 0;
	for (; i < BLOCK_PART_NAME_MAX; i++) {
		uint16_t c = (uint16_t)(u[2 * i] | u[2 * i + 1] << 8);
		if (!c) break;
		out[i] = c < 0x80 ? (char)c : '?';
	}
	out[i] = '\0';
}

static int guid_is_zero(const uint8_t *g)
{
	for (int i = 0; i < 16; i++) if (g[i]) return 0;
	return 1;
}

/* Read the GPT header at device LBA lba into buf, validate signature + header CRC32.
 * LBAs in a GPT count the disk's logical blocks; ours count 512 bytes, so scale. */
static int read_valid_header(block_device_t *dev, uint64_t lba, uint32_t scale, uint8_t *buf)
{
	if (block_read(dev, lba * scale, buf, 1) != 0) return -1;
	if (memcmp(buf, GPT_SIG, 8) != 0) return -1;

	uint32_t hsize = rd32(buf + 12);
	if (hsize < 92 || hsize > 512) return -1;

	uint32_t stored = rd32(buf + 16);
	memset(buf + 16, 0, 4);                 /* CRC field reads as zero */
	uint32_t calc = crc32(buf, hsize);
	if (calc != stored) return -1;
	if (rd64(buf + 24) != lba) return -1;        /* the header must say where it is */
	return 0;
}

int gpt_probe(block_device_t *dev)
{
	uint64_t sec_phys = (uint64_t)buddy_alloc(0);
	if (!sec_phys) return -1;
	uint8_t *sec = (uint8_t *)PHYS_TO_VIRT(sec_phys);   /* CPU-side buffer */

	uint32_t scale = dev->lba_size > BLOCK_SIZE ? dev->lba_size / BLOCK_SIZE : 1;
	uint64_t dev_lbas = dev->size_blocks / scale;
	int from_backup = 0;
	if (read_valid_header(dev, 1, scale, sec) != 0) {
		uint64_t last = dev_lbas ? dev_lbas - 1 : 0;
		if (!last || read_valid_header(dev, last, scale, sec) != 0) {
			buddy_free(sec_phys, 0);
			return -1;
		}
		from_backup = 1;
	}

	uint64_t entry_lba   = rd64(sec + 72);
	uint32_t num_entries = rd32(sec + 80);
	uint32_t entry_size  = rd32(sec + 84);
	uint32_t stored_crc  = rd32(sec + 88);
	char     disk_guid[37];
	block_guid_text(sec + 56, disk_guid);
	buddy_free(sec_phys, 0);

	if (entry_size < 128 || entry_size > 512 || (entry_size & 7)) return -1;
	if (!num_entries || num_entries > (1u << 20)) return -1;
	if (from_backup)
		kprintf(KERN_WARN "gpt: primary header invalid, using backup at LBA %lu\n",
		        (unsigned long)(dev_lbas - 1));

	/* Read the whole entry array into one buffer so we can CRC-verify it and
	 * parse without re-reading. Cap the buffer; arrays beyond it skip the CRC. */
	uint64_t bytes  = (uint64_t)num_entries * entry_size;
	uint64_t sectors = (bytes + 511) / 512;
	uint64_t pages   = (sectors * 512 + 4095) / 4096;
	int order = 0;
	while (((uint64_t)1 << order) < pages) order++;
	if (order > 6) return -1;                       /* > 256 KB array: unsupported */
	if (entry_lba >= dev_lbas || sectors > (dev_lbas - entry_lba) * scale) return -1;
	uint64_t buf_phys = (uint64_t)buddy_alloc(order);
	if (!buf_phys) return -1;
	uint8_t *arr = (uint8_t *)PHYS_TO_VIRT(buf_phys);

	for (uint64_t s = 0; s < sectors; s++) {
		if (block_read(dev, entry_lba * scale + s, arr + s * 512, 1) != 0) {
			buddy_free(buf_phys, order);
			return -1;
		}
	}
	if (crc32(arr, (uint32_t)bytes) != stored_crc) {  /* corrupt entry array */
		buddy_free(buf_phys, order);
		return -1;
	}

	block_set_gpt_disk(dev, disk_guid);
	if (num_entries > GPT_MAX_ENTRIES) num_entries = GPT_MAX_ENTRIES;
	int found = 0;
	for (uint32_t i = 0; i < num_entries; i++) {
		uint8_t *e = arr + (uint64_t)i * entry_size;
		if (guid_is_zero(e)) continue;           /* unused slot */
		uint64_t first = rd64(e + 32);
		uint64_t last  = rd64(e + 40);
		if (last < first || last >= dev_lbas) {
			kprintf(KERN_WARN "gpt: %s entry %u outside the disk, skipped\n", dev->name, i);
			continue;
		}

		char label[BLOCK_PART_NAME_MAX + 1];
		gpt_name(e, label);
		block_part_info_t info = { .name = label, .type_guid = e, .part_guid = e + 16,
		                           .kernel_only = strcmp(label, GPT_KERNEL_ONLY_NAME) == 0 };
		block_device_t *pd = block_partition_add(dev, first * scale, (last - first + 1) * scale,
		                                         found, &info);
		if (!pd) break;
		kprintf(KERN_INFO "gpt: %s start=%lu size=%lu label=%s%s\n",
		        pd->name, (unsigned long)first, (unsigned long)(last - first + 1),
		        label, info.kernel_only ? " kernel-only" : "");
		found++;
	}

	buddy_free(buf_phys, order);
	return found > 0 ? 0 : -1;
}
