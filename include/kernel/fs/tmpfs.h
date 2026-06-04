#ifndef TMPFS_H
#define TMPFS_H

#include <kernel/vfs.h>

int  tmpfs_mount(vfs_mount_t *mnt, void *data);
void tmpfs_register(void);

#endif
