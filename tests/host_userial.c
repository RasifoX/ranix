/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Host test of the USB serial arithmetic, against descriptors of real boards:
 *   cc -I. tests/host_userial.c drivers/usb/userial_proto.c && ./a.out
 * Expected request bytes come from the public documents named in userial_proto.h
 * and from known values (FTDI 9600 = 0x4138, CH340 9600 = 0xB202). */
#include <stdio.h>
#include <string.h>
#include "drivers/usb/userial_proto.h"

static int fails, passes;
#define CHECK(name, cond) do { if (cond) passes++; else { fails++; printf("FAIL: %s\n", name); } } while (0)

/* Arduino Uno R3 (ATmega16U2, 2341:0043): CDC-ACM, union 0 -> 1 */
static const uint8_t uno[] = {
	9, 2, 62, 0, 2, 1, 0, 0xC0, 50,
	9, 4, 0, 0, 1, 2, 2, 1, 0,
	5, 0x24, 0, 0x10, 0x01,
	4, 0x24, 2, 6,
	5, 0x24, 6, 0, 1,
	7, 5, 0x82, 3, 8, 0, 0xFF,
	9, 4, 1, 0, 2, 0x0A, 0, 0, 0,
	7, 5, 0x04, 2, 64, 0, 1,
	7, 5, 0x83, 2, 64, 0, 1,
};
/* Arduino Leonardo style composite: IAD, CDC on 2 and 3 (no union), HID on 0 */
static const uint8_t leo[] = {
	9, 2, 100, 0, 3, 1, 0, 0x80, 250,
	9, 4, 0, 0, 1, 3, 0, 0, 0,
	9, 0x21, 1, 1, 0, 1, 0x22, 101, 0,
	7, 5, 0x84, 3, 64, 0, 1,
	8, 0x0B, 2, 2, 2, 2, 1, 0,
	9, 4, 2, 0, 1, 2, 2, 0, 0,
	7, 5, 0x81, 3, 16, 0, 64,
	9, 4, 3, 0, 2, 0x0A, 0, 0, 0,
	7, 5, 0x02, 2, 64, 0, 0,
	7, 5, 0x83, 2, 64, 0, 0,
};
/* CH340 (1a86:7523): vendor class, bulk 0x82/0x02 of 32, interrupt 0x81 */
static const uint8_t ch340[] = {
	9, 2, 39, 0, 1, 1, 0, 0x80, 49,
	9, 4, 0, 0, 3, 0xFF, 1, 2, 0,
	7, 5, 0x82, 2, 32, 0, 0,
	7, 5, 0x02, 2, 32, 0, 0,
	7, 5, 0x81, 3, 8, 0, 1,
};
/* CP2102 (10c4:ea60) and QEMU's FTDI (0403:6001): one vendor interface, 64-byte bulk */
static const uint8_t cp2102[] = {
	9, 2, 32, 0, 1, 1, 0, 0x80, 50,
	9, 4, 0, 0, 2, 0xFF, 0, 0, 2,
	7, 5, 0x81, 2, 64, 0, 0,
	7, 5, 0x01, 2, 64, 0, 0,
};
static const uint8_t ftdi[] = {
	9, 2, 32, 0, 1, 1, 0, 0x80, 50,
	9, 4, 0, 0, 2, 0xFF, 0xFF, 0xFF, 2,
	7, 5, 0x81, 2, 64, 0, 0,
	7, 5, 0x02, 2, 64, 0, 0,
};
/* not serial: a boot keyboard, and a CDC-ECM network adapter (subclass 6) */
static const uint8_t kbd[] = {
	9, 2, 34, 0, 1, 1, 0, 0xA0, 50,
	9, 4, 0, 0, 1, 3, 1, 1, 0,
	9, 0x21, 0x11, 1, 0, 1, 0x22, 63, 0,
	7, 5, 0x81, 3, 8, 0, 10,
};
static const uint8_t ecm[] = {
	9, 2, 71, 0, 2, 1, 0, 0xC0, 50,
	9, 4, 0, 0, 1, 2, 6, 0, 0,
	5, 0x24, 6, 0, 1,
	7, 5, 0x81, 3, 16, 0, 9,
	9, 4, 1, 0, 0, 0x0A, 0, 0, 0,
	9, 4, 1, 1, 2, 0x0A, 0, 0, 0,
	7, 5, 0x82, 2, 64, 0, 0,
	7, 5, 0x03, 2, 64, 0, 0,
};

static void t_match(void)
{
	us_desc_t d;
	CHECK("uno acm", us_match(0x2341, 0x0043, uno, sizeof uno, &d) == US_ACM);
	CHECK("uno ifaces", d.ctrl_iface == 0 && d.data_iface == 1);
	CHECK("uno eps", d.ep_in == 0x83 && d.ep_out == 0x04 && d.mps_in == 64 && d.mps_out == 64);
	CHECK("leo acm", us_match(0x2341, 0x8036, leo, sizeof leo, &d) == US_ACM);
	CHECK("leo ifaces", d.ctrl_iface == 2 && d.data_iface == 3 && d.ep_in == 0x83 && d.ep_out == 0x02);
	CHECK("ch340", us_match(0x1A86, 0x7523, ch340, sizeof ch340, &d) == US_CH341);
	CHECK("ch340 eps", d.ep_in == 0x82 && d.ep_out == 0x02 && d.mps_in == 32);
	CHECK("cp2102", us_match(0x10C4, 0xEA60, cp2102, sizeof cp2102, &d) == US_CP210X);
	CHECK("cp2102 eps", d.ep_in == 0x81 && d.ep_out == 0x01 && d.ctrl_iface == 0);
	CHECK("ftdi", us_match(0x0403, 0x6001, ftdi, sizeof ftdi, &d) == US_FTDI);
	CHECK("ftdi eps", d.ep_in == 0x81 && d.ep_out == 0x02 && d.mps_in == 64);
	CHECK("kbd no", us_match(0x046D, 0xC31C, kbd, sizeof kbd, &d) == US_NONE);
	CHECK("ecm no", us_match(0x0525, 0xA4A1, ecm, sizeof ecm, &d) == US_NONE);
	uint8_t rndis[sizeof uno];
	memcpy(rndis, uno, sizeof uno);
	rndis[9 + 7] = 0xFF;          /* the same interfaces with protocol 0xFF are RNDIS, a network */
	CHECK("rndis is not serial", us_match(0x0525, 0xA4A2, rndis, sizeof rndis, &d) == US_NONE);
	CHECK("known id, wrong shape", us_match(0x1A86, 0x7523, kbd, sizeof kbd, &d) == US_NONE);
	/* every truncation of every descriptor is safe and never matches half a device */
	int bad = 0;
	for (uint32_t n = 0; n < sizeof uno; n++)
		if (us_match(0x2341, 0x0043, uno, n, &d) != US_NONE) bad++;
	CHECK("uno truncated", bad == 0);
	uint8_t junk[64];
	for (int i = 0; i < 64; i++) junk[i] = (uint8_t)(i * 37 + 1);
	junk[0] = 0;   /* a zero length must not loop */
	CHECK("junk", us_match(0x1234, 0x5678, junk, sizeof junk, &d) == US_NONE);
}

static void t_termios(void)
{
	CHECK("B9600", us_cflag_baud(13, 0) == 9600);
	CHECK("B115200", us_cflag_baud(0x1002, 0) == 115200);
	CHECK("B4000000", us_cflag_baud(0x100F, 0) == 4000000);
	CHECK("BOTHER", us_cflag_baud(0x1000 | 0x30, 250000) == 250000);
	CHECK("B0", us_cflag_baud(0, 1234) == 0);
	CHECK("code 115200", us_baud_code(115200) == 0x1002);
	CHECK("code 1200", us_baud_code(1200) == 9);
	CHECK("code 250000", us_baud_code(250000) == US_BOTHER);
	us_line_t l;
	us_line_from_cflag(0x1002 | 0x30, 0, &l);
	CHECK("8N1", l.baud == 115200 && l.bits == 8 && l.parity == 0 && !l.stop2 && !l.rtscts);
	us_line_from_cflag(13 | 0x20 | US_PARENB | US_CSTOPB | US_CRTSCTS, 0, &l);
	CHECK("7E2 rtscts", l.bits == 7 && l.parity == 2 && l.stop2 && l.rtscts);
	us_line_from_cflag(13 | 0x30 | US_PARENB | US_PARODD | US_CMSPAR, 0, &l);
	CHECK("mark", l.parity == 3);
	CHECK("acm drops rtscts", !(us_cflag_fix(US_ACM, 13 | US_CRTSCTS) & US_CRTSCTS));
	CHECK("cp210x keeps rtscts", us_cflag_fix(US_CP210X, 13 | US_CRTSCTS) & US_CRTSCTS);
	CHECK("ftdi cs5 to cs8", (us_cflag_fix(US_FTDI, 13) & US_CSIZE) == 0x30);
	CHECK("no parity, no parodd", !(us_cflag_fix(US_CP210X, 13 | US_PARODD) & US_PARODD));
}

static void t_divisors(void)
{
	uint8_t p, v;
	CHECK("ch341 115200", us_ch341_divisor(115200, &p, &v) == 0 && p == 3 && v == 0xCC);
	CHECK("ch341 9600", us_ch341_divisor(9600, &p, &v) == 0 && p == 2 && v == 0xB2);
	CHECK("ch341 2400", us_ch341_divisor(2400, &p, &v) == 0 && p == 1 && v == 0xD9);
	CHECK("ch341 921600", us_ch341_divisor(921600, &p, &v) == 0 && p == 7 && v == 0xF3);
	CHECK("ch341 1M", us_ch341_divisor(1000000, &p, &v) == 0 && p == 3 && v == 250);
	CHECK("ch341 too slow", us_ch341_divisor(20, &p, &v) != 0);
	/* every standard speed within 3 % */
	static const uint32_t sp[] = { 300, 1200, 2400, 4800, 9600, 19200, 38400, 57600,
	                               115200, 230400, 250000, 460800, 500000, 1000000, 2000000 };
	static const uint32_t base[8] = { 11719, 93750, 750000, 6000000, 0, 0, 0, 12000000 };
	int off = 0;
	for (unsigned i = 0; i < sizeof sp / sizeof sp[0]; i++) {
		if (us_ch341_divisor(sp[i], &p, &v) != 0) { off++; continue; }
		double real = (double)base[p] / (256 - v), err = real / sp[i] - 1;
		if (err > 0.03 || err < -0.03) { off++; printf("  ch341 %u -> %.0f\n", sp[i], real); }
	}
	CHECK("ch341 within 3%", off == 0);
	uint16_t a, b;
	CHECK("ftdi 9600", us_ftdi_divisor(9600, &a, &b) == 0 && a == 0x4138 && b == 0);
	CHECK("ftdi 115200", us_ftdi_divisor(115200, &a, &b) == 0 && a == 0x001A && b == 0);
	CHECK("ftdi 3M", us_ftdi_divisor(3000000, &a, &b) == 0 && a == 0 && b == 0);
	CHECK("ftdi 2M", us_ftdi_divisor(2000000, &a, &b) == 0 && a == 1 && b == 0);
	/* 3 MHz / 2.875: fraction 7/8 is code 7, whose top bit is bit 16 (wIndex bit 0) */
	CHECK("ftdi bit16", us_ftdi_divisor(1043478, &a, &b) == 0 && b == 1 && (a & 0x3FFF) == 2 && a >> 14 == 3);
	/* below a divisor of 2 only 3 MBd (0) and 2 MBd (1) exist: 1.6 MBd becomes 2 MBd */
	CHECK("ftdi no 1.875", us_ftdi_divisor(1600000, &a, &b) == 0 && a == 1 && b == 0);
	CHECK("ftdi too slow", us_ftdi_divisor(100, &a, &b) != 0);
}

static int has(const us_req_t *r, int n, uint8_t rtype, uint8_t req, uint16_t val, uint16_t idx)
{
	for (int i = 0; i < n; i++)
		if (r[i].rtype == rtype && r[i].req == req && r[i].val == val && r[i].idx == idx) return i + 1;
	return 0;
}

static void t_requests(void)
{
	us_desc_t acm = { .kind = US_ACM, .ctrl_iface = 2 }, ch = { .kind = US_CH341 },
	          cp = { .kind = US_CP210X, .ctrl_iface = 0 }, ft = { .kind = US_FTDI };
	us_req_t r[US_MAXREQ];
	us_line_t l = { .baud = 115200, .bits = 8 };
	int n = us_line_reqs(&acm, &l, r);
	static const uint8_t lc[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };
	CHECK("acm line coding", n == 1 && r[0].rtype == 0x21 && r[0].req == 0x20 && r[0].idx == 2 &&
	      r[0].len == 7 && !memcmp(r[0].data, lc, 7));
	n = us_modem_reqs(&acm, 1, 1, r);
	CHECK("acm dtr rts", n == 1 && has(r, n, 0x21, 0x22, 3, 2));
	n = us_modem_reqs(&acm, 0, 0, r);
	CHECK("acm drop", has(r, n, 0x21, 0x22, 0, 2));
	CHECK("acm break", us_break_reqs(&acm, &l, 1, r) == 1 && has(r, 1, 0x21, 0x23, 0xFFFF, 2));
	l.baud = 0;
	CHECK("B0 no line", us_line_reqs(&acm, &l, r) == 0);

	l = (us_line_t){ .baud = 9600, .bits = 8 };
	n = us_line_reqs(&ch, &l, r);
	CHECK("ch341 rate reg", has(r, n, 0x40, 0x9A, 0x1312, 0xB282));
	CHECK("ch341 lcr 8n1", has(r, n, 0x40, 0x9A, 0x2518, 0xC3));
	l.parity = 2; l.stop2 = 1; l.bits = 7;
	n = us_line_reqs(&ch, &l, r);
	CHECK("ch341 lcr 7e2", has(r, n, 0x40, 0x9A, 0x2518, 0xC0 | 2 | 0x04 | 0x18));
	n = us_modem_reqs(&ch, 1, 0, r);
	CHECK("ch341 dtr low-active", n == 1 && r[0].req == 0xA4 && r[0].val == (uint16_t)~0x20);
	CHECK("ch341 open", us_open_reqs(&ch, r) == 2 && r[0].req == 0x5F && r[0].len == 2 && r[1].req == 0xA1);
	CHECK("ch341 break is read-modify-write", us_break_reqs(&ch, &l, 1, r) == US_BREAK_RMW);
	CHECK("ch341 break on", us_ch341_break(0xFF, 0xC3, 1) == (0xFE | 0x83 << 8));
	CHECK("ch341 break off", us_ch341_break(0xFE, 0x83, 0) == (0xFF | 0xC3 << 8));

	l = (us_line_t){ .baud = 115200, .bits = 8, .rtscts = 1 };
	n = us_line_reqs(&cp, &l, r);
	int k = has(r, n, 0x41, 0x1E, 0, 0);
	CHECK("cp210x baud", k && r[k - 1].len == 4 && r[k - 1].data[0] == 0x00 && r[k - 1].data[1] == 0xC2 &&
	      r[k - 1].data[2] == 0x01);
	CHECK("cp210x line 8n1", has(r, n, 0x41, 0x03, 0x0800, 0));
	k = has(r, n, 0x41, 0x13, 0, 0);
	CHECK("cp210x flow", k && r[k - 1].len == 16 && r[k - 1].data[0] == 0x09 && r[k - 1].data[4] == 0x80);
	CHECK("cp210x open", us_open_reqs(&cp, r) == 1 && has(r, 1, 0x41, 0x00, 1, 0));
	CHECK("cp210x close", us_close_reqs(&cp, r) == 1 && has(r, 1, 0x41, 0x00, 0, 0));
	n = us_modem_reqs(&cp, 0, 1, r);
	CHECK("cp210x mhs", has(r, n, 0x41, 0x07, 0x0302, 0));

	l = (us_line_t){ .baud = 9600, .bits = 8, .parity = 1 };
	n = us_line_reqs(&ft, &l, r);
	CHECK("ftdi baud", has(r, n, 0x40, 3, 0x4138, 0));
	CHECK("ftdi data 8O1", has(r, n, 0x40, 4, 0x0108, 0));
	CHECK("ftdi flow off", has(r, n, 0x40, 2, 0, 0));
	n = us_modem_reqs(&ft, 1, 1, r);
	CHECK("ftdi modem", has(r, n, 0x40, 1, 0x0303, 0));
	CHECK("ftdi break keeps 8O1", us_break_reqs(&ft, &l, 1, r) == 1 && has(r, 1, 0x40, 4, 0x4108, 0));
	CHECK("ftdi break off", us_break_reqs(&ft, &l, 0, r) == 1 && has(r, 1, 0x40, 4, 0x0108, 0));
}

static void t_rx(void)
{
	/* FTDI: two packets of 64 (2 status + 62 data) then a short one of 2 + 3 */
	uint8_t b[64 + 64 + 5];
	for (int i = 0; i < (int)sizeof b; i++) b[i] = (uint8_t)('A' + i % 26);
	b[0] = 0x31; b[1] = 0x60; b[64] = 0x31; b[65] = 0x60; b[128] = 0xB1; b[129] = 0x60;
	uint32_t msr = 0;
	uint32_t n = us_rx_payload(US_FTDI, 64, b, sizeof b, &msr);
	CHECK("ftdi strip len", n == 62 + 62 + 3);
	CHECK("ftdi strip data", b[0] == 'A' + 2 % 26 && b[62] == 'A' + 66 % 26 && b[124] == 'A' + 130 % 26);
	CHECK("ftdi msr", msr == (US_TIOCM_CTS | US_TIOCM_DSR | US_TIOCM_CAR));
	uint8_t st[2] = { 0x01, 0x60 };
	CHECK("ftdi status only", us_rx_payload(US_FTDI, 64, st, 2, &msr) == 0);
	uint8_t raw[5] = "hello";
	msr = 7;
	CHECK("acm raw", us_rx_payload(US_ACM, 64, raw, 5, &msr) == 5 && msr == 7 && raw[0] == 'h');
}

int main(void)
{
	t_match();
	t_termios();
	t_divisors();
	t_requests();
	t_rx();
	printf("userial: %d passed, %d failed\n", passes, fails);
	if (!fails) printf("ALL PASS\n");
	return fails != 0;
}
