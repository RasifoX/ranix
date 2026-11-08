// SPDX-License-Identifier: MIT
/*
 * Ranix: the Gen9 render engine (RCS) for programs, over the DRM core.
 *
 * Copyright © 2014-2022 Intel Corporation (the parts marked "from intel_lrc.c" and
 * "from intel_uncore.c", Linux 6.1.176 drivers/gpu/drm/i915, MIT)
 * Copyright © 2026 Ranix (the rest)
 *
 * See NOTICE in this directory for the MIT permission text and what was changed.
 *
 * What this file does, in plain words: it runs the GPU's render engine for
 * programs. A request (a batch buffer to run) is written into its context's
 * ring, queued, and handed to the engine through the execlist port; when the
 * engine writes the request's number back into a status page, the request is
 * done. Nothing starts at boot: the first program that submits work wakes the
 * GT (forcewake) and the engine.
 *
 * If a request makes no progress for 2 s the engine is reset, every waiting
 * request is ended, and the GT stays off until reboot; the display never
 * depends on it. RPS (GPU frequency) lives here too but is off until "rps on".
 */
#include <kernel/mm/pmm.h>
#include <kernel/mm/vm.h>
#include <kernel/pci.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/proc.h>
#include <kernel/sync.h>
#include <linux/types.h>
#include <linux/bits.h>
#include "i915_reg_defs.h"
#include "gt/intel_gt_regs.h"
#include "gt/intel_engine_regs.h"
#include "gt/intel_gpu_commands.h"
#include "gt/intel_lrc_reg.h"
#include "../drm_priv.h"
#include "intel_display.h"
#include "ranix_gt.h"


#define _MASKED_FIELD(mask, value) (((mask) << 16) | (value))
#define _MASKED_BIT_ENABLE(a)  _MASKED_FIELD((u32)(a), (u32)(a))
#define _MASKED_BIT_DISABLE(a) _MASKED_FIELD((u32)(a), 0u)

#define RCS_BASE            RENDER_RING_BASE_
#define RENDER_RING_BASE_   0x02000
#define GEN9_RCS_CONTEXT_SZ (22 * PAGE_SIZE)
#define LRC_STATE_OFFSET    PAGE_SIZE
#define INTEL_LEGACY_32B_CONTEXT 1
#define GEN8_CTX_VALID      (1 << 0)
#define GEN8_CTX_PRIVILEGE  (1 << 8)
#define GEN8_CTX_ADDRESSING_MODE_SHIFT 3
#define HWS_SEQNO           0x40          /* dword index we write the seqno to */
#define HWS_CSB0            0x10
#define HWS_CSB_WRITE       0x1f

static volatile u8 *mmio;
static int gt_state;          /* 0 untried, 1 works, -1 turned off after a failure */
static char last[512];

static u32 rd(i915_reg_t r) { return *(volatile u32 *)(mmio + i915_mmio_reg_offset(r)); }
static void wr(i915_reg_t r, u32 v) { *(volatile u32 *)(mmio + i915_mmio_reg_offset(r)) = v; }

static int wait_for(i915_reg_t r, u32 mask, u32 val, int ms)
{
	uint64_t end = kernel_ticks + (uint64_t)ms;
	while ((rd(r) & mask) != val) {
		if (kernel_ticks > end) return -ETIME_;
		drm_sleep_ms(1);
	}
	return 0;
}

/* ── forcewake (protocol from intel_uncore.c: ack clear, set, ack set; the
 *    FORCEWAKE_KERNEL_FALLBACK toggle is WaRsForcewakeAddDelayForAck) ── */

static const struct { i915_reg_t req, ack; const char *name; } fw_domains[] = {
	{ FORCEWAKE_RENDER_GEN9, FORCEWAKE_ACK_RENDER_GEN9, "render" },
	{ FORCEWAKE_GT_GEN9,     FORCEWAKE_ACK_GT_GEN9,     "gt" },
	{ FORCEWAKE_MEDIA_GEN9,  FORCEWAKE_ACK_MEDIA_GEN9,  "media" },
};
#define NFW 3

static int fw_get(void)
{
	for (int i = 0; i < NFW; i++) wait_for(fw_domains[i].ack, FORCEWAKE_KERNEL, 0, 50);
	for (int i = 0; i < NFW; i++) wr(fw_domains[i].req, _MASKED_BIT_ENABLE(FORCEWAKE_KERNEL));
	for (int i = 0; i < NFW; i++) {
		if (!wait_for(fw_domains[i].ack, FORCEWAKE_KERNEL, FORCEWAKE_KERNEL, 50)) continue;
		int ok = 0;
		for (int pass = 1; pass <= 10 && !ok; pass++) {      /* the fallback kick */
			wait_for(fw_domains[i].ack, FORCEWAKE_KERNEL_FALLBACK, 0, 10);
			wr(fw_domains[i].req, _MASKED_BIT_ENABLE(FORCEWAKE_KERNEL_FALLBACK));
			drm_sleep_ms(1);
			wait_for(fw_domains[i].ack, FORCEWAKE_KERNEL_FALLBACK, FORCEWAKE_KERNEL_FALLBACK, 10);
			ok = (rd(fw_domains[i].ack) & FORCEWAKE_KERNEL) != 0;
			wr(fw_domains[i].req, _MASKED_BIT_DISABLE(FORCEWAKE_KERNEL_FALLBACK));
		}
		if (!ok) {
			kprintf("i915: forcewake %s: no ack (0x%x)\n", fw_domains[i].name, rd(fw_domains[i].ack));
			return -ETIME_;
		}
	}
	return 0;
}

static void fw_put(void)
{
	for (int i = 0; i < NFW; i++) wr(fw_domains[i].req, _MASKED_BIT_DISABLE(FORCEWAKE_KERNEL));
}

/* ── logical ring context register state (from intel_lrc.c) ── */

#define NOP(x) (BIT(7) | (x))
#define LRI(count, flags) ((flags) << 6 | (count))
#define POSTED BIT(0)
#define REG(x) (((x) >> 2))
#define REG16(x) (((x) >> 9) | BIT(7)), (((x) >> 2) & 0x7f)
#define END 0

static void set_offsets(u32 *regs, const u8 *data, u32 base)
{
	while (*data) {
		u8 count, flags;
		if (*data & BIT(7)) {           /* skip */
			count = *data++ & ~BIT(7);
			regs += count;
			continue;
		}
		count = *data & 0x3f;
		flags = *data >> 6;
		data++;
		*regs = MI_LOAD_REGISTER_IMM(count);
		if (flags & POSTED) *regs |= MI_LRI_FORCE_POSTED;
		regs++;
		do {
			u32 offset = 0;
			u8 v;
			do {
				v = *data++;
				offset <<= 7;
				offset |= v & ~BIT(7);
			} while (v & BIT(7));
			regs[0] = base + (offset << 2);
			regs += 2;
		} while (--count);
	}
}

static const u8 gen9_rcs_offsets[] = {
	NOP(1),
	LRI(14, POSTED),
	REG16(0x244), REG(0x34), REG(0x30), REG(0x38), REG(0x3c), REG(0x168), REG(0x140),
	REG(0x110), REG(0x11c), REG(0x114), REG(0x118), REG(0x1c0), REG(0x1c4), REG(0x1c8),
	NOP(3),
	LRI(9, POSTED),
	REG16(0x3a8), REG16(0x28c), REG16(0x288), REG16(0x284), REG16(0x280), REG16(0x27c),
	REG16(0x278), REG16(0x274), REG16(0x270),
	NOP(13),
	LRI(1, 0),
	REG(0xc8),
	NOP(13),
	LRI(44, POSTED),
	REG(0x28), REG(0x9c), REG(0xc0), REG(0x178), REG(0x17c), REG16(0x358), REG(0x170),
	REG(0x150), REG(0x154), REG(0x158), REG16(0x41c), REG16(0x600), REG16(0x604),
	REG16(0x608), REG16(0x60c), REG16(0x610), REG16(0x614), REG16(0x618), REG16(0x61c),
	REG16(0x620), REG16(0x624), REG16(0x628), REG16(0x62c), REG16(0x630), REG16(0x634),
	REG16(0x638), REG16(0x63c), REG16(0x640), REG16(0x644), REG16(0x648), REG16(0x64c),
	REG16(0x650), REG16(0x654), REG16(0x658), REG16(0x65c), REG16(0x660), REG16(0x664),
	REG16(0x668), REG16(0x66c), REG16(0x670), REG16(0x674), REG16(0x678), REG16(0x67c),
	REG(0x68),
	END
};

#define LRC_RING_MI_MODE_GEN9 0x54        /* lrc_ring_mi_mode() for Gen9 */
#define LRC_RING_BB_OFFSET_GEN9 0x64      /* lrc_ring_bb_offset() for Gen9 */

/* __lrc_init_regs() for a GGTT-only (32-bit legacy, no PPGTT) context */
static void lrc_init_regs(u32 *regs, int inhibit)
{
	if (inhibit) memset(regs, 0, PAGE_SIZE);
	set_offsets(regs, gen9_rcs_offsets, RCS_BASE);
	u32 ctl = _MASKED_BIT_ENABLE(CTX_CTRL_INHIBIT_SYN_CTX_SWITCH);
	ctl |= _MASKED_BIT_DISABLE(CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT);
	if (inhibit) ctl |= CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT;
	ctl |= _MASKED_BIT_DISABLE(CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT | CTX_CTRL_RS_CTX_ENABLE);
	regs[CTX_CONTEXT_CONTROL] = ctl;
	regs[CTX_TIMESTAMP] = 0;
	regs[LRC_RING_BB_OFFSET_GEN9 + 1] = 0;
	/* PDPs stay 0: nothing here is PPGTT-addressed */
	regs[LRC_RING_MI_MODE_GEN9 + 1] &= ~STOP_RING;          /* __reset_stop_ring */
	regs[LRC_RING_MI_MODE_GEN9 + 1] |= STOP_RING << 16;
}

/* ── objects ── */

typedef struct { drm_gem_t *o; u32 ggtt; } kobj_t;

static int kobj_new(kobj_t *k, uint64_t size)
{
	k->o = drm_gem_alloc(size);
	if (!k->o) return -ENOMEM_;
	int e = intel_ggtt_bind(k->o);
	if (e) { drm_gem_put(k->o); k->o = NULL; return e; }
	k->ggtt = (u32)k->o->ggtt;
	return 0;
}

static void kobj_free(kobj_t *k) { if (k->o) drm_gem_put(k->o); k->o = NULL; }

static u32 *kv(kobj_t *k, uint64_t off) { return drm_gem_vaddr(k->o, off, NULL); }

/* read a dword the GPU wrote: drop our cached copy first */
static u32 kread(kobj_t *k, uint64_t off)
{
	drm_gem_flush(k->o, off & ~63ULL, 64);
	return *(volatile u32 *)kv(k, off);
}

static void engine_reset(void)
{
	wr(RING_RESET_CTL(RCS_BASE), _MASKED_BIT_ENABLE(RESET_CTL_REQUEST_RESET));
	int ready = !wait_for(RING_RESET_CTL(RCS_BASE), RESET_CTL_READY_TO_RESET, RESET_CTL_READY_TO_RESET, 20);
	wr(GEN6_GDRST, GEN6_GRDOM_RENDER);
	int done = !wait_for(GEN6_GDRST, GEN6_GRDOM_RENDER, 0, 50);
	wr(RING_RESET_CTL(RCS_BASE), _MASKED_BIT_DISABLE(RESET_CTL_REQUEST_RESET));
	kprintf("i915: render engine reset (ready %d, done %d)\n", ready, done);
}

static int put(char *o, int n, const char *s) { while (*s && n < 1300) o[n++] = *s++; return n; }
static int putdec_(char *o, int n, uint64_t v)
{
	char t[24]; int k = 0;
	do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (k) o[n++] = t[--k];
	return n;
}
static int puthex(char *o, int n, u32 v)
{
	const char *h = "0123456789abcdef";
	if (n > 1300) return n;
	for (int k = 28; k >= 0; k -= 4) o[n++] = h[(v >> k) & 15];
	o[n++] = ' ';
	return n;
}

/* ── the engine for clients: requests on RCS through execlists ──────────────
 *
 * One request runs at a time. A request is appended to its context's ring
 * (invalidate, MI_BATCH_BUFFER_START, flush + seqno write: gen8_engine_cs.c),
 * queued, and written to ELSP when the engine is idle; the 1 ms tick watches the
 * seqno in the HWSP and starts the next one. A request older than 2 s wedges the
 * GT: task context resets the engine, completes everything with an error and the
 * GT stays off for the boot. The forcewake taken at start is held while started.
 */

#define RING_SIZE      (16 * PAGE_SIZE)
#define QMAX           64
#define HANG_TICKS     2000
#define LRC_PPHWSP_SCRATCH_ADDR (0x34 * 4)
#define INTEL_LEGACY_64B_CONTEXT 3

struct gt_ctx {
	kobj_t   img, ring;
	u32      tail;           /* where the next request goes */
	u64      pml4;           /* 0: GGTT only */
	int      fresh;          /* no save yet: restore inhibited */
	int      queued;         /* requests not yet complete */
	u32      id;
};

static struct {
	int      started;
	int      wedged;
	kobj_t   hws;
	u32      seqno;          /* last issued */
	struct { gt_ctx_t *c; u32 tail, seqno; } q[QMAX];
	int      qh, qn;         /* queue head, count (q[qh] runs when inflight) */
	int      inflight;       /* requests from q[qh] in the running submission (same context) */
	uint64_t since;          /* tick the inflight one went to ELSP */
	u32      done_forced;    /* after a wedge: everything up to here counts as done */
	u32      resets;
	u32      ctx_ids;
	/* The engine idles with the last context still loaded and saves it only when
	 * it switches to another one (seen on Casper: a new image at that context's
	 * GGTT slot got overwritten by the save). So a context is freed only after the
	 * engine has moved to the kernel context, as i915 parks on its kernel context. */
	gt_ctx_t *current;
	gt_ctx_t *kctx;
	/* context-status buffer (6 entries in the HWSP): the engine is idle only once it
	 * reports ACTIVE_TO_IDLE; an ELSP written while it is still switching out can be
	 * lost (seen on Casper after ~30 back-to-back requests) */
	u32       csb_head;
	int       hw_idle;
} E;

#define CSB_ENTRIES          6               /* GEN8_CSB_ENTRIES */
#define CTX_STATUS_IDLE_ACTIVE (1 << 0)
#define CTX_STATUS_ACTIVE_IDLE (1 << 3)

/* reset_csb_pointers() (intel_execlists_submission.c) */
static void reset_csb(void)
{
	u32 rv = CSB_ENTRIES - 1;
	wr(RING_CONTEXT_STATUS_PTR(RCS_BASE), 0xffffu << 16 | rv << 8 | rv);
	(void)rd(RING_CONTEXT_STATUS_PTR(RCS_BASE));
	E.csb_head = rv;
	u32 *h = kv(&E.hws, 0);
	h[HWS_CSB_WRITE] = rv;
	for (int i = 0; i < 2 * CSB_ENTRIES; i++) h[HWS_CSB0 + i] = ~0u;
	drm_gem_flush(E.hws.o, 0, PAGE_SIZE);
	wr(RING_CONTEXT_STATUS_PTR(RCS_BASE), 0xffffu << 16 | rv << 8 | rv);
	(void)rd(RING_CONTEXT_STATUS_PTR(RCS_BASE));
	E.hw_idle = 1;
}

/* under gt_lock: consume new context-status events (process_csb, reduced) */
static void process_csb_locked(void)
{
	volatile u32 *h = kv(&E.hws, 0);
	drm_gem_flush(E.hws.o, HWS_CSB0 * 4, (HWS_CSB_WRITE + 1 - HWS_CSB0) * 4);
	u32 wp = h[HWS_CSB_WRITE] & 7;
	if (wp >= CSB_ENTRIES) return;
	int moved = 0;
	while (E.csb_head != wp) {
		E.csb_head = (E.csb_head + 1) % CSB_ENTRIES;
		u32 st = h[HWS_CSB0 + 2 * E.csb_head];
		if (st & CTX_STATUS_IDLE_ACTIVE) E.hw_idle = 0;
		if (st & CTX_STATUS_ACTIVE_IDLE) E.hw_idle = 1;
		moved = 1;
	}
	if (moved) wr(RING_CONTEXT_STATUS_PTR(RCS_BASE), _MASKED_FIELD(7u << 8, E.csb_head << 8));
}
static spinlock_t gt_lock = SPINLOCK_INIT;
static mutex_t gt_mutex = MUTEX_INIT;



static inline int seq_passed(u32 a, u32 b) { return (s32)(a - b) >= 0; }   /* a at or after b */

static u32 hws_seqno(void)
{
	if (!E.hws.o) return 0;
	return kread(&E.hws, HWS_SEQNO * 4);        /* flush our copy: the CS write may not snoop */
}

u32 gt_seqno_done(void)
{
	u32 s = hws_seqno();
	if (E.wedged && seq_passed(E.done_forced, s)) s = E.done_forced;
	return s;
}

int gt_usable(void) { return E.started && !E.wedged; }
u32 gt_reset_count(void) { return E.resets; }

/* bdw_setup_private_ppat() (intel_gtt.c): index 0 WB LLC (also what every GGTT
 * access uses), 1 WC, 2 WB eLLC override (Gen9 scanout), 3 UC, 4-7 WB LLC/eLLC by age */
static void setup_private_ppat(void)
{
#define PPAT(i, x) ((u64)(x) << ((i) * 8))
	u64 pat = PPAT(0, 3 | (1 << 2)) | PPAT(1, 1 | (2 << 2)) | PPAT(2, 3 | (0 << 2)) | PPAT(3, 0) |
	          PPAT(4, 3 | (2 << 2) | (0 << 4)) | PPAT(5, 3 | (2 << 2) | (1 << 4)) |
	          PPAT(6, 3 | (2 << 2) | (2 << 4)) | PPAT(7, 3 | (2 << 2) | (3 << 4));
#undef PPAT
	wr(GEN8_PRIVATE_PAT_LO, (u32)pat);
	wr(GEN8_PRIVATE_PAT_HI, (u32)(pat >> 32));
}

/* ── RPS: the GT frequency ──
 * The firmware leaves the GT at its lowest bin (100 MHz on Casper) until a driver
 * asks for more. As i915 does when it has engine busy stats (rps_timer, rps_work
 * in intel_rps.c), sample how busy the render engine was and step the software
 * request (GEN6_RPNSWREQ) up above 95 % busy and down below 85 %, doubling the step
 * while it keeps going one way; a client waiting on the GPU boosts it to RP1 at
 * once (i915's waitboost). Limits from GEN6_RP_STATE_CAP; units of 50/3 MHz.
 * Package power and heat stay under the thermal guard's RAPL limit, which the
 * PCU applies to the GT as well. */
#define GEN6_RPNSWREQ_       _MMIO(0xa008)
#define GEN6_RPSTAT1_        _MMIO(0xa01c)
#define GEN6_RC_VIDEO_FREQ_  _MMIO(0xa00c)
#define GEN6_RP_CONTROL_     _MMIO(0xa024)
#define GEN6_RP_UP_THRESHOLD_   _MMIO(0xa02c)
#define GEN6_RP_DOWN_THRESHOLD_ _MMIO(0xa030)
#define GEN6_RP_UP_EI_       _MMIO(0xa068)
#define GEN6_RP_DOWN_EI_     _MMIO(0xa06c)
#define GEN6_RP_IDLE_HYSTERSIS_ _MMIO(0xa070)
#define GEN6_RP_STATE_CAP_   _MMIO(0x140000 + 0x5998)
#define GEN9_FREQUENCY_(x)   ((u32)(x) << 23)
#define RPS_EI_TICKS         10            /* evaluation interval, ms */

static struct {
	int on, off_by_user;
	u32 rp0, rp1, rpn;                     /* hw units (50/3 MHz) */
	u32 max_soft, min_soft;
	u32 cur;
	int last_adj;
	u32 busy, ticks, idle_ticks;
	u32 ups, downs, boosts;
} R;

static u32 rps_mhz(u32 v) { return v * 50 / 3; }

static void rps_write(u32 v)
{
	if (v > R.max_soft) v = R.max_soft;
	if (v < R.min_soft) v = R.min_soft;
	if (v == R.cur) return;
	wr(GEN6_RPNSWREQ_, GEN9_FREQUENCY_(v));
	R.cur = v;
}

/* gen6_rps_get_freq_caps + gen9_rps_enable + rps_set_power(HIGH_POWER), Gen9 BC;
 * the thresholds are in 16 GT clocks (12 MHz crystal on KBL): unused here (no PM
 * interrupts), but RP_CONTROL must enable RPS for the PCU to take requests */
static void rps_init(void)
{
	u32 cap = rd(GEN6_RP_STATE_CAP_);
	R.rp0 = (cap & 0xff) * 3;
	R.rp1 = ((cap >> 8) & 0xff) * 3;
	R.rpn = ((cap >> 16) & 0xff) * 3;
	if (!R.rp0 || R.rp0 > 0x1ff || R.rpn > R.rp1 || R.rp1 > R.rp0 || R.rp0 == R.rpn) {
		kprintf("i915: RPS off: unexpected RP_STATE_CAP %x\n", cap);
		return;
	}
	R.max_soft = R.rp0; R.min_soft = R.rpn;
	R.on = 0;
	/* Off by default: Casper powered off ~30 s into glmark2 with this on
	 * (2026-11-08 20:13), cause not known yet. "rps on" programs it. */
	kprintf("i915: RPS available: %u-%u MHz (RP1 %u), off until \"rps on\"\n",
	        rps_mhz(R.rpn), rps_mhz(R.rp0), rps_mhz(R.rp1));
}

static void rps_enable_hw(void)
{
	const u32 clk = 12000000, up_ei = 10000, down_ei = 32000;    /* in 1.28 us, HIGH_POWER */
#define PM_IV(ns) ((u32)(((uint64_t)(ns) * clk / 1000000000ULL + 15) / 16))
	wr(GEN6_RC_VIDEO_FREQ_, GEN9_FREQUENCY_(R.rp1));
	wr(GEN6_RP_IDLE_HYSTERSIS_, 0xa);
	wr(GEN6_RP_UP_EI_, PM_IV((uint64_t)up_ei * 1000));
	wr(GEN6_RP_UP_THRESHOLD_, PM_IV((uint64_t)up_ei * 95 * 10));
	wr(GEN6_RP_DOWN_EI_, PM_IV((uint64_t)down_ei * 1000));
	wr(GEN6_RP_DOWN_THRESHOLD_, PM_IV((uint64_t)down_ei * 85 * 10));
#undef PM_IV
	wr(GEN6_RP_CONTROL_, (1u << 11) | (2u << 9) | (1u << 8) | (1u << 7) | (2u << 3) | 2u);
	R.cur = 0;
	R.max_soft = R.rp0;
	rps_write(R.rpn);
	R.on = 1;
	kprintf("i915: RPS on: %u-%u MHz (RP1 %u), software-sampled every %u ms\n",
	        rps_mhz(R.rpn), rps_mhz(R.rp0), rps_mhz(R.rp1), RPS_EI_TICKS);
}

/* every tick, under gt_lock */
static void rps_sample_locked(void)
{
	if (!R.on) return;
	int busy = E.inflight || E.qn;
	R.busy += busy;
	R.idle_ticks = busy ? 0 : R.idle_ticks + 1;
	if (++R.ticks < RPS_EI_TICKS) return;
	u32 b = R.busy, t = R.ticks;
	R.busy = 0; R.ticks = 0;
	if (100 * b > 95 * t && R.cur < R.max_soft) {
		int adj = R.last_adj > 0 ? 2 * R.last_adj : 3;
		R.last_adj = adj;
		rps_write(R.cur + (u32)adj);
		R.ups++;
	} else if (100 * b < 85 * t && R.cur > R.min_soft) {
		int adj = R.last_adj < 0 ? 2 * R.last_adj : -3;
		if (R.idle_ticks >= 100) adj = -(int)(R.cur - R.min_soft);   /* idle 100 ms: lowest at once */
		R.last_adj = adj;
		rps_write(R.cur > (u32)-adj + R.min_soft ? R.cur - (u32)-adj : R.min_soft);
		R.downs++;
	} else R.last_adj = 0;
}

/* a client waits for the GPU: no slower than RP1 */
static void rps_boost(void)
{
	if (!R.on || R.cur >= R.rp1) return;
	uint64_t f = spin_lock_irq(&gt_lock);
	if (R.cur < R.rp1) { rps_write(R.rp1); R.last_adj = 0; R.boosts++; }
	spin_unlock_irq(&gt_lock, f);
}

int gt_start(void)
{
	mutex_lock(&gt_mutex);
	int e = 0;
	if (E.started) { e = E.wedged ? -EIO_ : 0; goto out; }
	mmio = intel_mmio();
	if (!mmio || gt_state < 0) { e = -ENODEV_; goto out; }
	if (fw_get()) { gt_state = -1; e = -EIO_; goto out; }
	setup_private_ppat();
	if ((e = kobj_new(&E.hws, PAGE_SIZE))) { fw_put(); goto out; }
	memset(kv(&E.hws, 0), 0, PAGE_SIZE);
	drm_gem_flush(E.hws.o, 0, PAGE_SIZE);
	wr(RING_HWSTAM(RCS_BASE), ~0u);
	wr(RING_MODE_GEN7(RCS_BASE), _MASKED_BIT_ENABLE(GFX_RUN_LIST_ENABLE));
	wr(RING_MI_MODE(RCS_BASE), _MASKED_BIT_DISABLE(STOP_RING));
	wr(RING_HWS_PGA(RCS_BASE), E.hws.ggtt);
	(void)rd(RING_HWS_PGA(RCS_BASE));
	reset_csb();
	rps_init();
	E.started = 1;
	gt_state = 1;
	mutex_unlock(&gt_mutex);
	E.kctx = gt_ctx_create(0);
	if (!E.kctx) kprintf("i915: no kernel context: contexts will not be freed safely\n");
	mutex_lock(&gt_mutex);
	kprintf("i915: render engine started (execlists, HWSP %x)\n", E.hws.ggtt);
out:
	mutex_unlock(&gt_mutex);
	return e;
}

gt_ctx_t *gt_ctx_create(u64 pml4)
{
	gt_ctx_t *c = kmalloc(sizeof(*c));
	if (!c) return NULL;
	memset(c, 0, sizeof(*c));
	if (kobj_new(&c->img, GEN9_RCS_CONTEXT_SZ + PAGE_SIZE) || kobj_new(&c->ring, RING_SIZE)) {
		kobj_free(&c->img); kobj_free(&c->ring); kfree(c);
		return NULL;
	}
	u32 *regs = kv(&c->img, LRC_STATE_OFFSET);
	lrc_init_regs(regs, 1);
	if (pml4) {                                   /* ASSIGN_CTX_PML4 */
		regs[CTX_PDP0_UDW] = (u32)(pml4 >> 32);
		regs[CTX_PDP0_LDW] = (u32)pml4;
	}
	regs[CTX_RING_HEAD] = 0;
	regs[CTX_RING_TAIL] = 0;
	regs[CTX_RING_START] = c->ring.ggtt;
	regs[CTX_RING_CTL] = RING_CTL_SIZE(RING_SIZE) | RING_VALID;
	drm_gem_flush(c->img.o, 0, c->img.o->size);
	c->pml4 = pml4;
	c->fresh = 1;
	c->id = ++E.ctx_ids;
	return c;
}

int gt_wait_seqno(u32 seqno, int64_t timeout_ns);

int gt_submit(gt_ctx_t *c, u64 addr, u32 *seqno_out);

void gt_ctx_destroy(gt_ctx_t *c)
{
	if (!c) return;
	/* its requests must be off the engine before the image and ring go */
	for (int i = 0; i < 3000 && c->queued; i++) drm_sleep_ms(1);
	if (E.current == c && E.kctx && !E.wedged) {     /* move the engine off it: its image gets saved now */
		u32 seq;
		if (!gt_submit(E.kctx, ~0ULL, &seq)) gt_wait_seqno(seq, 2000000000LL);
	}
	if (c->queued) kprintf("i915: context %u destroyed with %d requests queued\n", c->id, c->queued);
	kobj_free(&c->ring);
	kobj_free(&c->img);
	kfree(c);
}

/* under gt_lock: put q[qh] on the engine */
static void elsp_submit_locked(void)
{
	gt_ctx_t *c = E.q[E.qh].c;
	u32 *regs = kv(&c->img, LRC_STATE_OFFSET);
	if (!c->fresh)       /* saved once: restore from the image from now on */
		regs[CTX_CONTEXT_CONTROL] = _MASKED_BIT_ENABLE(CTX_CTRL_INHIBIT_SYN_CTX_SWITCH) |
		                            _MASKED_BIT_DISABLE(CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT |
		                                                CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT | CTX_CTRL_RS_CTX_ENABLE);
	/* every queued request of this same context, back to back in its ring, goes in
	 * one submission (i915 also submits a context at its latest tail): no idle gap
	 * between them waiting for the next tick */
	int k = 1;
	while (k < E.qn && E.q[(E.qh + k) % QMAX].c == c) k++;
	regs[CTX_RING_TAIL] = E.q[(E.qh + k - 1) % QMAX].tail;
	drm_gem_flush(c->img.o, LRC_STATE_OFFSET, 64);          /* CONTEXT_CONTROL .. RING_TAIL */
	u64 desc = (u64)c->img.ggtt |
	           ((u64)(c->pml4 ? INTEL_LEGACY_64B_CONTEXT : INTEL_LEGACY_32B_CONTEXT) << GEN8_CTX_ADDRESSING_MODE_SHIFT) |
	           GEN8_CTX_VALID | GEN8_CTX_PRIVILEGE | ((u64)c->id << 32);
	__asm__ volatile("" ::: "memory");
	E.current = c;
	wr(RING_ELSP(RCS_BASE), 0);
	wr(RING_ELSP(RCS_BASE), 0);
	wr(RING_ELSP(RCS_BASE), (u32)(desc >> 32));
	wr(RING_ELSP(RCS_BASE), (u32)desc);
	E.inflight = k;
	E.hw_idle = 0;
	E.since = kernel_ticks;
}

/* under gt_lock: retire what finished, start what is next */
static void advance_locked(void)
{
	process_csb_locked();
	while (E.inflight) {
		u32 done = gt_seqno_done();
		if (!seq_passed(done, E.q[E.qh].seqno)) {
			if (!E.wedged && kernel_ticks - E.since > HANG_TICKS) E.wedged = 2;   /* reset in task context */
			return;
		}
		gt_ctx_t *c = E.q[E.qh].c;
		c->fresh = 0;
		c->queued--;
		E.qh = (E.qh + 1) % QMAX;
		E.qn--;
		E.inflight--;
		E.since = kernel_ticks;               /* progress: the hang clock restarts */
	}
	if (!E.inflight && E.qn && !E.hw_idle && !E.wedged && kernel_ticks - E.since > HANG_TICKS)
		E.wedged = 2;                         /* never went idle: reset in task context */
	/* next one only on an idle engine: its last context is saved by then */
	if (!E.inflight && E.qn && !E.wedged && E.hw_idle) elsp_submit_locked();
}

void gt_tick(void)
{
	if (!E.started) return;
	uint64_t f = spin_lock_irq(&gt_lock);
	if (E.qn || E.inflight) advance_locked();
	rps_sample_locked();
	spin_unlock_irq(&gt_lock, f);
}

/* the engine hung: reset it, call every request done, stay off */
static void wedge(void)
{
	mutex_lock(&gt_mutex);
	uint64_t f = spin_lock_irq(&gt_lock);
	int need = E.wedged != 1;
	E.wedged = 1;
	E.done_forced = E.seqno;
	spin_unlock_irq(&gt_lock, f);
	if (need) {
		kprintf("i915: GPU hang (seqno %u of %u): resetting render engine, GT off for this boot\n",
		        hws_seqno(), E.seqno);
		kprintf("i915: hang state: head %x tail %x acthd %x ipehr %x eir %x esr %x execl %x/%x ctl %x\n",
		        rd(RING_HEAD(RCS_BASE)), rd(RING_TAIL(RCS_BASE)), rd(RING_ACTHD(RCS_BASE)),
		        rd(_MMIO(RCS_BASE + 0x68)), rd(RING_EIR(RCS_BASE)), rd(RING_ESR(RCS_BASE)),
		        rd(RING_EXECLIST_STATUS_LO(RCS_BASE)), rd(_MMIO(RCS_BASE + 0x238)), rd(RING_CTL(RCS_BASE)));
		engine_reset();
		E.resets++;
		gt_state = -1;
		f = spin_lock_irq(&gt_lock);
		while (E.qn) { E.q[E.qh].c->queued--; E.qh = (E.qh + 1) % QMAX; E.qn--; }
		E.inflight = 0;
		spin_unlock_irq(&gt_lock, f);
	}
	mutex_unlock(&gt_mutex);
}

/* room for n dwords before the end of the ring, else pad with MI_NOOP and wrap */
static void ring_make_room(gt_ctx_t *c, u32 n)
{
	if (c->tail + n * 4 + 64 <= RING_SIZE) return;
	u32 from = c->tail;
	/* the ring's pages are not contiguous in the kernel map: address each dword */
	while (c->tail < RING_SIZE) { *kv(&c->ring, c->tail) = MI_NOOP; c->tail += 4; }
	drm_gem_flush(c->ring.o, from, RING_SIZE - from);
	c->tail = 0;
}

static void emit_pipe_control(u32 *cs, u32 flags, u32 offset)
{
	memset(cs, 0, 6 * 4);
	cs[0] = GFX_OP_PIPE_CONTROL(6);
	cs[1] = flags;
	cs[2] = offset;
}

/* queue batch at addr (PPGTT if the context has one, else GGTT); seqno out */
int gt_submit(gt_ctx_t *c, u64 addr, u32 *seqno_out)
{
	if (E.wedged == 2) wedge();
	if (!gt_usable()) return -EIO_;
	for (int i = 0; i < 2000 && E.qn >= QMAX - 1; i++) drm_sleep_ms(1);
	if (E.qn >= QMAX - 1) return -EBUSY_;

	u32 cs[48], n = 0;
	/* gen8_emit_flush_rcs(EMIT_INVALIDATE), with the Gen9 null PIPE_CONTROL first */
	emit_pipe_control(cs + n, 0, 0); n += 6;
	emit_pipe_control(cs + n, PIPE_CONTROL_CS_STALL | PIPE_CONTROL_TLB_INVALIDATE |
	                  PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE | PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE |
	                  PIPE_CONTROL_VF_CACHE_INVALIDATE | PIPE_CONTROL_CONST_CACHE_INVALIDATE |
	                  PIPE_CONTROL_STATE_CACHE_INVALIDATE | PIPE_CONTROL_QW_WRITE |
	                  PIPE_CONTROL_STORE_DATA_INDEX, LRC_PPHWSP_SCRATCH_ADDR); n += 6;
	/* gen8_emit_bb_start(); addr ~0: a request with no batch (parking on the kernel context) */
	if (addr != ~0ULL) {
		cs[n++] = MI_ARB_ON_OFF | MI_ARB_ENABLE;
		cs[n++] = MI_BATCH_BUFFER_START_GEN8 | (c->pml4 ? BIT(8) : 0);
		cs[n++] = (u32)addr;
		cs[n++] = (u32)(addr >> 32);
		cs[n++] = MI_ARB_ON_OFF | MI_ARB_DISABLE;
		cs[n++] = MI_NOOP;
	}

	mutex_lock(&gt_mutex);
	u32 seq = ++E.seqno;
	/* gen8_emit_fini_breadcrumb_rcs(): flush caches, then the seqno into the HWSP */
	emit_pipe_control(cs + n, PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH | PIPE_CONTROL_DEPTH_CACHE_FLUSH |
	                  PIPE_CONTROL_DC_FLUSH_ENABLE, 0); n += 6;
	cs[n++] = GFX_OP_PIPE_CONTROL(6);
	cs[n++] = PIPE_CONTROL_FLUSH_ENABLE | PIPE_CONTROL_CS_STALL | PIPE_CONTROL_QW_WRITE | PIPE_CONTROL_GLOBAL_GTT_IVB;
	cs[n++] = E.hws.ggtt + HWS_SEQNO * 4;
	cs[n++] = 0;
	cs[n++] = seq;
	cs[n++] = 0;
	/* gen8_emit_fini_breadcrumb_tail(): arbitration back on, or the engine can never
	 * leave this context (seen on Casper: later ELSP writes were ignored) */
	cs[n++] = MI_USER_INTERRUPT;
	cs[n++] = MI_ARB_ON_OFF | MI_ARB_ENABLE;
	cs[n++] = MI_ARB_CHECK;                  /* gen8_emit_wa_tail() */
	cs[n++] = MI_NOOP;

	ring_make_room(c, n);
	u32 start = c->tail;
	for (u32 i = 0; i < n; i++) *kv(&c->ring, c->tail + i * 4) = cs[i];   /* per page, see ring_make_room */
	c->tail += n * 4;
	drm_gem_flush(c->ring.o, start, n * 4);

	uint64_t f = spin_lock_irq(&gt_lock);
	int slot = (E.qh + E.qn) % QMAX;
	E.q[slot].c = c;
	E.q[slot].tail = c->tail;
	E.q[slot].seqno = seq;
	E.qn++;
	c->queued++;
	if (!E.inflight) advance_locked();
	spin_unlock_irq(&gt_lock, f);
	/* the previous context reports ACTIVE_TO_IDLE a few microseconds after its last
	 * seqno write; a client that saw that seqno and submits at once would otherwise
	 * wait for the next tick. Watch the status buffer for up to 100 us. */
	for (uint64_t until = drm_now_ns() + 100000; drm_now_ns() < until; ) {
		f = spin_lock_irq(&gt_lock);
		int waiting = !E.inflight && E.qn && !E.wedged;
		if (waiting) advance_locked();
		int started = E.inflight || !E.qn;
		spin_unlock_irq(&gt_lock, f);
		if (!waiting || started) break;
#if defined(__x86_64__)
		__asm__ volatile("pause");
#endif
	}
	mutex_unlock(&gt_mutex);
	*seqno_out = seq;
	return 0;
}

/* 0 when seqno is done, -ETIME at the deadline (timeout_ns < 0: forever), -EINTR, -EIO after a hang */
int gt_wait_seqno(u32 seqno, int64_t timeout_ns)
{
	if (!seqno || !E.started) return 0;
	uint64_t end = timeout_ns < 0 ? ~0ULL : drm_now_ns() + (uint64_t)timeout_ns;
	if (timeout_ns && !seq_passed(gt_seqno_done(), seqno)) rps_boost();
	/* short requests finish in microseconds: poll for up to 200 us before sleeping
	 * a whole tick, and start whatever is queued as soon as this one is seen done */
	uint64_t spin_end = drm_now_ns() + 200000;
	for (;;) {
		if (seq_passed(gt_seqno_done(), seqno)) {
			uint64_t f = spin_lock_irq(&gt_lock);
			if (E.started && (E.qn || E.inflight)) advance_locked();
			spin_unlock_irq(&gt_lock, f);
			return E.wedged == 1 && seq_passed(seqno, hws_seqno() + 1) ? -EIO_ : 0;
		}
		if (E.wedged == 2) { wedge(); continue; }
		uint64_t now = drm_now_ns();
		if (now >= end) return -ETIME_;
		if (now < spin_end) {
#if defined(__x86_64__)
			__asm__ volatile("pause");
#elif defined(__aarch64__)
			__asm__ volatile("yield");
#endif
			continue;
		}
		drm_sleep_ms(1);
		if (task_has_actionable_signal()) return -EINTR_;
	}
}

int gt_seqno_busy(u32 seqno) { return seqno && E.started && !seq_passed(gt_seqno_done(), seqno); }

/* ── control: /sys/class/drm/ranix_gt ── */

int ranix_gt_command(const char *cmd, size_t len)
{
	mmio = intel_mmio();
	if (!mmio) return -ENODEV_;
	if (len >= 5 && !memcmp(cmd, "audit", 5)) { intel_ggtt_audit(last, (int)sizeof last); return 0; }   /* read only */
	if (len >= 7 && !memcmp(cmd, "rps off", 7)) {        /* A/B: back to the lowest bin, no sampling */
		R.off_by_user = 1;
		if (R.on) { uint64_t f = spin_lock_irq(&gt_lock); R.on = 0; R.max_soft = R.min_soft; rps_write(R.rpn); spin_unlock_irq(&gt_lock, f); }
		return 0;
	}
	if (len >= 6 && !memcmp(cmd, "rps on", 6)) {
		R.off_by_user = 0;
		if (E.started && R.rp0 && !R.on) { uint64_t f = spin_lock_irq(&gt_lock); rps_enable_hw(); spin_unlock_irq(&gt_lock, f); }
		return 0;
	}
	if (len >= 8 && !memcmp(cmd, "rps max ", 8)) {        /* rps max <MHz>: a ceiling, RPn..RP0 */
		u32 mhz = 0;
		for (size_t i = 8; i < len && cmd[i] >= '0' && cmd[i] <= '9'; i++) mhz = mhz * 10 + (u32)(cmd[i] - '0');
		u32 v = (mhz * 3 + 49) / 50;
		if (!R.rp0 || v < R.rpn || v > R.rp0) return -EINVAL_;
		uint64_t f = spin_lock_irq(&gt_lock);
		R.max_soft = v;
		if (R.cur > v) rps_write(v);
		spin_unlock_irq(&gt_lock, f);
		return 0;
	}
	return -EINVAL_;
}

int ranix_gt_status(char *o)
{
	int n = put(o, 0, gt_state == 1 ? "state: works\n" : gt_state < 0 ? "state: off (failed)\n" : "state: untried\n");
	if (E.started) {
		n = put(o, n, "engine: seqno ");
		n = puthex(o, n, E.seqno);
		n = put(o, n, "done ");
		n = puthex(o, n, gt_seqno_done());
		n = put(o, n, "queued ");
		n = puthex(o, n, (u32)E.qn);
		n = put(o, n, "resets ");
		n = puthex(o, n, E.resets);
		o[n++] = '\n';
		if (R.rp0) {
			n = put(o, n, R.on ? "rps: on" : "rps: off");
			n = put(o, n, " req_MHz "); n = putdec_(o, n, rps_mhz(R.cur));
			n = put(o, n, " min_MHz "); n = putdec_(o, n, rps_mhz(R.min_soft));
			n = put(o, n, " max_MHz "); n = putdec_(o, n, rps_mhz(R.max_soft));
			n = put(o, n, " rp1_MHz "); n = putdec_(o, n, rps_mhz(R.rp1));
			n = put(o, n, " rp0_MHz "); n = putdec_(o, n, rps_mhz(R.rp0));
			n = put(o, n, " ups "); n = putdec_(o, n, R.ups);
			n = put(o, n, " downs "); n = putdec_(o, n, R.downs);
			n = put(o, n, " boosts "); n = putdec_(o, n, R.boosts);
			o[n++] = '\n';
		}
	}
	return put(o, n, last);
}

/* for ranix_power: the GT's actual and requested frequency; 0 when the engine is off */
int gt_freq_mhz(uint32_t *act, uint32_t *req)
{
	if (!E.started || !mmio) return 0;
	*act = ((rd(GEN6_RPSTAT1_) >> 23) & 0x1ff) * 50 / 3;    /* GEN6_RPSTAT1 CAGF, 50/3 MHz units */
	*req = rps_mhz(R.cur);
	return 1;
}
