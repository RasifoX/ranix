#ifndef USER_NAMESPACE_H
#define USER_NAMESPACE_H

#include <uapi/types.h>

/*
 * User namespaces (Linux kuid model). Credentials are stored as kuids — the
 * uid as seen in the initial namespace (= what the rest of the kernel and the
 * filesystem use). A user namespace defines a mapping between its own uid/gid
 * numbers and kuids; getuid/stat translate kuid->ns via from_kuid, while
 * setuid and ownership translate ns->kuid via make_kuid. Permission checks
 * compare kuids directly, so they are namespace-independent.
 *
 * A new user namespace starts with an empty map (everything reads as the
 * overflow id 65534) until /proc/<pid>/uid_map and gid_map are written. The
 * creator becomes all-powerful inside its own namespace — the basis of
 * rootless containers (uid 0 inside, an unprivileged kuid outside).
 */
#define UIDMAP_MAX_EXTENTS 5
#define OVERFLOW_UID 65534u

typedef struct { uint32_t first; uint32_t lower_first; uint32_t count; } uid_extent_t;

struct process;

typedef struct user_namespace {
	int                    refcount;
	unsigned long          inum;
	struct user_namespace *parent;
	int                    level;
	uint32_t               owner_uid;   /* kuid of the creator */
	uid_extent_t           uid_map[UIDMAP_MAX_EXTENTS]; int n_uid;
	uid_extent_t           gid_map[UIDMAP_MAX_EXTENTS]; int n_gid;
} user_namespace_t;

extern user_namespace_t init_user_ns;

user_namespace_t *current_user_ns(void);
user_namespace_t *user_ns_get(user_namespace_t *ns);
void              user_ns_put(user_namespace_t *ns);
user_namespace_t *user_ns_create_child(user_namespace_t *parent, uint32_t owner_uid);

/* kuid <-> namespace-local id translation */
uint32_t from_kuid(user_namespace_t *ns, uint32_t kuid);   /* kuid -> ns-local (OVERFLOW_UID if unmapped) */
uint32_t make_kuid(user_namespace_t *ns, uint32_t uid);    /* ns-local -> kuid ((uint32_t)-1 if unmapped) */
uint32_t from_kgid(user_namespace_t *ns, uint32_t kgid);
uint32_t make_kgid(user_namespace_t *ns, uint32_t gid);

/* parse+install a uid_map/gid_map line ("inside outside count"); is_gid selects */
int user_ns_set_map(user_namespace_t *ns, const char *buf, int is_gid);
int user_ns_format_map(user_namespace_t *ns, char *buf, int sz, int is_gid);

struct vfs_file;
user_namespace_t *nsfile_to_user(struct vfs_file *f);

#endif
