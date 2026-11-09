/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * The /sys entries libdrm and Mesa's loader read to find a DRM device and its bus:
 *
 *   /sys/dev/char/226:{0,128}/{dev,uevent,subsystem@,device/}
 *   /sys/class/drm/{card0,renderD128}/   (the same node directories)
 *   /sys/class/drm/card0-<conn>-1/{status,enabled,dpms,modes}
 *   /sys/class/drm/version
 *   .../device/ = the display's PCI function: vendor, device, subsystem_vendor,
 *       subsystem_device, revision, class, config, boot_vga, uevent (PCI_SLOT_NAME),
 *       subsystem@ -> bus/pci, driver@, drm/{card0,renderD128}/
 *
 * libdrm's drmGetDevices2: stat(".../device/drm") says "this is DRM", readlink of
 * ".../device/subsystem" names the bus, PCI_SLOT_NAME in ".../device/uevent" the
 * address, and the id files the device. The tree is generated on lookup, like
 * the rest of sysfs; symlinks only where libdrm reads one.
 */
#include <kernel/vfs.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>
#include <kernel/hal.h>
#include <kernel/pci.h>
#include <kernel/proc.h>
#include "drm_priv.h"
#include "i915/ranix_gt.h"


enum {
	N_NONE, N_DEV, N_DEVCHAR, N_CLASS, N_NODE0, N_NODE1, N_PCI, N_PCIDRM, N_CONN,
	F_FIRST,
	F_DEV0 = F_FIRST, F_DEV1, F_UEV0, F_UEV1, F_VERSION,
	F_PVENDOR, F_PDEVICE, F_PSVENDOR, F_PSDEVICE, F_PREV, F_PCLASS, F_PUEVENT, F_PCONFIG, F_PBOOTVGA,
	F_CSTATUS, F_CENABLED, F_CDPMS, F_CMODES, F_FLIP, F_GT, F_POWER, F_ACPI,
	L_FIRST,
	L_SUBDRM = L_FIRST, L_SUBPCI, L_DRIVER,
};

typedef struct { const char *name; int node; } child_t;

static const child_t ch_dev[]     = { { "char", N_DEVCHAR } };
static const child_t ch_devchar[] = { { "226:0", N_NODE0 }, { "226:128", N_NODE1 } };
static const child_t ch_class[]   = { { "card0", N_NODE0 }, { "renderD128", N_NODE1 }, { "card0-X-1", N_CONN },
                                      { "version", F_VERSION }, { "ranix_flip", F_FLIP },
                                      { "ranix_gt", F_GT }, { "ranix_power", F_POWER },
                                      { "ranix_acpi_tables", F_ACPI } };
static const child_t ch_node0[]   = { { "dev", F_DEV0 }, { "uevent", F_UEV0 }, { "subsystem", L_SUBDRM }, { "device", N_PCI } };
static const child_t ch_node1[]   = { { "dev", F_DEV1 }, { "uevent", F_UEV1 }, { "subsystem", L_SUBDRM }, { "device", N_PCI } };
static const child_t ch_pci[]     = {
	{ "uevent", F_PUEVENT }, { "subsystem", L_SUBPCI }, { "driver", L_DRIVER }, { "drm", N_PCIDRM },
	{ "vendor", F_PVENDOR }, { "device", F_PDEVICE }, { "subsystem_vendor", F_PSVENDOR },
	{ "subsystem_device", F_PSDEVICE }, { "revision", F_PREV }, { "class", F_PCLASS },
	{ "config", F_PCONFIG }, { "boot_vga", F_PBOOTVGA },
};
static const child_t ch_pcidrm[]  = { { "card0", N_NODE0 }, { "renderD128", N_NODE1 } };
static const child_t ch_conn[]    = { { "status", F_CSTATUS }, { "enabled", F_CENABLED }, { "dpms", F_CDPMS },
                                      { "modes", F_CMODES }, { "subsystem", L_SUBDRM } };

/* Closed by default: what libdrm and Mesa need to find the device is world-readable,
 * as on Linux; Ranix's own control and diagnostic files are root's alone, and only
 * ranix_flip and ranix_gt take writes. */
static uint32_t node_mode(int node)
{
	if (node < F_FIRST) return S_IFDIR | 0555;
	if (node >= L_FIRST) return S_IFLNK | 0777;
	switch (node) {
	case F_FLIP: case F_GT:     return S_IFREG | 0600;
	case F_POWER: case F_ACPI:  return S_IFREG | 0400;
	default:                    return S_IFREG | 0444;
	}
}

static const child_t *children(int node, int *n)
{
#define C(a) do { *n = (int)(sizeof(a) / sizeof(a[0])); return a; } while (0)
	int ready = drm_is_ready();
	switch (node) {
	case N_DEV:     C(ch_dev);
	case N_DEVCHAR: if (!ready) break; C(ch_devchar);
	case N_CLASS:   if (!ready) { *n = 1; return &ch_class[3]; } C(ch_class);
	case N_NODE0:   C(ch_node0);
	case N_NODE1:   C(ch_node1);
	case N_PCI:     if (!drm_pci_found) { *n = 4; return ch_pci; } C(ch_pci);   /* platform: no ids */
	case N_PCIDRM:  C(ch_pcidrm);
	case N_CONN:    C(ch_conn);
	}
#undef C
	*n = 0;
	return NULL;
}

static const char *conn_name(void)
{
	return drm_display() && drm_display()->conn_type == DRM_MODE_CONNECTOR_eDP ? "card0-eDP-1" : "card0-Virtual-1";
}

static const char *child_name(const child_t *c)
{
	return c->node == N_CONN ? conn_name() : c->name;
}

static vfs_iops_t dir_iops, link_iops;
static vfs_fops_t file_fops, dir_fops;
static uint64_t ino_next = 0xd2000000ULL;

static vfs_inode_t *mk(int node)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino = ino_next++;
	in->refcount = 1;
	in->priv = (void *)(uintptr_t)node;
	in->priv_static = 1;
	if (node < F_FIRST) {
		in->mode = S_IFDIR | 0555; in->nlink = 2; in->iops = &dir_iops; in->fops = &dir_fops; in->no_cache = 1;
	} else if (node < L_FIRST) {
		in->mode = node_mode(node); in->nlink = 1; in->fops = &file_fops;
		in->size = node == F_PCONFIG ? 256 : 4096;
	} else {
		in->mode = S_IFLNK | 0777; in->nlink = 1; in->iops = &link_iops;
	}
	return in;
}

static vfs_inode_t *dir_lookup(vfs_inode_t *dir, const char *name)
{
	int n;
	const child_t *c = children((int)(uintptr_t)dir->priv, &n);
	for (int i = 0; i < n; i++)
		if (strcmp(child_name(&c[i]), name) == 0) return mk(c[i].node);
	return NULL;
}

static int dir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	int n;
	const child_t *c = children((int)(uintptr_t)dir->priv, &n);
	if ((int)idx >= n) return -1;
	const char *nm = child_name(&c[idx]);
	int node = c[idx].node;
	out->ino = 0xd2f00000ULL + (uint64_t)node;
	out->mode = node_mode(node);
	out->next_off = idx + 1;
	size_t l = strlen(nm);
	memcpy(out->name, nm, l + 1);
	return 0;
}

static int link_readlink(vfs_inode_t *in, char *buf, size_t sz)
{
	const char *t, *drv = drm_display() ? drm_display()->sysfs_driver : "platform/drivers/simple-framebuffer";
	switch ((int)(uintptr_t)in->priv) {
	case L_SUBDRM: t = "../../../class/drm"; break;
	case L_SUBPCI: t = drm_pci_found ? "../../../../bus/pci" : "../../../../bus/platform"; break;
	default:       t = "../../../../bus/"; break;
	}
	char full[96];
	size_t l = strlen(t), m = (int)(uintptr_t)in->priv == L_DRIVER ? strlen(drv) : 0;
	memcpy(full, t, l);
	memcpy(full + l, drv, m);
	l += m;
	if (l >= sz) l = sz - 1;
	memcpy(buf, full, l);
	buf[l] = 0;
	return 0;
}

/* ── file contents ── */

static int puts_(char *o, const char *s) { size_t l = strlen(s); memcpy(o, s, l); return (int)l; }

static int putdec(char *o, uint32_t v)
{
	char t[12]; int n = 0, i = 0;
	do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (n) o[i++] = t[--n];
	return i;
}

static int puthex(char *o, uint32_t v, int digits, int prefix)
{
	const char *h = "0123456789abcdef";
	int i = 0;
	if (prefix) { o[i++] = '0'; o[i++] = 'x'; }
	for (int k = digits - 1; k >= 0; k--) o[i++] = h[(v >> (k * 4)) & 15];
	return i;
}

static int puthexu(char *o, uint32_t v, int digits)       /* PCI_ID wants upper case */
{
	const char *h = "0123456789ABCDEF";
	for (int k = digits - 1; k >= 0; k--) *o++ = h[(v >> (k * 4)) & 15];
	return digits;
}

static uint32_t cfg(int reg, int bytes)
{
	uint32_t v = pci_read32(drm_pci_bus, drm_pci_dev, drm_pci_fn, (uint8_t)(reg & ~3));
	v >>= (reg & 3) * 8;
	return bytes == 4 ? v : v & ((1u << (bytes * 8)) - 1);
}

static int gen(int node, char *o)
{
	int n = 0;
	switch (node) {
	case F_DEV0: case F_DEV1:
		n += puts_(o + n, "226:");
		n += putdec(o + n, node == F_DEV0 ? 0 : DRM_MINOR_RENDER);
		o[n++] = '\n';
		break;
	case F_UEV0: case F_UEV1:
		n += puts_(o + n, "MAJOR=226\nMINOR=");
		n += putdec(o + n, node == F_UEV0 ? 0 : DRM_MINOR_RENDER);
		n += puts_(o + n, node == F_UEV0 ? "\nDEVNAME=dri/card0\n" : "\nDEVNAME=dri/renderD128\n");
		n += puts_(o + n, "DEVTYPE=drm_minor\n");
		break;
	case F_VERSION: n += puts_(o, "drm 1.1.0 20060810\n"); break;
	case F_PVENDOR:   n += puthex(o, cfg(0x00, 2), 4, 1); o[n++] = '\n'; break;
	case F_PDEVICE:   n += puthex(o, cfg(0x02, 2), 4, 1); o[n++] = '\n'; break;
	case F_PSVENDOR:  n += puthex(o, cfg(0x2c, 2), 4, 1); o[n++] = '\n'; break;
	case F_PSDEVICE:  n += puthex(o, cfg(0x2e, 2), 4, 1); o[n++] = '\n'; break;
	case F_PREV:      n += puthex(o, cfg(0x08, 1), 2, 1); o[n++] = '\n'; break;
	case F_PCLASS:    n += puthex(o, cfg(0x08, 4) >> 8, 6, 1); o[n++] = '\n'; break;
	case F_PBOOTVGA:  n += puts_(o, "1\n"); break;
	case F_PCONFIG:
		for (int r = 0; r < 256; r += 4) { uint32_t v = cfg(r, 4); memcpy(o + r, &v, 4); }
		n = 256;
		break;
	case F_PUEVENT:
		n += puts_(o + n, "DRIVER=");
		n += puts_(o + n, drm_driver_name);
		if (drm_pci_found) {
			n += puts_(o + n, "\nPCI_CLASS=");
			n += puthexu(o + n, cfg(0x08, 4) >> 8, 6);
			n += puts_(o + n, "\nPCI_ID=");
			n += puthexu(o + n, cfg(0x00, 2), 4); o[n++] = ':'; n += puthexu(o + n, cfg(0x02, 2), 4);
			n += puts_(o + n, "\nPCI_SUBSYS_ID=");
			n += puthexu(o + n, cfg(0x2c, 2), 4); o[n++] = ':'; n += puthexu(o + n, cfg(0x2e, 2), 4);
			n += puts_(o + n, "\nPCI_SLOT_NAME=0000:");
			n += puthex(o + n, drm_pci_bus, 2, 0); o[n++] = ':';
			n += puthex(o + n, drm_pci_dev, 2, 0); o[n++] = '.';
			o[n++] = (char)('0' + drm_pci_fn);
		}
		o[n++] = '\n';
		break;
	case F_CSTATUS:  n += puts_(o, drm_kms_connector_status() ? "connected\n" : "disconnected\n"); break;
	case F_CENABLED: n += puts_(o, "enabled\n"); break;
	case F_GT:
		n += ranix_gt_status(o);
		break;
	case F_POWER:
		n += ranix_power_status(o);
		break;
	case F_FLIP:
		n += puts_(o, drm_kms_hwflip(-1) ? "1\n" : "0\n");
		n += drm_kms_debug(o + n);
		break;
	case F_CDPMS: {
		static const char *const d[] = { "On\n", "Standby\n", "Suspend\n", "Off\n" };
		uint32_t v = drm_kms_dpms();
		n += puts_(o, d[v < 4 ? v : 0]);
		break;
	}
	case F_CMODES: {
		uint32_t w, h;
		if (drm_kms_mode_size(&w, &h)) {
			n += putdec(o + n, w); o[n++] = 'x'; n += putdec(o + n, h); o[n++] = '\n';
		}
		break;
	}
	}
	return n;
}

/* ranix_acpi_tables (root, read only): every table the XSDT/RSDT lists except the
 * MSDM/SLIC licence tables, then the
 * DSDT, raw and back to back (each starts with its own header and length), for
 * offline decoding with ACPICA's tools. Diagnostic only; nothing is evaluated. */
#ifdef __x86_64__
#include <asm/mmu.h>
uint64_t mb2_get_rsdp(void);
static int acpi_table_list(uint64_t *pa, uint32_t *ln, int max)
{
	uint64_t r = mb2_get_rsdp();
	if (!r) return 0;
	const uint8_t *rs = (const uint8_t *)PHYS_TO_VIRT(r);
	if (memcmp(rs, "RSD PTR ", 8)) return 0;
	uint64_t root; int w;
	if (rs[15] >= 2 && *(const uint64_t *)(rs + 24)) { root = *(const uint64_t *)(rs + 24); w = 8; }
	else { root = *(const uint32_t *)(rs + 16); w = 4; }
	const uint8_t *rt = (const uint8_t *)PHYS_TO_VIRT(root);
	uint32_t rl = *(const uint32_t *)(rt + 4);
	if (rl < 36 || rl > 4096) return 0;
	int n = 0;
	for (uint32_t off = 36; off + (uint32_t)w <= rl && n < max - 1; off += (uint32_t)w) {
		uint64_t t = 0;
		memcpy(&t, rt + off, (size_t)w);
		if (!t) continue;
		const uint8_t *h = (const uint8_t *)PHYS_TO_VIRT(t);
		uint32_t l = *(const uint32_t *)(h + 4);
		if (l < 36 || l > (4u << 20)) continue;
		if (!memcmp(h, "MSDM", 4) || !memcmp(h, "SLIC", 4)) continue;   /* Windows licence data: not ours to hand out */
		pa[n] = t; ln[n] = l; n++;
		if (!memcmp(h, "FACP", 4) && l >= 44 && n < max) {      /* the DSDT hangs off the FADT */
			uint64_t d = *(const uint32_t *)(h + 40);
			if (l >= 148 && *(const uint64_t *)(h + 140)) d = *(const uint64_t *)(h + 140);
			const uint8_t *dh = d ? (const uint8_t *)PHYS_TO_VIRT(d) : 0;
			if (dh && !memcmp(dh, "DSDT", 4)) {
				uint32_t dl = *(const uint32_t *)(dh + 4);
				if (dl >= 36 && dl <= (4u << 20)) { pa[n] = d; ln[n] = dl; n++; }
			}
		}
	}
	return n;
}
static ssize_t acpi_read(vfs_file_t *f, void *buf, size_t len)
{
	if (task_current()->euid != 0) return -EPERM_;
	uint64_t pa[64]; uint32_t ln[64];
	int n = acpi_table_list(pa, ln, 64);
	uint64_t pos = (uint64_t)f->pos, base = 0;
	for (int i = 0; i < n; i++) {
		if (pos < base + ln[i]) {
			size_t c = (size_t)(base + ln[i] - pos);
			if (c > len) c = len;
			if (copy_fop_to_user(buf, (const void *)(PHYS_TO_VIRT(pa[i]) + (pos - base)), c) != 0) return -EFAULT_;
			f->pos += (off_t)c;
			return (ssize_t)c;
		}
		base += ln[i];
	}
	return 0;
}
#endif

static ssize_t file_read(vfs_file_t *f, void *buf, size_t len)
{
	if (!(node_mode((int)(uintptr_t)f->inode->priv) & 0004) && task_current()->euid != 0) return -EACCES_;
#ifdef __x86_64__
	if ((int)(uintptr_t)f->inode->priv == F_ACPI) return acpi_read(f, buf, len);
#endif
	char tmp[1600];
	int n = gen((int)(uintptr_t)f->inode->priv, tmp);
	if (f->pos >= n) return 0;
	size_t c = (size_t)(n - f->pos);
	if (c > len) c = len;
	if (copy_fop_to_user(buf, tmp + f->pos, c) != 0) return -EFAULT_;
	f->pos += (off_t)c;
	return (ssize_t)c;
}

/* ranix_flip: "1" puts client buffers on the plane directly, "0" copies them */
static ssize_t file_write(vfs_file_t *f, const void *buf, size_t len)
{
	int node = (int)(uintptr_t)f->inode->priv;
	if (node != F_FLIP && node != F_GT) return -EACCES_;
	if (task_current()->euid != 0) return -EPERM_;
	if (node == F_GT) {
		char cmd[16];
		size_t n = len < sizeof(cmd) ? len : sizeof(cmd);
		if (copy_fop_from_user(cmd, buf, n) != 0) return -EFAULT_;
		if (mutex_lock_killable(&drm_lock)) return -EINTR_;
		int r = ranix_gt_command(cmd, n);
		mutex_unlock(&drm_lock);
		return r < 0 ? r : (ssize_t)len;
	}
	char c;
	if (!len || copy_fop_from_user(&c, buf, 1) != 0) return -EFAULT_;
	if (c != '0' && c != '1' && c != 't') return -EINVAL_;
	if (mutex_lock_killable(&drm_lock)) return -EINTR_;
	int r = c == 't' ? drm_kms_selftest() : drm_kms_hwflip(c == '1');
	mutex_unlock(&drm_lock);
	return r < 0 ? r : (ssize_t)len;
}

static vfs_iops_t dir_iops  = { .lookup = dir_lookup, .readdir = dir_readdir };
static vfs_iops_t link_iops = { .readlink = link_readlink };
static vfs_fops_t file_fops = { .read = file_read, .write = file_write };
static vfs_fops_t dir_fops  = { 0 };

/* the two roots sysfs.c hands out: /sys/dev and /sys/class/drm */
vfs_inode_t *drm_sysfs_root(const char *which)
{
	return mk(strcmp(which, "dev") == 0 ? N_DEV : N_CLASS);
}
