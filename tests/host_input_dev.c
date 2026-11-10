/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Host test of /dev/input/eventN and jsN (drivers/input/input_dev.c) with the
 * kernel's interfaces stubbed (tests/host_input_dev/stub.h):
 *   cc -Itests/host_input_dev tests/host_input_dev.c drivers/input/input_dev.c && ./a.out
 * A gamepad is added, opened as evdev and js, reports values, and leaves. */
#include <stdio.h>
#include <string.h>
#include "tests/host_input_dev/stub.h"
#include "include/kernel/input_dev.h"

uint64_t kernel_ticks = 5000;
int stub_locks, stub_wakes, stub_sleeps, stub_hidden[16], stub_nnodes;
char stub_names[16][16];
void *stub_priv[16];
vfs_fops_t *stub_fops[16];

void devfs_register_device_gid_priv(const char *name, mode_t mode, gid_t gid, dev_t rdev,
                                    vfs_fops_t *fops, void *priv)
{
	(void)mode; (void)gid; (void)rdev;
	strncpy(stub_names[stub_nnodes], name, 15);
	stub_priv[stub_nnodes] = priv;
	stub_fops[stub_nnodes++] = fops;
}
void devfs_set_hidden(const char *name, int hidden)
{
	for (int i = 0; i < stub_nnodes; i++) if (!strcmp(stub_names[i], name)) stub_hidden[i] = hidden;
}
static int node(const char *name)
{
	for (int i = 0; i < stub_nnodes; i++) if (!strcmp(stub_names[i], name)) return i;
	return -1;
}

static int fails, passes;
#define CHECK(name, cond) do { if (cond) passes++; else { fails++; printf("FAIL: %s\n", name); } } while (0)

struct js_event { uint32_t time; int16_t value; uint8_t type, number; };
#define IOR(t, nr, sz) (2u << 30 | (uint32_t)(sz) << 16 | (uint32_t)(t) << 8 | (nr))

static int open_node(const char *name, vfs_inode_t *in, vfs_file_t *f)
{
	int n = node(name);
	if (n < 0) return -1;
	in->priv = stub_priv[n];
	memset(f, 0, sizeof *f);
	return stub_fops[n]->open(in, f, 0);
}

static struct input_dev *pad(void)
{
	input_dev_info_t i;
	memset(&i, 0, sizeof i);
	strcpy(i.name, "Test Pad");
	i.bustype = 3; i.vendor = 0x046d; i.product = 0xc216; i.version = 0x0110;
	i.keybits[0x130 / 64] |= 1ULL << (0x130 % 64) | 1ULL << (0x131 % 64);   /* A, B */
	i.absbits = 1 << 0 | 1 << 1 | 1 << 0x10;
	i.absmin[0] = 0; i.absmax[0] = 255; i.absmin[1] = 0; i.absmax[1] = 255;
	i.absmin[0x10] = -1; i.absmax[0x10] = 1;
	i.joystick = 1;
	return input_dev_add(&i);
}

static void t_nodes_and_ioctls(struct input_dev *d)
{
	CHECK("added", d != 0);
	CHECK("nodes", node("input/event1") >= 0 && node("input/js0") >= 0 && !stub_hidden[node("input/event1")]);
	vfs_inode_t in; vfs_file_t f;
	CHECK("open ev", open_node("input/event1", &in, &f) == 0);
	vfs_fops_t *o = stub_fops[node("input/event1")];
	char name[64] = {0};
	CHECK("gname", o->ioctl(&f, IOR('E', 0x06, 64), (uint64_t)(uintptr_t)name) == 9 && !strcmp(name, "Test Pad"));
	uint16_t id[4];
	CHECK("gid", o->ioctl(&f, IOR('E', 0x02, 8), (uint64_t)(uintptr_t)id) == 0 && id[1] == 0x046d && id[2] == 0xc216);
	uint64_t ev = 0, abs = 0, keys[12] = {0};
	CHECK("gbit 0", o->ioctl(&f, IOR('E', 0x20, 8), (uint64_t)(uintptr_t)&ev) == 8 && ev == 0xB);
	CHECK("gbit abs", o->ioctl(&f, IOR('E', 0x23, 8), (uint64_t)(uintptr_t)&abs) == 8 && abs == 0x10003);
	CHECK("gbit key", o->ioctl(&f, IOR('E', 0x21, 96), (uint64_t)(uintptr_t)keys) == 96 && (keys[4] >> 48 & 3) == 3);
	int32_t ai[6];
	CHECK("gabs x", o->ioctl(&f, IOR('E', 0x40, 24), (uint64_t)(uintptr_t)ai) == 0 && ai[1] == 0 && ai[2] == 255);
	CHECK("gabs missing", o->ioctl(&f, IOR('E', 0x42, 24), (uint64_t)(uintptr_t)ai) < 0);
	CHECK("not readable yet", o->poll(&f) == 0x4);
	f.flags = O_NONBLOCK;
	struct input_event e[8];
	CHECK("eagain", o->read(&f, e, sizeof e) == -EAGAIN);

	input_dev_report(d, EV_ABS, 0, 200);
	input_dev_report(d, EV_KEY, 0x130, 1);
	input_dev_report(d, EV_KEY, 0x2c0, 1);          /* not a key it has: dropped */
	input_dev_sync(d);
	CHECK("readable", o->poll(&f) & 1);
	ssize_t n = o->read(&f, e, sizeof e);
	CHECK("frame", n == 3 * (ssize_t)sizeof e[0] && e[0].type == EV_ABS && e[0].value == 200 &&
	      e[1].type == EV_KEY && e[1].code == 0x130 && e[2].type == EV_SYN && e[2].code == 0 &&
	      e[0].time.tv_sec == 5);
	input_dev_report(d, EV_ABS, 0, 200);             /* unchanged: nothing */
	input_dev_sync(d);
	CHECK("no change no frame", o->read(&f, e, sizeof e) == -EAGAIN);
	uint64_t ks[12] = {0};
	o->ioctl(&f, IOR('E', 0x18, 96), (uint64_t)(uintptr_t)ks);
	CHECK("gkey", ks[0x130 / 64] >> (0x130 % 64) & 1);
	CHECK("gabs value", o->ioctl(&f, IOR('E', 0x40, 24), (uint64_t)(uintptr_t)ai) == 0 && ai[0] == 200);

	/* a reader that never reads: SYN_DROPPED, then fresh frames */
	for (int i = 0; i < 300; i++) { input_dev_report(d, EV_ABS, 1, i % 2 ? 10 : 20); input_dev_sync(d); }
	n = o->read(&f, e, sizeof e);
	CHECK("dropped", n > 0 && e[0].type == EV_SYN && e[0].code == 3);
	while (o->read(&f, e, sizeof e) > 0) ;
	CHECK("locks balanced", stub_locks == 0);
	o->close(&f);
}

static void t_js(struct input_dev *d)
{
	vfs_inode_t in; vfs_file_t f;
	CHECK("open js", open_node("input/js0", &in, &f) == 0);
	vfs_fops_t *o = stub_fops[node("input/js0")];
	f.flags = O_NONBLOCK;
	uint8_t na = 0, nb = 0;
	o->ioctl(&f, IOR('j', 0x11, 1), (uint64_t)(uintptr_t)&na);
	o->ioctl(&f, IOR('j', 0x12, 1), (uint64_t)(uintptr_t)&nb);
	CHECK("js counts", na == 3 && nb == 2);
	struct js_event j[16];
	ssize_t n = o->read(&f, j, sizeof j);
	CHECK("js init", n == 5 * 8 && j[0].type == 0x81 && j[0].value == 1 && j[2].type == 0x82 &&
	      j[2].number == 0 && j[2].value > 18000);   /* X 200 of 0..255 */
	input_dev_report(d, EV_ABS, 0x10, -1);
	input_dev_report(d, EV_KEY, 0x131, 1);
	input_dev_sync(d);
	n = o->read(&f, j, sizeof j);
	CHECK("js events", n == 16 && j[0].type == 2 && j[0].number == 2 && j[0].value == -32767 &&
	      j[1].type == 1 && j[1].number == 1 && j[1].value == 1);
	uint8_t axmap[64];
	o->ioctl(&f, IOR('j', 0x32, 64), (uint64_t)(uintptr_t)axmap);
	CHECK("js axmap", axmap[0] == 0 && axmap[1] == 1 && axmap[2] == 0x10);
	o->close(&f);
}

static void t_gone(struct input_dev *d)
{
	vfs_inode_t in; vfs_file_t f;
	open_node("input/event1", &in, &f);
	vfs_fops_t *o = stub_fops[node("input/event1")];
	input_dev_gone(d);
	CHECK("hidden", stub_hidden[node("input/event1")] && stub_hidden[node("input/js0")]);
	struct input_event e[4];
	CHECK("read enodev", o->read(&f, e, sizeof e) == -ENODEV);
	CHECK("poll hup", (o->poll(&f) & 0x18) == 0x18);
	vfs_inode_t in2; vfs_file_t f2;
	CHECK("open gone", open_node("input/event1", &in2, &f2) == -ENODEV);
	struct input_dev *d2 = pad();                    /* slot 0 still held open: a new one */
	CHECK("second slot", d2 && d2 != d && node("input/event2") >= 0);
	o->close(&f);
	input_dev_gone(d2);
	struct input_dev *d3 = pad();                    /* both free: the first again */
	CHECK("slot reused", d3 == d && !stub_hidden[node("input/event1")] && stub_nnodes == 4);
}

int main(void)
{
	struct input_dev *d = pad();
	t_nodes_and_ioctls(d);
	t_js(d);
	t_gone(d);
	printf("input_dev: %d passed, %d failed\n", passes, fails);
	if (!fails) printf("ALL PASS\n");
	return fails != 0;
}
