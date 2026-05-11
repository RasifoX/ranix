#ifndef DEVFS_H
#define DEVFS_H

#include <kernel/vfs.h>

void devfs_register(void);
void devfs_register_device(const char *name, mode_t mode, dev_t rdev, vfs_fops_t *fops);
int  devfs_mount(vfs_mount_t *mnt, void *data);

#endif
