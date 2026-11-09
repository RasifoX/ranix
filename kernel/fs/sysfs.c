/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* ── sysfs (/sys) ─────────────────────────────────────────────────────────────
 * A small read-only pseudo-filesystem mirroring the Linux /sys layout that
 * tooling looks for, built like procfs (dir inodes carry lookup/readdir iops,
 * file inodes carry a read fop). Currently exposes:
 *   /sys/kernel/{ostype,osrelease,hostname}
 *   /sys/class/net/<dev>/{address,mtu,operstate}   (one dir per registered netdev)
 * Directory entries are generated on demand from the live netdev list, so lo
 * and eth0 appear automatically.
 * ───────────────────────────────────────────────────────────────────────── */
#include <kernel/vfs.h>
#include <kernel/proc.h>
#include <kernel/input.h>
#include <kernel/sound.h>
#include <kernel/capabilities.h>
#include <kernel/hal.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/mmu.h>
#include <kernel/lib/string.h>
#include <kernel/net/net.h>
#include <kernel/block.h>
#include <kernel/pci.h>
#include <kernel/mm/hugetlb.h>
#include <kernel/kprintf.h>

#include <kernel/nsproxy.h>
#include <kernel/fs/devfs.h>
#include <kernel/smp.h>
#include <kernel/fb.h>
#include <input/ps2/synaptics.h>
#include <drivers/input/ps2.h>
#include <kernel/errno.h>

/* the PS/2 touchpad exists only on x86: weak, so NULL on arm */
#pragma weak ps2_aux_report
#pragma weak syn_tap

enum { SF_OSTYPE = 1, SF_OSRELEASE, SF_HOSTNAME, SF_NET_ADDR, SF_NET_MTU, SF_NET_STATE,
       SF_BLK_SIZE, SF_BLK_STAT, SF_BLK_RO, SF_BLK_RO_DENIED, SF_BLK_ID, SF_CPU_ONLINE, SF_CPU_POSSIBLE,
       SF_CGROUP_CTRL, SF_CGROUP_PROCS, SF_THP_ENABLED,
       SF_PCI_VENDOR, SF_PCI_DEVICE, SF_PCI_CAPS,
       SF_NR_HUGEPAGES, SF_FREE_HUGEPAGES, SF_KEYMAP, SF_VOLUME, SF_TOUCHPAD, SF_BACKLIGHT, SF_CSTATE };

static int put_u64(char *o, uint64_t v)
{
	char t[20]; int i = 0, n = 0;
	if (v == 0) { o[0] = '0'; return 1; }
	while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
	while (i) o[n++] = t[--i];
	return n;
}

typedef struct { int type; int dev; } sysfs_priv_t;

static uint64_t sys_ino_next = 0x9000;
static uint64_t sys_ino_alloc(void) { return sys_ino_next++; }

static vfs_iops_t sysfs_root_iops, sysfs_kernel_iops, sysfs_class_iops,
                  sysfs_net_iops, sysfs_netdev_iops, sysfs_block_iops,
                  sysfs_blockdev_iops, sysfs_devices_iops, sysfs_system_iops,
                  sysfs_cpu_iops, sysfs_fs_iops, sysfs_cgroup_iops, sysfs_bus_iops,
                  sysfs_pci_iops, sysfs_pcidev_iops, sysfs_pcidev_entry_iops,
                  sysfs_mm_iops, sysfs_thp_iops, sysfs_hugepages_iops,
                  sysfs_hugepages2m_iops;
static vfs_fops_t sysfs_file_fops;
static vfs_fops_t sysfs_dir_fops = { 0 };

static vfs_inode_t *sysfs_dir(vfs_iops_t *iops, int dev)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = sys_ino_alloc();
	in->mode     = S_IFDIR | 0555;
	in->nlink    = 2;
	in->refcount = 1;
	in->iops     = iops;
	in->fops     = &sysfs_dir_fops;
	in->no_cache = 1;   /* netdev list is dynamic */
	if (dev >= 0) {
		sysfs_priv_t *p = kmalloc(sizeof(*p));
		if (p) { p->type = 0; p->dev = dev; in->priv = p; }
	}
	return in;
}

static vfs_inode_t *sysfs_file(int type, int dev)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = sys_ino_alloc();
	in->mode     = S_IFREG | 0444;
	in->nlink    = 1;
	in->refcount = 1;
	in->fops     = &sysfs_file_fops;
	sysfs_priv_t *p = kmalloc(sizeof(*p));
	if (!p) { kfree(in); return NULL; }
	p->type = type; p->dev = dev;
	in->priv = p;
	return in;
}

static int put_str(char *o, const char *s)
{
	int i = 0;
	while (s[i]) { o[i] = s[i]; i++; }
	return i;
}

static ssize_t sysfs_file_read(vfs_file_t *f, void *buf, size_t len)
{
	sysfs_priv_t *pr = f->inode->priv;
	if (!pr) return -1;

	uint64_t phys = (uint64_t)buddy_alloc(0);
	if (!phys) return -1;
	char *tmp = (char *)PHYS_TO_VIRT(phys);
	int n = 0;

	switch (pr->type) {
	case SF_OSTYPE:    n = put_str(tmp, "Ranix\n");  break;
	case SF_OSRELEASE: n = put_str(tmp, "6.6.0-ranix\n");  break;
	case SF_HOSTNAME:  n = put_str(tmp, current_uts()->nodename); tmp[n++] = '\n'; break;
	case SF_NET_ADDR: {
		netdev_t *d = net_dev(pr->dev);
		if (d) {
			const char *h = "0123456789abcdef";
			for (int i = 0; i < 6; i++) {
				if (i) tmp[n++] = ':';
				tmp[n++] = h[d->mac[i] >> 4];
				tmp[n++] = h[d->mac[i] & 15];
			}
			tmp[n++] = '\n';
		}
		break;
	}
	case SF_BLK_RO: {
		block_device_t *b = block_device_get(pr->dev);
		n = put_str(tmp, b && b->ro ? "1\n" : "0\n");
		break;
	}
	case SF_BLK_RO_DENIED: {
		block_device_t *b = block_device_get(pr->dev);
		if (b) { n = put_u64(tmp, b->ro_denied); tmp[n++] = '\n'; }
		break;
	}
	case SF_BLK_ID: {
		block_device_t *b = block_device_get(pr->dev);
		if (b) n = block_device_ids(b, tmp, 4096);
		break;
	}
	case SF_NET_MTU:   n = put_str(tmp, "1500\n"); break;
	case SF_NET_STATE: n = put_str(tmp, "up\n");   break;
	case SF_BLK_SIZE: {
		block_device_t *b = block_device_get(pr->dev);
		if (b) { n = put_u64(tmp, b->size_blocks); tmp[n++] = '\n'; }
		break;
	}
	case SF_BLK_STAT: {
		/* "<read-transfers> <sectors-read>": sequential readahead shows up as
		 * far fewer transfers than sectors. */
		block_device_t *b = block_device_get(pr->dev);
		if (b) {
			n  = put_u64(tmp, b->nr_reads);     tmp[n++] = ' ';
			n += put_u64(tmp + n, b->nr_read_secs); tmp[n++] = '\n';
		}
		break;
	}
	case SF_CPU_ONLINE:
	case SF_CPU_POSSIBLE:
		if (smp_ncpus <= 1) { n = put_str(tmp, "0\n"); }
		else { n = put_str(tmp, "0-"); n += put_u64(tmp + n, (uint64_t)(smp_ncpus - 1)); tmp[n++] = '\n'; }
		break;
	case SF_CGROUP_CTRL:  n = put_str(tmp, "cpu memory pids freezer\n"); break;
	case SF_CGROUP_PROCS: n = put_str(tmp, "1\n"); break;
	case SF_THP_ENABLED:  n = put_str(tmp, "always [madvise] never\n"); break;
	case SF_TOUCHPAD: { n = ps2_aux_report ? ps2_aux_report((char *)tmp, 128) : 0; break; }
	case SF_VOLUME: n = put_u64(tmp, (uint64_t)sound_volume()); tmp[n++] = '\n'; break;
	case SF_CSTATE: {
		n = put_u64(tmp, (uint64_t)arch_idle_get()); tmp[n++] = '\n';
		break;
	}
	case SF_BACKLIGHT: {
		int v = backlight_get();
		if (v < 0) return -ENODEV;
		n = put_u64(tmp, (uint64_t)v); tmp[n++] = '\n';
		break;
	}
	case SF_KEYMAP: {
		const char *m = input_keymap == KEYMAP_TR ? "tr\n" : "us\n";
		n = 3; memcpy(tmp, m, 3); break;
	}
	case SF_NR_HUGEPAGES:  n = put_u64(tmp, (uint64_t)huge2m_nr_total()); tmp[n++] = '\n'; break;
	case SF_FREE_HUGEPAGES: n = put_u64(tmp, (uint64_t)huge2m_nr_free()); tmp[n++] = '\n'; break;
	case SF_PCI_VENDOR:
	case SF_PCI_DEVICE: {
		int bdf = pr->dev;
		uint16_t v = pci_read16((uint8_t)((bdf >> 8) & 0xff), (uint8_t)((bdf >> 3) & 0x1f),
		                        (uint8_t)(bdf & 7), pr->type == SF_PCI_VENDOR ? PCI_VENDOR_ID : PCI_DEVICE_ID);
		const char *h = "0123456789abcdef";
		tmp[n++] = '0'; tmp[n++] = 'x';
		tmp[n++] = h[(v >> 12) & 15]; tmp[n++] = h[(v >> 8) & 15];
		tmp[n++] = h[(v >> 4) & 15];  tmp[n++] = h[v & 15];
		tmp[n++] = '\n';
		break;
	}
	case SF_PCI_CAPS: {
		int bdf = pr->dev;
		uint8_t ids[32];
		int cnt = pci_cap_ids((uint8_t)((bdf >> 8) & 0xff), (uint8_t)((bdf >> 3) & 0x1f),
		                      (uint8_t)(bdf & 7), ids, 32);
		const char *h = "0123456789abcdef";
		for (int i = 0; i < cnt; i++) {
			if (i) tmp[n++] = ' ';
			tmp[n++] = h[(ids[i] >> 4) & 15];
			tmp[n++] = h[ids[i] & 15];
		}
		tmp[n++] = '\n';
		break;
	}
	default:           n = 0; break;
	}

	ssize_t ret = 0;
	if (f->pos < (off_t)n) {
		size_t avail = (size_t)(n - f->pos);
		if (len > avail) len = avail;
		if (copy_fop_to_user(buf, tmp + (size_t)f->pos, len) != 0) { buddy_free(phys, 0); return -EFAULT; }
		f->pos += (off_t)len;
		ret = (ssize_t)len;
	}
	buddy_free(phys, 0);
	return ret;
}

/* The person's knobs (volume, brightness, touchpad) belong to a group: its
 * members may turn them without CAP_SYS_ADMIN. */
static gid_t sysfs_knob_gid(int type)
{
	return type == SF_VOLUME ? DEVFS_GID_AUDIO : type == SF_BACKLIGHT ? DEVFS_GID_VIDEO :
	       type == SF_TOUCHPAD ? DEVFS_GID_INPUT : 0;
}

static int sysfs_may_turn(int type)
{
	process_t *p = task_current();
	gid_t g = sysfs_knob_gid(type);
	if (capable_task(p, CAP_SYS_ADMIN)) return 1;
	if (!g || !p) return 0;
	if (p->egid == g) return 1;
	for (int i = 0; i < p->ngroups; i++) if (p->groups[i] == g) return 1;
	return 0;
}

static ssize_t sysfs_file_write(vfs_file_t *f, const void *buf, size_t len)
{
	sysfs_priv_t *pr = f->inode->priv;
	if (!pr || (pr->type != SF_BACKLIGHT && pr->type != SF_CSTATE && pr->type != SF_NR_HUGEPAGES && pr->type != SF_KEYMAP && pr->type != SF_VOLUME && pr->type != SF_TOUCHPAD)) return -EINVAL;
	char kbuf[32];
	size_t n = len < sizeof(kbuf) ? len : sizeof(kbuf);
	if (copy_fop_from_user(kbuf, buf, n) != 0) return -EFAULT;
	if (pr->type == SF_VOLUME) {
		long pct = 0;
		if (!sysfs_may_turn(SF_VOLUME)) return -1;
		for (size_t j = 0; j < n && kbuf[j] >= '0' && kbuf[j] <= '9'; j++) pct = pct * 10 + (kbuf[j] - '0');
		sound_set_volume((int)pct);
		return (ssize_t)len;
	}
	if (pr->type == SF_CSTATE) {
		long v = 0;
		if (!capable_task(task_current(), CAP_SYS_ADMIN)) return -1;
		for (size_t j = 0; j < n && kbuf[j] >= '0' && kbuf[j] <= '9'; j++) v = v * 10 + (kbuf[j] - '0');
		int e = arch_idle_set((int)v);
		return e ? e : (ssize_t)len;
	}
	if (pr->type == SF_BACKLIGHT) {
		long pct = 0;
		if (!sysfs_may_turn(SF_BACKLIGHT)) return -1;
		for (size_t j = 0; j < n && kbuf[j] >= '0' && kbuf[j] <= '9'; j++) pct = pct * 10 + (kbuf[j] - '0');
		int e = backlight_set((int)pct);
		return e ? e : (ssize_t)len;
	}
	if (pr->type == SF_TOUCHPAD) {           /* "tap 0" or "tap 1" */
		if (!sysfs_may_turn(SF_TOUCHPAD)) return -1;
		if (n < 5 || memcmp(kbuf, "tap ", 4) != 0 || !&syn_tap) return -EINVAL;
		syn_tap = kbuf[4] == '1';
		return (ssize_t)len;
	}
	if (pr->type == SF_KEYMAP) {
		if (!capable_task(task_current(), CAP_SYS_ADMIN)) return -1;
		if (n >= 2 && kbuf[0] == 't' && kbuf[1] == 'r') input_keymap = KEYMAP_TR;
		else if (n >= 2 && kbuf[0] == 'u' && kbuf[1] == 's') input_keymap = KEYMAP_US;
		else return -EINVAL;
		return (ssize_t)len;
	}
	/* nr_hugepages reserves physical memory: root's decision, not the mode bits' */
	if (!sysfs_may_turn(SF_NR_HUGEPAGES)) return -1;
	long v = 0; size_t i = 0;
	while (i < n && kbuf[i] >= '0' && kbuf[i] <= '9') { v = v * 10 + (kbuf[i] - '0'); i++; }
	huge2m_nr_set(v);
	return (ssize_t)len;
}

static vfs_fops_t sysfs_file_fops = { .read = sysfs_file_read, .write = sysfs_file_write };

/* nr_hugepages is writable; other sysfs files are read-only. */
static vfs_inode_t *sysfs_file_rw(int type, int dev)
{
	vfs_inode_t *in = sysfs_file(type, dev);
	if (in) {
		gid_t g = sysfs_knob_gid(type);
		in->mode = S_IFREG | (g ? 0664 : 0644);
		in->gid  = g;
	}
	return in;
}

/* ── /sys (root): kernel/, class/ ────────────────────────────── */
static vfs_inode_t *sysfs_root_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	if (strcmp(name, "kernel")  == 0) return sysfs_dir(&sysfs_kernel_iops, -1);
	if (strcmp(name, "class")   == 0) return sysfs_dir(&sysfs_class_iops, -1);
	if (strcmp(name, "block")   == 0) return sysfs_dir(&sysfs_block_iops, -1);
	if (strcmp(name, "devices") == 0) return sysfs_dir(&sysfs_devices_iops, -1);
	if (strcmp(name, "fs")      == 0) return sysfs_dir(&sysfs_fs_iops, -1);
	if (strcmp(name, "bus")     == 0) return sysfs_dir(&sysfs_bus_iops, -1);
	if (strcmp(name, "dev")     == 0) return drm_sysfs_root("dev");
	return NULL;
}
static int sysfs_root_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	const char *names[] = { "kernel", "class", "block", "devices", "fs", "bus", "dev" };
	if (idx >= 7) return -1;
	out->ino = sys_ino_alloc(); out->mode = S_IFDIR | 0555; out->next_off = idx + 1;
	put_str(out->name, names[idx]); out->name[strlen(names[idx])] = '\0';
	return 0;
}

/* ── /sys/kernel ─────────────────────────────────────────────── */
static const struct { const char *name; int type; } kernel_files[] = {
	{ "ostype", SF_OSTYPE }, { "osrelease", SF_OSRELEASE }, { "hostname", SF_HOSTNAME },
};
static vfs_inode_t *sysfs_kernel_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	if (strcmp(name, "mm") == 0) return sysfs_dir(&sysfs_mm_iops, -1);
	if (strcmp(name, "keymap") == 0) return sysfs_file_rw(SF_KEYMAP, -1);
	if (strcmp(name, "volume") == 0) return sysfs_file_rw(SF_VOLUME, -1);
	if (strcmp(name, "touchpad") == 0) return sysfs_file_rw(SF_TOUCHPAD, -1);
	if (strcmp(name, "backlight") == 0) return sysfs_file_rw(SF_BACKLIGHT, -1);
	if (strcmp(name, "cstate") == 0) return sysfs_file_rw(SF_CSTATE, -1);
	if (strcmp(name, "ranix_thermal") == 0) return thermal_sysfs_file();
	if (strcmp(name, "ranix_ec") == 0) return casper_ec_sysfs_file();
	for (unsigned i = 0; i < 3; i++)
		if (strcmp(kernel_files[i].name, name) == 0)
			return sysfs_file(kernel_files[i].type, -1);
	return NULL;
}
static int sysfs_kernel_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx > 5) return -1;
	out->ino = sys_ino_alloc(); out->next_off = idx + 1;
	if (idx == 4) { out->mode = S_IFREG | 0644; put_str(out->name, "keymap"); out->name[6] = '\0'; return 0; }
	if (idx == 5) { out->mode = S_IFREG | 0644; put_str(out->name, "volume"); out->name[6] = '\0'; return 0; }
	if (idx == 3) { out->mode = S_IFDIR | 0555; put_str(out->name, "mm"); out->name[2] = '\0'; return 0; }
	out->mode = S_IFREG | 0444;
	put_str(out->name, kernel_files[idx].name);
	out->name[strlen(kernel_files[idx].name)] = '\0';
	return 0;
}

/* ── /sys/kernel/mm/{transparent_hugepage,hugepages} ─────────── */
static vfs_inode_t *sysfs_mm_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	if (strcmp(name, "transparent_hugepage") == 0) return sysfs_dir(&sysfs_thp_iops, -1);
	if (strcmp(name, "hugepages") == 0)            return sysfs_dir(&sysfs_hugepages_iops, -1);
	return NULL;
}
static int sysfs_mm_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	const char *names[] = { "transparent_hugepage", "hugepages" };
	if (idx >= 2) return -1;
	out->ino = sys_ino_alloc(); out->mode = S_IFDIR | 0555; out->next_off = idx + 1;
	put_str(out->name, names[idx]); out->name[strlen(names[idx])] = '\0';
	return 0;
}

/* /sys/kernel/mm/hugepages/hugepages-2048kB — the 2 MB pool, Linux's path */
static vfs_inode_t *sysfs_hugepages_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	if (strcmp(name, "hugepages-2048kB") == 0) return sysfs_dir(&sysfs_hugepages2m_iops, -1);
	return NULL;
}
static int sysfs_hugepages_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx != 0) return -1;
	out->ino = sys_ino_alloc(); out->mode = S_IFDIR | 0555; out->next_off = 1;
	put_str(out->name, "hugepages-2048kB"); out->name[16] = '\0';
	return 0;
}
static const struct { const char *name; int type; } hugepages2m_files[] = {
	{ "nr_hugepages", SF_NR_HUGEPAGES }, { "free_hugepages", SF_FREE_HUGEPAGES },
};
static vfs_inode_t *sysfs_hugepages2m_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	if (strcmp(name, "nr_hugepages") == 0)   return sysfs_file_rw(SF_NR_HUGEPAGES, -1);
	if (strcmp(name, "free_hugepages") == 0) return sysfs_file(SF_FREE_HUGEPAGES, -1);
	return NULL;
}
static int sysfs_hugepages2m_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx >= 2) return -1;
	out->ino = sys_ino_alloc();
	out->mode = (idx == 0) ? (S_IFREG | 0644) : (S_IFREG | 0444);
	out->next_off = idx + 1;
	put_str(out->name, hugepages2m_files[idx].name);
	out->name[strlen(hugepages2m_files[idx].name)] = '\0';
	return 0;
}
static vfs_inode_t *sysfs_thp_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	if (strcmp(name, "enabled") == 0) return sysfs_file(SF_THP_ENABLED, -1);
	return NULL;
}
static int sysfs_thp_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx != 0) return -1;
	out->ino = sys_ino_alloc(); out->mode = S_IFREG | 0644; out->next_off = 1;
	put_str(out->name, "enabled"); out->name[7] = '\0';
	return 0;
}

/* ── /sys/fs/cgroup ──────────────────────────────────────────── */
static vfs_inode_t *sysfs_fs_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	if (strcmp(name, "cgroup") == 0) return sysfs_dir(&sysfs_cgroup_iops, -1);
	return NULL;
}
static int sysfs_fs_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx != 0) return -1;
	out->ino = sys_ino_alloc(); out->mode = S_IFDIR | 0555; out->next_off = 1;
	put_str(out->name, "cgroup"); out->name[6] = '\0';
	return 0;
}
static const struct { const char *name; int type; } cgroup_files[] = {
	{ "cgroup.controllers", SF_CGROUP_CTRL }, { "cgroup.procs", SF_CGROUP_PROCS },
};
static vfs_inode_t *sysfs_cgroup_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	for (unsigned i = 0; i < 2; i++)
		if (strcmp(cgroup_files[i].name, name) == 0)
			return sysfs_file(cgroup_files[i].type, -1);
	return NULL;
}
static int sysfs_cgroup_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx >= 2) return -1;
	out->ino = sys_ino_alloc(); out->mode = S_IFREG | 0444; out->next_off = idx + 1;
	put_str(out->name, cgroup_files[idx].name);
	out->name[strlen(cgroup_files[idx].name)] = '\0';
	return 0;
}

/* ── /sys/bus/pci/devices ────────────────────────────────────── */
static int pci_nth_device(uint32_t idx, uint8_t *bus, uint8_t *dev, uint8_t *fn)
{
	uint32_t seen = 0;
	for (uint16_t b = 0; b < 4; b++)        /* QEMU/most boards: a few buses */
		for (uint8_t d = 0; d < 32; d++)
			for (uint8_t f = 0; f < 8; f++)
				if (pci_read16((uint8_t)b, d, f, PCI_VENDOR_ID) != 0xffff) {
					if (seen++ == idx) { *bus = (uint8_t)b; *dev = d; *fn = f; return 0; }
					if (f == 0 && !(pci_read16((uint8_t)b, d, 0, 0x0e) & 0x80)) break; /* not multifunction */
				}
	return -1;
}
static vfs_inode_t *sysfs_bus_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	if (strcmp(name, "pci") == 0) return sysfs_dir(&sysfs_pci_iops, -1);
	return NULL;
}
static int sysfs_bus_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx != 0) return -1;
	out->ino = sys_ino_alloc(); out->mode = S_IFDIR | 0555; out->next_off = 1;
	put_str(out->name, "pci"); out->name[3] = '\0';
	return 0;
}
static vfs_inode_t *sysfs_pci_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	if (strcmp(name, "devices") == 0) return sysfs_dir(&sysfs_pcidev_iops, -1);
	return NULL;
}
static int sysfs_pci_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx != 0) return -1;
	out->ino = sys_ino_alloc(); out->mode = S_IFDIR | 0555; out->next_off = 1;
	put_str(out->name, "devices"); out->name[7] = '\0';
	return 0;
}
/* one entry per present PCI function, named 0000:BB:DD.F */
static int sysfs_pcidev_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	uint8_t b, d, f;
	if (pci_nth_device(idx, &b, &d, &f) != 0) return -1;
	const char *hx = "0123456789abcdef";
	char *p = out->name;
	*p++='0';*p++='0';*p++='0';*p++='0';*p++=':';
	*p++=hx[b>>4];*p++=hx[b&15];*p++=':';
	*p++=hx[d>>4];*p++=hx[d&15];*p++='.';
	*p++=hx[f&7];*p='\0';
	out->ino = sys_ino_alloc(); out->mode = S_IFDIR | 0555; out->next_off = idx + 1;
	return 0;
}
/* /sys/bus/pci/devices/<addr>: vendor, device id files */
static int sysfs_hexv(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}
static vfs_inode_t *sysfs_pcidev_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	if (strlen(name) < 12) return NULL;                 /* 0000:bb:dd.f */
	int bb = (sysfs_hexv(name[5]) << 4) | sysfs_hexv(name[6]);
	int dd = (sysfs_hexv(name[8]) << 4) | sysfs_hexv(name[9]);
	int f  = sysfs_hexv(name[11]);
	if (bb < 0 || dd < 0 || f < 0) return NULL;
	if (pci_read16((uint8_t)bb, (uint8_t)dd, (uint8_t)f, PCI_VENDOR_ID) == 0xffff) return NULL;
	return sysfs_dir(&sysfs_pcidev_entry_iops, (bb << 8) | (dd << 3) | f);
}
static const struct { const char *name; int type; } pcidev_files[] = {
	{ "vendor", SF_PCI_VENDOR }, { "device", SF_PCI_DEVICE },
	{ "capabilities", SF_PCI_CAPS },
};
static vfs_inode_t *sysfs_pcidev_entry_lookup(vfs_inode_t *dir, const char *name)
{
	sysfs_priv_t *pr = dir->priv;
	int bdf = pr ? pr->dev : 0;
	unsigned nf = sizeof(pcidev_files) / sizeof(pcidev_files[0]);
	for (unsigned i = 0; i < nf; i++)
		if (strcmp(pcidev_files[i].name, name) == 0)
			return sysfs_file(pcidev_files[i].type, bdf);
	return NULL;
}
static int sysfs_pcidev_entry_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx >= sizeof(pcidev_files) / sizeof(pcidev_files[0])) return -1;
	out->ino = sys_ino_alloc(); out->mode = S_IFREG | 0444; out->next_off = idx + 1;
	put_str(out->name, pcidev_files[idx].name);
	out->name[strlen(pcidev_files[idx].name)] = '\0';
	return 0;
}

/* ── /sys/class: net/ ────────────────────────────────────────── */
static vfs_inode_t *sysfs_class_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	if (strcmp(name, "net") == 0) return sysfs_dir(&sysfs_net_iops, -1);
	if (strcmp(name, "drm") == 0) return drm_sysfs_root("class/drm");
	return NULL;
}
static int sysfs_class_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	const char *names[] = { "net", "drm" };
	if (idx >= 2) return -1;
	out->ino = sys_ino_alloc(); out->mode = S_IFDIR | 0555; out->next_off = idx + 1;
	put_str(out->name, names[idx]); out->name[3] = '\0';
	return 0;
}

/* ── /sys/class/net: one dir per netdev ──────────────────────── */
static vfs_inode_t *sysfs_net_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	for (int i = 0; ; i++) {
		netdev_t *d = net_dev(i);
		if (!d) break;
		if (strcmp(d->name, name) == 0) return sysfs_dir(&sysfs_netdev_iops, i);
	}
	return NULL;
}
static int sysfs_net_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	netdev_t *d = net_dev((int)idx);
	if (!d) return -1;
	out->ino = sys_ino_alloc(); out->mode = S_IFDIR | 0555; out->next_off = idx + 1;
	put_str(out->name, d->name); out->name[strlen(d->name)] = '\0';
	return 0;
}

/* ── /sys/class/net/<dev>: address, mtu, operstate ───────────── */
static const struct { const char *name; int type; } netdev_files[] = {
	{ "address", SF_NET_ADDR }, { "mtu", SF_NET_MTU }, { "operstate", SF_NET_STATE },
};
static vfs_inode_t *sysfs_netdev_lookup(vfs_inode_t *dir, const char *name)
{
	sysfs_priv_t *pr = dir->priv;
	int dev = pr ? pr->dev : 0;
	for (unsigned i = 0; i < 3; i++)
		if (strcmp(netdev_files[i].name, name) == 0)
			return sysfs_file(netdev_files[i].type, dev);
	return NULL;
}
static int sysfs_netdev_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx >= 3) return -1;
	out->ino = sys_ino_alloc(); out->mode = S_IFREG | 0444; out->next_off = idx + 1;
	put_str(out->name, netdev_files[idx].name);
	out->name[strlen(netdev_files[idx].name)] = '\0';
	return 0;
}

/* ── /sys/block: one dir per block device ────────────────────── */
static vfs_inode_t *sysfs_block_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	int cnt = block_device_count();
	for (int i = 0; i < cnt; i++) {
		block_device_t *b = block_device_get(i);
		if (b && strcmp(b->name, name) == 0) return sysfs_dir(&sysfs_blockdev_iops, i);
	}
	return NULL;
}
static int sysfs_block_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	block_device_t *b = ((int)idx < block_device_count()) ? block_device_get((int)idx) : NULL;
	if (!b) return -1;
	out->ino = sys_ino_alloc(); out->mode = S_IFDIR | 0555; out->next_off = idx + 1;
	put_str(out->name, b->name); out->name[strlen(b->name)] = '\0';
	return 0;
}
static vfs_inode_t *sysfs_blockdev_lookup(vfs_inode_t *dir, const char *name)
{
	sysfs_priv_t *pr = dir->priv;
	int d = pr ? pr->dev : 0;
	if (strcmp(name, "size") == 0) return sysfs_file(SF_BLK_SIZE, d);
	if (strcmp(name, "stat") == 0) return sysfs_file(SF_BLK_STAT, d);
	if (strcmp(name, "ro") == 0) return sysfs_file(SF_BLK_RO, d);
	if (strcmp(name, "ro_denied") == 0) return sysfs_file(SF_BLK_RO_DENIED, d);
	if (strcmp(name, "id") == 0) return sysfs_file(SF_BLK_ID, d);
	return NULL;
}
static const char *blockdev_files[] = { "size", "stat", "ro", "ro_denied", "id" };
static int sysfs_blockdev_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx >= 2) return -1;
	out->ino = sys_ino_alloc(); out->mode = S_IFREG | 0444; out->next_off = idx + 1;
	put_str(out->name, blockdev_files[idx]); out->name[strlen(blockdev_files[idx])] = '\0';
	return 0;
}

/* ── /sys/devices/system/cpu/{online,possible} ───────────────── */
static vfs_inode_t *sysfs_devices_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	if (strcmp(name, "system") == 0) return sysfs_dir(&sysfs_system_iops, -1);
	return NULL;
}
static int sysfs_devices_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx != 0) return -1;
	out->ino = sys_ino_alloc(); out->mode = S_IFDIR | 0555; out->next_off = 1;
	put_str(out->name, "system"); out->name[6] = '\0';
	return 0;
}
static vfs_inode_t *sysfs_system_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	if (strcmp(name, "cpu") == 0) return sysfs_dir(&sysfs_cpu_iops, -1);
	return NULL;
}
static int sysfs_system_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx != 0) return -1;
	out->ino = sys_ino_alloc(); out->mode = S_IFDIR | 0555; out->next_off = 1;
	put_str(out->name, "cpu"); out->name[3] = '\0';
	return 0;
}
static const struct { const char *name; int type; } cpu_files[] = {
	{ "online", SF_CPU_ONLINE }, { "possible", SF_CPU_POSSIBLE },
};
/* cpu0, cpu1, ...: programs count CPUs by these directories (stress-ng, lscpu) */
static vfs_inode_t *sysfs_cpun_lookup(vfs_inode_t *dir, const char *name) { (void)dir; (void)name; return NULL; }
static int sysfs_cpun_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out) { (void)dir; (void)idx; (void)out; return -1; }
static vfs_iops_t sysfs_cpun_iops = { .lookup = sysfs_cpun_lookup, .readdir = sysfs_cpun_readdir };

static vfs_inode_t *sysfs_cpu_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	for (unsigned i = 0; i < 2; i++)
		if (strcmp(cpu_files[i].name, name) == 0) return sysfs_file(cpu_files[i].type, -1);
	if (name[0] == 'c' && name[1] == 'p' && name[2] == 'u' && name[3] >= '0' && name[3] <= '9') {
		int n = 0;
		for (const char *c = name + 3; *c; c++) {
			if (*c < '0' || *c > '9') return NULL;
			n = n * 10 + (*c - '0');
		}
		if (n < smp_ncpus) return sysfs_dir(&sysfs_cpun_iops, n);
	}
	return NULL;
}
static int sysfs_cpu_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx >= 2 + (uint32_t)smp_ncpus) return -1;
	out->ino = sys_ino_alloc(); out->next_off = idx + 1;
	if (idx >= 2) {
		uint32_t n = idx - 2, k = 3;
		out->mode = S_IFDIR | 0555;
		put_str(out->name, "cpu");
		if (n >= 100) out->name[k++] = (char)('0' + n / 100);
		if (n >= 10) out->name[k++] = (char)('0' + n / 10 % 10);
		out->name[k++] = (char)('0' + n % 10);
		out->name[k] = '\0';
		return 0;
	}
	out->mode = S_IFREG | 0444;
	put_str(out->name, cpu_files[idx].name); out->name[strlen(cpu_files[idx].name)] = '\0';
	return 0;
}

static vfs_iops_t sysfs_root_iops     = { .lookup = sysfs_root_lookup,     .readdir = sysfs_root_readdir     };
static vfs_iops_t sysfs_kernel_iops   = { .lookup = sysfs_kernel_lookup,   .readdir = sysfs_kernel_readdir   };
static vfs_iops_t sysfs_class_iops    = { .lookup = sysfs_class_lookup,    .readdir = sysfs_class_readdir    };
static vfs_iops_t sysfs_net_iops      = { .lookup = sysfs_net_lookup,      .readdir = sysfs_net_readdir      };
static vfs_iops_t sysfs_netdev_iops   = { .lookup = sysfs_netdev_lookup,   .readdir = sysfs_netdev_readdir   };
static vfs_iops_t sysfs_block_iops    = { .lookup = sysfs_block_lookup,    .readdir = sysfs_block_readdir    };
static vfs_iops_t sysfs_blockdev_iops = { .lookup = sysfs_blockdev_lookup, .readdir = sysfs_blockdev_readdir };
static vfs_iops_t sysfs_devices_iops  = { .lookup = sysfs_devices_lookup,  .readdir = sysfs_devices_readdir  };
static vfs_iops_t sysfs_system_iops   = { .lookup = sysfs_system_lookup,   .readdir = sysfs_system_readdir   };
static vfs_iops_t sysfs_cpu_iops      = { .lookup = sysfs_cpu_lookup,      .readdir = sysfs_cpu_readdir      };
static vfs_iops_t sysfs_fs_iops       = { .lookup = sysfs_fs_lookup,       .readdir = sysfs_fs_readdir       };
static vfs_iops_t sysfs_cgroup_iops   = { .lookup = sysfs_cgroup_lookup,   .readdir = sysfs_cgroup_readdir   };
static vfs_iops_t sysfs_bus_iops      = { .lookup = sysfs_bus_lookup,      .readdir = sysfs_bus_readdir      };
static vfs_iops_t sysfs_pci_iops      = { .lookup = sysfs_pci_lookup,      .readdir = sysfs_pci_readdir      };
static vfs_iops_t sysfs_pcidev_iops   = { .lookup = sysfs_pcidev_lookup,   .readdir = sysfs_pcidev_readdir   };
static vfs_iops_t sysfs_pcidev_entry_iops = { .lookup = sysfs_pcidev_entry_lookup, .readdir = sysfs_pcidev_entry_readdir };
static vfs_iops_t sysfs_mm_iops       = { .lookup = sysfs_mm_lookup,       .readdir = sysfs_mm_readdir       };
static vfs_iops_t sysfs_thp_iops      = { .lookup = sysfs_thp_lookup,      .readdir = sysfs_thp_readdir      };
static vfs_iops_t sysfs_hugepages_iops   = { .lookup = sysfs_hugepages_lookup,   .readdir = sysfs_hugepages_readdir   };
static vfs_iops_t sysfs_hugepages2m_iops = { .lookup = sysfs_hugepages2m_lookup, .readdir = sysfs_hugepages2m_readdir };

int sysfs_mount(vfs_mount_t *mnt, void *data)
{
	(void)data;
	vfs_inode_t *root = kmalloc(sizeof(*root));
	if (!root) return -1;
	memset(root, 0, sizeof(*root));
	root->ino      = 1;
	root->mode     = S_IFDIR | 0555;
	root->nlink    = 2;
	root->refcount = 1;
	root->iops     = &sysfs_root_iops;
	root->fops     = &sysfs_dir_fops;
	root->no_cache = 1;

	vfs_dentry_t *rd = vfs_dentry_create(NULL, "/", root);
	if (!rd) { kfree(root); return -1; }
	mnt->root = rd;
	return 0;
}

void sysfs_register(void)
{
	vfs_register_fs("sysfs", sysfs_mount);
}
