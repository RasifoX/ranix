#ifndef PROCFS_H
#define PROCFS_H

#include <kernel/vfs.h>

void procfs_register(void);
int  procfs_mount(vfs_mount_t *mnt, void *data);

#endif
