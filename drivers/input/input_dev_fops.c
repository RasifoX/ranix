/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * Input devices: the file side of /dev/input/eventN and /dev/input/jsN (open,
 * read, poll, ioctl). Included by drivers/input/input_dev.c, not built alone.
 */

#define IOC_DIR(c)  ((c) >> 30)
#define IOC_TYPE(c) (((c) >> 8) & 0xff)
#define IOC_NR(c)   ((c) & 0xff)
#define IOC_SIZE(c) (((c) >> 16) & 0x3fff)

static int idev_open(vfs_inode_t *in, vfs_file_t *f, int js)
{
	input_dev_t *d = in->priv;
	ev_client_t *c = kmalloc(sizeof *c);
	if (!c) return -ENOMEM;
	memset(c, 0, sizeof *c);
	c->dev = d;
	c->js = js;
	uint64_t g = spin_lock_irq(&g_idev_lock);
	int ok = d->present;
	if (ok) d->opens++;
	spin_unlock_irq(&g_idev_lock, g);
	if (!ok) {
		kfree(c);
		return -ENODEV;
	}
	uint64_t fl = spin_lock_irq(&d->lock);
	if (js) {                                  /* the state as it is, marked init */
		for (uint32_t k = 0; k < INPUT_DEV_KEYS; k++) {
			if (!bit(d->info.keybits, k)) continue;
			struct input_event e = { .type = EV_KEY, .code = (uint16_t)k, .value = bit(d->keystate, k) };
			cl_push_js(d, c, &e, 0x80);
		}
		for (uint16_t a = 0; a < 64; a++) {
			if (!(d->info.absbits >> a & 1)) continue;
			struct input_event e = { .type = EV_ABS, .code = a, .value = d->absval[a] };
			cl_push_js(d, c, &e, 0x80);
		}
	}
	c->next = d->clients;
	d->clients = c;
	spin_unlock_irq(&d->lock, fl);
	f->priv = c;
	return 0;
}

static int evd_open(vfs_inode_t *in, vfs_file_t *f, int flags) { (void)flags; return idev_open(in, f, 0); }
static int jsd_open(vfs_inode_t *in, vfs_file_t *f, int flags) { (void)flags; return idev_open(in, f, 1); }

static int idev_close(vfs_file_t *f)
{
	ev_client_t *c = f->priv;
	if (!c) return 0;
	input_dev_t *d = c->dev;
	uint64_t fl = spin_lock_irq(&d->lock);
	for (ev_client_t **p = &d->clients; *p; p = &(*p)->next)
		if (*p == c) { *p = c->next; break; }
	if (d->grab == c) d->grab = 0;
	spin_unlock_irq(&d->lock, fl);
	uint64_t g = spin_lock_irq(&g_idev_lock);
	d->opens--;
	spin_unlock_irq(&g_idev_lock, g);
	kfree(c);
	f->priv = 0;
	return 0;
}

static ssize_t idev_read(vfs_file_t *f, void *buf, size_t len)
{
	ev_client_t *c = f->priv;
	input_dev_t *d = c->dev;
	size_t rec = c->js ? sizeof(struct js_event) : sizeof(struct input_event);
	if (len < rec) return -EINVAL;
	uint8_t k[16 * sizeof(struct input_event)];
	size_t max = len < sizeof k ? len : sizeof k, n = 0;
	uint64_t fl = spin_lock_irq(&d->lock);
	while (c->head == c->tail) {
		if (!d->present) { spin_unlock_irq(&d->lock, fl); return -ENODEV; }
		if (f->flags & O_NONBLOCK) { spin_unlock_irq(&d->lock, fl); return -EAGAIN; }
		if (task_has_actionable_signal()) { spin_unlock_irq(&d->lock, fl); return -EINTR; }
		sleep_on_locked(&d->wq, &d->lock, fl);
		fl = spin_lock_irq(&d->lock);
	}
	while (c->head != c->tail && n + rec <= max) {
		if (c->js) memcpy(k + n, &c->jq[c->tail], rec);
		else       memcpy(k + n, &c->q[c->tail], rec);
		c->tail = (c->tail + 1) % CL_RING;
		n += rec;
	}
	spin_unlock_irq(&d->lock, fl);
	return copy_fop_to_user(buf, k, n) ? -EFAULT : (ssize_t)n;
}

static int idev_poll(vfs_file_t *f)
{
	ev_client_t *c = f->priv;
	int in = c->head != c->tail;
	if (!c->dev->present) return (in ? 0x0001 : 0) | 0x0008 | 0x0010;   /* POLLERR | POLLHUP */
	return (in ? 0x0001 : 0) | 0x0004;
}

/* Copy n bytes of src to the caller, at most the size the request carries.
 * Returns the bytes copied, as Linux's evdev does. */
static int put_var(uint64_t arg, uint32_t cmd, const void *src, size_t n)
{
	size_t sz = IOC_SIZE(cmd);
	if (n > sz) n = sz;
	return copy_to_user((void *)(uintptr_t)arg, src, n) ? -EFAULT : (int)n;
}

static int evd_bits(input_dev_t *d, uint32_t cmd, uint64_t arg)
{
	uint32_t ev = IOC_NR(cmd) - 0x20;
	if (ev == 0) return put_var(arg, cmd, &(uint64_t){ 1u << EV_SYN | 1u << EV_KEY | 1u << EV_ABS }, 8);
	if (ev == EV_KEY) return put_var(arg, cmd, d->info.keybits, sizeof d->info.keybits);
	if (ev == EV_ABS) return put_var(arg, cmd, &d->info.absbits, 8);
	uint8_t zero[16] = {0};
	return put_var(arg, cmd, zero, ev == 0x15 ? 16 : 8);   /* EV_FF: 128 bits */
}

static int evd_ioctl(vfs_file_t *f, uint32_t cmd, uint64_t arg)
{
	ev_client_t *c = f->priv;
	input_dev_t *d = c->dev;
	void *ua = (void *)(uintptr_t)arg;
	if (IOC_TYPE(cmd) != 'E') return -EINVAL;
	uint32_t nr = IOC_NR(cmd);
	if (IOC_DIR(cmd) == 2 && nr >= 0x20 && nr < 0x40) return evd_bits(d, cmd, arg);
	if (IOC_DIR(cmd) == 2 && nr >= 0x40 && nr < 0x80) {      /* EVIOCGABS */
		uint32_t a = nr - 0x40;
		if (!(d->info.absbits >> a & 1)) return -EINVAL;
		int32_t ai[6] = { d->absval[a], d->info.absmin[a], d->info.absmax[a], 0, 0, 0 };
		return put_var(arg, cmd, ai, sizeof ai) < 0 ? -EFAULT : 0;
	}
	switch (nr) {
	case 0x01: { int v = 0x010001; return copy_to_user(ua, &v, 4) ? -EFAULT : 0; }    /* GVERSION */
	case 0x02: {                                                                     /* GID */
		uint16_t id[4] = { d->info.bustype, d->info.vendor, d->info.product, d->info.version };
		return copy_to_user(ua, id, 8) ? -EFAULT : 0;
	}
	case 0x06: return put_var(arg, cmd, d->info.name, strlen(d->info.name) + 1);       /* GNAME */
	case 0x07: case 0x08: return -ENOENT;                                            /* GPHYS, GUNIQ */
	case 0x09: case 0x19: case 0x1a: case 0x1b: {                                    /* GPROP, GLED, GSND, GSW */
		uint64_t z = 0;
		return put_var(arg, cmd, &z, 8);
	}
	case 0x18: {                                                                     /* GKEY */
		uint64_t ks[INPUT_DEV_KEYS / 64];
		uint64_t fl = spin_lock_irq(&d->lock);
		memcpy(ks, d->keystate, sizeof ks);
		spin_unlock_irq(&d->lock, fl);
		return put_var(arg, cmd, ks, sizeof ks);
	}
	case 0x84: { int z = 0; return copy_to_user(ua, &z, 4) ? -EFAULT : 0; }          /* GEFFECTS */
	case 0x90: {                                                                     /* GRAB */
		uint64_t fl = spin_lock_irq(&d->lock);
		int r = 0;
		if (arg) { if (d->grab && d->grab != c) r = -EBUSY; else d->grab = c; }
		else if (d->grab == c) d->grab = 0;
		else r = -EINVAL;
		spin_unlock_irq(&d->lock, fl);
		return r;
	}
	case 0xa0: { int id; if (copy_from_user(&id, ua, 4)) return -EFAULT; return id == 0 || id == 1 ? 0 : -EINVAL; }
	}
	return -EINVAL;
}

static int jsd_ioctl(vfs_file_t *f, uint32_t cmd, uint64_t arg)
{
	ev_client_t *c = f->priv;
	input_dev_t *d = c->dev;
	void *ua = (void *)(uintptr_t)arg;
	if (IOC_TYPE(cmd) != 'j') return -EINVAL;
	int nb = js_index(d->info.keybits, INPUT_DEV_KEYS), na = js_index(&d->info.absbits, 64);
	switch (IOC_NR(cmd)) {
	case 0x01: { uint32_t v = 0x020100; return copy_to_user(ua, &v, 4) ? -EFAULT : 0; }   /* GVERSION */
	case 0x11: { uint8_t v = (uint8_t)na; return copy_to_user(ua, &v, 1) ? -EFAULT : 0; } /* GAXES */
	case 0x12: { uint8_t v = (uint8_t)nb; return copy_to_user(ua, &v, 1) ? -EFAULT : 0; } /* GBUTTONS */
	case 0x13: return put_var(arg, cmd, d->info.name, strlen(d->info.name) + 1);           /* GNAME */
	case 0x32: {                                                                         /* GAXMAP */
		uint8_t m[64] = {0};
		for (int a = 0, i = 0; a < 64; a++) if (d->info.absbits >> a & 1) m[i++] = (uint8_t)a;
		return put_var(arg, cmd, m, sizeof m) < 0 ? -EFAULT : 0;
	}
	case 0x34: {                                                                         /* GBTNMAP */
		uint16_t *m = kmalloc(1024);
		if (!m) return -ENOMEM;
		memset(m, 0, 1024);
		for (int k = 0, i = 0; k < INPUT_DEV_KEYS && i < 512; k++) if (bit(d->info.keybits, k)) m[i++] = (uint16_t)k;
		int r = put_var(arg, cmd, m, 1024) < 0 ? -EFAULT : 0;
		kfree(m);
		return r;
	}
	}
	return -EINVAL;
}

static vfs_fops_t evd_fops = {
	.open = evd_open, .close = idev_close, .read = idev_read, .poll = idev_poll, .ioctl = evd_ioctl,
};
static vfs_fops_t jsd_fops = {
	.open = jsd_open, .close = idev_close, .read = idev_read, .poll = idev_poll, .ioctl = jsd_ioctl,
};
