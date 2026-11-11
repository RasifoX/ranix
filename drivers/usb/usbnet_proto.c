/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * USB network adapters (CDC-ECM, RNDIS): descriptors, MAC string, RNDIS messages.
 * Pure functions; see usbnet_proto.h for what each does and where it comes from.
 */
#include "usbnet_proto.h"

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* The first alternate setting of interface num with a bulk IN and a bulk OUT. */
static int data_eps(const uint8_t *c, uint32_t total, int num, un_desc_t *o)
{
	int in_it = 0, alt = -1;
	for (uint32_t i = 0; i + 2 <= total; ) {
		uint8_t blen = c[i], btype = c[i + 1];
		if (blen < 2 || blen > total - i) break;
		if (btype == 4 && blen >= 9) {
			if (o->ep_in && o->ep_out) return 1;    /* the previous alternate had both */
			in_it = c[i + 2] == num;
			alt = c[i + 3];
			o->ep_in = o->ep_out = 0;
		} else if (btype == 5 && blen >= 7 && in_it && (c[i + 3] & 3) == 2) {
			uint16_t mps = (uint16_t)(c[i + 4] | c[i + 5] << 8) & 0x7FF;
			if ((c[i + 2] & 0x80) && !o->ep_in) { o->ep_in = c[i + 2]; o->mps_in = mps; o->data_alt = (uint8_t)alt; }
			if (!(c[i + 2] & 0x80) && !o->ep_out) { o->ep_out = c[i + 2]; o->mps_out = mps; }
		}
		i += blen;
	}
	return o->ep_in && o->ep_out && o->mps_in && o->mps_out;
}

static int comm_kind(const uint8_t *d)
{
	uint8_t cls = d[5], sub = d[6], proto = d[7];
	if (cls == 2 && sub == 6) return UN_ECM;
	if ((cls == 2 && sub == 2 && proto == 0xFF) || (cls == 0xE0 && sub == 1 && proto == 3) ||
	    (cls == 0xEF && sub == 4 && proto == 1))
		return UN_RNDIS;
	return UN_NONE;
}

int un_match(const uint8_t *c, uint32_t total, un_desc_t *out)
{
	un_desc_t d = {0};
	int ctrl = -1, data = -1, in_ctrl = 0;
	for (uint32_t i = 0; i + 2 <= total; ) {
		uint8_t blen = c[i], btype = c[i + 1];
		if (blen < 2 || blen > total - i) break;
		if (btype == 4 && blen >= 9) {
			in_ctrl = 0;
			int k = comm_kind(c + i);
			if (k && ctrl < 0) {
				d.kind = (uint8_t)k;
				ctrl = c[i + 2];
				in_ctrl = 1;
			} else if (c[i + 5] == 0x0A && ctrl >= 0 && data < 0) {
				data = c[i + 2];
			}
		} else if (btype == 0x24 && blen >= 5 && in_ctrl && c[i + 2] == 6) {
			data = c[i + 4];                     /* union: its first subordinate */
		} else if (btype == 0x24 && blen >= 4 && in_ctrl && c[i + 2] == 0x0F) {
			d.mac_index = c[i + 3];              /* ECM: Ethernet networking */
		}
		i += blen;
	}
	if (ctrl < 0 || data < 0 || !data_eps(c, total, data, &d)) return UN_NONE;
	if (d.kind == UN_ECM && !d.mac_index) return UN_NONE;
	d.ctrl_iface = (uint8_t)ctrl;
	d.data_iface = (uint8_t)data;
	*out = d;
	return d.kind;
}

static int hexval(uint8_t ch)
{
	if (ch >= '0' && ch <= '9') return ch - '0';
	if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
	return -1;
}

int un_mac_from_string(const uint8_t *s, uint32_t len, uint8_t mac[6])
{
	if (len < 2 + 24 || s[0] < 2 + 24 || s[1] != 3) return -1;
	for (int i = 0; i < 6; i++) {
		const uint8_t *p = s + 2 + i * 4;          /* two UTF-16 digits */
		int hi = p[1] ? -1 : hexval(p[0]), lo = p[3] ? -1 : hexval(p[2]);
		if (hi < 0 || lo < 0) return -1;
		mac[i] = (uint8_t)(hi << 4 | lo);
	}
	return 0;
}

/* ---- RNDIS ---- */

uint32_t un_rndis_init(uint8_t *b, uint32_t reqid, uint32_t max_transfer)
{
	wr32(b, RNDIS_INIT_MSG); wr32(b + 4, 24); wr32(b + 8, reqid);
	wr32(b + 12, 1); wr32(b + 16, 0); wr32(b + 20, max_transfer);   /* version 1.0 */
	return 24;
}

uint32_t un_rndis_query(uint8_t *b, uint32_t reqid, uint32_t oid, uint32_t inlen)
{
	wr32(b, RNDIS_QUERY_MSG); wr32(b + 4, 28 + inlen); wr32(b + 8, reqid);
	wr32(b + 12, oid); wr32(b + 16, inlen); wr32(b + 20, 20); wr32(b + 24, 0);
	for (uint32_t i = 0; i < inlen; i++) b[28 + i] = 0;
	return 28 + inlen;
}

uint32_t un_rndis_set(uint8_t *b, uint32_t reqid, uint32_t oid, const uint8_t *val, uint32_t n)
{
	wr32(b, RNDIS_SET_MSG); wr32(b + 4, 28 + n); wr32(b + 8, reqid);
	wr32(b + 12, oid); wr32(b + 16, n); wr32(b + 20, 20); wr32(b + 24, 0);
	for (uint32_t i = 0; i < n; i++) b[28 + i] = val[i];
	return 28 + n;
}

int un_rndis_reply(const uint8_t *b, uint32_t len, uint32_t want, uint32_t reqid,
                   const uint8_t **info, uint32_t *n)
{
	if (len < 16 || rd32(b) != (want | RNDIS_CMPLT)) return -1;
	uint32_t mlen = rd32(b + 4);
	if (mlen > len || mlen < 16 || rd32(b + 8) != reqid || rd32(b + 12) != 0) return -1;
	if (want == RNDIS_QUERY_MSG) {
		if (mlen < 24) return -1;
		uint32_t ilen = rd32(b + 16), ioff = rd32(b + 20);
		if (ioff > mlen - 8 || ilen > mlen - 8 - ioff) return -1;
		*info = b + 8 + ioff;
		*n = ilen;
	}
	return 0;
}

void un_rndis_wrap(uint8_t *b, uint32_t n)
{
	for (int i = 0; i < RNDIS_HDR; i++) b[i] = 0;
	wr32(b, RNDIS_PACKET_MSG);
	wr32(b + 4, RNDIS_HDR + n);
	wr32(b + 8, RNDIS_HDR - 8);                  /* data offset, from this field */
	wr32(b + 12, n);
}

int un_rndis_unwrap(const uint8_t *b, uint32_t len, un_frame_t each, void *ctx)
{
	int count = 0;
	for (uint32_t at = 0; at + RNDIS_HDR <= len; ) {
		const uint8_t *m = b + at;
		uint32_t mlen = rd32(m + 4), off = rd32(m + 8), n = rd32(m + 12);
		if (rd32(m) != RNDIS_PACKET_MSG || mlen < RNDIS_HDR || mlen > len - at) break;
		if (off < RNDIS_HDR - 8 || off > mlen - 8 || n > mlen - 8 - off) break;
		if (n) { each(ctx, m + 8 + off, n); count++; }
		at += mlen;
	}
	return count;
}
