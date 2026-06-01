#ifndef BLOCK_H
#define BLOCK_H

#include <uapi/types.h>

#define BLOCK_SIZE 512

typedef struct block_device {
    char     name[32];
    uint64_t size_blocks;
    
    int (*read)(struct block_device *dev, uint64_t block, void *buf, uint64_t count);
    int (*write)(struct block_device *dev, uint64_t block, const void *buf, uint64_t count);
    
    void *priv;
} block_device_t;

int  block_device_register(block_device_t *dev);

/* Register a partition slice of parent as "<parent>p<idx>". Reads/writes are
 * offset by start_lba. Shared by the MBR and GPT probers. */
block_device_t *block_partition_add(block_device_t *parent, uint64_t start_lba,
                                    uint64_t nr_sectors, int idx);

block_device_t *block_device_find(const char *name);
int  block_device_count(void);
block_device_t *block_device_get(int index);

#endif
