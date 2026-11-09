/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef PROCFS_H
#define PROCFS_H

#include <kernel/vfs.h>

void procfs_register(void);
int  procfs_mount(vfs_mount_t *mnt, void *data);

#endif
