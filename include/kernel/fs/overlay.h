#ifndef OVERLAY_H
#define OVERLAY_H

#include <kernel/vfs.h>

int  overlay_mount(vfs_mount_t *mnt, void *data);
void overlay_register(void);

#endif
