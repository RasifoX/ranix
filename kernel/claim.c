/* ── Ranix claims: visible, expiring holds on machine resources ───────────────
 * 2026-11-08: several agents shared one laptop through a flock file on another
 * machine. A reboot by one cut another's GPU test; the people at the desk could
 * not see who was doing what. A claim is the kernel's answer: a named hold on a
 * resource ("gpu", "reboot", "display"), with holder, purpose and expiry, that
 *   - root and members of group "claim" (DEVFS_GID_CLAIM) can take, test and
 *     wait for (ioctl on /dev/claim, 0660 root:claim),
 *   - they can see all claims as JSON (read /dev/claim; poll says when it changed), so
 *     the desktop shows "Agent C: GPU test, until 18:30" with a stop button,
 *   - ends by itself: on expiry, when its file closes (holder died), or by STOP,
 *     which also ends the holder's work (SIGTERM, then SIGKILL after 5 s). The
 *     process to stop must be one the taker may signal (signal_may_send), and it
 *     is kept as pid + start_tick, so a reused pid is never hit,
 *   - is honoured where it matters: reboot(2) from anyone but init fails with
 *     EBUSY while somebody else holds "reboot"; fjord-sys asks CLAIM_IOC_CHECK.
 * uAPI: include/uapi/claim.h. Design: arastirma/ajan-insan-ozellikleri.md.
 * ─────────────────────────────────────────────────────────────────────────── */

#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/sync.h>
#include <kernel/hal.h>
#include <kernel/vfs.h>
#include <kernel/kprintf.h>
#include <kernel/board.h>   /* TIMER_HZ */
#include <kernel/kout.h>
#include <kernel/asess.h>
#include <kernel/mm/heap.h>
#include <kernel/fs/devfs.h>
#include <kernel/lib/string.h>
#include <uapi/claim.h>

extern volatile uint64_t kernel_ticks;
extern spinlock_t sched_lock;
extern void ranix_walltime(int64_t *sec, int64_t *nsec);
extern void poll_notify(void);


#define NCLAIM      64
#define PER_UID_MAX 16
#define NKILL       16

struct claim {
	int        used;
	uint64_t   id;
	char       res[CLAIM_RES_MAX];
	char       holder[CLAIM_HOLDER_MAX];
	char       purpose[CLAIM_PURPOSE_MAX];
	uint32_t   flags;
	uid_t      uid;
	pid_t      pid, stop_pid;
	uint64_t   stop_start;     /* start_tick of stop_pid when taken: a reused pid is not it */
	int        guards;         /* taken by root or group claim: may hold back reboot(2) */
	uint64_t   asess;
	char       comm[16];
	int64_t    since;          /* epoch */
	uint64_t   expire_tick;    /* 0 = none */
	vfs_file_t *file;          /* attached: ends when this open file closes */
};

static struct claim tab[NCLAIM];
static spinlock_t   cl_lock = SPINLOCK_INIT;
static volatile uint64_t cl_gen = 1;
static uint64_t     cl_next_id = 1;
static struct { pid_t pid; uint64_t start, at; } kill_later[NKILL];

struct cfile { uint64_t snap_gen; char *snap; size_t snap_len; };

static int64_t wall_now(void) { int64_t s, ns; ranix_walltime(&s, &ns); return s; }

static void cpy(char *d, const char *s, size_t cap)
{
	size_t i = 0;
	for (; i + 1 < cap && s[i]; i++) d[i] = s[i];
	d[i] = 0;
}

static int res_ok(const char *r)
{
	if (!r[0]) return 0;
	for (int i = 0; r[i]; i++) {
		char c = r[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' || c == '-'))
			return 0;
	}
	return 1;
}

/* Caller holds cl_lock. Drops expired claims; returns how many went. */
static int sweep_locked(void)
{
	int n = 0;
	for (int i = 0; i < NCLAIM; i++)
		if (tab[i].used && tab[i].expire_tick && kernel_ticks >= tab[i].expire_tick) {
			kprintf(KERN_INFO "claim: %lu %s (%s) expired\n", (unsigned long)tab[i].id, tab[i].res, tab[i].holder);
			tab[i].used = 0; n++;
		}
	if (n) cl_gen++;
	return n;
}

static void changed(void) { poll_notify(); }

static int conflicts(const struct claim *c, const char *res, uint32_t flags, const char *holder, uid_t uid)
{
	if (!c->used || strcmp(c->res, res) != 0) return 0;
	if (strcmp(c->holder, holder) == 0 && c->uid == uid) return 0;   /* same holder: re-take */
	if ((c->flags & CLAIM_F_SHARED) && (flags & CLAIM_F_SHARED)) return 0;
	return 1;
}

static void fill_busy(struct claim_req *q, const struct claim *c)
{
	cpy(q->busy_holder, c->holder, sizeof q->busy_holder);
	cpy(q->busy_purpose, c->purpose, sizeof q->busy_purpose);
	q->busy_id = c->id;
	q->busy_until = c->expire_tick
	    ? wall_now() + (int64_t)((c->expire_tick - kernel_ticks + TIMER_HZ - 1) / TIMER_HZ) : 0;
}

/* ── report: JSON for /dev/claim, text for the rescue channel ──────────────── */
static void put_claim(kout_t *o, const struct claim *c, int json, int64_t now)
{
	long left = c->expire_tick ? (long)((c->expire_tick > kernel_ticks ? c->expire_tick - kernel_ticks : 0) / TIMER_HZ) : -1;
	if (!json) {
		kout_s(o, "claim "); kout_u(o, c->id); kout_c(o, ' '); kout_s(o, c->res);
		kout_s(o, (c->flags & CLAIM_F_SHARED) ? " shared " : " exclusive ");
		kout_s(o, "holder=\""); kout_s(o, c->holder); kout_s(o, "\" purpose=\""); kout_s(o, c->purpose);
		kout_s(o, "\" pid="); kout_i(o, c->pid); kout_s(o, " left_s="); kout_i(o, left); kout_c(o, '\n');
		return;
	}
	kout_s(o, "{\"id\":"); kout_u(o, c->id);
	kout_s(o, ",\"res\":\""); kout_js(o, c->res, sizeof c->res);
	kout_s(o, "\",\"mode\":\""); kout_s(o, (c->flags & CLAIM_F_SHARED) ? "shared" : "exclusive");
	kout_s(o, "\",\"holder\":\""); kout_js(o, c->holder, sizeof c->holder);
	kout_s(o, "\",\"purpose\":\""); kout_js(o, c->purpose, sizeof c->purpose);
	kout_s(o, "\",\"uid\":"); kout_u(o, c->uid);
	kout_s(o, ",\"pid\":"); kout_i(o, c->pid);
	kout_s(o, ",\"stop_pid\":"); kout_i(o, c->stop_pid);
	kout_s(o, ",\"comm\":\""); kout_js(o, c->comm, sizeof c->comm);
	kout_s(o, "\",\"asess\":"); kout_u(o, c->asess);
	kout_s(o, ",\"since\":"); kout_i(o, c->since);
	kout_s(o, ",\"until\":"); kout_i(o, left >= 0 ? now + left : 0);
	kout_s(o, ",\"left_s\":"); kout_i(o, left);
	kout_s(o, ",\"attached\":"); kout_s(o, c->file ? "true" : "false");
	kout_c(o, '}');
}

/* Copies under the lock, formats outside it. */
int claim_report(kout_t *o, int json)
{
	struct claim *snap = kmalloc(sizeof(struct claim) * NCLAIM);
	if (!snap) { kout_s(o, json ? "{\"error\":\"no memory\"}\n" : "no memory\n"); return 0; }
	uint64_t f = spin_lock_irq(&cl_lock);
	sweep_locked();
	int n = 0;
	for (int i = 0; i < NCLAIM; i++) if (tab[i].used) snap[n++] = tab[i];
	uint64_t gen = cl_gen;
	spin_unlock_irq(&cl_lock, f);
	int64_t now = wall_now();
	if (json) { kout_s(o, "{\"gen\":"); kout_u(o, gen); kout_s(o, ",\"now\":"); kout_i(o, now); kout_s(o, ",\"claims\":["); }
	else if (!n) kout_s(o, "no claims\n");
	for (int i = 0; i < n; i++) {
		if (json && i) kout_c(o, ',');
		put_claim(o, &snap[i], json, now);
	}
	if (json) kout_s(o, "]}\n");
	kfree(snap);
	return n;
}

/* ── reboot(2) gate ────────────────────────────────────────────────────────── */
/* 1 if `caller` is, or descends from, pid a or b. Walks parents with pinned
 * references (proc_get takes sched_lock itself: never call it holding that). */
static int descends(process_t *caller, pid_t a, pid_t b)
{
	process_t *p = caller, *held = NULL;
	int hit = 0;
	for (int depth = 0; p && depth < 64; depth++) {
		pid_t pid = (pid_t)p->pid, tg = p->tgid, pp = p->ppid;
		if ((a && (pid == a || tg == a)) || (b && (pid == b || tg == b))) { hit = 1; break; }
		if (pp <= 0) break;
		process_t *n = proc_get(pp);
		if (held) proc_put(held);
		held = p = n;
	}
	if (held) proc_put(held);
	return hit;
}

/* Called by sys_reboot for any caller but init. 0 = go ahead, -EBUSY = someone
 * else holds "reboot" (logged with who and why). A holder may reboot itself.
 * Only claims taken by root or a member of group claim count. */
int claim_reboot_check(process_t *caller)
{
	struct { pid_t pid, stop; uint64_t id; } c[8]; int n = 0;
	uint64_t f = spin_lock_irq(&cl_lock);
	sweep_locked();
	for (int i = 0; i < NCLAIM && n < 8; i++)
		if (tab[i].used && tab[i].guards && strcmp(tab[i].res, "reboot") == 0) {
			c[n].pid = tab[i].pid; c[n].stop = tab[i].stop_pid; c[n].id = tab[i].id; n++;
		}
	spin_unlock_irq(&cl_lock, f);
	for (int i = 0; i < n; i++) {
		if (descends(caller, c[i].pid, c[i].stop)) continue;
		kprintf(KERN_WARN "claim: reboot by pid %d (%s) refused: claim %lu holds it (see /dev/claim)\n",
		        (int)caller->pid, caller->name, (unsigned long)c[i].id);
		return -16;
	}
	return 0;
}

/* ── ioctls ────────────────────────────────────────────────────────────────── */
/* The process a STOP of this claim ends: the caller, or stop_pid (as the caller's
 * pid namespace numbers it) if the caller may signal it. 0 or -ESRCH / -EPERM. */
static int stop_target(process_t *me, pid_t nr, pid_t *pid, uint64_t *start)
{
	process_t *t = nr > 0 ? proc_get_user(nr) : proc_get((pid_t)me->tgid);
	if (!t) return -3;                                                     /* ESRCH */
	int ok = signal_may_send(me, t, 15);
	*pid = (pid_t)t->tgid;
	*start = t->start_tick;
	proc_put(t);
	return ok ? 0 : -1;                                                    /* EPERM */
}

static int do_take(vfs_file_t *file, struct claim_req *q)
{
	process_t *me = task_current();
	q->res[CLAIM_RES_MAX - 1] = 0; q->holder[CLAIM_HOLDER_MAX - 1] = 0; q->purpose[CLAIM_PURPOSE_MAX - 1] = 0;
	if (!res_ok(q->res)) return -22;
	if (!q->holder[0]) cpy(q->holder, me->name, sizeof q->holder);
	if ((q->flags & CLAIM_F_DETACH) && !q->secs) return -22;   /* a detached claim must end by itself */
	if (q->secs > 7 * 24 * 3600) return -22;
	uid_t uid = me->euid;
	pid_t stop_pid;
	uint64_t stop_start;
	int r = stop_target(me, q->stop_pid, &stop_pid, &stop_start);
	if (r) return r;
	int guards = uid == 0 || task_in_group(me, DEVFS_GID_CLAIM);
	for (;;) {
		uint64_t f = spin_lock_irq(&cl_lock);
		int swept = sweep_locked();
		struct claim *busy = 0, *same = 0, *free_slot = 0;
		int mine = 0;
		for (int i = 0; i < NCLAIM; i++) {
			struct claim *c = &tab[i];
			if (!c->used) { if (!free_slot) free_slot = c; continue; }
			if (c->uid == uid) mine++;
			if (conflicts(c, q->res, q->flags, q->holder, uid)) { if (!busy) busy = c; }
			else if (strcmp(c->res, q->res) == 0 && strcmp(c->holder, q->holder) == 0 && c->uid == uid &&
			         ((c->flags ^ q->flags) & CLAIM_F_SHARED) == 0) same = c;
		}
		if (!busy) {
			struct claim *c = same ? same : free_slot;
			if (!c) { spin_unlock_irq(&cl_lock, f); return -28; }        /* ENOSPC */
			if (!same && mine >= PER_UID_MAX && uid != 0) { spin_unlock_irq(&cl_lock, f); return -28; }
			if (!same) {
				memset(c, 0, sizeof *c);
				c->used = 1; c->id = cl_next_id++;
				cpy(c->res, q->res, sizeof c->res);
				cpy(c->holder, q->holder, sizeof c->holder);
				c->uid = uid; c->since = wall_now();
			}
			/* same holder taking again: refresh purpose, expiry and ownership */
			cpy(c->purpose, q->purpose, sizeof c->purpose);
			c->flags = q->flags & (CLAIM_F_SHARED | CLAIM_F_DETACH);
			c->pid = (pid_t)me->tgid;
			c->stop_pid = stop_pid;
			c->stop_start = stop_start;
			c->guards = guards;
			c->asess = asess_id_of(me);
			cpy(c->comm, me->name, sizeof c->comm);
			c->expire_tick = q->secs ? kernel_ticks + (uint64_t)q->secs * TIMER_HZ : 0;
			c->file = (q->flags & CLAIM_F_DETACH) ? 0 : file;
			q->id = c->id;
			cl_gen++;
			spin_unlock_irq(&cl_lock, f);
			kprintf(KERN_INFO "claim: %lu %s taken by \"%s\" (%s) for %u s\n", (unsigned long)q->id,
			        q->res, q->holder, q->purpose, q->secs);
			changed();
			return 0;
		}
		fill_busy(q, busy);
		spin_unlock_irq(&cl_lock, f);
		if (swept) changed();
		if (!(q->flags & CLAIM_F_WAIT)) return -16;                       /* EBUSY */
		if (task_has_actionable_signal()) return -4;                       /* EINTR */
		task_sleep_until(kernel_ticks + TIMER_HZ / 4);
	}
}

static int may_manage(const struct claim *c)
{
	process_t *me = task_current();
	return me->euid == 0 || me->euid == c->uid;
}

/* The process taken as stop_pid, if it is still that process (not a reused pid). */
static process_t *same_process(pid_t pid, uint64_t start)
{
	process_t *p = pid > 1 ? proc_get(pid) : 0;
	if (p && p->start_tick != start) { proc_put(p); p = 0; }
	return p;
}

/* SIGTERM to the holder's group, each member as the stopper may signal it;
 * SIGKILL 5 s later by claimd if it is still the same process. */
static void stop_holder(pid_t pid, uint64_t start)
{
	process_t *me = task_current(), *p = same_process(pid, start);
	if (!p) {
		kprintf(KERN_INFO "claim: stop target %d is gone, nothing signalled\n", (int)pid);
		return;
	}
	task_kill_many(me, pid, 15);
	if (signal_may_send(me, p, 15)) task_signal_group(p, 15);
	proc_put(p);
	uint64_t f = spin_lock_irq(&cl_lock);
	for (int i = 0; i < NKILL; i++)
		if (!kill_later[i].pid) {
			kill_later[i].pid = pid; kill_later[i].start = start; kill_later[i].at = kernel_ticks + 5 * TIMER_HZ;
			break;
		}
	spin_unlock_irq(&cl_lock, f);
}

static int do_by_id(struct claim_req *q, uint32_t cmd)
{
	uint64_t f = spin_lock_irq(&cl_lock);
	sweep_locked();
	struct claim *c = 0;
	for (int i = 0; i < NCLAIM; i++) if (tab[i].used && tab[i].id == q->id) c = &tab[i];
	if (!c) { spin_unlock_irq(&cl_lock, f); return -2; }                  /* ENOENT */
	if (!may_manage(c)) { spin_unlock_irq(&cl_lock, f); return -1; }      /* EPERM */
	pid_t victim = 0;
	uint64_t victim_start = 0;
	char res[CLAIM_RES_MAX], holder[CLAIM_HOLDER_MAX];
	cpy(res, c->res, sizeof res); cpy(holder, c->holder, sizeof holder);
	if (cmd == CLAIM_IOC_EXTEND) {
		if (!q->secs || q->secs > 7 * 24 * 3600) { spin_unlock_irq(&cl_lock, f); return -22; }
		c->expire_tick = kernel_ticks + (uint64_t)q->secs * TIMER_HZ;
	} else {
		if (cmd == CLAIM_IOC_STOP) { victim = c->stop_pid; victim_start = c->stop_start; }
		c->used = 0;
	}
	cl_gen++;
	spin_unlock_irq(&cl_lock, f);
	process_t *me = task_current();
	kprintf(KERN_INFO "claim: %lu %s (%s) %s by pid %d (%s)\n", (unsigned long)q->id, res, holder,
	        cmd == CLAIM_IOC_EXTEND ? "extended" : cmd == CLAIM_IOC_STOP ? "STOPPED" : "released",
	        (int)me->pid, me->name);
	if (victim && victim != (pid_t)me->tgid) stop_holder(victim, victim_start);
	changed();
	return 0;
}

static int do_check(struct claim_req *q)
{
	q->res[CLAIM_RES_MAX - 1] = 0; q->holder[CLAIM_HOLDER_MAX - 1] = 0;
	uint64_t f = spin_lock_irq(&cl_lock);
	sweep_locked();
	for (int i = 0; i < NCLAIM; i++)
		if (tab[i].used && strcmp(tab[i].res, q->res) == 0 && strcmp(tab[i].holder, q->holder) != 0) {
			fill_busy(q, &tab[i]);
			spin_unlock_irq(&cl_lock, f);
			return -16;
		}
	spin_unlock_irq(&cl_lock, f);
	return 0;
}

static int cdev_ioctl(vfs_file_t *file, uint32_t cmd, uint64_t arg)
{
	if (cmd < CLAIM_IOC_TAKE || cmd > CLAIM_IOC_CHECK) return -25;
	struct claim_req q;
	if (copy_fop_from_user(&q, (void *)arg, sizeof q)) return -14;
	int r;
	switch (cmd) {
	case CLAIM_IOC_TAKE:  r = do_take(file, &q); break;
	case CLAIM_IOC_CHECK: r = do_check(&q); break;
	default:              r = do_by_id(&q, cmd); break;
	}
	if ((r == 0 || r == -16) && copy_fop_to_user((void *)arg, &q, sizeof q)) return -14;
	return r;
}

/* ── file ops ──────────────────────────────────────────────────────────────── */
static int cdev_open(vfs_inode_t *in, vfs_file_t *f, int flags)
{
	(void)in; (void)flags;
	struct cfile *cf = kmalloc(sizeof *cf);
	if (!cf) return -12;
	memset(cf, 0, sizeof *cf);
	f->priv = cf;
	return 0;
}

static int cdev_close(vfs_file_t *f)
{
	int gone = 0;
	uint64_t fl = spin_lock_irq(&cl_lock);
	for (int i = 0; i < NCLAIM; i++)
		if (tab[i].used && tab[i].file == f) { tab[i].used = 0; gone++; }
	if (gone) cl_gen++;
	spin_unlock_irq(&cl_lock, fl);
	if (gone) changed();
	struct cfile *cf = f->priv;
	if (cf) { if (cf->snap) kfree(cf->snap); kfree(cf); f->priv = 0; }
	return 0;
}

#define SNAP_MAX (24 * 1024)

/* A read returns one snapshot to EOF. Once it is consumed, the next read
 * returns a fresh one if the table changed since, else EOF: `cat` ends, a
 * desktop that keeps the fd open polls, then reads the new table. */
static ssize_t cdev_read(vfs_file_t *f, void *buf, size_t len)
{
	struct cfile *cf = f->priv;
	if (!cf) return -9;
	if (!cf->snap || ((size_t)f->pos >= cf->snap_len && cf->snap_gen != cl_gen)) {
		if (!cf->snap) { cf->snap = kmalloc(SNAP_MAX); if (!cf->snap) return -12; }
		kout_t o; kout_init(&o, cf->snap, SNAP_MAX);
		cf->snap_gen = cl_gen;
		claim_report(&o, 1);
		cf->snap_len = o.len;
		f->pos = 0;
	}
	if ((size_t)f->pos >= cf->snap_len) return 0;
	size_t n = cf->snap_len - (size_t)f->pos;
	if (n > len) n = len;
	if (copy_fop_to_user(buf, cf->snap + f->pos, n)) return -14;
	f->pos += (off_t)n;
	return (ssize_t)n;
}

static int cdev_poll(vfs_file_t *f)
{
	struct cfile *cf = f->priv;
	if (!cf || !cf->snap) return 1;
	if ((size_t)f->pos < cf->snap_len) return 1;
	return cf->snap_gen != cl_gen;
}

static vfs_fops_t claim_fops = {
	.open = cdev_open, .close = cdev_close, .read = cdev_read, .poll = cdev_poll, .ioctl = cdev_ioctl,
};

/* Expiry and the delayed SIGKILL of STOP, once a second. */
static void claim_thread(void)
{
	for (;;) {
		task_sleep_until(kernel_ticks + TIMER_HZ);
		pid_t kill_now[NKILL]; uint64_t kill_start[NKILL]; int nk = 0;
		uint64_t f = spin_lock_irq(&cl_lock);
		int swept = sweep_locked();
		for (int i = 0; i < NKILL; i++)
			if (kill_later[i].pid && kernel_ticks >= kill_later[i].at) {
				kill_now[nk] = kill_later[i].pid; kill_start[nk++] = kill_later[i].start; kill_later[i].pid = 0;
			}
		spin_unlock_irq(&cl_lock, f);
		for (int i = 0; i < nk; i++) {                 /* allowed at STOP; still the same process? */
			process_t *p = same_process(kill_now[i], kill_start[i]);
			if (!p) continue;
			task_signal_pgrp(kill_now[i], 9);
			task_signal_group(p, 9);
			proc_put(p);
		}
		if (swept) changed();
	}
}

void claim_init(void)
{
	devfs_register_device_gid("claim", S_IFCHR | 0660, DEVFS_GID_CLAIM, 0x0A91, &claim_fops);
	proc_create("claimd", claim_thread);
}
