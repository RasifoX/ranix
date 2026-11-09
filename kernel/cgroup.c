/*
 * cgroups v2 — pids controller (fork-bomb protection).
 *
 * A cgroup hierarchy mounted at /sys/fs/cgroup. Each cgroup has a pids.max
 * limit applied to its subtree (self + descendants); pids.current tracks the
 * subtree process count. A fork is rejected (EAGAIN) if it would push any
 * ancestor over its limit. Processes join a cgroup by writing their pid to
 * cgroup.procs; mkdir creates a child cgroup.
 *
 * This is additive: the only hooks into the core are cgroup_can_fork /
 * cgroup_charge_fork / cgroup_exit, called from the process lifecycle.
 *
 * A cgroup made through cgroupfs lives for good. One made for a session goes when
 * its last holder does: the session, a task, an address space or a file charged
 * to it. Its struct is then parked for the next session rather than freed, since
 * an open cgroupfs file may still point at it; gen tells that file it is stale.
 *
 * Memory is charged when a page is allocated and uncharged when it is freed, not
 * when a space maps or unmaps it. So a fork adds nothing but its page tables, and
 * a page shared copy-on-write stays charged once, however many spaces come and go.
 */
#include <kernel/vfs.h>
#include <kernel/proc.h>
#include <kernel/cgroup.h>
#include <kernel/capabilities.h>
#include <kernel/pid_namespace.h>
#include <kernel/hal.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/mmu.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>
#include <kernel/nsproxy.h>
#include <kernel/errno.h>


typedef struct cgroup {
	char            name[64];
	struct cgroup  *parent;
	struct cgroup  *children;   /* first child */
	struct cgroup  *sibling;    /* next sibling */
	long            pids_max;   /* -1 = unlimited */
	long            nr_procs;   /* processes in this subtree */
	long            mem_max;    /* memory.max bytes, -1 = unlimited */
	long            mem_cur;    /* memory.current bytes in this subtree */
	int             frozen;     /* cgroup.freeze: 1 = this subtree is frozen */
	int             cpu_weight; /* cpu.weight: 1..10000, default 100 */
	uint32_t        refs;       /* holders; a child, a task, a space, a file count one each */
	uint32_t        gen;        /* bumped when the struct is parked */
} cgroup_t;

static cgroup_t cgroup_root = { .name = "", .pids_max = -1, .nr_procs = 0, .mem_max = -1,
                                .cpu_weight = 100, .refs = 1 };
static cgroup_t *cg_parked;   /* released structs, linked through ->sibling */
static spinlock_t cg_lock = SPINLOCK_INIT;

static void cg_mem_add(cgroup_t *cg, long n)
{
	for (cgroup_t *c = cg; c; c = c->parent) {
		c->mem_cur += n;
		if (c->mem_cur < 0) c->mem_cur = 0;
	}
}

static void cg_put_locked(cgroup_t *cg)
{
	while (cg && --cg->refs == 0) {
		cgroup_t **pp = &cg->parent->children;
		while (*pp != cg) pp = &(*pp)->sibling;
		*pp = cg->sibling;
		cgroup_t *up = cg->parent;
		/* pages of a file mapped elsewhere when it went: their cgroup is gone */
		cg_mem_add(up, -cg->mem_cur);
		cg->gen++;
		cg->sibling = cg_parked;
		cg_parked = cg;
		cg = up;
	}
}

void cgroup_put(void *cgp)
{
	uint64_t f = spin_lock_irq(&cg_lock);
	cg_put_locked(cgp);
	spin_unlock_irq(&cg_lock, f);
}

static cgroup_t *cg_alloc(cgroup_t *parent, const char *name, long mem_max, long pids_max)
{
	uint64_t f = spin_lock_irq(&cg_lock);
	cgroup_t *cg = cg_parked;
	if (cg) cg_parked = cg->sibling;
	spin_unlock_irq(&cg_lock, f);
	uint32_t gen = cg ? cg->gen : 0;
	if (!cg) cg = kmalloc(sizeof(*cg));
	if (!cg) return NULL;
	memset(cg, 0, sizeof(*cg));
	int l = 0; while (name[l] && l < 63) { cg->name[l] = name[l]; l++; }
	cg->name[l] = '\0';
	cg->pids_max   = pids_max;
	cg->mem_max    = mem_max;
	cg->cpu_weight = 100;
	cg->parent     = parent;
	cg->refs       = 1;
	cg->gen        = gen;
	f = spin_lock_irq(&cg_lock);
	parent->refs++;
	cg->sibling = parent->children;
	parent->children = cg;
	spin_unlock_irq(&cg_lock, f);
	return cg;
}

/* /cgroup/asess/<name>. The parent's limit is what all sessions together may
 * hold, whatever each was promised, and it is set once, by the first session. */
/* All sessions together may use what is free now, on top of what they hold, less
 * what the host keeps: what the host itself has in use is not there to promise. */
void *cgroup_session_create(const char *name, long mem_max, long pids_max, long avail, long reserve)
{
	static cgroup_t *all;
	uint64_t f = spin_lock_irq(&cg_lock);
	cgroup_t *parent = all;
	spin_unlock_irq(&cg_lock, f);
	if (!parent) {
		parent = cg_alloc(&cgroup_root, "asess", 0, -1);
		if (!parent) return NULL;
		f = spin_lock_irq(&cg_lock);
		cgroup_t *lost = all ? parent : NULL;
		if (!all) all = parent;
		parent = all;
		if (lost) cg_put_locked(lost);
		spin_unlock_irq(&cg_lock, f);
	}
	f = spin_lock_irq(&cg_lock);
	long cap = avail + parent->mem_cur - reserve;
	parent->mem_max = cap > 0 ? cap : 0;
	spin_unlock_irq(&cg_lock, f);
	return cg_alloc(parent, name, mem_max, pids_max);
}

void cgroup_mem_usage(void *cgp, long *cur, long *max)
{
	uint64_t f = spin_lock_irq(&cg_lock);
	*cur = ((cgroup_t *)cgp)->mem_cur;
	*max = ((cgroup_t *)cgp)->mem_max;
	spin_unlock_irq(&cg_lock, f);
}

static int cg_over(const cgroup_t *cg, long n)
{
	for (const cgroup_t *c = cg; c; c = c->parent)
		if (c->mem_max >= 0 && c->mem_cur + n > c->mem_max) return 1;
	return 0;
}

static void cg_vm_add(vm_space_t *vm, cgroup_t *cg, long n)
{
	cg_mem_add(cg, n);
	if (!vm->mem_cg) cg->refs++;
	vm->mem_cg = cg;
	vm->mem_charged += n;
}

/* memory controller: charge n bytes to the space's cgroup and its ancestors; fail
 * if over memory.max */
int cgroup_charge_mem(vm_space_t *vm, void *cgp, long n)
{
	uint64_t f = spin_lock_irq(&cg_lock);
	cgroup_t *cg = vm->mem_cg ? vm->mem_cg : cgp ? (cgroup_t *)cgp : &cgroup_root;
	if (cg_over(cg, n)) {
		spin_unlock_irq(&cg_lock, f);
		return -1;
	}
	cg_vm_add(vm, cg, n);
	spin_unlock_irq(&cg_lock, f);
	return 0;
}

/* For a page table, which vm_map cannot refuse in time. It may overshoot the limit
 * by a few pages; the next user page then fails. */
void cgroup_charge_table(vm_space_t *vm)
{
	if (vm == kernel_vm_space) return;
	process_t *p = task_current();
	uint64_t f = spin_lock_irq(&cg_lock);
	cgroup_t *cg = vm->mem_cg ? vm->mem_cg : p && p->cgroup ? (cgroup_t *)p->cgroup : &cgroup_root;
	cg_vm_add(vm, cg, PAGE_SIZE);
	vm->pt_charged += PAGE_SIZE;
	spin_unlock_irq(&cg_lock, f);
}

/* Files leave the top of the budget to the tasks: a write can fail with ENOSPC and
 * be handled, but a task out of pages dies on its next copy-on-write. */
#define FILE_HEADROOM (1L << 20)

static int cg_over_file(cgroup_t *cg, long n)
{
	for (cgroup_t *c = cg; c; c = c->parent) {
		if (c->mem_max < 0) continue;
		long room = c->mem_max / 16 < FILE_HEADROOM ? c->mem_max / 16 : FILE_HEADROOM;
		if (n > 0 && c->mem_cur + n + room > c->mem_max) return 1;
	}
	return 0;
}

/* File data on tmpfs and ramfs, memfd and SysV shm is paid for by whoever last
 * grew it, whole, for as long as it lasts and wherever it lives. So a session
 * cannot grow a host file for free, and a file it leaves behind stops holding its
 * cgroup once the host grows it. *owner starts NULL and have is what it holds. */
int cgroup_charge_file(void **owner, long have, long n)
{
	process_t *p = task_current();
	uint64_t f = spin_lock_irq(&cg_lock);
	cgroup_t *cg = p && p->cgroup ? (cgroup_t *)p->cgroup : &cgroup_root;
	cgroup_t *old = *owner;
	long want = old == cg ? n : have + n;
	if (cg_over(cg, want) || cg_over_file(cg, want)) {
		spin_unlock_irq(&cg_lock, f);
		return -1;
	}
	cg_mem_add(cg, want);
	if (old != cg) {
		cg->refs++;
		*owner = cg;
		if (old) {
			cg_mem_add(old, -have);
			cg_put_locked(old);
		}
	}
	spin_unlock_irq(&cg_lock, f);
	return 0;
}

void *cgroup_current(void)
{
	process_t *p = task_current();
	return p && p->cgroup ? p->cgroup : &cgroup_root;
}

/* As cgroup_charge_file for a charge with a known payer, from any context.
 * cgroup_file_release ends it. With force it may overshoot: the caller bounds it. */
int cgroup_charge_to(void *owner, long n, int force)
{
	cgroup_t *cg = owner;
	if (!cg) return 0;
	uint64_t f = spin_lock_irq(&cg_lock);
	if (!force && (cg_over(cg, n) || cg_over_file(cg, n))) {
		spin_unlock_irq(&cg_lock, f);
		return -1;
	}
	cg_mem_add(cg, n);
	cg->refs++;
	spin_unlock_irq(&cg_lock, f);
	return 0;
}

void cgroup_file_uncharge(void *owner, long n)
{
	if (!owner) return;
	uint64_t f = spin_lock_irq(&cg_lock);
	cg_mem_add(owner, -n);
	spin_unlock_irq(&cg_lock, f);
}

void cgroup_file_release(void *owner, long n)
{
	if (!owner) return;
	uint64_t f = spin_lock_irq(&cg_lock);
	cg_mem_add(owner, -n);
	cg_put_locked(owner);
	spin_unlock_irq(&cg_lock, f);
}

/* 1 if cgp is the same cgroup as ancestorp or a descendant of it. Lockless
 * parent walk — used by the OOM killer to scope its victim search to a subtree. */
int cgroup_is_in_subtree(void *cgp, void *ancestorp)
{
	cgroup_t *anc = ancestorp ? (cgroup_t *)ancestorp : &cgroup_root;
	for (cgroup_t *c = cgp ? (cgroup_t *)cgp : &cgroup_root; c; c = c->parent)
		if (c == anc) return 1;
	return 0;
}

void cgroup_uncharge_mem(vm_space_t *vm, long n)
{
	uint64_t f = spin_lock_irq(&cg_lock);
	if (vm->mem_cg) cg_mem_add(vm->mem_cg, -n);
	vm->mem_charged = vm->mem_charged > n ? vm->mem_charged - n : 0;
	spin_unlock_irq(&cg_lock, f);
}

/* The space is gone, its pages uncharged as they went: what is left is its tables. */
void cgroup_vm_release(vm_space_t *vm)
{
	cgroup_uncharge_mem(vm, vm->pt_charged);
	if (vm->mem_cg) cgroup_put(vm->mem_cg);
	vm->mem_cg = NULL;
}

/* cpu.weight of a process's own cgroup (1..10000; default 100 = baseline) */
int cgroup_cpu_weight(void *cgp)
{
	cgroup_t *cg = cgp ? (cgroup_t *)cgp : &cgroup_root;
	return cg->cpu_weight ? cg->cpu_weight : 100;
}

/* a process is frozen if its cgroup or any ancestor has cgroup.freeze set */
int cgroup_frozen(void *cgp)
{
	for (cgroup_t *c = cgp ? (cgroup_t *)cgp : &cgroup_root; c; c = c->parent)
		if (c->frozen) return 1;
	return 0;
}

/* ── cgroup namespace ──────────────────────────────────────── */

struct cgroup_namespace {
	int           refcount;
	unsigned long inum;
	cgroup_t     *root;     /* the cgroup that appears as "/" in this ns */
};

struct cgroup_namespace init_cgroup_ns = { .refcount = 1, .inum = 0xF0000008UL, .root = &cgroup_root };
static spinlock_t cgns_lock = SPINLOCK_INIT;

struct cgroup_namespace *current_cgroup_ns(void)
{
	process_t *p = task_current();
	return (p && p->cgroup_ns) ? p->cgroup_ns : &init_cgroup_ns;
}

struct cgroup_namespace *cgroup_ns_get(struct cgroup_namespace *ns)
{
	if (!ns) ns = &init_cgroup_ns;
	if (ns != &init_cgroup_ns) {
		uint64_t f = spin_lock_irq(&cgns_lock);
		ns->refcount++;
		spin_unlock_irq(&cgns_lock, f);
	}
	return ns;
}

void cgroup_ns_put(struct cgroup_namespace *ns)
{
	if (!ns || ns == &init_cgroup_ns) return;
	uint64_t f = spin_lock_irq(&cgns_lock);
	int last = (--ns->refcount == 0);
	spin_unlock_irq(&cgns_lock, f);
	if (last) kfree(ns);
}

struct cgroup_namespace *cgroup_ns_create(void)
{
	process_t *p = task_current();
	struct cgroup_namespace *ns = kmalloc(sizeof(*ns));
	if (!ns) return NULL;
	ns->refcount = 1;
	ns->inum     = ns_inum_alloc();
	ns->root     = p && p->cgroup ? (cgroup_t *)p->cgroup : &cgroup_root;
	return ns;
}

unsigned long cgroup_ns_inum(struct cgroup_namespace *ns)
{
	return (ns ? ns : &init_cgroup_ns)->inum;
}

/* /proc/<pid>/cgroup path of p's cgroup, relative to the caller's cgroup ns
 * root. Format component is just the path; caller adds the "0::" prefix. */
int cgroup_proc_path(process_t *p, char *buf, int sz)
{
	cgroup_t *cg     = p && p->cgroup ? (cgroup_t *)p->cgroup : &cgroup_root;
	cgroup_t *nsroot = current_cgroup_ns()->root;
	/* collect names from cg up to (not including) nsroot */
	cgroup_t *chain[16]; int n = 0;
	for (cgroup_t *c = cg; c && c != nsroot && n < 16; c = c->parent) chain[n++] = c;
	int pos = 0;
	if (n == 0) { if (pos < sz - 1) buf[pos++] = '/'; }  /* at the ns root */
	for (int i = n - 1; i >= 0; i--) {
		if (pos < sz - 1) buf[pos++] = '/';
		for (int k = 0; chain[i]->name[k] && pos < sz - 1; k++) buf[pos++] = chain[i]->name[k];
	}
	buf[pos] = '\0';
	return pos;
}

/* ── charge / uncharge (subtree counts, limit check) ─────────── */

/* Reserve one pid in cg's subtree; fails if any ancestor is at its limit. */
static int cg_charge(cgroup_t *cg)
{
	uint64_t f = spin_lock_irq(&cg_lock);
	for (cgroup_t *c = cg; c; c = c->parent)
		if (c->pids_max >= 0 && c->nr_procs + 1 > c->pids_max) {
			spin_unlock_irq(&cg_lock, f);
			return -1;
		}
	for (cgroup_t *c = cg; c; c = c->parent) c->nr_procs++;
	cg->refs++;
	spin_unlock_irq(&cg_lock, f);
	return 0;
}

static void cg_uncharge(cgroup_t *cg)
{
	uint64_t f = spin_lock_irq(&cg_lock);
	for (cgroup_t *c = cg; c; c = c->parent)
		if (c->nr_procs > 0) c->nr_procs--;
	cg_put_locked(cg);
	spin_unlock_irq(&cg_lock, f);
}

/* ── process lifecycle hooks (called from kernel/proc.c) ─────── */

void *cgroup_root_get(void) { return &cgroup_root; }

int cgroup_can_fork(void *cgp)
{
	return cg_charge(cgp ? (cgroup_t *)cgp : &cgroup_root);
}

void cgroup_fork_attach(struct process *child, void *cgp)
{
	child->cgroup = cgp ? cgp : &cgroup_root;
	/* the pid was already charged in cgroup_can_fork() */
}

void cgroup_cancel_fork(void *cgp)
{
	cg_uncharge(cgp ? (cgroup_t *)cgp : &cgroup_root);
}

void cgroup_exit(struct process *p)
{
	if (p->cgroup) { cg_uncharge((cgroup_t *)p->cgroup); p->cgroup = NULL; }
}

static void cg_add_procs(cgroup_t *cg, long n)
{
	for (cgroup_t *c = cg; c; c = c->parent) c->nr_procs += n;
}

/* Move n tasks and the charges of their address space. Their count leaves first,
 * so an ancestor of both cgroups is not counted twice against its limit. */
int cgroup_migrate(void *fromp, void *top, long n, vm_space_t *vm)
{
	cgroup_t *from = fromp, *to = top;
	uint64_t f = spin_lock_irq(&cg_lock);
	cg_add_procs(from, -n);
	for (cgroup_t *c = to; c; c = c->parent)
		if (c->pids_max >= 0 && c->nr_procs + n > c->pids_max) {
			cg_add_procs(from, n);
			spin_unlock_irq(&cg_lock, f);
			return -1;
		}
	cg_add_procs(to, n);
	to->refs += (uint32_t)n;
	for (long i = 0; i < n; i++) cg_put_locked(from);
	if (vm) {
		if (vm->mem_charged) {
			for (cgroup_t *c = vm->mem_cg; c; c = c->parent) c->mem_cur -= vm->mem_charged;
			for (cgroup_t *c = to; c; c = c->parent) c->mem_cur += vm->mem_charged;
		}
		to->refs++;
		if (vm->mem_cg) cg_put_locked(vm->mem_cg);
		vm->mem_cg = to;
	}
	spin_unlock_irq(&cg_lock, f);
	return 0;
}

/* ── cgroupfs ────────────────────────────────────────────────── */

enum { CG_PROCS = 0, CG_PIDS_MAX, CG_PIDS_CURRENT, CG_CONTROLLERS, CG_MEM_MAX, CG_MEM_CUR,
       CG_FREEZE, CG_CPU_WEIGHT, CG_NFILES };
static const char *cg_files[] = { "cgroup.procs", "pids.max", "pids.current", "cgroup.controllers",
                                  "memory.max", "memory.current", "cgroup.freeze", "cpu.weight" };

typedef struct { cgroup_t *cg; uint32_t gen; int file; } cg_priv_t;   /* file=-1 → directory */

static vfs_inode_t *cg_dir_inode(cgroup_t *cg);

/* append a decimal long to buf at *pos */
static int cg_putlong(char *b, long v)
{
	int i = 0;
	if (v < 0) { b[i++] = '-'; v = -v; }
	char rev[24]; int r = 0;
	if (v == 0) rev[r++] = '0';
	while (v) { rev[r++] = '0' + (int)(v % 10); v /= 10; }
	while (r) b[i++] = rev[--r];
	return i;
}

static ssize_t cg_file_read(vfs_file_t *f, void *buf, size_t len)
{
	cg_priv_t *pr = f->inode->priv;
	char tmp[96]; int n = 0;
	if (pr->gen != pr->cg->gen) return -ENODEV;
	switch (pr->file) {
	case CG_PIDS_MAX:
		if (pr->cg->pids_max < 0) { tmp[n++]='m';tmp[n++]='a';tmp[n++]='x'; }
		else n += cg_putlong(tmp, pr->cg->pids_max);
		tmp[n++] = '\n';
		break;
	case CG_PIDS_CURRENT:
		n = cg_putlong(tmp, pr->cg->nr_procs); tmp[n++] = '\n';
		break;
	case CG_MEM_MAX:
		if (pr->cg->mem_max < 0) { tmp[n++]='m';tmp[n++]='a';tmp[n++]='x'; }
		else n += cg_putlong(tmp, pr->cg->mem_max);
		tmp[n++] = '\n';
		break;
	case CG_MEM_CUR:
		n = cg_putlong(tmp, pr->cg->mem_cur); tmp[n++] = '\n';
		break;
	case CG_FREEZE:
		tmp[n++] = pr->cg->frozen ? '1' : '0'; tmp[n++] = '\n';
		break;
	case CG_CPU_WEIGHT:
		n = cg_putlong(tmp, pr->cg->cpu_weight ? pr->cg->cpu_weight : 100); tmp[n++] = '\n';
		break;
	case CG_CONTROLLERS:
		for (const char *s = "cpu memory pids\n"; *s; s++) tmp[n++] = *s;
		break;
	default: /* cgroup.procs: pids whose cgroup is exactly this one */
		for (int i = 0; i < task_count(); i++) {
			process_t *p = task_get(i);
			if (p && (pid_t)p->pid == (pid_t)p->tgid && (cgroup_t *)p->cgroup == pr->cg) {
				int nr = task_pid_nr_ns(p, current_pid_ns());
				if (nr) { n += cg_putlong(tmp + n, nr); tmp[n++]='\n'; }
			}
			if (n > (int)sizeof(tmp) - 24) break;
		}
		break;
	}
	if (f->pos >= (off_t)n) return 0;
	size_t avail = (size_t)(n - f->pos);
	if (len > avail) len = avail;
	if (copy_to_user(buf, tmp + f->pos, len) != 0) return -EFAULT;
	f->pos += (off_t)len;
	return (ssize_t)len;
}

static ssize_t cg_file_write(vfs_file_t *f, const void *buf, size_t len)
{
	cg_priv_t *pr = f->inode->priv;
	char kb[64];
	size_t n = len < sizeof(kb) - 1 ? len : sizeof(kb) - 1;
	if (pr->gen != pr->cg->gen) return -ENODEV;
	if (copy_from_user(kb, buf, n) != 0) return -EFAULT;
	kb[n] = '\0';

	if (pr->file == CG_PIDS_MAX) {
		if (kb[0] == 'm') { pr->cg->pids_max = -1; }           /* "max" */
		else { long v = 0; for (int i = 0; kb[i] >= '0' && kb[i] <= '9'; i++) v = v*10 + (kb[i]-'0'); pr->cg->pids_max = v; }
		return (ssize_t)len;
	}
	if (pr->file == CG_MEM_MAX) {
		if (kb[0] == 'm') { pr->cg->mem_max = -1; }             /* "max" */
		else { long v = 0; for (int i = 0; kb[i] >= '0' && kb[i] <= '9'; i++) v = v*10 + (kb[i]-'0'); pr->cg->mem_max = v; }
		return (ssize_t)len;
	}
	if (pr->file == CG_FREEZE) {
		pr->cg->frozen = (kb[0] == '1');
		if (!pr->cg->frozen) proc_wake_frozen();
		return (ssize_t)len;
	}
	if (pr->file == CG_CPU_WEIGHT) {
		long v = 0; for (int i = 0; kb[i] >= '0' && kb[i] <= '9'; i++) v = v*10 + (kb[i]-'0');
		if (v < 1) v = 1; if (v > 10000) v = 10000;
		pr->cg->cpu_weight = (int)v;
		return (ssize_t)len;
	}
	if (pr->file == CG_PROCS) {
		long pid = 0; for (int i = 0; kb[i] >= '0' && kb[i] <= '9'; i++) pid = pid*10 + (kb[i]-'0');
		process_t *t = pid ? proc_get_user((pid_t)pid) : task_current();
		if (!t) return -ESRCH;
		if (!pid) proc_hold(t);
		ssize_t ret = (ssize_t)len;
		if (!task_may_control(t, CAP_SYS_ADMIN))
			ret = -13; /* EACCES */
		else {
			int r = proc_move_cgroup(t, pr->cg);
			if (r == -1) ret = -11; /* EAGAIN: target subtree full */
			else if (r)  ret = r;
		}
		proc_put(t);
		return ret;
	}
	return -EINVAL;   /* read-only file */
}

static vfs_fops_t cg_file_fops = { .read = cg_file_read, .write = cg_file_write };

static vfs_inode_t *cg_file_inode(cgroup_t *cg, int file)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->mode     = S_IFREG | (file == CG_PIDS_CURRENT || file == CG_CONTROLLERS ? 0444 : 0644);
	in->nlink    = 1;
	in->refcount = 1;
	in->fops     = &cg_file_fops;
	cg_priv_t *pr = kmalloc(sizeof(*pr));
	if (!pr) { kfree(in); return NULL; }
	pr->cg = cg; pr->gen = cg->gen; pr->file = file;
	in->priv = pr;
	return in;
}

static vfs_inode_t *cg_lookup(vfs_inode_t *dir, const char *name)
{
	cg_priv_t *dp = dir->priv;
	cgroup_t *cg = dp->cg;
	if (dp->gen != cg->gen) return NULL;
	for (int i = 0; i < CG_NFILES; i++)
		if (strcmp(name, cg_files[i]) == 0) return cg_file_inode(cg, i);
	uint64_t f = spin_lock_irq(&cg_lock);
	for (cgroup_t *c = cg->children; c; c = c->sibling)
		if (strcmp(c->name, name) == 0) { spin_unlock_irq(&cg_lock, f); return cg_dir_inode(c); }
	spin_unlock_irq(&cg_lock, f);
	return NULL;
}

static int cg_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	cg_priv_t *dp = dir->priv;
	if (dp->gen != dp->cg->gen) return -1;
	if (idx < CG_NFILES) {
		const char *nm = cg_files[idx];
		int l = 0; while (nm[l]) l++;
		out->ino = 0; out->mode = S_IFREG | 0644; out->next_off = idx + 1;
		memcpy(out->name, nm, l); out->name[l] = '\0';
		return 0;
	}
	uint32_t k = CG_NFILES;
	uint64_t f = spin_lock_irq(&cg_lock);
	for (cgroup_t *c = dp->cg->children; c; c = c->sibling, k++) {
		if (k == idx) {
			int l = 0; while (c->name[l]) l++;
			out->ino = 0; out->mode = S_IFDIR | 0555; out->next_off = idx + 1;
			memcpy(out->name, c->name, l); out->name[l] = '\0';
			spin_unlock_irq(&cg_lock, f);
			return 0;
		}
	}
	spin_unlock_irq(&cg_lock, f);
	return -1;
}

static int cg_mkdir(vfs_inode_t *dir, const char *name, mode_t mode)
{
	(void)mode;
	cg_priv_t *dp = dir->priv;
	if (dp->gen != dp->cg->gen) return -ENOENT;
	return cg_alloc(dp->cg, name, -1, -1) ? 0 : -12;
}

static vfs_iops_t cg_dir_iops = { .lookup = cg_lookup, .readdir = cg_readdir, .mkdir = cg_mkdir };
static vfs_fops_t cg_dir_fops = { 0 };

static vfs_inode_t *cg_dir_inode(cgroup_t *cg)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->mode     = S_IFDIR | 0555;
	in->nlink    = 2;
	in->refcount = 1;
	in->iops     = &cg_dir_iops;
	in->fops     = &cg_dir_fops;
	in->no_cache = 1;
	cg_priv_t *pr = kmalloc(sizeof(*pr));
	if (!pr) { kfree(in); return NULL; }
	pr->cg = cg; pr->gen = cg->gen; pr->file = -1;
	in->priv = pr;
	return in;
}

int cgroup_mount(vfs_mount_t *mnt, void *data)
{
	(void)data;
	vfs_inode_t *root = cg_dir_inode(&cgroup_root);
	if (!root) return -1;
	root->ino = 1;
	vfs_dentry_t *rd = vfs_dentry_create(NULL, "/", root);
	if (!rd) { kfree(root); return -1; }
	mnt->root = rd;
	return 0;
}

void cgroup_register(void)
{
	vfs_register_fs("cgroup2", cgroup_mount);
}
