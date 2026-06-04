#ifndef BLOCK_H
#define BLOCK_H

#include <uapi/types.h>
#include <kernel/sync.h>

#define BLOCK_SIZE 512

typedef struct block_device {
    char     name[32];
    uint64_t size_blocks;

    int (*read)(struct block_device *dev, uint64_t block, void *buf, uint64_t count);
    int (*write)(struct block_device *dev, uint64_t block, const void *buf, uint64_t count);

    void *priv;

    /* the drivers carry per-device scratch state and no internal locking, so all
     * access funnels through block_read/block_write which serialize on this. */
    spinlock_t lock;
    uint64_t   nr_reads;     /* device read transfers issued */
    uint64_t   nr_read_secs; /* sectors moved by those transfers */
} block_device_t;

int  block_device_register(block_device_t *dev);

/* Serialized, counted device I/O. Filesystems and the cache must go through
 * these rather than dev->read/dev->write directly. */
int  block_read(block_device_t *dev, uint64_t lba, void *buf, uint64_t count);
int  block_write(block_device_t *dev, uint64_t lba, const void *buf, uint64_t count);

/* Register a partition slice of parent as "<parent>p<idx>". Reads/writes are
 * offset by start_lba. Shared by the MBR and GPT probers. */
block_device_t *block_partition_add(block_device_t *parent, uint64_t start_lba,
                                    uint64_t nr_sectors, int idx);

block_device_t *block_device_find(const char *name);
int  block_device_count(void);
block_device_t *block_device_get(int index);

#endif
