/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef OVERLAY_H
#define OVERLAY_H

#include <kernel/vfs.h>

int  overlay_mount(vfs_mount_t *mnt, void *data);
void overlay_register(void);

#endif
