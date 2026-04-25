#include <kernel/block.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>

#define MAX_BLOCK_DEVICES 8

static block_device_t *devices[MAX_BLOCK_DEVICES];
static int             dev_cnt = 0;
static spinlock_t      block_lock = SPINLOCK_INIT;

int block_device_register(block_device_t *dev)
{
    uint64_t f = spin_lock_irq(&block_lock);
    if (dev_cnt >= MAX_BLOCK_DEVICES) {
        spin_unlock_irq(&block_lock, f);
        return -1;
    }
    devices[dev_cnt++] = dev;
    spin_unlock_irq(&block_lock, f);
    return 0;
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
