// SPDX-License-Identifier: MIT
/*
 * Ranix: /sys/class/drm/ranix_power, one line of what the laptop is drawing and
 * how hot it is, read only and for root only. Nothing here changes hardware state.
 *
 * RAPL package (0x611) and graphics plane PP1 (0x641) energy, in units from 0x606,
 * averaged since the previous read; package (0x1B1) and core (0x19C) temperature
 * against TjMax (0x1A2); the GT's frequency while the engine runs; on Casper, fan
 * and EC temperatures from the EC's shared window (read only).
 */
#include <kernel/mm/vm.h>
#include <kernel/pci.h>
#include "../drm_priv.h"
#include "intel_display.h"
#include "ranix_gt.h"

#ifdef __x86_64__
static uint64_t rdmsr_(uint32_t m)
{
	uint32_t lo, hi;
	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m));
	return (uint64_t)hi << 32 | lo;
}
#endif

static int putdec_(char *o, int n, uint64_t v)
{
	char t[24]; int k = 0;
	do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (k) o[n++] = t[--k];
	return n;
}

static int put(char *o, int n, const char *s) { while (*s) o[n++] = *s++; return n; }

int ranix_power_status(char *o)
{
	int n = 0;
	if (!intel_mmio()) return put(o, 0, "no Intel GPU here\n");   /* RAPL MSRs only on the real part */
#ifdef __x86_64__
	static uint64_t last_ns;
	static uint32_t last_pkg, last_gfx;
	uint64_t unit = (rdmsr_(0x606) >> 8) & 0x1f;              /* energy unit = 2^-unit J */
	uint32_t pkg = (uint32_t)rdmsr_(0x611), gfx = (uint32_t)rdmsr_(0x641);
	uint64_t now = drm_now_ns();
	uint32_t tjmax = (uint32_t)(rdmsr_(0x1A2) >> 16) & 0xff;
	uint32_t ptemp = tjmax - ((uint32_t)(rdmsr_(0x1B1) >> 16) & 0x7f);
	uint32_t ctemp = tjmax - ((uint32_t)(rdmsr_(0x19C) >> 16) & 0x7f);
	uint64_t mw_pkg = 0, mw_gfx = 0;
	if (last_ns && now > last_ns) {
		uint64_t dt_us = (now - last_ns) / 1000;
		/* uJ = delta * 1e6 / 2^unit; mW = uJ * 1000 / dt_us */
		mw_pkg = ((uint64_t)(pkg - last_pkg) * 1000000ULL >> unit) * 1000ULL / (dt_us ? dt_us : 1);
		mw_gfx = ((uint64_t)(gfx - last_gfx) * 1000000ULL >> unit) * 1000ULL / (dt_us ? dt_us : 1);
	}
	last_ns = now; last_pkg = pkg; last_gfx = gfx;
	n = put(o, n, "pkg_mW "); n = putdec_(o, n, mw_pkg);
	n = put(o, n, " gfx_mW "); n = putdec_(o, n, mw_gfx);
	n = put(o, n, " pkg_C "); n = putdec_(o, n, ptemp);
	n = put(o, n, " core_C "); n = putdec_(o, n, ctemp);
	n = put(o, n, " gt_MHz ");
	uint32_t act, req;
	if (gt_freq_mhz(&act, &req)) {
		n = putdec_(o, n, act);
		n = put(o, n, " req_MHz "); n = putdec_(o, n, req);
	} else n = put(o, n, "-");
	/* read only, for the power-off investigation: VR current limit PP0_CURRENT_CONFIG
	 * (0x601: limit in 1/8 A, bit 31 lock) and the turbo ratio limits (0x1AD) */
	{
		uint64_t cc = rdmsr_(0x601), tr = rdmsr_(0x1AD);
		n = put(o, n, " pp0_limit_A "); n = putdec_(o, n, (cc & 0x1fff) / 8);
		n = put(o, n, (cc >> 31) & 1 ? " locked" : " unlocked");
		n = put(o, n, " turbo_ratios "); n = putdec_(o, n, tr & 0xff); o[n++] = '/';
		n = putdec_(o, n, (tr >> 8) & 0xff); o[n++] = '/'; n = putdec_(o, n, (tr >> 16) & 0xff); o[n++] = '/';
		n = putdec_(o, n, (tr >> 24) & 0xff);
	}
	/* Casper's EC shares a window at 0xFEFF0380 (DSDT EC0.ECW1, SystemMemory): read
	 * only. F0TS fan tach (rpm = 0x1C8402 / (2 * ~F0TS), as EC0.TACH), PCPT the CPU
	 * temperature the EC acts on, ECRT/EPSV its critical/passive points, F0CS fan state. */
	static volatile uint8_t *ecw;
	/* only on the board whose DSDT this comes from (Casper: GPU subsystem 1b0a:22c4) */
	if (!ecw && pci_read32(drm_pci_bus, drm_pci_dev, drm_pci_fn, 0x2c) == 0x22c41b0aU)
		ecw = ioremap(0xFEFF0380, 0x80, VM_READ | VM_NOCACHE | VM_DEVICE);
	if (ecw) {
		uint16_t f0ts = (uint16_t)(ecw[0x68] | ecw[0x69] << 8);
		uint16_t t = (uint16_t)~f0ts;
		uint32_t rpm = (t == 0 || t == 0xFFFF) ? 0 : 0x1C8402u / (2u * t);
		n = put(o, n, " fan_rpm "); n = putdec_(o, n, rpm);
		n = put(o, n, " fan_state "); n = putdec_(o, n, ecw[0x6C]);
		n = put(o, n, " ec_cpu_C "); n = putdec_(o, n, (uint16_t)(ecw[0x6E] | ecw[0x6F] << 8));
		n = put(o, n, " ec_crt_C "); n = putdec_(o, n, ecw[0x64]);
		n = put(o, n, " ec_psv_C "); n = putdec_(o, n, ecw[0x65]);
		n = put(o, n, " ec_act "); n = putdec_(o, n, ecw[0x66]);
	}
	o[n++] = '\n';
#else
	n = put(o, n, "no power counters on this architecture\n");
#endif
	return n;
}

