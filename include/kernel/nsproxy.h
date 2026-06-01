#ifndef NSPROXY_H
#define NSPROXY_H

/*
 * Namespaces isolate a global system resource so that processes in
 * different namespaces see independent instances of it. This is the UTS
 * namespace: it isolates the hostname and NIS domain name reported by
 * uname(2) / sethostname(2).
 *
 * A namespace is reference-counted and shared by all processes that
 * belong to it. fork() shares the parent's namespace; unshare(CLONE_NEWUTS)
 * gives the caller a private copy. The initial namespace (init_uts_ns) is
 * permanent and never freed.
 */

#define CLONE_NEWUTS 0x04000000

typedef struct uts_namespace {
	int           refcount;
	unsigned long inum;          /* unique identity, exposed via /proc/<pid>/ns/uts */
	char          nodename[65];  /* hostname */
	char          domainname[65];
} uts_namespace_t;

extern uts_namespace_t init_uts_ns;

/* the UTS namespace of the calling process (init_uts_ns if unset) */
uts_namespace_t *current_uts(void);

/* refcount helpers; get returns the same ns for convenient assignment */
uts_namespace_t *uts_ns_get(uts_namespace_t *ns);
void             uts_ns_put(uts_namespace_t *ns);

/* allocate a private copy of `old` with refcount 1 (NULL on OOM) */
uts_namespace_t *uts_ns_clone(uts_namespace_t *old);

#endif
