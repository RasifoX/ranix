#include <kernel/block.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/vfs.h>
#include <kernel/fs/devfs.h>

#define BLKDEV_INIT_CAP 8

static block_device_t **devices;
static int              dev_cnt = 0;
static int              dev_cap = 0;
static spinlock_t       block_lock = SPINLOCK_INIT;

/* byte-addressable view of a block device, exposed as a /dev/<name> node so
 * mount(2), dd and friends can open the disk by path (devtmpfs behaviour). */
static ssize_t blkdev_read(vfs_file_t *f, void *buf, size_t len)
{
    block_device_t *bd = f->inode ? f->inode->priv : NULL;
    if (!bd || !bd->read) return -5;
    uint64_t total = bd->size_blocks * BLOCK_SIZE;
    uint64_t pos = (uint64_t)f->pos;
    if (pos >= total) return 0;
    if (len > total - pos) len = (size_t)(total - pos);

    uint8_t  sect[BLOCK_SIZE];
    uint8_t *out  = buf;
    size_t   done = 0;
    while (done < len) {
        uint64_t lba = (pos + done) / BLOCK_SIZE;
        uint32_t off = (uint32_t)((pos + done) % BLOCK_SIZE);
        if (bd->read(bd, lba, sect, 1) != 0)
            return done ? (ssize_t)done : -5;
        size_t chunk = BLOCK_SIZE - off;
        if (chunk > len - done) chunk = len - done;
        memcpy(out + done, sect + off, chunk);
        done += chunk;
    }
    f->pos += (off_t)done;
    return (ssize_t)done;
}

static ssize_t blkdev_write(vfs_file_t *f, const void *buf, size_t len)
{
    block_device_t *bd = f->inode ? f->inode->priv : NULL;
    if (!bd || !bd->write || !bd->read) return -5;
    uint64_t total = bd->size_blocks * BLOCK_SIZE;
    uint64_t pos = (uint64_t)f->pos;
    if (pos >= total) return -28; /* ENOSPC */
    if (len > total - pos) len = (size_t)(total - pos);

    uint8_t        sect[BLOCK_SIZE];
    const uint8_t *in   = buf;
    size_t         done = 0;
    while (done < len) {
        uint64_t lba   = (pos + done) / BLOCK_SIZE;
        uint32_t off   = (uint32_t)((pos + done) % BLOCK_SIZE);
        size_t   chunk = BLOCK_SIZE - off;
        if (chunk > len - done) chunk = len - done;
        if (chunk != BLOCK_SIZE && bd->read(bd, lba, sect, 1) != 0)
            return done ? (ssize_t)done : -5;
        memcpy(sect + off, in + done, chunk);
        if (bd->write(bd, lba, sect, 1) != 0)
            return done ? (ssize_t)done : -5;
        done += chunk;
    }
    f->pos += (off_t)done;
    return (ssize_t)done;
}

static vfs_fops_t blkdev_fops = {
    .read  = blkdev_read,
    .write = blkdev_write,
};

static int blkdev_minor = 0;

int block_device_register(block_device_t *dev)
{
    devfs_register_device_priv(dev->name, S_IFBLK | 0660,
                               (dev_t)((8 << 8) | (blkdev_minor++ & 0xff)),
                               &blkdev_fops, dev);

    uint64_t f = spin_lock_irq(&block_lock);
    if (dev_cnt >= dev_cap) {
        int new_cap = dev_cap ? dev_cap * 2 : BLKDEV_INIT_CAP;
        block_device_t **nb = kmalloc((size_t)new_cap * sizeof(block_device_t *));
        if (!nb) { spin_unlock_irq(&block_lock, f); return -1; }
        if (devices) {
            memcpy(nb, devices, (size_t)dev_cnt * sizeof(block_device_t *));
            kfree(devices);
        }
        devices = nb;
        dev_cap = new_cap;
    }
    devices[dev_cnt++] = dev;
    spin_unlock_irq(&block_lock, f);
    return 0;
}

typedef struct {
    block_device_t *parent;
    uint64_t        start;
} part_priv_t;

static int part_read(block_device_t *dev, uint64_t lba, void *buf, uint64_t count)
{
    part_priv_t *p = dev->priv;
    return p->parent->read(p->parent, p->start + lba, buf, count);
}

static int part_write(block_device_t *dev, uint64_t lba, const void *buf, uint64_t count)
{
    part_priv_t *p = dev->priv;
    return p->parent->write(p->parent, p->start + lba, buf, count);
}

block_device_t *block_partition_add(block_device_t *parent, uint64_t start_lba,
                                    uint64_t nr_sectors, int idx)
{
    part_priv_t    *pp = kmalloc(sizeof(*pp));
    block_device_t *pd = kmalloc(sizeof(*pd));
    if (!pp || !pd) { kfree(pp); kfree(pd); return NULL; }

    pp->parent = parent;
    pp->start  = start_lba;

    memset(pd, 0, sizeof(*pd));
    int n = 0;
    while (parent->name[n] && n < 28) { pd->name[n] = parent->name[n]; n++; }
    pd->name[n++] = 'p';
    if (idx >= 10) pd->name[n++] = (char)('0' + idx / 10);
    pd->name[n++] = (char)('0' + idx % 10);
    pd->name[n]   = '\0';

    pd->size_blocks = nr_sectors;
    pd->read        = part_read;
    pd->write       = part_write;
    pd->priv        = pp;

    if (block_device_register(pd) != 0) { kfree(pp); kfree(pd); return NULL; }
    return pd;
}

block_device_t *block_device_find(const char *name)
{
    uint64_t f = spin_lock_irq(&block_lock);
    for (int i = 0; i < dev_cnt; i++) {
        if (strcmp(devices[i]->name, name) == 0) {
            spin_unlock_irq(&block_lock, f);
            return devices[i];
        }
    }
    spin_unlock_irq(&block_lock, f);
    return NULL;
}

int block_device_count(void)
{
    return dev_cnt;
}

block_device_t *block_device_get(int index)
{
    if (index < 0 || index >= dev_cnt) return NULL;
    return devices[index];
}
