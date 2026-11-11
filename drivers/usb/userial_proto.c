/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * USB serial adapters: descriptors to endpoints, termios to control requests.
 * Pure functions; see userial_proto.h for what each does and where it comes from.
 */
#include "userial_proto.h"

/* ---- which device ---- */

static const struct { uint16_t vid, pid; uint8_t kind; } us_ids[] = {
	{ 0x1A86, 0x7523, US_CH341 },   /* CH340 */
	{ 0x1A86, 0x7522, US_CH341 },   /* CH340K */
	{ 0x1A86, 0x5523, US_CH341 },   /* CH341 in serial mode */
	{ 0x10C4, 0xEA60, US_CP210X },  /* CP2102, CP2104, CP2102N */
	{ 0x10C4, 0xEA61, US_CP210X },
	{ 0x10C4, 0xEA63, US_CP210X },
	{ 0x10C4, 0xEA70, US_CP210X },  /* CP2105: its first port */
	{ 0x10C4, 0xEA71, US_CP210X },  /* CP2108: its first port */
	{ 0x10C4, 0xEA80, US_CP210X },
	{ 0x0403, 0x6001, US_FTDI },    /* FT232R, FT232BM; QEMU usb-serial */
	{ 0x0403, 0x6015, US_FTDI },    /* FT-X series */
};

/* The bulk endpoints of interface num (alternate setting 0). 1 when it has both. */
static int iface_bulk(const uint8_t *c, uint32_t total, int num, us_desc_t *o)
{
	int in_it = 0;
	o->ep_in = o->ep_out = 0;
	for (uint32_t i = 0; i + 2 <= total; ) {
		uint8_t blen = c[i], btype = c[i + 1];
		if (blen < 2 || blen > total - i) break;
		if (btype == 4 && blen >= 9) {
			in_it = c[i + 2] == num && c[i + 3] == 0;
		} else if (btype == 5 && blen >= 7 && in_it && (c[i + 3] & 3) == 2) {
			uint16_t mps = (uint16_t)(c[i + 4] | c[i + 5] << 8) & 0x7FF;
			if ((c[i + 2] & 0x80) && !o->ep_in) { o->ep_in = c[i + 2]; o->mps_in = mps; }
			if (!(c[i + 2] & 0x80) && !o->ep_out) { o->ep_out = c[i + 2]; o->mps_out = mps; }
		}
		i += blen;
	}
	return o->ep_in && o->ep_out && o->mps_in && o->mps_out;
}

/* CDC-ACM: a communication interface of the ACM subclass, and the data interface
 * its union descriptor names (or, without one, the next data class interface). */
static int acm_find(const uint8_t *c, uint32_t total, us_desc_t *o)
{
	int ctrl = -1, data = -1, in_ctrl = 0;
	for (uint32_t i = 0; i + 2 <= total; ) {
		uint8_t blen = c[i], btype = c[i + 1];
		if (blen < 2 || blen > total - i) break;
		if (btype == 4 && blen >= 9) {
			in_ctrl = 0;
			if (c[i + 5] == 2 && c[i + 6] == 2 && c[i + 7] != 0xFF && ctrl < 0) {   /* 0xFF: RNDIS */
				ctrl = c[i + 2];
				in_ctrl = 1;
			} else if (c[i + 5] == 0x0A && ctrl >= 0 && data < 0) {
				data = c[i + 2];
			}
		} else if (btype == 0x24 && blen >= 5 && c[i + 2] == 6 && in_ctrl) {
			data = c[i + 4];        /* union: control, then its first subordinate */
		}
		i += blen;
	}
	if (ctrl < 0 || data < 0 || !iface_bulk(c, total, data, o)) return 0;
	o->ctrl_iface = (uint8_t)ctrl;
	o->data_iface = (uint8_t)data;
	return 1;
}

int us_match(uint16_t vid, uint16_t pid, const uint8_t *cfg, uint32_t total, us_desc_t *out)
{
	us_desc_t d = {0};
	for (unsigned k = 0; k < sizeof(us_ids) / sizeof(us_ids[0]); k++) {
		if (us_ids[k].vid != vid || us_ids[k].pid != pid) continue;
		if (!iface_bulk(cfg, total, 0, &d)) return US_NONE;
		d.kind = us_ids[k].kind;
		*out = d;
		return d.kind;
	}
	if (!acm_find(cfg, total, &d)) return US_NONE;
	d.kind = US_ACM;
	*out = d;
	return US_ACM;
}

/* ---- termios ---- */

static const uint32_t us_low[16] = {
	0, 50, 75, 110, 134, 150, 200, 300, 600, 1200, 1800, 2400, 4800, 9600, 19200, 38400
};
static const uint32_t us_high[16] = {   /* CBAUDEX | 1 .. 15; [0] is BOTHER */
	0, 57600, 115200, 230400, 460800, 500000, 576000, 921600, 1000000,
	1152000, 1500000, 2000000, 2500000, 3000000, 3500000, 4000000
};

uint32_t us_cflag_baud(uint32_t cflag, uint32_t ospeed)
{
	uint32_t code = cflag & US_CBAUD;
	if (code == US_BOTHER) return ospeed;
	if (code & US_BOTHER) return us_high[code & 15];
	return us_low[code & 15];
}

uint32_t us_baud_code(uint32_t baud)
{
	for (uint32_t i = 0; i < 16; i++) {
		if (us_low[i] == baud) return i;
		if (i && us_high[i] == baud) return US_BOTHER | i;
	}
	return US_BOTHER;
}

uint32_t us_cflag_fix(int kind, uint32_t cflag)
{
	if (kind == US_ACM || kind == US_CH341)
		cflag &= ~US_CRTSCTS;              /* no hardware flow control in either */
	if (kind == US_FTDI && (cflag & US_CSIZE) < 0x20)
		cflag |= US_CSIZE;                 /* FT232R: 7 or 8 data bits */
	if (!(cflag & US_PARENB))
		cflag &= ~(US_PARODD | US_CMSPAR);
	return cflag;
}

void us_line_from_cflag(uint32_t cflag, uint32_t ospeed, us_line_t *l)
{
	l->baud = us_cflag_baud(cflag, ospeed);
	l->bits = (uint8_t)(5 + ((cflag & US_CSIZE) >> 4));
	l->stop2 = !!(cflag & US_CSTOPB);
	l->rtscts = !!(cflag & US_CRTSCTS);
	if (!(cflag & US_PARENB))  l->parity = 0;
	else if (cflag & US_CMSPAR) l->parity = (cflag & US_PARODD) ? 3 : 4;
	else                        l->parity = (cflag & US_PARODD) ? 1 : 2;
}

/* ---- divisors ---- */

/* CH340/CH341: rate = base(ps) / (256 - div), base 6 MHz / 8^(3-ps). Two rates are
 * only reachable on the 12 MHz base (prescaler 7), as FreeBSD's table has it. */
int us_ch341_divisor(uint32_t baud, uint8_t *pre, uint8_t *div)
{
	static const uint32_t base[4] = { 11719, 93750, 750000, 6000000 };
	if (baud == 921600) { *pre = 7; *div = 0xF3; return 0; }
	if (baud == 307200) { *pre = 7; *div = 0xD9; return 0; }
	if (!baud) return -1;
	for (int ps = 3; ps >= 0; ps--) {
		uint32_t q = (base[ps] + baud / 2) / baud;
		if (q >= 2 && q <= 256) {
			*pre = (uint8_t)ps;
			*div = (uint8_t)(256 - q);
			return 0;
		}
	}
	return -1;
}

/* FTDI (AN232B-05): divisor of 3 MHz in eighths. The fraction is a 3-bit code in
 * bits 14-16; 0 means 3 MBd and 1 means 2 MBd. */
int us_ftdi_divisor(uint32_t baud, uint16_t *val, uint16_t *idx)
{
	static const uint8_t code[8] = { 0, 3, 2, 4, 1, 5, 6, 7 };
	if (baud < 183 || baud > 3000000) return -1;
	uint32_t d8 = (24000000u + baud / 2) / baud, enc;
	if (d8 < 12)       enc = 0;
	else if (d8 < 16)  enc = 1;
	else               enc = (d8 >> 3) | (uint32_t)code[d8 & 7] << 14;
	*val = (uint16_t)enc;
	*idx = (uint16_t)(enc >> 16);
	return 0;
}

/* ---- requests ---- */

static us_req_t *rq(us_req_t *r, uint8_t rtype, uint8_t req, uint16_t val, uint16_t idx)
{
	r->rtype = rtype;
	r->req = req;
	r->val = val;
	r->idx = idx;
	r->len = 0;
	return r;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int us_open_reqs(const us_desc_t *d, us_req_t *r)
{
	switch (d->kind) {
	case US_CH341:
		rq(&r[0], 0xC0, 0x5F, 0, 0)->len = 2;      /* read version: wakes the chip */
		rq(&r[1], 0x40, 0xA1, 0, 0);               /* serial init */
		return 2;
	case US_CP210X:
		rq(&r[0], 0x41, 0x00, 1, d->ctrl_iface);   /* IFC_ENABLE */
		return 1;
	case US_FTDI:
		rq(&r[0], 0x40, 0, 0, 0);                  /* reset */
		rq(&r[1], 0x40, 0, 1, 0);                  /* purge receive */
		rq(&r[2], 0x40, 0, 2, 0);                  /* purge transmit */
		return 3;
	}
	return 0;
}

int us_close_reqs(const us_desc_t *d, us_req_t *r)
{
	if (d->kind != US_CP210X) return 0;
	rq(&r[0], 0x41, 0x00, 0, d->ctrl_iface);       /* IFC_ENABLE off */
	return 1;
}

static int ch341_line(const us_line_t *l, us_req_t *r)
{
	static const uint8_t par[5] = { 0x00, 0x08, 0x18, 0x28, 0x38 };
	uint8_t pre, div;
	int n = 0;
	if (us_ch341_divisor(l->baud, &pre, &div) == 0)
		rq(&r[n++], 0x40, 0x9A, 0x1312, (uint16_t)((pre | 0x80) | div << 8));
	uint8_t lcr = 0xC0 | (uint8_t)(l->bits - 5) | (l->stop2 ? 0x04 : 0) | par[l->parity];
	rq(&r[n++], 0x40, 0x9A, 0x2518, lcr);
	return n;
}

static int cp210x_line(const us_desc_t *d, const us_line_t *l, us_req_t *r)
{
	uint16_t i = d->ctrl_iface;
	rq(&r[0], 0x41, 0x1E, 0, i)->len = 4;          /* SET_BAUDRATE */
	put32(r[0].data, l->baud);
	rq(&r[1], 0x41, 0x03, (uint16_t)((l->stop2 ? 2 : 0) | l->parity << 4 | l->bits << 8), i);
	rq(&r[2], 0x41, 0x13, 0, i)->len = 16;         /* SET_FLOW */
	for (int k = 0; k < 16; k++) r[2].data[k] = 0;
	put32(r[2].data, l->rtscts ? 0x09 : 0x01);     /* DTR on; CTS handshake */
	put32(r[2].data + 4, l->rtscts ? 0x80 : 0x40); /* RTS: flow control, or on */
	return 3;
}

/* FTDI SET_DATA: data bits, parity (bits 8-10), stop bits (11-13), break (14) */
static uint16_t ftdi_data(const us_line_t *l, int brk)
{
	return (uint16_t)(l->bits | l->parity << 8 | (l->stop2 ? 2 : 0) << 11 | (brk ? 1 : 0) << 14);
}

int us_line_reqs(const us_desc_t *d, const us_line_t *l, us_req_t *r)
{
	if (!l->baud) return 0;                        /* B0: only the wires drop */
	switch (d->kind) {
	case US_ACM:
		rq(&r[0], 0x21, 0x20, 0, d->ctrl_iface)->len = 7;   /* SET_LINE_CODING */
		put32(r[0].data, l->baud);
		r[0].data[4] = l->stop2 ? 2 : 0;
		r[0].data[5] = l->parity;
		r[0].data[6] = l->bits;
		return 1;
	case US_CH341:
		return ch341_line(l, r);
	case US_CP210X:
		return cp210x_line(d, l, r);
	case US_FTDI: {
		uint16_t v, ix;
		int n = 0;
		if (us_ftdi_divisor(l->baud, &v, &ix) == 0)
			rq(&r[n++], 0x40, 3, v, ix);
		rq(&r[n++], 0x40, 4, ftdi_data(l, 0), 0);
		rq(&r[n++], 0x40, 2, 0, l->rtscts ? 0x0100 : 0);
		return n;
	}
	}
	return 0;
}

int us_modem_reqs(const us_desc_t *d, int dtr, int rts, us_req_t *r)
{
	uint16_t b = (uint16_t)((dtr ? 1 : 0) | (rts ? 2 : 0));
	switch (d->kind) {
	case US_ACM:   rq(&r[0], 0x21, 0x22, b, d->ctrl_iface); return 1;
	case US_CH341: rq(&r[0], 0x40, 0xA4, (uint16_t)~(b << 5), 0); return 1;   /* active low */
	case US_CP210X: rq(&r[0], 0x41, 0x07, b | 0x0300, d->ctrl_iface); return 1;
	case US_FTDI:  rq(&r[0], 0x40, 1, b | 0x0300, 0); return 1;
	}
	return 0;
}

int us_break_reqs(const us_desc_t *d, const us_line_t *l, int on, us_req_t *r)
{
	if (d->kind == US_ACM)   { rq(&r[0], 0x21, 0x23, on ? 0xFFFF : 0, d->ctrl_iface); return 1; }
	if (d->kind == US_CP210X) { rq(&r[0], 0x41, 0x05, on ? 1 : 0, d->ctrl_iface); return 1; }
	if (d->kind == US_FTDI)  { rq(&r[0], 0x40, 4, ftdi_data(l, on), 0); return 1; }
	if (d->kind == US_CH341) return US_BREAK_RMW;
	return -1;
}

/* CH341: register 0x18 bit 0 low = break, register 0x05 bit 6 = transmitter on
 * (the chip holds the line low with the transmitter off). */
uint16_t us_ch341_break(uint8_t reg18, uint8_t reg05, int on)
{
	if (on) { reg18 &= (uint8_t)~0x01; reg05 &= (uint8_t)~0x40; }
	else    { reg18 |= 0x01; reg05 |= 0x40; }
	return (uint16_t)(reg18 | reg05 << 8);
}

/* ---- received bytes ---- */

uint32_t us_rx_payload(int kind, uint16_t mps, uint8_t *buf, uint32_t len, uint32_t *msr)
{
	if (kind != US_FTDI || !mps) return len;
	uint32_t out = 0;
	for (uint32_t at = 0; at < len; at += mps) {
		uint32_t n = len - at < mps ? len - at : mps;
		if (n < 2) break;
		uint8_t s = buf[at];
		*msr = (s & 0x10 ? US_TIOCM_CTS : 0) | (s & 0x20 ? US_TIOCM_DSR : 0) |
		       (s & 0x40 ? US_TIOCM_RNG : 0) | (s & 0x80 ? US_TIOCM_CAR : 0);
		for (uint32_t k = 2; k < n; k++) buf[out++] = buf[at + k];
	}
	return out;
}
