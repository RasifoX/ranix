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
 */
#include <kernel/vfs.h>
#include <kernel/proc.h>
#include <kernel/cgroup.h>
#include <kernel/hal.h>
#include <kernel/mm/heap.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>

extern unsigned long ns_inum_alloc(void);

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
} cgroup_t;

static cgroup_t cgroup_root = { .name = "", .pids_max = -1, .nr_procs = 0, .mem_max = -1 };
static spinlock_t cg_lock = SPINLOCK_INIT;

/* memory controller: charge n bytes to cg's subtree; fail if over memory.max */
int cgroup_charge_mem(void *cgp, long n)
{
	cgroup_t *cg = cgp ? (cgroup_t *)cgp : &cgroup_root;
	uint64_t f = spin_lock_irq(&cg_lock);
	for (cgroup_t *c = cg; c; c = c->parent)
		if (c->mem_max >= 0 && c->mem_cur + n > c->mem_max) {
			spin_unlock_irq(&cg_lock, f);
			return -1;
		}
	for (cgroup_t *c = cg; c; c = c->parent) c->mem_cur += n;
	spin_unlock_irq(&cg_lock, f);
	return 0;
}

void cgroup_uncharge_mem(void *cgp, long n)
{
	cgroup_t *cg = cgp ? (cgroup_t *)cgp : &cgroup_root;
	uint64_t f = spin_lock_irq(&cg_lock);
	for (cgroup_t *c = cg; c; c = c->parent) {
		c->mem_cur -= n;
		if (c->mem_cur < 0) c->mem_cur = 0;
	}
	spin_unlock_irq(&cg_lock, f);
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
	spin_unlock_irq(&cg_lock, f);
	return 0;
}

static void cg_uncharge(cgroup_t *cg)
{
	uint64_t f = spin_lock_irq(&cg_lock);
	for (cgroup_t *c = cg; c; c = c->parent)
		if (c->nr_procs > 0) c->nr_procs--;
	spin_unlock_irq(&cg_lock, f);
}

/* ── process lifecycle hooks (called from kernel/proc.c) ─────── */

void *cgroup_root_get(void) { return &cgroup_root; }

int cgroup_can_fork(struct process *parent)
{
	cgroup_t *cg = parent->cgroup ? parent->cgroup : &cgroup_root;
	return cg_charge(cg);   /* charges on success */
}

void cgroup_fork_attach(struct process *child, struct process *parent)
{
	child->cgroup = parent->cgroup ? parent->cgroup : &cgroup_root;
	/* the pid was already charged in cgroup_can_fork() */
}

void cgroup_cancel_fork(struct process *parent)
{
	cgroup_t *cg = parent->cgroup ? parent->cgroup : &cgroup_root;
	cg_uncharge(cg);
}

void cgroup_exit(struct process *p)
{
	if (p->cgroup) { cg_uncharge((cgroup_t *)p->cgroup); p->cgroup = NULL; }
}

/* ── cgroupfs ────────────────────────────────────────────────── */

enum { CG_PROCS = 0, CG_PIDS_MAX, CG_PIDS_CURRENT, CG_CONTROLLERS, CG_MEM_MAX, CG_MEM_CUR, CG_FREEZE, CG_NFILES };
static const char *cg_files[] = { "cgroup.procs", "pids.max", "pids.current", "cgroup.controllers",
                                  "memory.max", "memory.current", "cgroup.freeze" };

typedef struct { cgroup_t *cg; int file; } cg_priv_t;   /* file=-1 → directory */

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
	case CG_CONTROLLERS:
		for (const char *s = "pids memory\n"; *s; s++) tmp[n++] = *s;
		break;
	default: /* cgroup.procs: pids whose cgroup is exactly this one */
		for (int i = 0; i < task_count(); i++) {
			process_t *p = task_get(i);
			if (p && (cgroup_t *)p->cgroup == pr->cg) { n += cg_putlong(tmp + n, (long)p->pid); tmp[n++]='\n'; }
			if (n > (int)sizeof(tmp) - 24) break;
		}
		break;
	}
	if (f->pos >= (off_t)n) return 0;
	size_t avail = (size_t)(n - f->pos);
	if (len > avail) len = avail;
	if (copy_to_user(buf, tmp + f->pos, len) != 0) return -14;
	f->pos += (off_t)len;
	return (ssize_t)len;
}

static ssize_t cg_file_write(vfs_file_t *f, const void *buf, size_t len)
{
	cg_priv_t *pr = f->inode->priv;
	char kb[64];
	size_t n = len < sizeof(kb) - 1 ? len : sizeof(kb) - 1;
	if (copy_from_user(kb, buf, n) != 0) return -14;
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
		return (ssize_t)len;
	}
	if (pr->file == CG_PROCS) {
		long pid = 0; for (int i = 0; kb[i] >= '0' && kb[i] <= '9'; i++) pid = pid*10 + (kb[i]-'0');
		process_t *t = proc_find((pid_t)pid);
		if (!t) return -3; /* ESRCH */
		cgroup_t *old = t->cgroup ? t->cgroup : &cgroup_root;
		if (old == pr->cg) return (ssize_t)len;
		if (cg_charge(pr->cg) != 0) return -11; /* EAGAIN: target subtree full */
		cg_uncharge(old);
		t->cgroup = pr->cg;
		return (ssize_t)len;
	}
	return -22; /* EINVAL: read-only file */
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
	pr->cg = cg; pr->file = file;
	in->priv = pr;
	return in;
}

static vfs_inode_t *cg_lookup(vfs_inode_t *dir, const char *name)
{
	cg_priv_t *dp = dir->priv;
	cgroup_t *cg = dp->cg;
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
	cgroup_t *cg = kmalloc(sizeof(*cg));
	if (!cg) return -12;
	memset(cg, 0, sizeof(*cg));
	int l = 0; while (name[l] && l < 63) { cg->name[l] = name[l]; l++; }
	cg->name[l] = '\0';
	cg->pids_max = -1;
	cg->mem_max  = -1;
	cg->parent = dp->cg;
	uint64_t f = spin_lock_irq(&cg_lock);
	cg->sibling = dp->cg->children;
	dp->cg->children = cg;
	spin_unlock_irq(&cg_lock, f);
	return 0;
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
	pr->cg = cg; pr->file = -1;
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
	extern void vfs_register_fs(const char *, int (*)(vfs_mount_t *, void *));
	vfs_register_fs("cgroup2", cgroup_mount);
}
