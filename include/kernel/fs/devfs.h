/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef DEVFS_H
#define DEVFS_H

#include <kernel/vfs.h>

void devfs_register(void);
void devfs_register_device(const char *name, mode_t mode, dev_t rdev, vfs_fops_t *fops);
void devfs_register_device_priv(const char *name, mode_t mode, dev_t rdev, vfs_fops_t *fops, void *priv);
/* Groups that own device nodes, as in the image's /etc/group. A person's
 * session is a member; agent sessions and other users are not. */
#define DEVFS_GID_AUDIO 18
#define DEVFS_GID_INPUT 23
#define DEVFS_GID_VIDEO 27
#define DEVFS_GID_CLAIM 950  /* /dev/claim: who may hold machine resources */
/* Same as devfs_register_device, with the node owned by root:gid. */
void devfs_register_device_gid(const char *name, mode_t mode, gid_t gid, dev_t rdev, vfs_fops_t *fops);
vfs_fops_t *devfs_fops_for_rdev(dev_t rdev, void **priv_out);
int  devfs_mount(vfs_mount_t *mnt, void *data);

#endif
