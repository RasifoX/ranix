#ifndef DEVFS_H
#define DEVFS_H

#include <kernel/vfs.h>

void devfs_register(void);
int  devfs_mount(vfs_mount_t *mnt, void *data);

#endif
