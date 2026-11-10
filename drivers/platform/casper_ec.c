/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * Casper Nirvana embedded controller: tell it an OS is running, the way the
 * firmware's own AML does under Linux and Windows. Off unless asked for.
 *
 * A full OS runs \_SB.PCI0._INI and EC0._INI at boot. In this board's DSDT (OEM
 * "CASPER", table "ERA") they write three bytes of EC RAM:
 *
 *   PCI0._INI  0x0616 &= ~0x01   _WAK repeats it after S3/S4, next to 0x0617
 *   EC0._INI   0x04A0 |=  0x08
 *              0x0617  =  1      OSYS >= 0x07DC (Windows 8); Linux answers _OSI
 *                                "Windows 2015", so it writes 1 as well
 *
 * Ranix has no AML interpreter, so without this driver none of it happens and the
 * EC may keep a policy meant for "no OS". The keyboard backlight calls in
 * PCI0._INI (ECXT 0x86) are not made: they set a light, not a mode.
 *
 * EC RAM is reached through the mailbox the AML methods RRAM and WRAM use, a
 * window at 0xFEFF0700 (EC0.ECW2), byte by byte in their order:
 *   CDT2 = address low, CDT1 = address high, [CDT3 = value],
 *   CMD1 = 0x80 read / 0x81 write, ESEM = 0x0F rings the EC.
 * The EC clears CMD1 when it is done. AML checks CMD1, then sleeps 1 ms, at most
 * 127 times; CMD1 still set means failure. A read's byte is in EDA1. The fields
 * are declared "Lock", so every byte access holds the ACPI global lock (firmware
 * may use the window too), and the EC0.MUEC mutex keeps one command at a time.
 *
 * Two deliberate differences from the AML: a failed read aborts (EC0._INI would
 * go on and write 0xFF to 0x04A0), and the global lock wait is bounded.
 *
 * Doors: /sys/kernel/ranix_ec (0600, root): reading shows the state; writing
 * "on", "off" or "read" acts. "off" puts back the bits "on" changed, as they
 * were read before. ranix.ec_osflags=1 on the command line turns it on at boot.
 * Only the board with this DSDT matches (GPU subsystem 1b0a:22c4 and the DSDT's
 * OEM IDs); everywhere else the node reports "board none" and refuses.
 */
#include <uapi/types.h>
#include <kernel/pci.h>
#include <kernel/mm/vm.h>
#include <kernel/kprintf.h>
#include <kernel/kout.h>
#include <kernel/sync.h>
#include <kernel/time.h>
#include <kernel/proc.h>
#include <kernel/cmdline.h>
#include <kernel/errno.h>
#include <kernel/hal.h>
#include <kernel/init.h>
#include <kernel/lib/string.h>
#include "casper_ec.h"

#define EC_MAILBOX      0xFEFF0700ULL   /* EC0.ECW2, SystemMemory, 0x80 bytes */
#define EC_MAILBOX_LEN  0x80
#define MB_ESEM         0x00
#define MB_CMD1         0x01
#define MB_CDT1         0x02
#define MB_CDT2         0x03
#define MB_CDT3         0x04
#define MB_EDA1         0x40
#define CMD_READ        0x80
#define CMD_WRITE       0x81
#define ESEM_RING       0x0F
#define POLLS           0x7F            /* AML: Local0 = 0x7F, Sleep (One) each */
#define FW_LOCK_MS      1000            /* AML waits forever; a stuck lock fails here */

#define GPU_SUBSYS      0x22c41b0aU     /* 00:02.0 subsystem 1b0a:22c4 */
#define INTEL_VID       0x8086

/* The three bytes, in the order a booting OS writes them. A byte becomes
 * (old & ~mask) | val; "off" puts back (now & ~mask) | (old & mask). */
static const struct { uint16_t addr; uint8_t mask, val; const char *from; } item[] = {
	{ 0x0616, 0x01, 0x00, "PCI0._INI" },
	{ 0x04A0, 0x08, 0x08, "EC0._INI" },
	{ 0x0617, 0xFF, 0x01, "EC0._INI" },
};
#define NITEMS (int)(sizeof(item) / sizeof(item[0]))

static struct {
	int board;                         /* 1 when this is the board the DSDT is from */
	const char *why;                   /* why not, for the node */
	volatile uint8_t *mb;
	int on;
	int have_old, have_now;
	uint8_t old[NITEMS], now[NITEMS];
	uint8_t written[NITEMS];
	uint32_t commands, timeouts, lock_busy;
	int last_err;
} E = { .why = "not probed" };

static mutex_t ec_lock = MUTEX_INIT;    /* EC0.MUEC, and one on/off at a time */

/* AML Sleep (One) is at least a millisecond; one tick may be less */
static void sleep_1ms(void)
{
	task_sleep_until(kernel_ticks + 1 + TIMER_HZ / 1000);
}

static int mb_get(int off, uint8_t *v)
{
	if (arch_fw_lock(FW_LOCK_MS)) { E.lock_busy++; return -EBUSY; }
	*v = E.mb[off];
	arch_fw_unlock();
	return 0;
}

static int mb_put(int off, uint8_t v)
{
	if (arch_fw_lock(FW_LOCK_MS)) { E.lock_busy++; return -EBUSY; }
	E.mb[off] = v;
	arch_fw_unlock();
	return 0;
}

/* One RRAM or WRAM, with ec_lock held. */
static int ec_cmd(uint8_t cmd, uint16_t addr, uint8_t val, uint8_t *out)
{
	uint8_t busy;
	int r, n = POLLS;
	E.commands++;
	if ((r = mb_put(MB_CDT2, (uint8_t)(addr & 0xFF))) || (r = mb_put(MB_CDT1, (uint8_t)(addr >> 8))))
		return r;
	if (cmd == CMD_WRITE && (r = mb_put(MB_CDT3, val)))
		return r;
	if ((r = mb_put(MB_CMD1, cmd)) || (r = mb_put(MB_ESEM, ESEM_RING)))
		return r;
	for (;;) {                         /* While ((Local0 && CMD1)) { Sleep (One); Local0-- } */
		if ((r = mb_get(MB_CMD1, &busy))) return r;
		if (!n || !busy) break;
		sleep_1ms();
		n--;
	}
	if ((r = mb_get(MB_CMD1, &busy))) return r;   /* If ((CMD1 == Zero)) */
	if (busy) { E.timeouts++; return -ETIMEDOUT; }
	return out ? mb_get(MB_EDA1, out) : 0;
}

static int ec_read(uint16_t addr, uint8_t *v)  { return ec_cmd(CMD_READ, addr, 0, v); }
static int ec_write(uint16_t addr, uint8_t v)  { return ec_cmd(CMD_WRITE, addr, v, NULL); }

static int read_all(uint8_t *v)
{
	for (int i = 0; i < NITEMS; i++) {
		int r = ec_read(item[i].addr, &v[i]);
		if (r) return r;
	}
	return 0;
}

static void log_bytes(const char *what, const uint8_t *v)
{
	kprintf(KERN_INFO "casper_ec: %s 0x%x=0x%x 0x%x=0x%x 0x%x=0x%x\n", what,
	        item[0].addr, v[0], item[1].addr, v[1], item[2].addr, v[2]);
}

static int refresh(const char *what)
{
	int r = read_all(E.now);
	E.have_now = !r;
	if (r) kprintf(KERN_WARN "casper_ec: reading back failed (%d)\n", r);
	else log_bytes(what, E.now);
	return r;
}

static int flags_on(void)
{
	if (E.on) return 0;
	int r = read_all(E.old);
	if (r) { kprintf(KERN_WARN "casper_ec: reading the old values failed (%d), nothing written\n", r); return r; }
	E.have_old = 1;
	log_bytes("before", E.old);
	E.on = 1;                          /* from here "off" can undo whatever got written */
	for (int i = 0; i < NITEMS && !r; i++) {
		uint8_t v = (uint8_t)((E.old[i] & ~item[i].mask) | item[i].val);
		r = ec_write(item[i].addr, v);
		if (!r) E.written[i] = 1;
		kprintf(KERN_INFO "casper_ec: %s: 0x%x <- 0x%x (%d)\n", item[i].from, item[i].addr, v, r);
	}
	int rb = refresh("after");
	return r ? r : rb;
}

static int flags_off(void)
{
	if (!E.on) return 0;
	uint8_t cur;
	int r = 0;
	for (int i = NITEMS - 1; i >= 0; i--) {
		if (!E.written[i]) continue;
		int ri = ec_read(item[i].addr, &cur);
		if (!ri) {
			uint8_t v = (uint8_t)((cur & ~item[i].mask) | (E.old[i] & item[i].mask));
			ri = ec_write(item[i].addr, v);
			kprintf(KERN_INFO "casper_ec: restore 0x%x <- 0x%x (%d)\n", item[i].addr, v, ri);
		}
		if (ri) r = ri;
		else E.written[i] = 0;
	}
	if (!r) E.on = 0;
	int rb = refresh("restored");
	return r ? r : rb;
}

int casper_ec_command(const char *cmd, size_t len)
{
	while (len && (cmd[len - 1] == '\n' || cmd[len - 1] == ' ')) len--;
	int op = len == 2 && !memcmp(cmd, "on", 2) ? 1 : len == 3 && !memcmp(cmd, "off", 3) ? 0 :
	         len == 4 && !memcmp(cmd, "read", 4) ? 2 : -1;
	if (op < 0) return -EINVAL;
	if (!E.board) return -ENODEV;
	if (mutex_lock_killable(&ec_lock)) return -EINTR;
	int r = op == 1 ? flags_on() : op == 0 ? flags_off() : refresh("read");
	E.last_err = r;
	mutex_unlock(&ec_lock);
	return r;
}

static void put_bytes(kout_t *o, const char *key, int have, const uint8_t *v)
{
	kout_s(o, key);
	for (int i = 0; i < NITEMS; i++) {
		kout_c(o, ' ');
		if (have) kout_x(o, v[i]);
		else kout_c(o, '-');
	}
	kout_c(o, '\n');
}

int casper_ec_board(void) { return E.board; }

int casper_ec_status(char *buf, size_t cap)
{
	kout_t o;
	kout_init(&o, buf, cap);
	kout_s(&o, "board ");
	kout_s(&o, E.board ? "casper\n" : "none (");
	if (!E.board) { kout_s(&o, E.why); kout_s(&o, ")\n"); }
	kout_s(&o, "osflags ");
	kout_s(&o, E.on ? "on\n" : "off\n");
	kout_s(&o, "bytes");
	for (int i = 0; i < NITEMS; i++) { kout_c(&o, ' '); kout_x(&o, item[i].addr); }
	kout_c(&o, '\n');
	put_bytes(&o, "before", E.have_old, E.old);
	put_bytes(&o, "now", E.have_now, E.now);
	kout_s(&o, "commands "); kout_u(&o, E.commands);
	kout_s(&o, " timeouts "); kout_u(&o, E.timeouts);
	kout_s(&o, " lock_busy "); kout_u(&o, E.lock_busy);
	kout_s(&o, " last_error "); kout_i(&o, E.last_err);
	kout_c(&o, '\n');
	return (int)o.len;
}

static int board_matches(void)
{
	char oem[7], table[9];
	if ((pci_read32(0, 2, 0, PCI_VENDOR_ID) & 0xFFFF) != INTEL_VID ||
	    pci_read32(0, 2, 0, 0x2C) != GPU_SUBSYS) { E.why = "GPU subsystem"; return 0; }
	if (arch_fw_oem(oem, table) || strcmp(oem, "CASPER") || strcmp(table, "ERA     ")) {
		E.why = "DSDT OEM"; return 0;
	}
	return 1;
}

/* Kernel threads here never return, so this one parks after its single job. */
static void boot_on(void)
{
	casper_ec_command("on", 2);
	for (;;) task_sleep_until(kernel_ticks + 3600ULL * TIMER_HZ);
}

void casper_ec_init(void)
{
	if (!board_matches()) return;
	E.mb = ioremap(EC_MAILBOX, EC_MAILBOX_LEN, VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);
	if (!E.mb) { E.why = "ioremap"; return; }
	E.board = 1;
	E.why = "";
	char v[4];
	int boot = cmdline_get("ranix.ec_osflags", v, sizeof(v)) && !strcmp(v, "1");
	kprintf(KERN_INFO "casper_ec: board matched, OS flags %s\n", boot ? "on at boot" : "off (ranix.ec_osflags=1 or /sys/kernel/ranix_ec)");
	if (boot) proc_create("casper_ec", boot_on);   /* the EC answers in ms; boot need not wait */
}
