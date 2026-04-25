#include <kernel/block.h>
#include <kernel/kprintf.h>
#include <kernel/lib/string.h>

/* Very basic Virtio-BLK skeleton for x86_64 */
static int virtio_blk_read(block_device_t *dev, uint64_t block, void *buf, uint64_t count)
{
    (void)dev; (void)block; (void)buf; (void)count;
    return -1;
}

static int virtio_blk_write(block_device_t *dev, uint64_t block, const void *buf, uint64_t count)
{
    (void)dev; (void)block; (void)buf; (void)count;
    return -1;
}

static block_device_t vblk_dev;

void virtio_blk_init(void)
{
    /* In a real OS, we would scan PCI here */
    memset(&vblk_dev, 0, sizeof(vblk_dev));
    strcpy(vblk_dev.name, "vblk0");
    vblk_dev.read  = virtio_blk_read;
    vblk_dev.write = virtio_blk_write;

    if (block_device_register(&vblk_dev) == 0) {
        kprintf("[  drv] virtio-blk: registered\n");
    }
}
