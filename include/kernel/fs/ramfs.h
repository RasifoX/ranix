/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef RAMFS_H
#define RAMFS_H

#include <kernel/vfs.h>

void ramfs_register(void);
int  ramfs_mount(vfs_mount_t *mnt, void *data);

vfs_inode_t *ramfs_create_anon(void);

#endif
