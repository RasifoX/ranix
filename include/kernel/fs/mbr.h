#ifndef MBR_H
#define MBR_H

#include <kernel/block.h>

/* Parse MBR of dev, register partitions as vblk0p0, vblk0p1, ... */
int mbr_probe(block_device_t *dev);

#endif
