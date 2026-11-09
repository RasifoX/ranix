/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* gpt.c and mbr.c against mutated disk heads, under ASan and UBSan: nothing they parse may
 * put a partition outside the disk. usage: gpt_host DISK.img SEED ROUNDS [LBASIZE] */
#include "shim.h"
#include "kernel/fs/gpt.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static uint8_t *img;
static uint64_t sectors, hits, parts;

int block_read(block_device_t *d, uint64_t lba, void *buf, uint64_t n)
{
	(void)d;
	if (lba >= sectors || n > sectors - lba) return -5;
	memcpy(buf, img + lba * 512, n * 512);
	hits++;
	return 0;
}

block_device_t *block_partition_add(block_device_t *p, uint64_t start, uint64_t n, int idx,
                                    const block_part_info_t *info)
{
	static block_device_t dummy;
	(void)idx;
	if (!n || start >= p->size_blocks || n > p->size_blocks - start) abort();
	if (info && strlen(info->name) > BLOCK_PART_NAME_MAX) abort();
	parts++;
	return &dummy;
}

void block_guid_text(const uint8_t *g, char *out) { (void)g; strcpy(out, "x"); }
void block_set_gpt_disk(block_device_t *d, const char *g) { strcpy(d->gpt_disk, g); }
void *buddy_alloc(int order) { return malloc(4096u << order); }
void buddy_free(uint64_t p, int order) { (void)order; free((void *)(uintptr_t)p); }

uint32_t crc32(const void *data, size_t n)
{
	uint32_t c = 0xFFFFFFFFu;
	const uint8_t *p = data;
	while (n--) {
		c ^= *p++;
		for (int i = 0; i < 8; i++) c = c >> 1 ^ (0xEDB88320u & -(c & 1));
	}
	return ~c;
}

/* Keep a mutated header's CRC valid half the time, or a fuzz never gets past it. */
static void reseal(uint32_t hsec)
{
	uint8_t *h = img + (uint64_t)hsec * 512;
	if (memcmp(h, "EFI PART", 8)) return;
	uint32_t hs;
	memcpy(&hs, h + 12, 4);
	if (hs < 92 || hs > 512) return;
	memset(h + 16, 0, 4);
	uint32_t c = crc32(h, hs);
	memcpy(h + 16, &c, 4);
}

int main(int argc, char **argv)
{
	if (argc < 4) return 2;
	int fd = open(argv[1], O_RDONLY);
	struct stat st;
	fstat(fd, &st);
	img = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_NORESERVE, fd, 0);
	sectors = (uint64_t)st.st_size / 512;
	uint64_t r = strtoull(argv[2], NULL, 0) * 0x9E3779B97F4A7C15ULL + 1, rounds = strtoull(argv[3], NULL, 0);
	static uint8_t save[34 * 512], tail[33 * 512];
	memcpy(save, img, sizeof(save));
	memcpy(tail, img + (sectors - 33) * 512, sizeof(tail));
	block_device_t d;
	for (uint64_t it = 0; it < rounds; it++) {
		memset(&d, 0, sizeof(d));
		strcpy(d.name, "t");
		d.size_blocks = sectors;
		d.lba_size = argc > 4 ? (uint32_t)atoi(argv[4]) : 512;
		int ops = 1;
		#define RND() (r ^= r << 13, r ^= r >> 7, r ^= r << 17, r)
		ops += (int)(RND() % 12);
		for (int k = 0; k < ops; k++) {
			uint32_t off = (uint32_t)(RND() % (34 * 512));
			if (RND() & 1) off = 512 + (uint32_t)(RND() % 96);   /* the header itself */
			uint8_t v = (uint8_t)(RND() % 3 ? RND() : (RND() & 1 ? 0xFF : 0));
			img[off] = v;
		}
		if (RND() & 1) reseal(1);
		if (!(RND() % 4)) {             /* entries with a good CRC too */
			uint32_t c = 0, n, sz;
			memcpy(&n, img + 512 + 80, 4);
			memcpy(&sz, img + 512 + 84, 4);
			if ((uint64_t)n * sz <= 32 * 512) {
				c = crc32(img + 1024, n * sz);
				memcpy(img + 512 + 88, &c, 4);
				reseal(1);
			}
		}
		if (gpt_probe(&d) != 0) mbr_probe(&d);
		memcpy(img, save, sizeof(save));
		memcpy(img + (sectors - 33) * 512, tail, sizeof(tail));
	}
	printf("gpt fuzz %llu rounds, %llu reads, %llu partitions accepted, all inside the disk\n",
	       (unsigned long long)rounds, (unsigned long long)hits, (unsigned long long)parts);
	return 0;
}
