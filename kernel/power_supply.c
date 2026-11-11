/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Power supplies: the table drivers fill (mains adapter, battery) and the
 * /sys/class/power_supply/<name>/ tree made from it on lookup. The files and
 * their text are kernel/lib/ps_format.c. The directory is empty on a machine
 * whose driver has read nothing: no supply is ever invented, and a supply the
 * driver says is gone (a battery taken out) leaves it, as on Linux.
 * The table keeps its slots: a removed supply leaves a hole, so a file opened
 * before the removal reads ENODEV, never another supply's values.
 * Doors: none. Every file is read-only and world-readable, as on Linux. */
#include <kernel/power_supply.h>
#include <kernel/vfs.h>
#include <kernel/hal.h>
#include <kernel/sync.h>
#include <kernel/errno.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>

#define PS_MAX 4

static ps_t table[PS_MAX];          /* name[0] == 0: a free slot */
static spinlock_t ps_lock = SPINLOCK_INIT;

void power_supply_set(const ps_t *p)
{
	if (!p->name[0]) return;
	uint64_t f = spin_lock_irq(&ps_lock);
	int i = 0, fr = -1;
	for (; i < PS_MAX; i++) {
		if (!table[i].name[0]) { if (fr < 0) fr = i; continue; }
		if (!strncmp(table[i].name, p->name, sizeof(table[i].name) - 1)) break;
	}
	if (i == PS_MAX) i = fr;
	if (i >= 0) {
		table[i] = *p;
		table[i].name[sizeof(table[i].name) - 1] = 0;
	}
	spin_unlock_irq(&ps_lock, f);
}

void power_supply_remove(const char *name)
{
	uint64_t f = spin_lock_irq(&ps_lock);
	for (int i = 0; i < PS_MAX; i++)
		if (table[i].name[0] && !strncmp(table[i].name, name, sizeof(table[i].name) - 1))
			memset(&table[i], 0, sizeof(table[i]));
	spin_unlock_irq(&ps_lock, f);
}

/* the supply in slot s: 0, or -1 when the slot is free or past the end */
static int slot_get(int s, ps_t *out)
{
	if (s < 0 || s >= PS_MAX) return -1;
	uint64_t f = spin_lock_irq(&ps_lock);
	int ok = table[s].name[0] != 0;
	if (ok) *out = table[s];
	spin_unlock_irq(&ps_lock, f);
	return ok ? 0 : -1;
}

int power_supply_get(int idx, ps_t *out)
{
	for (int s = 0; s < PS_MAX; s++)
		if (slot_get(s, out) == 0 && idx-- == 0) return 0;
	return -1;
}

/* The lid: -1 unknown, 0 closed, 1 open (/sys/kernel/lid). Set by the board driver. */
static volatile int lid_state = -1;
void power_lid_set(int open) { lid_state = open; }
int  power_lid_get(void) { return lid_state; }

static int find(const char *name, ps_t *out)
{
	for (int s = 0; s < PS_MAX; s++)
		if (slot_get(s, out) == 0 && !strcmp(out->name, name)) return s;
	return -1;
}

/* ---- the tree: priv encodes (table slot + 1) << 8 | attribute slot + 1 ---- */

static vfs_iops_t root_iops, sup_iops;
static vfs_fops_t file_fops, dir_fops;
static uint64_t ino_next = 0xd5000000ULL;

static vfs_inode_t *mk(uint32_t mode, uintptr_t priv)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino = ino_next++;
	in->refcount = 1;
	in->priv = (void *)priv;
	in->priv_static = 1;
	in->mode = mode;
	if (S_ISDIR(mode)) {
		in->nlink = 2; in->fops = &dir_fops; in->no_cache = 1;
		in->iops = priv ? &sup_iops : &root_iops;
	} else {
		in->nlink = 1; in->fops = &file_fops; in->size = 4096;
	}
	return in;
}

static vfs_inode_t *root_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	ps_t p;
	int i = find(name, &p);
	return i < 0 ? NULL : mk(S_IFDIR | 0555, (uintptr_t)(i + 1) << 8);
}

static int root_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	ps_t p;
	if (power_supply_get((int)idx, &p)) return -1;   /* the idx-th supply there is: holes skipped */
	out->ino = 0xd5f00000ULL + idx;
	out->mode = S_IFDIR | 0555;
	out->next_off = idx + 1;
	memcpy(out->name, p.name, strlen(p.name) + 1);
	return 0;
}

static vfs_inode_t *sup_lookup(vfs_inode_t *dir, const char *name)
{
	int s = (int)((uintptr_t)dir->priv >> 8) - 1;
	ps_t p;
	if (slot_get(s, &p)) return NULL;
	int a = ps_attr_slot(&p, name);
	return a < 0 ? NULL : mk(S_IFREG | 0444, ((uintptr_t)(s + 1) << 8) | (uintptr_t)(a + 1));
}

static int sup_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	int s = (int)((uintptr_t)dir->priv >> 8) - 1;
	ps_t p;
	if (slot_get(s, &p)) return -1;
	const char *n = ps_attr_name(&p, (int)idx);
	if (!n) return -1;
	out->ino = 0xd5e00000ULL + ((uint64_t)s << 8) + idx;
	out->mode = S_IFREG | 0444;
	out->next_off = idx + 1;
	memcpy(out->name, n, strlen(n) + 1);
	return 0;
}

static ssize_t file_read(vfs_file_t *f, void *buf, size_t len)
{
	uintptr_t v = (uintptr_t)f->inode->priv;
	int s = (int)(v >> 8) - 1, a = (int)(v & 0xFF) - 1;
	ps_t p;
	char tmp[512];
	if (slot_get(s, &p)) return -ENODEV;
	int len_txt = ps_attr_format_slot(&p, a, tmp, sizeof(tmp));
	if (len_txt < 0) return -ENODEV;             /* the value went away since the lookup */
	if (f->pos >= len_txt) return 0;
	size_t c = (size_t)(len_txt - f->pos);
	if (c > len) c = len;
	if (copy_fop_to_user(buf, tmp + f->pos, c) != 0) return -EFAULT;
	f->pos += (off_t)c;
	return (ssize_t)c;
}

static vfs_iops_t root_iops = { .lookup = root_lookup, .readdir = root_readdir };
static vfs_iops_t sup_iops  = { .lookup = sup_lookup,  .readdir = sup_readdir };
static vfs_fops_t file_fops = { .read = file_read };
static vfs_fops_t dir_fops  = { 0 };

vfs_inode_t *power_supply_sysfs_root(void) { return mk(S_IFDIR | 0555, 0); }
