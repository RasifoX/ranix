/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Host test of the USB network arithmetic (CDC-ECM, RNDIS):
 *   cc -I. tests/host_usbnet.c drivers/usb/usbnet_proto.c && ./a.out
 * Configurations in the shape of a Linux Ethernet gadget, which is what the
 * roboRIO's USB port and QEMU's usb-net offer: one ECM, one RNDIS. */
#include <stdio.h>
#include <string.h>
#include "drivers/usb/usbnet_proto.h"

static int fails, passes;
#define CHECK(name, cond) do { if (cond) passes++; else { fails++; printf("FAIL: %s\n", name); } } while (0)

static const uint8_t ecm[] = {
	9, 2, 80, 0, 2, 1, 0, 0xC0, 1,
	9, 4, 0, 0, 1, 2, 6, 0, 0,
	5, 0x24, 0, 0x10, 0x01,
	5, 0x24, 6, 0, 1,
	13, 0x24, 0x0F, 3, 0, 0, 0, 0, 0xEA, 0x05, 0, 0, 0,
	7, 5, 0x81, 3, 16, 0, 9,
	9, 4, 1, 0, 0, 0x0A, 0, 0, 0,
	9, 4, 1, 1, 2, 0x0A, 0, 0, 0,
	7, 5, 0x82, 2, 64, 0, 0,
	7, 5, 0x03, 2, 64, 0, 0,
};
static const uint8_t rndis[] = {
	9, 2, 75, 0, 2, 2, 0, 0xC0, 1,
	9, 4, 0, 0, 1, 2, 2, 0xFF, 0,
	5, 0x24, 0, 0x10, 0x01,
	5, 0x24, 1, 0, 1,
	4, 0x24, 2, 0,
	5, 0x24, 6, 0, 1,
	7, 5, 0x81, 3, 8, 0, 9,
	9, 4, 1, 0, 2, 0x0A, 0, 0, 0,
	7, 5, 0x82, 2, 0x00, 0x02, 0,
	7, 5, 0x03, 2, 0x00, 0x02, 0,
};
static const uint8_t acm[] = {
	9, 2, 62, 0, 2, 1, 0, 0xC0, 50,
	9, 4, 0, 0, 1, 2, 2, 1, 0,
	5, 0x24, 6, 0, 1,
	7, 5, 0x82, 3, 8, 0, 0xFF,
	9, 4, 1, 0, 2, 0x0A, 0, 0, 0,
	7, 5, 0x04, 2, 64, 0, 1,
	7, 5, 0x83, 2, 64, 0, 1,
};

static uint8_t got[4][1600];
static uint32_t gotn[4];
static int ngot;
static void each(void *ctx, const uint8_t *f, uint32_t n)
{
	(void)ctx;
	if (ngot < 4 && n <= sizeof got[0]) { memcpy(got[ngot], f, n); gotn[ngot] = n; }
	ngot++;
}

static void t_match(void)
{
	un_desc_t d;
	CHECK("ecm", un_match(ecm, sizeof ecm, &d) == UN_ECM);
	CHECK("ecm ifaces", d.ctrl_iface == 0 && d.data_iface == 1 && d.data_alt == 1 && d.mac_index == 3);
	CHECK("ecm eps", d.ep_in == 0x82 && d.ep_out == 0x03 && d.mps_in == 64);
	CHECK("rndis", un_match(rndis, sizeof rndis, &d) == UN_RNDIS);
	CHECK("rndis eps", d.data_alt == 0 && d.ep_in == 0x82 && d.ep_out == 0x03 && d.mps_out == 512);
	CHECK("acm is not a network", un_match(acm, sizeof acm, &d) == UN_NONE);
	int bad = 0;
	for (uint32_t n = 0; n < sizeof ecm; n++) if (un_match(ecm, n, &d) != UN_NONE) bad++;
	CHECK("ecm truncated", bad == 0);
	uint8_t noalt[sizeof ecm];
	memcpy(noalt, ecm, sizeof ecm);
	noalt[57 + 4] = 0;            /* alternate 1 claims no endpoints: its bulk ones still follow */
	CHECK("ecm odd count still found", un_match(noalt, sizeof noalt, &d) == UN_ECM);
}

static void t_mac(void)
{
	const char *hex = "5254001234aB";
	uint8_t s[26], mac[6];
	s[0] = 26; s[1] = 3;
	for (int i = 0; i < 12; i++) { s[2 + 2 * i] = (uint8_t)hex[i]; s[3 + 2 * i] = 0; }
	CHECK("mac", un_mac_from_string(s, 26, mac) == 0 && mac[0] == 0x52 && mac[5] == 0xAB);
	s[5] = 1;
	CHECK("mac not ascii", un_mac_from_string(s, 26, mac) < 0);
	CHECK("mac short", un_mac_from_string(s, 20, mac) < 0);
}

static void t_rndis(void)
{
	uint8_t b[256];
	CHECK("init", un_rndis_init(b, 7, 4096) == 24 && b[0] == 2 && b[4] == 24 && b[8] == 7 && b[12] == 1 &&
	      b[20] == 0x00 && b[21] == 0x10);
	memset(b, 0xEE, sizeof b);
	CHECK("query", un_rndis_query(b, 8, OID_802_3_PERMANENT_ADDRESS, 6) == 34 && b[0] == 4 && b[4] == 34 &&
	      b[12] == 1 && b[15] == 1 && b[16] == 6 && b[20] == 20 && b[28] == 0 && b[33] == 0);
	uint8_t f[4] = { RNDIS_FILTER, 0, 0, 0 };
	CHECK("set", un_rndis_set(b, 9, OID_GEN_CURRENT_PACKET_FILTER, f, 4) == 32 && b[0] == 5 && b[4] == 32 &&
	      b[16] == 4 && b[28] == 0x0D);
	/* a query reply: type, length 30, id 8, status 0, info 6 bytes at offset 16 */
	uint8_t r[32] = { 0x04, 0, 0, 0x80, 30, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 6, 0, 0, 0, 16, 0, 0, 0,
	                  0x52, 0x54, 0, 0x12, 0x34, 0x77 };
	const uint8_t *info = 0;
	uint32_t n = 0;
	CHECK("reply", un_rndis_reply(r, 30, RNDIS_QUERY_MSG, 8, &info, &n) == 0 && n == 6 && info == r + 24 &&
	      info[5] == 0x77);
	CHECK("reply wrong id", un_rndis_reply(r, 30, RNDIS_QUERY_MSG, 9, &info, &n) < 0);
	r[20] = 200;
	CHECK("reply bad offset", un_rndis_reply(r, 30, RNDIS_QUERY_MSG, 8, &info, &n) < 0);
	r[20] = 16; r[12] = 1;
	CHECK("reply failed status", un_rndis_reply(r, 30, RNDIS_QUERY_MSG, 8, &info, &n) < 0);
	uint8_t ic[52] = { 0x02, 0, 0, 0x80, 52, 0, 0, 0, 7 };
	CHECK("init reply", un_rndis_reply(ic, 52, RNDIS_INIT_MSG, 7, &info, &n) == 0);

	/* two frames in one transfer, then junk */
	static uint8_t x[3000];
	memset(x, 0xEE, sizeof x);
	un_rndis_wrap(x, 60);
	for (int i = 0; i < 60; i++) x[RNDIS_HDR + i] = (uint8_t)i;
	un_rndis_wrap(x + RNDIS_HDR + 60, 1514);
	x[RNDIS_HDR + 60 + RNDIS_HDR] = 0xAA;
	uint32_t total = 2 * RNDIS_HDR + 60 + 1514;
	ngot = 0;
	CHECK("unwrap two", un_rndis_unwrap(x, total + 10, each, 0) == 2 && gotn[0] == 60 && got[0][59] == 59 &&
	      gotn[1] == 1514 && got[1][0] == 0xAA);
	x[4] = 0xFF; x[5] = 0xFF;        /* a length past the end: nothing */
	ngot = 0;
	CHECK("unwrap bad length", un_rndis_unwrap(x, total, each, 0) == 0 && ngot == 0);
	CHECK("zlp", un_needs_zlp(512, 512) && un_needs_zlp(1024, 64) && !un_needs_zlp(1558, 64) && !un_needs_zlp(0, 64));
}

int main(void)
{
	t_match();
	t_mac();
	t_rndis();
	printf("usbnet: %d passed, %d failed\n", passes, fails);
	if (!fails) printf("ALL PASS\n");
	return fails != 0;
}
