#include <kernel/block.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>

#define BLKDEV_INIT_CAP 8

static block_device_t **devices;
static int              dev_cnt = 0;
static int              dev_cap = 0;
static spinlock_t       block_lock = SPINLOCK_INIT;

int block_device_register(block_device_t *dev)
{
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
