/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * /sys/kernel/ranix_ec: the door to the Casper EC's "OS present" flags. Root
 * only, both ways (0600, and the uid is checked again here). Reading shows the
 * cached state and touches no hardware; writing "on", "off" or "read" acts.
 * See casper_ec.c for what those do.
 */
#include <kernel/vfs.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>
#include <kernel/proc.h>
#include <kernel/hal.h>
#include <kernel/errno.h>
#include "casper_ec.h"

static ssize_t ec_node_read(vfs_file_t *f, void *buf, size_t len)
{
	if (task_current()->euid != 0) return -EACCES;
	char o[512];
	int n = casper_ec_status(o, sizeof(o));
	if (f->pos >= n) return 0;
	size_t c = (size_t)(n - f->pos);
	if (c > len) c = len;
	if (copy_fop_to_user(buf, o + f->pos, c) != 0) return -EFAULT;
	f->pos += (off_t)c;
	return (ssize_t)c;
}

static ssize_t ec_node_write(vfs_file_t *f, const void *buf, size_t len)
{
	(void)f;
	if (task_current()->euid != 0) return -EPERM;
	char cmd[8];
	if (!len || len > sizeof(cmd)) return -EINVAL;
	if (copy_fop_from_user(cmd, buf, len) != 0) return -EFAULT;
	int r = casper_ec_command(cmd, len);
	return r < 0 ? r : (ssize_t)len;
}

static vfs_fops_t ec_node_fops = { .read = ec_node_read, .write = ec_node_write };

vfs_inode_t *casper_ec_sysfs_file(void)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino = 0xd3000002;
	in->mode = S_IFREG | 0600;
	in->nlink = 1;
	in->refcount = 1;
	in->size = 512;
	in->fops = &ec_node_fops;
	return in;
}
