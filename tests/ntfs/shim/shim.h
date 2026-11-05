#pragma once
/* Just enough of the kernel for gpt.c and mbr.c under ASan on the host. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define KERN_INFO ""
#define KERN_WARN ""
#define KERN_ERR ""
#define kprintf(...) ((void)0)
#define PHYS_TO_VIRT(x) (x)
#define BLOCK_SIZE 512
#define BLOCK_PART_NAME_MAX 36
typedef struct block_device {
	char name[32];
	uint64_t size_blocks;
	uint32_t lba_size;
	void *priv;
	char gpt_disk[37];
} block_device_t;
typedef struct {
	const char *name;
	const uint8_t *type_guid, *part_guid;
	int kernel_only;
} block_part_info_t;
int block_read(block_device_t *dev, uint64_t lba, void *buf, uint64_t count);
block_device_t *block_partition_add(block_device_t *parent, uint64_t start, uint64_t n, int idx,
                                    const block_part_info_t *info);
void block_guid_text(const uint8_t *g, char *out);
void block_set_gpt_disk(block_device_t *dev, const char *guid);
void *buddy_alloc(int order);
void buddy_free(uint64_t p, int order);
uint32_t crc32(const void *d, size_t n);
