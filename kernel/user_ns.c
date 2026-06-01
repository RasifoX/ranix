#include <kernel/user_namespace.h>
#include <kernel/proc.h>
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
