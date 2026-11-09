#include <kernel/user_namespace.h>
#include <kernel/nsproxy.h>
#include <kernel/proc.h>
#include <kernel/capabilities.h>
#include <kernel/mm/heap.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>

extern unsigned long ns_inum_alloc(void);

/* init ns: identity map (n_uid/n_gid == 0 means 1:1) */
user_namespace_t init_user_ns = { .refcount = 1, .inum = 0xF0000006UL, .level = 0 };
static spinlock_t userns_lock = SPINLOCK_INIT;

user_namespace_t *current_user_ns(void)
{
	process_t *p = task_current();
	return (p && p->user_ns) ? p->user_ns : &init_user_ns;
}

user_namespace_t *user_ns_get(user_namespace_t *ns)
{
	if (!ns) ns = &init_user_ns;
	if (ns != &init_user_ns) {
		uint64_t f = spin_lock_irq(&userns_lock);
		ns->refcount++;
		spin_unlock_irq(&userns_lock, f);
	}
	return ns;
}

void user_ns_put(user_namespace_t *ns)
{
	while (ns && ns != &init_user_ns) {
		uint64_t f = spin_lock_irq(&userns_lock);
		int last = (--ns->refcount == 0);
		spin_unlock_irq(&userns_lock, f);
		if (!last) return;
		user_namespace_t *parent = ns->parent;
		kfree(ns);
		ns = parent;
	}
}

user_namespace_t *user_ns_create_child(user_namespace_t *parent, uint32_t owner_uid)
{
	if (!parent) parent = &init_user_ns;
	user_namespace_t *ns = kmalloc(sizeof(*ns));
	if (!ns) return NULL;
	memset(ns, 0, sizeof(*ns));
	ns->refcount  = 1;
	ns->inum      = ns_inum_alloc();
	ns->parent    = user_ns_get(parent);
	ns->level     = parent->level + 1;
	ns->owner_uid = owner_uid;
	return ns;
}

/* ── id translation (extents store kuid in lower_first, flattened on write) ── */

static uint32_t map_down(uid_extent_t *m, int n, uint32_t local)  /* ns-local -> kuid */
{
	for (int i = 0; i < n; i++)
		if (local >= m[i].first && local - m[i].first < m[i].count)
			return m[i].lower_first + (local - m[i].first);
	return (uint32_t)-1;
}

static uint32_t map_up(uid_extent_t *m, int n, uint32_t kid)      /* kuid -> ns-local */
{
	for (int i = 0; i < n; i++)
		if (kid >= m[i].lower_first && kid - m[i].lower_first < m[i].count)
			return m[i].first + (kid - m[i].lower_first);
	return OVERFLOW_UID;
}

static int has_up(uid_extent_t *m, int n, uint32_t kid)
{
	for (int i = 0; i < n; i++)
		if (kid >= m[i].lower_first && kid - m[i].lower_first < m[i].count)
			return 1;
	return 0;
}

/* Does this kuid/kgid have a name in ns? (Linux kuid_has_mapping.) The
 * overflow id from from_kuid cannot answer it: 65534 may be mapped. */
int kuid_has_mapping(user_namespace_t *ns, uint32_t kuid)
{
	if (!ns || ns == &init_user_ns) return 1;
	return has_up(ns->uid_map, ns->n_uid, kuid);
}
int kgid_has_mapping(user_namespace_t *ns, uint32_t kgid)
{
	if (!ns || ns == &init_user_ns) return 1;
	return has_up(ns->gid_map, ns->n_gid, kgid);
}

uint32_t from_kuid(user_namespace_t *ns, uint32_t kuid)
{
	if (!ns || ns == &init_user_ns) return kuid;
	return map_up(ns->uid_map, ns->n_uid, kuid);
}
uint32_t make_kuid(user_namespace_t *ns, uint32_t uid)
{
	if (!ns || ns == &init_user_ns) return uid;
	return map_down(ns->uid_map, ns->n_uid, uid);
}
uint32_t from_kgid(user_namespace_t *ns, uint32_t kgid)
{
	if (!ns || ns == &init_user_ns) return kgid;
	return map_up(ns->gid_map, ns->n_gid, kgid);
}
uint32_t make_kgid(user_namespace_t *ns, uint32_t gid)
{
	if (!ns || ns == &init_user_ns) return gid;
	return map_down(ns->gid_map, ns->n_gid, gid);
}

/* ── /proc/<pid>/{uid,gid}_map parse + format ──────────────────── */

static uint32_t parse_u32(const char **s)
{
	while (**s == ' ' || **s == '\t') (*s)++;
	uint32_t v = 0;
	while (**s >= '0' && **s <= '9') { v = v * 10 + (uint32_t)(**s - '0'); (*s)++; }
	return v;
}

/* "inside outside count" lines. "outside" is in the parent ns; flatten to a
 * kuid so translation needs no recursion. Writable once. */
int user_ns_set_map(user_namespace_t *ns, const char *buf, int is_gid)
{
	if (!ns || ns == &init_user_ns) return -1;
	uid_extent_t *m = is_gid ? ns->gid_map : ns->uid_map;
	int *n = is_gid ? &ns->n_gid : &ns->n_uid;
	if (*n != 0) return -1;   /* write-once */

	const char *s = buf;
	int cnt = 0;
	while (*s && cnt < UIDMAP_MAX_EXTENTS) {
		while (*s == ' ' || *s == '\t' || *s == '\n') s++;
		if (!*s) break;
		uint32_t inside  = parse_u32(&s);
		uint32_t outside = parse_u32(&s);
		uint32_t count   = parse_u32(&s);
		if (count == 0) break;
		uint32_t kbase = ns->parent
		    ? (is_gid ? make_kgid(ns->parent, outside) : make_kuid(ns->parent, outside))
		    : outside;
		if (kbase == (uint32_t)-1) return -1;   /* outside not mapped in parent */
		m[cnt].first = inside; m[cnt].lower_first = kbase; m[cnt].count = count;
		cnt++;
		while (*s && *s != '\n') s++;
	}
	if (cnt == 0) return -1;
	*n = cnt;
	return 0;
}

/* Who may write a namespace's id map (Linux's new_idmap_permitted): root of the
 * machine may map anything. Anyone else must hold CAP_SETUID/SETGID in the new
 * namespace (its creator does until exec), be inside it or its creator in the
 * parent, and may map exactly one id: their own. */
int user_ns_map_permitted(struct process *writer, user_namespace_t *ns, const char *buf, int is_gid)
{
	process_t *w = (process_t *)writer;
	int cap = is_gid ? CAP_SETGID : CAP_SETUID;
	if (!ns || ns == &init_user_ns) return 0;
	if (capable_task(writer, cap)) return 1;
	user_namespace_t *wns = w->user_ns ? w->user_ns : &init_user_ns;
	if (wns != ns && wns != ns->parent) return 0;
	if (!cap_has(w->cap_effective, cap) || w->euid != ns->owner_uid) return 0;
	const char *s = buf;
	parse_u32(&s);
	uint32_t outside = parse_u32(&s);
	uint32_t count   = parse_u32(&s);
	while (*s == ' ' || *s == '\t' || *s == '\n') s++;
	if (count != 1 || *s) return 0;          /* one line, one id */
	uint32_t k = is_gid ? make_kgid(ns->parent, outside) : make_kuid(ns->parent, outside);
	return k == (is_gid ? w->egid : w->euid);
}

int user_ns_format_map(user_namespace_t *ns, char *buf, int sz, int is_gid)
{
	uid_extent_t *m = is_gid ? ns->gid_map : ns->uid_map;
	int n = is_gid ? ns->n_gid : ns->n_uid;
	int pos = 0;
	for (int i = 0; i < n && pos < sz - 40; i++) {
		uint32_t outside = ns->parent
		    ? (is_gid ? from_kgid(ns->parent, m[i].lower_first) : from_kuid(ns->parent, m[i].lower_first))
		    : m[i].lower_first;
		uint32_t vals[3] = { m[i].first, outside, m[i].count };
		for (int k = 0; k < 3; k++) {
			uint32_t v = vals[k]; char r[12]; int ri = 0;
			if (v == 0) r[ri++] = '0';
			while (v) { r[ri++] = '0' + (int)(v % 10); v /= 10; }
			while (ri) buf[pos++] = r[--ri];
			buf[pos++] = (k < 2) ? ' ' : '\n';
		}
	}
	return pos;
}

/* Does p hold cap in its own user namespace? (Linux ns_capable with
 * current_user_ns.) Capabilities gained by making a namespace count only for
 * what stays inside it, such as a chroot; capable_task asks for the machine. */
int capable_own_ns(struct process *p, int cap)
{
	return cap_has(((process_t *)p)->cap_effective, cap);
}

int capable_task(struct process *p, int cap)
{
	process_t *t = (process_t *)p;
	return cap_has(t->cap_effective, cap) &&
	       (!t->user_ns || t->user_ns == &init_user_ns);
}

/* CLONE_NEWUSER needs no privilege, and a task in a user namespace could stack a
 * new one first: exec there drops the capabilities of a non-root uid, which must
 * not stop `unshare -U` followed by a shell that unshares more. */
int may_create_ns(struct process *p, unsigned long flags)
{
	process_t *t = (process_t *)p;
	if (!(flags & (CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWPID |
	               CLONE_NEWNET | CLONE_NEWCGROUP | CLONE_NEWTIME)))
		return 1;
	return (flags & CLONE_NEWUSER) || (t->user_ns && t->user_ns != &init_user_ns) ||
	       cap_has(t->cap_effective, CAP_SYS_ADMIN);
}

int task_may_control(struct process *t, int cap)
{
	process_t *cur = task_current(), *tg = (process_t *)t;
	/* Linux: the caller's uid must be all three of the target's, and the target
	 * dumpable. A setuid-root program a user started (uid=user, euid=0) is not
	 * the user's to renice, re-limit or move. */
	if (cur == tg || cur->tgid == tg->tgid) return 1;
	if (cur->euid == tg->uid && cur->euid == tg->euid && cur->euid == tg->suid && tg->dumpable)
		return 1;
	return capable_task((struct process *)cur, cap);
}
