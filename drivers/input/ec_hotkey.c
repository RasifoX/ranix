/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * The ACPI embedded-controller event query (ACPI 6.4, 12.3 and 12.6) and what
 * one board's firmware makes of each event. Pure logic over three port
 * functions, so tests/host_ec_hotkey.c runs it against a model EC.
 *
 * The EC sets SCI_EVT in its status when an event waits. The OS writes QR_EC to
 * the command port; the EC answers with the event number on the data port. A
 * firmware AML method _Qxx then runs for event xx. Ranix runs no AML, so the
 * table below says, for a board, what that method would have done.
 */
#include "ec_hotkey.h"

#define WAITS 1000                 /* x ~10 us: the spec allows the EC 1 ms, give it 10 */

int ec_query(const ec_io_t *io)
{
	uint8_t st = io->in(EC_SC);
	if (!(st & EC_SCI_EVT)) return EC_NONE;
	if (st & (EC_IBF | EC_OBF)) return EC_BUSY;   /* firmware is talking to it: next time */
	io->out(EC_SC, EC_QR);
	for (int i = 0; i < WAITS; i++) {
		if (io->in(EC_SC) & EC_OBF) {
			int q = io->in(EC_DATA);
			return q ? q : EC_NONE;          /* 0: nothing was queued after all */
		}
		io->wait();
	}
	return EC_TIMEOUT;
}

/*
 * Casper Nirvana (DSDT OEM "CASPER", table "ERA"), EC0 scope, the branch taken
 * for Windows 8 and later (OSYS >= 0x07DC), which is what Linux and Windows get:
 *   _Q0E  Notify (GFX0.DD1F, 0x87)    brightness down
 *   _Q0F  Notify (GFX0.DD1F, 0x86)    brightness up
 *   _Q13  C1MD: set 2 E0 23           mute
 *   _Q14  C1MD: set 2 E0 21           volume down       (Fn+F11)
 *   _Q15  C1MD: set 2 E0 32           volume up         (Fn+F12)
 * C1MD has the EC type those bytes into the keyboard controller. Here the same
 * keys go into the keyboard path directly. Not taken: _Q0A (sleep), _Q0B
 * (radio), _Q10 (keyboard light), _Q11/_QD7 (Win+P), the rest (power, heat).
 */
int ec_hotkey_casper(int q)
{
	switch (q) {
	case 0x0E: return ECA_BRIGHT_DOWN;
	case 0x0F: return ECA_BRIGHT_UP;
	case 0x13: return ECA_MUTE;
	case 0x14: return ECA_VOL_DOWN;
	case 0x15: return ECA_VOL_UP;
	default:   return ECA_NONE;
	}
}
