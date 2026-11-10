/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * Input devices with a node each: /dev/input/eventN (evdev) and /dev/input/jsN
 * (the joystick interface), for joysticks and gamepads. See include/kernel/input_dev.h.
 *
 * evdev: struct input_event records, and the ioctls programs ask before they
 * trust a device (EVIOCGBIT, EVIOCGABS, EVIOCGNAME, EVIOCGID, EVIOCGKEY, ...),
 * which is what SDL, GLFW (WPILib's simulator) and Wine's winebus read.
 * js: 8-byte js_event records, buttons and axes numbered in code order, axes
 * scaled to -32767..32767, and on open one "init" event per button and axis.
 *
 * Each open is a client with its own queue. A device's values are kept, so only
 * changes go out; a report ends with SYN_REPORT. A client that falls behind
 * loses its queue and gets SYN_DROPPED, as on Linux. Nodes are never
 * unregistered (devfs has no unregister): a device that leaves is hidden, and
 * its slot is reused once its last reader closes.
 */
#include <kernel/input_dev.h>
#include <kernel/input.h>
#include <kernel/fs/devfs.h>
#include <kernel/sync.h>
#include <kernel/proc.h>
#include <kernel/hal.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/errno.h>
#include <kernel/time.h>
#include <kernel/board.h>

#define IDEV_MAX  8
#define CL_RING   256
#define FRAME_MAX 64

struct js_event { uint32_t time; int16_t value; uint8_t type, number; };

typedef struct ev_client {
	struct input_dev *dev;
	struct ev_client *next;
	int      js;
	uint32_t head, tail;
	struct input_event q[CL_RING];
	struct js_event    jq[CL_RING];
} ev_client_t;

typedef struct input_dev {
	input_dev_info_t info;
	char         ev_name[16], js_name[16];
	uint8_t      registered, present;
	int          opens;
	spinlock_t   lock;
	wait_queue_t wq;
	ev_client_t *clients, *grab;
	int32_t      absval[64];
	uint64_t     keystate[INPUT_DEV_KEYS / 64];
	struct input_event frame[FRAME_MAX];
	int          nframe;
} input_dev_t;

static input_dev_t g_idev[IDEV_MAX];
static spinlock_t g_idev_lock = SPINLOCK_INIT;
static vfs_fops_t evd_fops, jsd_fops;

static int bit(const uint64_t *b, uint32_t n) { return b[n / 64] >> (n % 64) & 1; }

/* ---- the driver's side ---- */

static void name_node(char *out, const char *base, int n)
{
	int k = 0;
	while (base[k]) { out[k] = base[k]; k++; }
	if (n >= 10) out[k++] = (char)('0' + n / 10);
	out[k++] = (char)('0' + n % 10);
	out[k] = 0;
}

input_dev_t *input_dev_add(const input_dev_info_t *info)
{
	uint64_t f = spin_lock_irq(&g_idev_lock);
	input_dev_t *d = 0;
	int slot = 0;
	for (int i = 0; i < IDEV_MAX && !d; i++)
		if (!g_idev[i].present && !g_idev[i].opens) { d = &g_idev[i]; slot = i; }
	if (d) d->present = 1;
	spin_unlock_irq(&g_idev_lock, f);
	if (!d) return 0;
	d->info = *info;
	d->info.name[sizeof d->info.name - 1] = 0;
	memset(d->absval, 0, sizeof d->absval);
	memset(d->keystate, 0, sizeof d->keystate);
	d->nframe = 0;
	d->grab = 0;
	for (int a = 0; a < 64; a++)                    /* a centred stick reads as centred */
		if (d->info.absbits >> a & 1) d->absval[a] = d->info.absmin[a] < 0 ? 0 : d->info.absmin[a];
	if (!d->registered) {
		d->lock = (spinlock_t)SPINLOCK_INIT;
		wait_queue_init(&d->wq);
		name_node(d->ev_name, "input/event", slot + 1);
		name_node(d->js_name, "input/js", slot);
		devfs_register_device_gid_priv(d->ev_name, S_IFCHR | 0660, DEVFS_GID_INPUT,
		                               (dev_t)(0x0D70 + slot), &evd_fops, d);
		devfs_register_device_gid_priv(d->js_name, S_IFCHR | 0660, DEVFS_GID_INPUT,
		                               (dev_t)(0x0D00 + slot), &jsd_fops, d);
		d->registered = 1;
	}
	devfs_set_hidden(d->ev_name, 0);
	devfs_set_hidden(d->js_name, !d->info.joystick);
	return d;
}

void input_dev_report(input_dev_t *d, uint16_t type, uint16_t code, int32_t value)
{
	if (!d) return;
	uint64_t f = spin_lock_irq(&d->lock);
	int changed = 0;
	if (type == EV_KEY && code < INPUT_DEV_KEYS && bit(d->info.keybits, code)) {
		value = !!value;
		changed = bit(d->keystate, code) != value;
		if (value) d->keystate[code / 64] |= 1ULL << (code % 64);
		else       d->keystate[code / 64] &= ~(1ULL << (code % 64));
	} else if (type == EV_ABS && code < 64 && (d->info.absbits >> code & 1)) {
		changed = d->absval[code] != value;
		d->absval[code] = value;
	}
	if (changed && d->nframe < FRAME_MAX) {
		struct input_event *e = &d->frame[d->nframe++];
		e->type = type;
		e->code = code;
		e->value = value;
	}
	spin_unlock_irq(&d->lock, f);
}

/* js numbering: buttons and axes in code order */
static int js_index(const uint64_t *bits, uint32_t code)
{
	int n = 0;
	for (uint32_t i = 0; i < code; i++) n += bit(bits, i);
	return n;
}

static int16_t js_scale(const input_dev_t *d, uint16_t code, int32_t v)
{
	int64_t lo = d->info.absmin[code], hi = d->info.absmax[code];
	if (hi <= lo) return 0;
	int64_t s = (int64_t)(v - lo) * 65534 / (hi - lo) - 32767;
	return (int16_t)(s < -32767 ? -32767 : s > 32767 ? 32767 : s);
}

static void cl_push_js(input_dev_t *d, ev_client_t *c, const struct input_event *e, uint8_t init)
{
	struct js_event j;
	j.time = (uint32_t)(kernel_ticks * 1000 / TIMER_HZ);
	if (e->type == EV_KEY) {
		j.type = 1 | init;
		j.number = (uint8_t)js_index(d->info.keybits, e->code);
		j.value = (int16_t)e->value;
	} else {
		j.type = 2 | init;
		j.number = (uint8_t)js_index(&d->info.absbits, e->code);
		j.value = js_scale(d, e->code, e->value);
	}
	uint32_t next = (c->head + 1) % CL_RING;
	if (next == c->tail) c->tail = (c->tail + 1) % CL_RING;   /* js keeps the newest */
	c->jq[c->head] = j;
	c->head = next;
}

/* One frame and its SYN_REPORT into an evdev client; d->lock held. */
static void cl_push_ev(input_dev_t *d, ev_client_t *c, int64_t sec, int64_t usec)
{
	uint32_t used = (c->head - c->tail) % CL_RING;
	struct input_event syn = { .type = EV_SYN, .code = 0, .value = 0 };
	if (used + (uint32_t)d->nframe + 1 >= CL_RING) {
		c->tail = c->head;                     /* fell behind: start over */
		syn.code = 3;                          /* SYN_DROPPED */
	}
	for (int i = 0; i <= d->nframe; i++) {
		struct input_event e = i < d->nframe && syn.code != 3 ? d->frame[i] : syn;
		e.time.tv_sec = sec;
		e.time.tv_usec = usec;
		c->q[c->head] = e;
		c->head = (c->head + 1) % CL_RING;
		if (e.type == EV_SYN) break;
	}
}

void input_dev_sync(input_dev_t *d)
{
	if (!d) return;
	uint64_t t = kernel_ticks;
	int64_t sec = (int64_t)(t / TIMER_HZ), usec = (int64_t)((t % TIMER_HZ) * (1000000 / TIMER_HZ));
	uint64_t f = spin_lock_irq(&d->lock);
	int any = d->nframe > 0;
	for (ev_client_t *c = d->clients; c && any; c = c->next) {
		if (d->grab && d->grab != c) continue;
		if (!c->js) { cl_push_ev(d, c, sec, usec); continue; }
		for (int i = 0; i < d->nframe; i++) cl_push_js(d, c, &d->frame[i], 0);
	}
	d->nframe = 0;
	spin_unlock_irq(&d->lock, f);
	if (any) {
		wake_up(&d->wq);
		poll_notify();
	}
}

void input_dev_gone(input_dev_t *d)
{
	if (!d) return;
	devfs_set_hidden(d->ev_name, 1);
	devfs_set_hidden(d->js_name, 1);
	uint64_t f = spin_lock_irq(&g_idev_lock);
	d->present = 0;
	spin_unlock_irq(&g_idev_lock, f);
	wake_up(&d->wq);
	poll_notify();
}

#include "input_dev_fops.c"
