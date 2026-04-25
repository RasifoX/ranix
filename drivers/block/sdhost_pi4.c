#include <kernel/block.h>
#include <kernel/board.h>
#include <kernel/kprintf.h>
#include <kernel/mm/vm.h>
#include <kernel/lib/string.h>

/* Very basic SD Card (EMMC2) skeleton for RPi4 */
typedef struct {
    uint64_t base;
} pi4_sd_t;

static int pi4_sd_read(block_device_t *dev, uint64_t block, void *buf, uint64_t count)
{
    (void)dev; (void)block; (void)buf; (void)count;
    /* Real EMMC CMD17/CMD18 would go here */
    return -1;
}

static int pi4_sd_write(block_device_t *dev, uint64_t block, const void *buf, uint64_t count)
{
    (void)dev; (void)block; (void)buf; (void)count;
    return -1;
}

static block_device_t sd_dev;
static pi4_sd_t sd_priv;

void pi4_sd_init(void)
{
    board_config_t *cfg = board_get_config();
    if (cfg->sd_base == 0) return;

    /* Map EMMC2 registers */
    sd_priv.base = (uint64_t)ioremap(cfg->sd_base, 0x1000, VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);
    
    memset(&sd_dev, 0, sizeof(sd_dev));
    strcpy(sd_dev.name, "sd0");
    sd_dev.size_blocks = 0; /* Should be detected during init */
    sd_dev.read  = pi4_sd_read;
    sd_dev.write = pi4_sd_write;
    sd_dev.priv  = &sd_priv;

    if (block_device_register(&sd_dev) == 0) {
        kprintf("[  drv] sdcard: registered rpi4 emmc2\n");
    }
}
