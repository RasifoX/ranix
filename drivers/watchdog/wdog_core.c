/* Watchdog policy: arm from the cmdline, decide when the machine has earned a
 * pet, and make every panic end in a reset. Hardware access is in itco.c.
 *
 * What a pet means. The TCO timer is reloaded only by a kernel thread, so a dead
 * scheduler or a stopped tick stops the pets with no further logic. That alone
 * misses a CPU wedged in a non-preemptible kernel loop while its neighbours run
 * fine, so one canary thread is pinned to each CPU and the pet thread refuses to
 * pet while any canary has gone stale. A canary beat is proof that CPU's run queue
 * was dispatched, which a tick counter is not: ticks keep arriving on a CPU stuck
 * in a kernel loop with IRQs on.
 *
 * During boot the threads do not pet. The non-preemptible boot path holds CPU0,
 * so the boot path itself pets with watchdog_touch(); a stall there stops the pets
 * even while a secondary CPU is perfectly healthy.
 */

#include <uapi/types.h>
#include <kernel/board.h>
#include <kernel/cmdline.h>
#include <kernel/cpumask.h>
#include <kernel/hal.h>
#include <kernel/irq.h>
#include <kernel/kprintf.h>
#include <kernel/mm/heap.h>
#include <kernel/proc.h>
#include <kernel/pstore.h>
#include <kernel/smp.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>
#include <kernel/watchdog.h>
#include <asm/io.h>
#include "itco.h"
#include <kernel/time.h>
#include <asm/cpu.h>
#include <kernel/tick.h>


#define WD_SECS_MIN        10
#define WD_SECS_MAX        1227   /* 2 stages of 1023 ticks at 0.6 s */
#define PANIC_DELAY_DEF    5      /* seconds on screen before an automatic reset */
#define PANIC_HW_MARGIN    6      /* TCO backstop, seconds after the software reset */
#define WD_COUNT_CHECK_SECS 3     /* several 0.6 s ticks, short of any timeout */

static unsigned wd_secs;
static int      wd_armed;
static uint64_t wd_stale_ms;
static volatile int wd_stop_pets;
static volatile int wd_boot_finished;
static volatile int wd_reported_stale;

static volatile uint64_t canary_beat[MAX_CPUS];
static volatile uint32_t canary_next;

static unsigned parse_uint(const char *s, int *ok)
{
	unsigned v = 0;
	*ok = *s != '\0';
	for (; *s; s++) {
		if (*s < '0' || *s > '9' || v > 100000) { *ok = 0; return 0; }
		v = v * 10 + (unsigned)(*s - '0');
	}
	return v;
}

/* One countdown is two stages of 0.6 s ticks: ticks = secs / 1.2. */
static unsigned secs_to_stage_ticks(unsigned secs)
{
	unsigned t = secs * 10 / 12;
	if (t < ITCO_TICKS_MIN) t = ITCO_TICKS_MIN;
	if (t > ITCO_TICKS_MAX) t = ITCO_TICKS_MAX;
	return t;
}

void watchdog_init(void)
{
	char buf[16];
	int ok;

	if (!cmdline_get("ranix.watchdog", buf, sizeof(buf))) return;

	unsigned secs = parse_uint(buf, &ok);
	if (!ok) {
		kprintf(KERN_WARN "wdog: bad ranix.watchdog='%s', staying off\n", buf);
		return;
	}
	if (secs == 0) {
		kprintf(KERN_INFO "wdog: off (ranix.watchdog=0)\n");
		return;
	}
	if (secs < WD_SECS_MIN) secs = WD_SECS_MIN;
	if (secs > WD_SECS_MAX) secs = WD_SECS_MAX;

	if (itco_probe() != 0) {
		kprintf(KERN_WARN "wdog: hardware timer unavailable, panic reset still active\n");
		return;
	}
	if (itco_fired_last_boot())
		kprintf(KERN_WARN "wdog: the previous reset was a tco timeout\n");

	unsigned ticks = secs_to_stage_ticks(secs);
	if (itco_start(ticks) != 0) return;

	wd_secs = secs;
	wd_stale_ms = (uint64_t)(secs / 2) * 1000;
	wd_armed = 1;
	kprintf(KERN_INFO "wdog: armed, reset %u s after the last pet (%u ticks per stage)\n",
	        secs, ticks);
}

/* Canary ages in real time. The HPET is independent of the tick, whose rate is
 * exactly what a sick or miscalibrated timer gets wrong; a wrong rate here would
 * turn into false resets. Machines without one fall back to the tick. */
static uint64_t wd_now_ms(void)
{
	uint64_t ns = hpet_now_ns();
	return ns ? ns / 1000000 : kernel_ticks * (1000 / TIMER_HZ);
}

void watchdog_touch(void)
{
	if (wd_armed && !wd_stop_pets) itco_pet();
}

void watchdog_boot_done(void)
{
	if (!wd_armed) return;
	uint64_t now = wd_now_ms();
	for (int i = 0; i < smp_ncpus; i++) canary_beat[i] = now;
	itco_pet();
	smp_mb();
	wd_boot_finished = 1;
}

static void pin_self_to(int cpu)
{
	cpumask_t *m = kmalloc(sizeof(*m));
	if (!m) return;
	cpumask_clear(m);
	cpumask_set_cpu(cpu, m);
	task_current()->cpu_affinity = m;
	if ((int)arch_get_cpu_ctx()->cpu_id != cpu) yield();
}

static void canary_thread(void)
{
	int cpu = (int)atomic_fetch_add32(&canary_next, 1);
	pin_self_to(cpu);
	for (;;) {
		/* Stamp the CPU we really ran on: a mis-pinned canary must not vouch
		 * for a CPU it never touched. */
		canary_beat[arch_get_cpu_ctx()->cpu_id] = wd_now_ms();
		task_sleep_until(kernel_ticks + TIMER_HZ);
	}
}

static int cpus_dispatching(uint64_t now)
{
	for (int i = 0; i < smp_ncpus; i++) {
		cpu_context_t *c = all_cpus[i];
		if (!c || !c->online) continue;
		uint64_t beat = canary_beat[i];
		uint64_t age = now > beat ? now - beat : 0;
		if (age > wd_stale_ms) {
			if (!wd_reported_stale) {
				wd_reported_stale = 1;
				process_t *cur = c->current;
				kprintf(KERN_ERR "wdog: cpu%d not dispatching for %lu s, on-cpu pid=%d (%s), not petting\n",
				        i, age / 1000, cur ? (int)cur->pid : -1, cur ? cur->name : "-");
			}
			return 0;
		}
	}
	if (wd_reported_stale) {
		wd_reported_stale = 0;
		kprintf(KERN_WARN "wdog: all cpus dispatching again, petting resumed\n");
	}
	return 1;
}

/* A timer that does not count is no protection, and on the Casper's Sunrise Point
 * the TCO counter stands still with TMR_HLT clear, under Linux's driver too. The
 * log says so; pets go on, harmless on a dead register and right if it ever runs. */
static void hw_timer_check(void)
{
	itco_pet();
	uint16_t before = itco_counter();
	task_sleep_until(kernel_ticks + WD_COUNT_CHECK_SECS * TIMER_HZ);
	uint16_t left = itco_counter();

	if (left < before) {
		kprintf(KERN_DEBUG "wdog: hardware timer counting (%u -> %u)\n",
		        (unsigned)before, (unsigned)left);
		return;
	}
	kprintf(KERN_WARN "wdog: hardware timer does not count (%u -> %u in %u s), NO hardware "
	        "reset on this machine; a hang resets only through ranix.deadline\n",
	        (unsigned)before, (unsigned)left, WD_COUNT_CHECK_SECS);
}

static void pet_thread(void)
{
	uint64_t period = (uint64_t)(wd_secs / 4) * TIMER_HZ;

	while (!wd_boot_finished)
		task_sleep_until(kernel_ticks + TIMER_HZ);
	if (!wd_stop_pets) hw_timer_check();

	for (;;) {
		task_sleep_until(kernel_ticks + period);
		if (wd_stop_pets) continue;
		if (cpus_dispatching(wd_now_ms())) {
			itco_pet();
			klog_pstore_beat(kernel_ticks);
		}
	}
}

/* Fault injection for the QEMU tests:
 *   ranix.wdtest=hang     IRQs off, spin: all ticks on this CPU stop
 *   ranix.wdtest=spin     kernel loop with IRQs on: ticks live, run queue dead
 *   ranix.wdtest=panic    plain panic
 *   ranix.wdtest=nested   fault while printing the panic
 *   ranix.wdtest=boothang stall the boot path with a secondary CPU healthy */
static void test_thread(void)
{
	char buf[16];
	cmdline_get("ranix.wdtest", buf, sizeof(buf));
	while (!wd_boot_finished && strcmp(buf, "panic") != 0)
		task_sleep_until(kernel_ticks + TIMER_HZ);
	task_sleep_until(kernel_ticks + 3 * TIMER_HZ);

	kprintf(KERN_WARN "wdog: test injecting %s\n", buf);
	if (strcmp(buf, "hang") == 0) {
		irq_disable();
		for (;;) cpu_relax();
	} else if (strcmp(buf, "spin") == 0) {
		pin_self_to(smp_ncpus - 1);
		for (;;) cpu_relax();
	} else if (strcmp(buf, "panic") == 0) {
		kpanic("wdtest: injected panic");
	} else if (strcmp(buf, "nested") == 0) {
		kpanic((const char *)0xDEAD000000000000UL);   /* non-canonical: must fault */
	}
	task_exit();
}

/* ranix.deadline=N: reset N seconds after boot, whatever userspace is doing. The
 * TCO only proves the kernel schedules; a hung script on a machine nobody can
 * reach leaves the kernel healthy and the watchdog fed forever. */
static volatile uint64_t deadline_tick;

static void deadline_check(void)
{
	if (!deadline_tick || kernel_ticks < deadline_tick) return;
	deadline_tick = 0;
	kprintf(KERN_WARN "wdog: deadline reached, resetting\n");
	board_reset();
}

static int deadline_registered;

/* /proc/sys/kernel/ranix_deadline: 0 disarms, N resets N seconds from now. A boot
 * that proves itself healthy (network and SSH up) turns the deadline off, so a
 * short deadline only catches boots that never get that far. */
int wdog_deadline_set(unsigned secs)
{
	if (secs > 7 * 24 * 3600) return -22;
	if (!secs) { deadline_tick = 0; kprintf(KERN_INFO "wdog: deadline off\n"); return 0; }
	deadline_tick = kernel_ticks + (uint64_t)secs * TIMER_HZ;
	if (!deadline_registered) { deadline_registered = 1; tick_register(deadline_check); }
	kprintf(KERN_INFO "wdog: deadline reset in %u s\n", secs);
	return 0;
}

unsigned wdog_deadline_left(void)
{
	uint64_t t = deadline_tick, now = kernel_ticks;
	if (!t) return 0;
	if (t <= now) return 1;
	return (unsigned)((t - now + TIMER_HZ - 1) / TIMER_HZ);
}

static void deadline_init(void)
{
	char buf[16];
	int ok;
	if (!cmdline_get("ranix.deadline", buf, sizeof(buf))) return;
	unsigned secs = parse_uint(buf, &ok);
	if (!ok || !secs) { kprintf(KERN_WARN "wdog: bad deadline value '%s'\n", buf); return; }
	wdog_deadline_set(secs);
}

void watchdog_start(void)
{
	deadline_init();
	if (cmdline_has("ranix.wdtest")) {
		char buf[16];
		cmdline_get("ranix.wdtest", buf, sizeof(buf));
		if (strcmp(buf, "boothang") == 0) {
			kprintf(KERN_WARN "wdog: test stalling the boot path\n");
			for (;;) cpu_relax();
		}
		proc_create("wdtest", test_thread);
	}
	if (!wd_armed) return;

	uint64_t now = wd_now_ms();
	for (int i = 0; i < smp_ncpus; i++) {
		canary_beat[i] = now;
		proc_create("wd_canary", canary_thread);
	}
	proc_create("wd_pet", pet_thread);
}

/* ── panic ──────────────────────────────────────────────────────────────────
 * Linux's panic=N names the delay; panic=0 means stay frozen, a negative value
 * resets at once. Without panic=, asking for a watchdog asks for an unattended
 * machine, so it implies a short delay. Plain boots keep the old frozen screen.
 * Read from the cmdline at panic time: cmdline_get takes no locks. */
static int panic_delay_secs(void)
{
	char buf[16];
	int ok;

	if (cmdline_get("panic", buf, sizeof(buf))) {
		if (buf[0] == '-') return 0;
		unsigned v = parse_uint(buf, &ok);
		if (ok && v > 3600) v = 3600;
		return (ok && v) ? (int)v : -1;
	}
	if (cmdline_get("ranix.watchdog", buf, sizeof(buf))) {
		unsigned v = parse_uint(buf, &ok);
		if (ok && v) return PANIC_DELAY_DEF;
	}
	return -1;
}

int panic_will_reset(void) { return panic_delay_secs() >= 0; }

void panic_reset_begin(const char *msg)
{
	wd_stop_pets = 1;
	pstore_end(PSTORE_STATE_PANIC, msg);

	/* Backstop for a panic that wedges while printing: the banner path takes the
	 * framebuffer lock and walks possibly corrupt memory. */
	int delay = panic_delay_secs();
	if (wd_armed && delay >= 0)
		itco_set_ticks(secs_to_stage_ticks((unsigned)delay + PANIC_HW_MARGIN));
}

void panic_reset_nested(void)
{
	if (panic_will_reset()) board_reset();
}

/* No timers here: IRQs are off and the tick is dead. HPET when the machine has
 * one; a bounded port-0x80 loop (about 1 us per write) caps it either way. */
static void panic_delay(int secs)
{
	uint64_t start = hpet_now_ns();
	uint64_t target = (uint64_t)secs * 1000000000ULL;
	uint64_t limit = (uint64_t)secs * 1000000ULL;

	for (uint64_t i = 0; i < limit; i++) {
		io_wait();
		if (start && hpet_now_ns() - start >= target) break;
	}
}

void panic_reset_finish(void)
{
	int delay = panic_delay_secs();
	if (delay < 0) return;
	panic_delay(delay);
	board_reset();
}
