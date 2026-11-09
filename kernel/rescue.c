/* ── Ranix rescue channel ─────────────────────────────────────────────────────
 * 2026-11-08, Casper: a task looped forever inside an ioctl holding drm_lock.
 * ping still answered (the kernel was fine), but sshd had been stopped by the
 * shutdown that then waited on the stuck task for good, and a person had to hold
 * the power button. This is the way back in for that case: a small
 * authenticated command set answered by the kernel's own network stack in the RX
 * path, next to ICMP echo, depending on nothing in userspace.
 *
 * Protocol and commands: include/uapi/rescue.h. Security: HMAC-SHA256 over a
 * single-use kernel nonce, so a command cannot be replayed and the key cannot be
 * brute-forced online (a bad MAC spends the nonce). No key loaded = channel off.
 * Replies are MACed too, so a client knows the answer came from this kernel.
 *
 * Hang-proofing: every command is answered from the RX path with no sleeping
 * lock. Work that may block (sync before reboot) runs on its own kernel thread,
 * and a tick deadline resets the board even if that thread never comes back.
 * ─────────────────────────────────────────────────────────────────────────── */

#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/sync.h>
#include <kernel/hal.h>
#include <kernel/board.h>
#include <kernel/block.h>
#include <kernel/vfs.h>
#include <kernel/config.h>
#include <kernel/kprintf.h>
#include <kernel/capabilities.h>
#include <kernel/kout.h>
#include <kernel/lib/sha256.h>
#include <kernel/lib/string.h>
#include <kernel/net/net.h>
#include <kernel/net/udp.h>
#include <kernel/mm/heap.h>
#include <kernel/fs/devfs.h>
#include <uapi/rescue.h>
#include <kernel/time.h>
#include <kernel/tick.h>
#include <kernel/watchdog.h>
#include <kernel/hungtask.h>
#include <kernel/claim.h>
#include <kernel/init.h>



/* ── keys and nonces ───────────────────────────────────────────────────────── */
static uint8_t  rx_key[RESCUE_KEY_BYTES];
static int      rx_key_set;
static uint8_t  rx_tkey[RESCUE_KEY_BYTES];
static uint64_t rx_tkey_until;           /* kernel_ticks; 0 = no test key */
static uint8_t  rx_boot[8];
static uint64_t rx_ctr;

#define NNONCE 8
static struct { uint8_t v[16]; uint64_t until; } rx_nonce[NNONCE];
static spinlock_t rx_lock = SPINLOCK_INIT;

/* stats for /dev/rescue */
static uint64_t st_hello, st_cmd, st_bad, st_last_cmd_tick;
static char     st_last_cmd[24];

static const char HEX[] = "0123456789abcdef";

static void hexenc(char *dst, const uint8_t *src, size_t n)
{
	for (size_t i = 0; i < n; i++) { dst[2 * i] = HEX[src[i] >> 4]; dst[2 * i + 1] = HEX[src[i] & 15]; }
	dst[2 * n] = 0;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int hexdec(uint8_t *dst, const char *src, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		int a = hexval(src[2 * i]), b = hexval(src[2 * i + 1]);
		if (a < 0 || b < 0) return -1;
		dst[i] = (uint8_t)(a << 4 | b);
	}
	return 0;
}

static uint64_t rdtsc_any(void)
{
#if defined(__x86_64__)
	uint32_t lo, hi;
	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return (uint64_t)hi << 32 | lo;
#else
	uint64_t v;
	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
	return v;
#endif
}

/* HMAC-SHA256 over the concatenation of up to three pieces. */
static void hmac3(const uint8_t key[32], const void *a, size_t an, const void *b, size_t bn,
                  const void *c, size_t cn, uint8_t out[32])
{
	uint8_t pad[64], ih[32];
	sha256_t s;
	memset(pad, 0x36, sizeof pad);
	for (int i = 0; i < 32; i++) pad[i] ^= key[i];
	sha256_init(&s); sha256_update(&s, pad, 64);
	if (an) sha256_update(&s, a, an);
	if (bn) sha256_update(&s, b, bn);
	if (cn) sha256_update(&s, c, cn);
	sha256_final(&s, ih);
	memset(pad, 0x5c, sizeof pad);
	for (int i = 0; i < 32; i++) pad[i] ^= key[i];
	sha256_init(&s); sha256_update(&s, pad, 64); sha256_update(&s, ih, 32);
	sha256_final(&s, out);
}

static int ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
	uint8_t d = 0;
	for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
	return d == 0;
}

/* Unique per boot and per call, unpredictable without the key: the counter and
 * TSC make it unique, keying the hash with the secret makes it unguessable. */
static void make_nonce(uint8_t out[16])
{
	struct { uint64_t ctr, tsc, ticks; int64_t wall; uint8_t boot[8]; } in;
	in.ctr = ++rx_ctr; in.tsc = rdtsc_any(); in.ticks = kernel_ticks;
	int64_t ns; ranix_walltime(&in.wall, &ns);
	memcpy(in.boot, rx_boot, 8);
	uint8_t h[32];
	hmac3(rx_key, "nonce", 5, &in, sizeof in, 0, 0, h);
	memcpy(out, h, 16);
}

/* ── replies ───────────────────────────────────────────────────────────────── */
#define RBUF_MAX   (48 * 1024)
#define CHUNK      1100
static char rbuf[RBUF_MAX];            /* serialized by net_poll's single runner */

static void send_raw(netdev_t *dev, uint32_t ip, uint16_t port, const char *s, size_t n)
{
	udp_tx(dev, ip, RESCUE_PORT, port, s, (uint16_t)n);
}

static void send_err(netdev_t *dev, uint32_t ip, uint16_t port, const char *why)
{
	char b[96];
	kout_t o; kout_init(&o, b, sizeof b);
	kout_s(&o, "RX1 ERR "); kout_s(&o, why); kout_c(&o, '\n');
	send_raw(dev, ip, port, b, o.len);
}

static void send_body(netdev_t *dev, uint32_t ip, uint16_t port, const uint8_t *key,
                      const char *nonce_hex, const char *body, size_t blen)
{
	static char pkt[CHUNK + 200];
	size_t off = 0;
	unsigned seq = 0;
	do {
		size_t n = blen - off > CHUNK ? CHUNK : blen - off;
		int more = off + n < blen;
		char head[96];
		kout_t h; kout_init(&h, head, sizeof head);
		kout_s(&h, "RX1 R "); kout_s(&h, nonce_hex); kout_c(&h, ' ');
		kout_u(&h, seq); kout_c(&h, ' '); kout_c(&h, more ? '1' : '0');
		uint8_t mac[32]; char mh[65];
		hmac3(key, head, h.len, "\n", 1, body + off, n, mac);
		hexenc(mh, mac, 32);
		kout_t p; kout_init(&p, pkt, sizeof pkt);
		kout_sn(&p, head, h.len); kout_c(&p, ' '); kout_s(&p, mh); kout_c(&p, '\n');
		memcpy(pkt + p.len, body + off, n);
		send_raw(dev, ip, port, pkt, p.len + n);
		off += n; seq++;
	} while (off < blen && seq < 48);
}

/* ── deferred work: sync, then reset / power off ───────────────────────────── */
enum { ACT_NONE, ACT_SYNC, ACT_REBOOT, ACT_POWEROFF };
static volatile int      rx_act;
static volatile uint64_t rx_hard_at;     /* tick deadline for reset/poweroff, whatever happens */
static volatile int      rx_hard_kind;
static volatile int      rx_sync_state;  /* 0 never, 1 running, 2 done */
static volatile uint64_t rx_sync_ms;
static process_t *volatile rx_worker;

static void hard_tick(void)
{
	uint64_t t = rx_hard_at;
	if (!t || kernel_ticks < t) return;
	rx_hard_at = 0;
	kprintf(KERN_WARN "rescue: deadline reached, %s now\n", rx_hard_kind == ACT_POWEROFF ? "power off" : "reset");
	if (rx_hard_kind == ACT_POWEROFF) board_poweroff();
	board_reset();
}

static void do_sync(void)
{
	rx_sync_state = 1;
	uint64_t t0 = kernel_ticks;
#if IS_ENABLED(CONFIG_FS_EXT2)
	ext2_sync_all();
#endif
	block_flush_all();
	rx_sync_ms = (kernel_ticks - t0) * (1000 / TIMER_HZ);
	rx_sync_state = 2;
}

static void rescue_worker(void)
{
	rx_worker = task_current();
	for (;;) {
		int a = rx_act;
		if (a == ACT_NONE) { task_sleep_until(kernel_ticks + TIMER_HZ / 10); continue; }
		rx_act = ACT_NONE;
		do_sync();
		if (a == ACT_REBOOT || a == ACT_POWEROFF) {
#if IS_ENABLED(CONFIG_FS_EXT2)
			ext2_shutdown_all();
#endif
			block_flush_all();
			kprintf(KERN_WARN "rescue: synced in %lu ms, %s\n", (unsigned long)rx_sync_ms,
			        a == ACT_POWEROFF ? "power off" : "reset");
			if (a == ACT_POWEROFF) board_poweroff();
			board_reset();
		}
	}
}

static void schedule_act(int act, unsigned grace_s)
{
	if (act == ACT_REBOOT || act == ACT_POWEROFF) {
		rx_hard_kind = act;
		rx_hard_at = kernel_ticks + (uint64_t)grace_s * TIMER_HZ + 2;
	}
	rx_act = act;
	if (rx_worker) task_wake(rx_worker);
}

/* ── commands ──────────────────────────────────────────────────────────────── */
static char st_letter(uint64_t s)
{
	switch (s) {
	case PROC_RUNNING: return 'R'; case PROC_READY: return 'Q'; case PROC_SLEEPING: return 'S';
	case PROC_ZOMBIE: return 'Z'; default: return 'X';
	}
}

static unsigned parse_u(const char **pp, int *ok)
{
	const char *p = *pp;
	while (*p == ' ') p++;
	unsigned v = 0; int any = 0;
	while (*p >= '0' && *p <= '9') { v = v * 10 + (unsigned)(*p - '0'); p++; any = 1; }
	*pp = p; *ok = any;
	return v;
}

static int word_is(const char **pp, const char *w)
{
	size_t n = strlen(w);
	if (strncmp(*pp, w, n) != 0) return 0;
	char c = (*pp)[n];
	if (c && c != ' ' && c != '\n') return 0;
	*pp += n;
	return 1;
}

static void cmd_ping(kout_t *o)
{
	int64_t ws, wn; ranix_walltime(&ws, &wn);
	kout_s(o, "ok uptime_s="); kout_u(o, kernel_ticks / TIMER_HZ);
	kout_s(o, " wall="); kout_i(o, ws);
	kout_s(o, " tasks="); kout_u(o, (uint64_t)task_count());
	kout_s(o, " unkillable="); kout_u(o, kwatch_unkillable_count);
	kout_s(o, " kstall="); kout_u(o, kwatch_kstall_count);
	kout_s(o, " sync="); kout_s(o, rx_sync_state == 2 ? "done" : rx_sync_state == 1 ? "running" : "never");
	if (rx_sync_state == 2) { kout_s(o, " sync_ms="); kout_u(o, rx_sync_ms); }
	kout_s(o, " deadline_s="); kout_u(o, wdog_deadline_left());
	if (rx_hard_at) { kout_s(o, " reset_in_s="); kout_u(o, rx_hard_at > kernel_ticks ? (rx_hard_at - kernel_ticks) / TIMER_HZ : 0); }
	kout_c(o, '\n');
}

static void cmd_ps(kout_t *o)
{
	kout_s(o, "PID TGID PPID UID ST INSYS SYSCALL CPU_S KILLED NAME\n");
	int n = task_count();
	for (int i = 0; i < n; i++) {
		uint64_t f = spin_lock_irq(&sched_lock);
		process_t *p = task_peek_locked(i);
		if (!p || p->magic != PROC_MAGIC) { spin_unlock_irq(&sched_lock, f); continue; }
		uint64_t pid = p->pid; int tgid = p->tgid, ppid = p->ppid, uid = (int)p->uid;
		char st = st_letter(p->state); int ins = p->in_syscall, sc = p->last_syscall;
		uint64_t cpu = p->ticks / TIMER_HZ;
		int killed = (p->sig_pending & (1UL << 8)) != 0;
		char name[17]; int k = 0;
		for (; k < 16 && p->name[k]; k++) name[k] = p->name[k];
		name[k] = 0;
		spin_unlock_irq(&sched_lock, f);
		kout_u(o, pid); kout_c(o, ' '); kout_i(o, tgid); kout_c(o, ' '); kout_i(o, ppid);
		kout_c(o, ' '); kout_i(o, uid); kout_c(o, ' '); kout_c(o, st); kout_c(o, ' ');
		kout_i(o, ins); kout_c(o, ' '); kout_i(o, sc); kout_c(o, ' '); kout_u(o, cpu);
		kout_c(o, ' '); kout_i(o, killed); kout_c(o, ' '); kout_s(o, name); kout_c(o, '\n');
	}
}

static void cmd_log(kout_t *o, unsigned want)
{
	if (!want) want = 4000;
	if (want > 40000) want = 40000;
	size_t total = kprintf_log_len();
	if (!total) return;
	char *tmp = kmalloc(total);
	if (!tmp) { kout_s(o, "no memory\n"); return; }
	size_t got = kprintf_read_log(tmp, total);
	size_t from = got > want ? got - want : 0;
	while (from < got && from && tmp[from - 1] != '\n') from++;   /* start on a line */
	kout_sn(o, tmp + from, got - from);
	kfree(tmp);
}

static void cmd_kill(kout_t *o, const char *a, int group)
{
	int ok, ok2;
	unsigned pid = parse_u(&a, &ok);
	unsigned sig = parse_u(&a, &ok2);
	if (!ok || !pid) { kout_s(o, "usage: kill <pid> [sig]\n"); return; }
	if (!ok2) sig = 9;
	if (sig < 1 || sig > 64) { kout_s(o, "bad signal\n"); return; }
	if (group) {
		task_signal_pgrp((pid_t)pid, (int)sig);
		kout_s(o, "signalled pgrp "); kout_u(o, pid); kout_c(o, '\n');
		return;
	}
	process_t *p = proc_get((pid_t)pid);
	if (!p) { kout_s(o, "no such pid\n"); return; }
	task_signal_group(p, (int)sig);
	kout_s(o, "signalled pid "); kout_u(o, pid); kout_s(o, " ("); kout_s(o, p->name);
	kout_s(o, ") sig "); kout_u(o, sig); kout_c(o, '\n');
	proc_put(p);
}

static void run_cmd(kout_t *o, const char *c)
{
	if (word_is(&c, "ping")) cmd_ping(o);
	else if (word_is(&c, "ps")) cmd_ps(o);
	else if (word_is(&c, "stuck")) kwatch_report(o, 1);
	else if (word_is(&c, "log")) { int ok; cmd_log(o, parse_u(&c, &ok)); }
	else if (word_is(&c, "killpg")) cmd_kill(o, c, 1);
	else if (word_is(&c, "kill")) cmd_kill(o, c, 0);
	else if (word_is(&c, "sync")) { schedule_act(ACT_SYNC, 0); kout_s(o, "sync started; see ping\n"); }
	else if (word_is(&c, "reboot-now")) { schedule_act(ACT_REBOOT, 0); kout_s(o, "reset now\n"); }
	else if (word_is(&c, "reboot")) {
		int ok; unsigned g = parse_u(&c, &ok);
		if (!ok) g = 10;
		if (g > 120) g = 120;
		schedule_act(ACT_REBOOT, g);
		kout_s(o, "sync, then reset; hard reset in "); kout_u(o, g); kout_s(o, " s at the latest\n");
	}
	else if (word_is(&c, "poweroff")) { schedule_act(ACT_POWEROFF, 10); kout_s(o, "sync, then power off (10 s at the latest)\n"); }
	else if (word_is(&c, "deadline")) {
		int ok; unsigned s = parse_u(&c, &ok);
		if (!ok) { kout_s(o, "deadline_s="); kout_u(o, wdog_deadline_left()); kout_c(o, '\n'); return; }
		kout_s(o, wdog_deadline_set(s) == 0 ? "ok\n" : "refused\n");
	}
	else if (word_is(&c, "claims")) claim_report(o, 0);
	else kout_s(o, "commands: ping ps stuck log [bytes] kill <pid> [sig] killpg <pgid> [sig] sync reboot [grace_s] reboot-now poweroff deadline [s] claims\n");
}

/* ── RX entry: from udp_rx, net_poll's single runner ───────────────────────── */
static uint64_t rl_tick, rl_count, rl_bad;

int rescue_udp_rx(netdev_t *dev, uint32_t src_ip, uint16_t src_port, const uint8_t *data, uint16_t len)
{
	if (!dev || dev->net_ns != &init_net_ns) return 0;   /* host only, never a container */
	/* LAN only: loopback, RFC 1918 and link-local sources. Anything routed in from
	 * the internet is dropped without an answer (not even an ERR). */
	{
		uint32_t a = src_ip;
		int lan = (a >> 24) == 127 || (a >> 24) == 10 || (a >> 20) == 0xAC1 ||
		          (a >> 16) == 0xC0A8 || (a >> 16) == 0xA9FE;
		if (!lan) { st_bad++; return 1; }
	}
	/* 40 packets a second in all, whatever they are, and at most 5 bad MACs a
	 * second: an answer is cheap but not free, and guessing gets nowhere */
	if (kernel_ticks / TIMER_HZ != rl_tick) { rl_tick = kernel_ticks / TIMER_HZ; rl_count = 0; rl_bad = 0; }
	if (++rl_count > 40 || rl_bad > 5) return 1;

	char m[600];
	if (len >= sizeof m) { send_err(dev, src_ip, src_port, "too long"); return 1; }
	memcpy(m, data, len); m[len] = 0;
	while (len && (m[len - 1] == '\n' || m[len - 1] == '\r')) m[--len] = 0;

	int have_test = rx_tkey_until && kernel_ticks < rx_tkey_until;
	if (!rx_key_set && !have_test) { send_err(dev, src_ip, src_port, "no key loaded"); return 1; }

	if (strcmp(m, "RX1 HELLO") == 0) {
		st_hello++;
		uint8_t nv[16]; char nh[33], bh[17];
		uint64_t f = spin_lock_irq(&rx_lock);
		make_nonce(nv);
		int slot = 0;
		for (int i = 1; i < NNONCE; i++) if (rx_nonce[i].until < rx_nonce[slot].until) slot = i;
		memcpy(rx_nonce[slot].v, nv, 16);
		rx_nonce[slot].until = kernel_ticks + RESCUE_NONCE_SECS * TIMER_HZ;
		spin_unlock_irq(&rx_lock, f);
		hexenc(nh, nv, 16); hexenc(bh, rx_boot, 8);
		char b[128]; kout_t o; kout_init(&o, b, sizeof b);
		kout_s(&o, "RX1 NONCE "); kout_s(&o, nh); kout_c(&o, ' ');
		kout_u(&o, kernel_ticks / TIMER_HZ); kout_c(&o, ' '); kout_s(&o, bh); kout_c(&o, '\n');
		send_raw(dev, src_ip, src_port, b, o.len);
		return 1;
	}

	/* "RX1 CMD <32 hex> <64 hex> <command>" */
	if (strncmp(m, "RX1 CMD ", 8) != 0 || len < 8 + 32 + 1 + 64 + 1 + 1 ||
	    m[40] != ' ' || m[105] != ' ') { send_err(dev, src_ip, src_port, "bad request"); return 1; }
	uint8_t nv[16], mac[32];
	if (hexdec(nv, m + 8, 16) || hexdec(mac, m + 41, 32)) { send_err(dev, src_ip, src_port, "bad hex"); return 1; }
	const char *cmd = m + 106;

	/* spend the nonce first, whatever the MAC says */
	int live = 0;
	uint64_t f = spin_lock_irq(&rx_lock);
	for (int i = 0; i < NNONCE; i++)
		if (rx_nonce[i].until > kernel_ticks && ct_equal(rx_nonce[i].v, nv, 16)) {
			rx_nonce[i].until = 0; live = 1; break;
		}
	spin_unlock_irq(&rx_lock, f);
	if (!live) { st_bad++; send_err(dev, src_ip, src_port, "stale nonce"); return 1; }

	/* "RX1 CMD <nonce> " is m[0..40], then the command */
	const uint8_t *key = 0;
	uint8_t want[32];
	if (rx_key_set) {
		hmac3(rx_key, m, 41, cmd, strlen(cmd), 0, 0, want);
		if (ct_equal(want, mac, 32)) key = rx_key;
	}
	if (!key && have_test) {
		hmac3(rx_tkey, m, 41, cmd, strlen(cmd), 0, 0, want);
		if (ct_equal(want, mac, 32)) key = rx_tkey;
	}
	if (!key) {
		st_bad++; rl_bad++;
		kprintf(KERN_WARN "rescue: bad MAC from %u.%u.%u.%u\n", src_ip >> 24, (src_ip >> 16) & 255,
		        (src_ip >> 8) & 255, src_ip & 255);
		send_err(dev, src_ip, src_port, "bad mac");
		return 1;
	}

	st_cmd++; st_last_cmd_tick = kernel_ticks;
	{ int i = 0; for (; i < 23 && cmd[i]; i++) st_last_cmd[i] = cmd[i]; st_last_cmd[i] = 0; }
	kprintf(KERN_WARN "rescue: '%s' from %u.%u.%u.%u%s\n", st_last_cmd, src_ip >> 24, (src_ip >> 16) & 255,
	        (src_ip >> 8) & 255, src_ip & 255, key == rx_tkey ? " (test key)" : "");
	kout_t o; kout_init(&o, rbuf, sizeof rbuf);
	run_cmd(&o, cmd);
	char nh[33]; hexenc(nh, nv, 16);
	send_body(dev, src_ip, src_port, key, nh, rbuf, o.len);
	return 1;
}

/* ── /dev/rescue ───────────────────────────────────────────────────────────── */
static mutex_t test_mtx = MUTEX_INIT;

static ssize_t rdev_read(vfs_file_t *f, void *buf, size_t len)
{
	static char b[2048];
	kout_t o; kout_init(&o, b, sizeof b);
	kout_s(&o, "port="); kout_u(&o, RESCUE_PORT);
	kout_s(&o, " key="); kout_s(&o, rx_key_set ? "set" : "unset");
	kout_s(&o, " testkey="); kout_s(&o, rx_tkey_until > kernel_ticks ? "set" : "unset");
	kout_s(&o, " hello="); kout_u(&o, st_hello);
	kout_s(&o, " cmd="); kout_u(&o, st_cmd);
	kout_s(&o, " bad="); kout_u(&o, st_bad);
	if (st_cmd) {
		kout_s(&o, " last_cmd_ago_s="); kout_u(&o, (kernel_ticks - st_last_cmd_tick) / TIMER_HZ);
		kout_s(&o, " last_cmd="); for (const char *s = st_last_cmd; *s; s++) kout_c(&o, *s == ' ' ? '_' : *s);
	}
	kout_c(&o, '\n');
	kwatch_report(&o, 0);
	if ((size_t)f->pos >= o.len) return 0;
	size_t n = o.len - (size_t)f->pos;
	if (n > len) n = len;
	if (copy_fop_to_user(buf, b + f->pos, n)) return -14;
	f->pos += (off_t)n;
	return (ssize_t)n;
}

static ssize_t rdev_write(vfs_file_t *f, const void *buf, size_t len)
{
	(void)f;
	if (!capable_task(task_current(), CAP_SYS_ADMIN)) return -1;
	char b[160];
	size_t want = len;                 /* report the whole write, newline included */
	if (len >= sizeof b) return -22;
	if (copy_fop_from_user(b, buf, len)) return -14;
	b[len] = 0;
	while (len && (b[len - 1] == '\n' || b[len - 1] == ' ')) b[--len] = 0;
	uint8_t k[32];
	if (strcmp(b, "off") == 0) {
		memset(rx_key, 0, 32); rx_key_set = 0; rx_tkey_until = 0;
		kprintf(KERN_INFO "rescue: keys cleared, channel off\n");
		return (ssize_t)want;
	}
	int test = strncmp(b, "testkey ", 8) == 0;
	const char *h = test ? b + 8 : (strncmp(b, "key ", 4) == 0 ? b + 4 : 0);
	if (!h || strlen(h) != 64 || hexdec(k, h, 32)) return -22;
	uint64_t fl = spin_lock_irq(&rx_lock);
	if (test) { memcpy(rx_tkey, k, 32); rx_tkey_until = kernel_ticks + 120 * TIMER_HZ; }
	else      { memcpy(rx_key, k, 32); rx_key_set = 1; }
	for (int i = 0; i < NNONCE; i++) rx_nonce[i].until = 0;
	spin_unlock_irq(&rx_lock, fl);
	memset(k, 0, sizeof k);
	kprintf(KERN_INFO "rescue: %s loaded, udp port %d\n", test ? "test key (120 s)" : "key", RESCUE_PORT);
	return (ssize_t)want;
}

static int rdev_ioctl(vfs_file_t *f, uint32_t cmd, uint64_t arg)
{
	(void)f;
	if (!capable_task(task_current(), CAP_SYS_ADMIN)) return -1;
	if (arg < 1 || arg > 60) return -22;
	uint64_t end = kernel_ticks + arg * TIMER_HZ;
	switch (cmd) {
	case RESCUE_IOC_TEST_LOOP:
		/* the shape of a data-driven kernel loop done right */
		for (volatile uint64_t i = 0; kernel_ticks < end; i++)
			if ((i & 1023) == 0 && kloop_should_stop()) return -4;
		return 0;
	case RESCUE_IOC_TEST_HOLD: {
		int r = mutex_lock_killable(&test_mtx);
		if (r) return r;
		while (kernel_ticks < end && !fatal_signal_pending(task_current()))
			task_sleep_until(kernel_ticks + TIMER_HZ / 20);
		mutex_unlock(&test_mtx);
		return 0;
	}
	case RESCUE_IOC_TEST_LOCK: {
		int r = mutex_lock_killable(&test_mtx);
		if (r) return r;
		mutex_unlock(&test_mtx);
		return 0;
	}
	case RESCUE_IOC_TEST_SPIN:
		/* the shape of the 2026-11-08 bug: never checks, never yields */
		while (kernel_ticks < end) cpu_relax();
		return 0;
	}
	return -25;   /* ENOTTY */
}

static vfs_fops_t rescue_fops = { .read = rdev_read, .write = rdev_write, .ioctl = rdev_ioctl };

void rescue_init(void)
{
	uint8_t h[32];
	struct { uint64_t tsc, ticks; int64_t wall; } s = { rdtsc_any(), kernel_ticks, 0 };
	int64_t ns; ranix_walltime(&s.wall, &ns);
	sha256(&s, sizeof s, h);
	memcpy(rx_boot, h, 8);
	devfs_register_device("rescue", S_IFCHR | 0600, 0x0A90, &rescue_fops);
	tick_register(hard_tick);
	proc_create("rescued", rescue_worker);
	kprintf(KERN_INFO "rescue: channel ready on udp %d (off until a key is loaded)\n", RESCUE_PORT);
}
