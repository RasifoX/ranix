/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Host test of the EC event query against a model EC:
 *   cc -I. tests/host_ec_hotkey.c drivers/input/ec_hotkey.c && ./a.out */
#include <stdio.h>
#include "drivers/input/ec_hotkey.h"

static uint8_t queue[16];
static int nq, obf, ibf_busy, mute_ec, data, cmds, data_reads;

static uint8_t in(uint16_t p)
{
	if (p == EC_SC)
		return (uint8_t)((nq ? EC_SCI_EVT : 0) | (obf ? EC_OBF : 0) | (ibf_busy ? EC_IBF : 0));
	data_reads++;
	obf = 0;
	return (uint8_t)data;
}

static void out(uint16_t p, uint8_t v)
{
	if (p != EC_SC || v != EC_QR) return;
	cmds++;
	if (mute_ec) return;              /* an EC that never answers */
	data = nq ? queue[0] : 0;
	for (int i = 1; i < nq; i++) queue[i - 1] = queue[i];
	if (nq) nq--;
	obf = 1;
}

static void wait_(void) {}
static const ec_io_t io = {in, out, wait_};
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
	CHECK(ec_query(&io) == EC_NONE && cmds == 0, "idle EC is not asked");
	queue[nq++] = 0x15; queue[nq++] = 0x14;
	CHECK(ec_query(&io) == 0x15, "first event");
	CHECK(ec_query(&io) == 0x14, "second event");
	CHECK(ec_query(&io) == EC_NONE && cmds == 2, "queue empty: two queries in all");
	queue[nq++] = 0x13; ibf_busy = 1;
	CHECK(ec_query(&io) == EC_BUSY && cmds == 2, "busy EC is left alone");
	ibf_busy = 0; obf = 1;
	int reads = data_reads;
	CHECK(ec_query(&io) == EC_BUSY && cmds == 2 && data_reads == reads, "someone else's byte is not taken");
	obf = 0; mute_ec = 1;
	CHECK(ec_query(&io) == EC_TIMEOUT, "a silent EC times out");
	mute_ec = 0;
	CHECK(ec_query(&io) == 0x13, "and the event is still there");
	CHECK(ec_hotkey_casper(0x14) == ECA_VOL_DOWN && ec_hotkey_casper(0x15) == ECA_VOL_UP &&
	      ec_hotkey_casper(0x13) == ECA_MUTE, "Casper: Fn+F11/F12 and mute");
	CHECK(ec_hotkey_casper(0x0E) == ECA_BRIGHT_DOWN && ec_hotkey_casper(0x0F) == ECA_BRIGHT_UP, "Casper: brightness");
	CHECK(ec_hotkey_casper(0x0A) == ECA_NONE && ec_hotkey_casper(0x50) == ECA_NONE, "Casper: others ignored");
	printf(fails ? "FAIL %d\n" : "OK\n", fails);
	return fails != 0;
}
