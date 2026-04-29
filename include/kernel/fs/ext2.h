#ifndef EXT2_H
#define EXT2_H

#include <kernel/vfs.h>
#include <kernel/block.h>

void ext2_register(void);
int  ext2_mount(vfs_mount_t *mnt, void *data);

#endif
