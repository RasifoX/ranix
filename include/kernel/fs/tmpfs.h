/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef TMPFS_H
#define TMPFS_H

#include <kernel/vfs.h>

int  tmpfs_mount(vfs_mount_t *mnt, void *data);
void tmpfs_register(void);

#endif
