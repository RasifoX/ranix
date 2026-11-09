/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef BLOCK_H
#define BLOCK_H

#include <uapi/types.h>
#include <kernel/sync.h>

#define BLOCK_SIZE 512
#define BLOCK_PART_NAME_MAX 36   /* GPT entry name, in UTF-16 code units */

typedef struct block_device {
    char     name[32];
    uint64_t size_blocks;

    int (*read)(struct block_device *dev, uint64_t block, void *buf, uint64_t count);
    /* After registration this is the guarded entry, so even a direct call meets the
     * read-only flag. The driver's own routine moves to drv_write, private to block.c. */
    int (*write)(struct block_device *dev, uint64_t block, const void *buf, uint64_t count);
    int (*drv_write)(struct block_device *dev, uint64_t block, const void *buf, uint64_t count);
    int (*flush)(struct block_device *dev);   /* drain the drive cache; NULL if none */
    volatile int dirty;                       /* writes since the last flush */

    void *priv;

    /* Write protection. Every disk starts read-only unless ranix.disk.rw names it. */
    volatile int ro;
    uint64_t     ro_denied;                   /* writes refused since boot */
    struct block_device *parent;              /* the disk of a partition, else NULL */
    uint32_t     lba_size;                    /* device logical block, 0 meaning 512 */
    char         serial[48];                  /* drive serial, empty if unknown */
    char         gpt_disk[37];                /* GPT disk GUID, lower case */
    char         part_uuid[37];               /* GPT partition GUID, lower case */

    char    part_name[BLOCK_PART_NAME_MAX + 1];   /* GPT label, empty if none */
    uint8_t part_type[16];                        /* GPT type GUID */
    int     kernel_only;                          /* no device node, not mountable */

    /* the drivers carry per-device scratch state and no internal locking, so all
     * access funnels through block_read/block_write which serialize on this.
     * Task context only: a drive cache flush holds it for as long as it takes. */
    mutex_t    lock;
    uint64_t   nr_reads;     /* device read transfers issued */
    uint64_t   nr_read_secs; /* sectors moved by those transfers */

    /* Mounts and open device nodes. A partition counts on its disk too, so a disk
     * at zero has nothing of it in use: a hot-plug driver may then give the
     * registered device to new hardware instead of registering another. */
    volatile int users;
} block_device_t;

int  block_device_register(block_device_t *dev);
int  block_is_device(const void *p);
void block_get(block_device_t *dev);
void block_put(block_device_t *dev);
/* A registered disk now stands for other media: its partitions empty until probed
 * again, its protection back as for a new disk. The caller has set name, size,
 * serial and lba_size. */
void block_device_rebind(block_device_t *dev);

/* Serialized, counted device I/O. Filesystems and the cache must go through
 * these rather than dev->read/dev->write directly. */
int  block_read(block_device_t *dev, uint64_t lba, void *buf, uint64_t count);
int  block_write(block_device_t *dev, uint64_t lba, const void *buf, uint64_t count);
int  block_flush_all(void);   /* make completed writes durable; first error wins */

typedef struct {
    const char    *name;        /* NUL-terminated GPT label */
    const uint8_t *type_guid;   /* 16 bytes */
    const uint8_t *part_guid;   /* 16 bytes, on-disk order */
    int            kernel_only;
} block_part_info_t;

/* A drive's serial from its identify data: space padded, trimmed here. */
void block_copy_serial(char *out, size_t cap, const char *raw, size_t len);
/* Text form of a GUID as stored on disk (first three fields little-endian). */
void block_guid_text(const uint8_t *g, char *out);
/* The GPT disk GUID of dev is known: record it and honour the allow list. */
void block_set_gpt_disk(block_device_t *dev, const char *guid);
/* Newline-separated identities the allow list and the allow command accept. */
int  block_device_ids(const block_device_t *dev, char *buf, int cap);
/* Lift or set protection. block_allow needs one of dev's identities in full. */
int  block_allow(block_device_t *dev, const char *id);
void block_protect(block_device_t *dev);

/* Register a partition slice of parent as "<parent>p<idx>". Reads/writes are
 * offset by start_lba. Shared by the MBR and GPT probers; MBR has no info. */
block_device_t *block_partition_add(block_device_t *parent, uint64_t start_lba,
                                    uint64_t nr_sectors, int idx,
                                    const block_part_info_t *info);

block_device_t *block_device_find(const char *name);
block_device_t *block_device_find_part(const char *label);   /* by GPT label */
int  block_device_count(void);
block_device_t *block_device_get(int index);

#endif
