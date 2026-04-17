#ifndef RAMFS_H
#define RAMFS_H

#include <kernel/vfs.h>

void ramfs_register(void);
int  ramfs_mount(vfs_mount_t *mnt, void *data);

#endif
