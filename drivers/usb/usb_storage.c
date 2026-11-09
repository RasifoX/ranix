/*
 * USB mass storage: Bulk-Only Transport, SCSI commands, the block device each
 * LUN 0 becomes, and the configuration parsing that finds the BOT interface.
 *
 * Part of the xHCI driver: compiled inside drivers/usb/xhci.c (a textual include,
 * like kernel/sys/syscall_*.c), so it shares that file's static types and state
 * and is not built on its own.
 */

/* ---- BOT (bulk-only transport) mass storage ---- */

typedef struct {
	uint32_t sig;       /* 0x43425355 'USBC' */
	uint32_t tag;
	uint32_t len;       /* data transfer length */
	uint8_t  flags;     /* bit7: 1=IN */
	uint8_t  lun;
	uint8_t  cblen;     /* command block length */
	uint8_t  cb[16];
} __attribute__((packed)) cbw_t;

typedef struct {
	uint32_t sig;       /* 0x53425355 'USBS' */
	uint32_t tag;
	uint32_t residue;
	uint8_t  status;
} __attribute__((packed)) csw_t;

#define BOT_OK      0
#define BOT_CHECK   1       /* the device reports a failure: ask for sense */
#define BOT_FAIL   (-1)     /* the transport lost its place, recovery has been run */

static void bot_reset(usb_dev_t *d)
{
	ctrl_xfer(d, 0x21, 0xFF, 0, d->iface, 0, 0);
	ctrl_xfer(d, 0x02, 1, 0, d->addr_in, 0, 0);
	ctrl_xfer(d, 0x02, 1, 0, d->addr_out, 0, 0);
}

static void bot_abort(usb_dev_t *d, int in, int rc)
{
	ep_recover(d, in, rc == XFER_STALL);
	bot_reset(d);
}

/* run one SCSI command via BOT: write CBW, optional data stage in d->data, read CSW.
 * want_all: anything short of dlen is a failure. */
static int bot_scsi(usb_dev_t *d, const uint8_t *cdb, int cdblen, int data_in,
                    uint32_t dlen, int want_all)
{
	cbw_t *cbw = (cbw_t *)d->dma;
	uint32_t tag = ++d->tag;
	memset(cbw, 0, sizeof(*cbw));
	cbw->sig = 0x43425355;
	cbw->tag = tag;
	cbw->len = dlen;
	cbw->flags = data_in ? 0x80 : 0;
	cbw->lun = 0;
	cbw->cblen = (uint8_t)cdblen;
	memcpy(cbw->cb, cdb, (size_t)cdblen);
	uint32_t got = 0;
	int rc = bulk_xfer(d, 0, d->dma_phys, 31, &got);
	if (rc != XFER_OK) { bot_abort(d, 0, rc); return BOT_FAIL; }

	uint32_t moved = 0;
	if (dlen) {
		rc = bulk_xfer(d, data_in, d->data_phys, dlen, &moved);
		if (rc == XFER_STALL) ep_recover(d, data_in, 1);   /* the CSW still follows */
		else if (rc != XFER_OK) { bot_abort(d, data_in, rc); return BOT_FAIL; }
	}

	csw_t *csw = (csw_t *)(d->dma + 64);
	memset(csw, 0, sizeof(*csw));
	rc = bulk_xfer(d, 1, d->dma_phys + 64, 13, &got);
	if (rc == XFER_STALL) {
		ep_recover(d, 1, 1);
		rc = bulk_xfer(d, 1, d->dma_phys + 64, 13, &got);
	}
	if (rc != XFER_OK) { bot_abort(d, 1, rc); return BOT_FAIL; }
	if (got != 13 || csw->sig != 0x53425355 || csw->tag != tag || csw->status > 1) {
		bot_reset(d);
		return BOT_FAIL;
	}
	if (csw->status) return BOT_CHECK;
	if (want_all && (moved != dlen || csw->residue)) return BOT_FAIL;
	return BOT_OK;
}

/* A command with the retries a real bridge needs: a unit attention after reset, or
 * not ready while the drive behind it wakes. */
static int usb_scsi(usb_dev_t *d, const uint8_t *cdb, int cdblen, int data_in,
                    uint32_t dlen, int want_all)
{
	for (int t = 0; t < 8; t++) {
		int rc = bot_scsi(d, cdb, cdblen, data_in, dlen, want_all);
		if (rc == BOT_OK) return 0;
		if (rc == BOT_FAIL) {
			if (t >= 2) return -1;
			continue;
		}
		uint8_t rs[6] = {0x03, 0, 0, 0, 18, 0};
		if (bot_scsi(d, rs, 6, 1, 18, 0) != BOT_OK) return -1;
		uint8_t key = d->data[2] & 0xF;
		if (key != 2 && key != 6) return -1;   /* only NOT READY and UNIT ATTENTION pass */
		delay_ms(key == 2 ? 200 : 10);
	}
	return -1;
}

/* One transfer of nb device blocks, in or out of d->data. */
static int usb_rw(usb_dev_t *d, int write, uint64_t blk, uint32_t nb)
{
	uint8_t cdb[16];
	int len;
	memset(cdb, 0, sizeof(cdb));
	if ((blk + nb) >> 32) {
		cdb[0] = write ? 0x8A : 0x88;          /* READ/WRITE(16), past 2 TB of 512s */
		for (int i = 0; i < 8; i++) cdb[2 + i] = (uint8_t)(blk >> (56 - 8 * i));
		cdb[12] = (uint8_t)(nb >> 8);
		cdb[13] = (uint8_t)nb;
		len = 16;
	} else {
		cdb[0] = write ? 0x2A : 0x28;
		cdb[2] = (uint8_t)(blk >> 24);
		cdb[3] = (uint8_t)(blk >> 16);
		cdb[4] = (uint8_t)(blk >> 8);
		cdb[5] = (uint8_t)blk;
		cdb[7] = (uint8_t)(nb >> 8);
		cdb[8] = (uint8_t)nb;
		len = 10;
	}
	return usb_scsi(d, cdb, len, !write, nb * d->blk, 1);
}

/* The block layer counts 512-byte sectors, a bridge may show 4 KB blocks. */
static int usb_read(block_device_t *dev, uint64_t lba, void *buf, uint64_t count)
{
	usb_disk_t *k = dev->priv;
	uint8_t *out = buf;
	int rc = 0;
	mutex_lock(&g_xhci.lock);
	usb_dev_t *d = k->dev;
	if (!d) { mutex_unlock(&g_xhci.lock); return -1; }
	uint32_t spb = d->blk / 512;
	while (count) {
		uint32_t in = (uint32_t)(lba % spb);
		uint64_t want = in + count;
		uint32_t max = DATA_BYTES / 512;
		uint32_t secs = want > max ? max : (uint32_t)want;
		uint32_t nb = (secs + spb - 1) / spb;
		if (nb * d->blk > DATA_BYTES) nb = DATA_BYTES / d->blk;
		if (usb_rw(d, 0, lba / spb, nb) != 0) { rc = -1; break; }
		uint64_t have = (uint64_t)nb * spb - in;
		if (have > count) have = count;
		memcpy(out, d->data + (uint64_t)in * 512, (size_t)have * 512);
		out += have * 512; lba += have; count -= have;
	}
	mutex_unlock(&g_xhci.lock);
	return rc;
}

static int usb_write(block_device_t *dev, uint64_t lba, const void *buf, uint64_t count)
{
	usb_disk_t *k = dev->priv;
	const uint8_t *in = buf;
	int rc = 0;
	mutex_lock(&g_xhci.lock);
	usb_dev_t *d = k->dev;
	if (!d) { mutex_unlock(&g_xhci.lock); return -1; }
	uint32_t spb = d->blk / 512;
	if ((lba | count) & (spb - 1)) { mutex_unlock(&g_xhci.lock); return -1; }   /* no partial device block */
	while (count) {
		uint32_t secs = count > DATA_BYTES / 512 ? DATA_BYTES / 512 : (uint32_t)count;
		secs -= secs % spb;
		memcpy(d->data, in, (size_t)secs * 512);
		if (usb_rw(d, 1, lba / spb, secs / spb) != 0) { rc = -1; break; }
		in += (uint64_t)secs * 512; lba += secs; count -= secs;
	}
	mutex_unlock(&g_xhci.lock);
	return rc;
}

/* ---- enumeration ---- */

/* set up an endpoint context for a bulk endpoint. ep_addr is the USB endpoint
 * address (bit7=dir). returns the DCI. */
static int setup_bulk_ep(usb_dev_t *d, uint8_t ep_addr, uint16_t mps, uint8_t burst, ring_t *r)
{
	int num = ep_addr & 0x0F;
	int in = (ep_addr & 0x80) ? 1 : 0;
	int dci = num * 2 + in;
	if (ring_init(r) != 0) return -1;
	uint32_t *ep = ctx_ep(d, dci);
	uint32_t ep_type = in ? 6 : 2;        /* bulk IN=6, bulk OUT=2 */
	ep[0] = 0;
	ep[1] = (ep_type << 3) | (3u << 1) |  /* CErr=3 */
	        ((uint32_t)burst << 8) | ((uint32_t)mps << 16);   /* MaxBurst, MaxPacket */
	ep[2] = (uint32_t)(r->phys | r->cycle);   /* TR dequeue ptr lo + DCS */
	ep[3] = (uint32_t)(r->phys >> 32);
	ep[4] = 3072;                         /* average TRB length for bandwidth estimates */
	return dci;
}

static void string_serial(usb_dev_t *d, uint8_t index)
{
	if (!index) return;
	/* first language id, then the string in it; ASCII is what a serial holds */
	if (ctrl_xfer(d, 0x80, 6, 0x0300, 0, 4, 1) != 0) return;
	uint16_t lang = (uint16_t)(d->dma[2] | d->dma[3] << 8);
	if (ctrl_xfer(d, 0x80, 6, (uint16_t)(0x0300 | index), lang, 254, 1) != 0) return;
	uint32_t n = d->dma[0];
	char s[47];
	uint32_t o = 0;
	if (n > 254) n = 254;
	for (uint32_t i = 2; i + 1 < n && o < sizeof(s); i += 2)
		s[o++] = d->dma[i + 1] ? '_' : (char)d->dma[i];
	block_copy_serial(d->serial, sizeof(d->serial), s, o);
}

/* Bring the configured device up as a disk. 0, or -1 with the device left unused. */
static int scsi_attach(usb_dev_t *d)
{
	uint8_t inq[6] = {0x12, 0, 0, 0, 36, 0};
	if (usb_scsi(d, inq, 6, 1, 36, 0) != 0) {
		kprintf(KERN_ERR "xhci: inquiry failed\n");
		return -1;
	}
	if ((d->data[0] & 0x1F) != 0) {              /* peripheral type 0: a disk */
		kprintf(KERN_INFO "xhci: lun 0 is not a disk (type %u)\n", d->data[0] & 0x1F);
		return -1;
	}

	uint8_t tur[6] = {0};
	for (int t = 0; t < 25; t++) {               /* five seconds for a drive to wake */
		if (usb_scsi(d, tur, 6, 0, 0, 0) == 0) break;
		delay_ms(200);
	}

	uint8_t rc10[10] = {0x25};
	if (usb_scsi(d, rc10, 10, 1, 8, 1) != 0) {
		kprintf(KERN_ERR "xhci: read capacity failed\n");
		return -1;
	}
	const uint8_t *b = d->data;
	uint64_t last = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
	uint32_t bsz  = (uint32_t)b[4] << 24 | (uint32_t)b[5] << 16 | (uint32_t)b[6] << 8 | b[7];
	if (last == 0xFFFFFFFFu) {                   /* too big for ten bytes */
		uint8_t rc16[16] = {0x9E, 0x10};
		rc16[13] = 32;
		if (usb_scsi(d, rc16, 16, 1, 32, 0) != 0) return -1;
		last = 0;
		for (int i = 0; i < 8; i++) last = last << 8 | b[i];
		bsz = (uint32_t)b[8] << 24 | (uint32_t)b[9] << 16 | (uint32_t)b[10] << 8 | b[11];
	}
	if ((bsz != 512 && bsz != 1024 && bsz != 2048 && bsz != 4096) || !last) {
		kprintf(KERN_ERR "xhci: unusable capacity, block size %u\n", bsz);
		return -1;
	}
	d->blk = bsz;

	/* a free name: registered and held by nothing, else never registered */
	usb_disk_t *k = NULL;
	for (int i = 0; i < MAX_DISKS && !k; i++)
		if (g_disk[i].registered && !g_disk[i].dev && !g_disk[i].bd.users) k = &g_disk[i];
	for (int i = 0; i < MAX_DISKS && !k; i++)
		if (!g_disk[i].registered) k = &g_disk[i];
	if (!k) {
		kprintf(KERN_WARN "xhci: all %d disk names are held\n", MAX_DISKS);
		return -1;
	}
	int nr = (int)(k - g_disk);
	block_device_t *bd = &k->bd;
	bd->size_blocks = (last + 1) * (bsz / 512);
	bd->lba_size = bsz;
	memcpy(bd->serial, d->serial, sizeof(bd->serial));
	d->disk = k;
	k->dev = d;
	if (k->registered) {
		block_device_rebind(bd);
		if (bd->ro) kprintf(KERN_INFO "block: %s read-only until allowed\n", bd->name);
	} else {
		int n = 0;
		bd->name[n++] = 'u'; bd->name[n++] = 's'; bd->name[n++] = 'b';
		if (nr >= 10) bd->name[n++] = (char)('0' + nr / 10);
		bd->name[n++] = (char)('0' + nr % 10);
		bd->name[n] = '\0';
		bd->read  = usb_read;
		bd->write = usb_write;
		bd->priv  = k;
		block_device_register(bd);
		k->registered = 1;
	}
	k->probe = 1;
	kprintf(KERN_INFO "xhci: %s %lu sectors (bsz %u)\n", bd->name,
	        (unsigned long)bd->size_blocks, bsz);
	return 0;
}

/* Find a BOT interface in the configuration descriptor at d->dma. */
typedef struct {
	int     found, uas_only;
	uint8_t iface;
	uint8_t ep_in, ep_out;
	uint16_t mps_in, mps_out;
	uint8_t burst_in, burst_out;
} msc_desc_t;

static void cfg_parse(const uint8_t *c, uint32_t total, msc_desc_t *m)
{
	int in_bot = 0, last_in = 0, any_uas = 0;
	memset(m, 0, sizeof(*m));
	for (uint32_t i = 0; i + 2 <= total; ) {
		uint8_t blen = c[i], btype = c[i + 1];
		if (blen < 2 || blen > total - i) break;
		if (btype == 4 && blen >= 9) {            /* interface */
			uint8_t cls = c[i + 5], sub = c[i + 6], proto = c[i + 7];
			in_bot = cls == 8 && sub == 6 && proto == 0x50 && c[i + 3] == 0 && !m->found;
			if (in_bot) { m->found = 1; m->iface = c[i + 2]; }
			if (cls == 8 && proto == 0x62) any_uas = 1;
		} else if (btype == 5 && blen >= 7 && in_bot) {   /* endpoint */
			uint8_t addr = c[i + 2];
			uint16_t mps = (uint16_t)(c[i + 4] | c[i + 5] << 8) & 0x7FF;
			if ((c[i + 3] & 3) == 2) {
				last_in = addr & 0x80;
				if (last_in) { m->ep_in = addr; m->mps_in = mps; }
				else { m->ep_out = addr; m->mps_out = mps; }
			}
		} else if (btype == 0x30 && blen >= 6 && in_bot) {  /* SS endpoint companion */
			if (last_in) m->burst_in = c[i + 2];
			else m->burst_out = c[i + 2];
		}
		i += blen;
	}
	m->uas_only = any_uas && !m->found;
	if (!m->ep_in || !m->ep_out || !m->mps_in || !m->mps_out) m->found = 0;
}
