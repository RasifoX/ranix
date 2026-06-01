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

#define CLONE_NEWNS   0x00020000
#define CLONE_NEWUSER 0x10000000
#define CLONE_NEWUTS  0x04000000
#define CLONE_NEWIPC  0x08000000
#define CLONE_NEWPID  0x20000000
#define CLONE_NEWTIME 0x00000080

/* shared global allocator for namespace identities (/proc/<pid>/ns/*) */
unsigned long ns_inum_alloc(void);

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

/* the UTS namespace an open /proc/<pid>/ns/uts fd refers to (NULL if the fd
 * is not such a namespace file); used by setns(2) */
struct vfs_file;
uts_namespace_t *nsfile_to_uts(struct vfs_file *f);

/*
 * IPC namespace — isolates SysV shared memory / semaphores and POSIX message
 * queues. Lifecycle lives in syscall_ipc.c (next to the object tables) so
 * teardown can reclaim the namespace's IPC objects.
 */
typedef struct ipc_namespace {
	int           refcount;
	unsigned long inum;
} ipc_namespace_t;

extern ipc_namespace_t init_ipc_ns;
ipc_namespace_t *current_ipc(void);
ipc_namespace_t *ipc_ns_get(ipc_namespace_t *ns);
void             ipc_ns_put(ipc_namespace_t *ns);
ipc_namespace_t *ipc_ns_clone(ipc_namespace_t *old);
ipc_namespace_t *nsfile_to_ipc(struct vfs_file *f);

/*
 * Time namespace — per-namespace offsets added to CLOCK_MONOTONIC and
 * CLOCK_BOOTTIME (CLOCK_REALTIME is unaffected). Created with
 * unshare(CLONE_NEWTIME); offsets are written via /proc/<pid>/timens_offsets
 * before any task joins the new namespace.
 */
typedef struct time_namespace {
	int           refcount;
	unsigned long inum;
	long          mono_sec, mono_nsec;   /* offset added to CLOCK_MONOTONIC */
	long          boot_sec, boot_nsec;   /* offset added to CLOCK_BOOTTIME */
} time_namespace_t;

extern time_namespace_t init_time_ns;
time_namespace_t *current_time_ns(void);
time_namespace_t *time_ns_get(time_namespace_t *ns);
void              time_ns_put(time_namespace_t *ns);
time_namespace_t *time_ns_clone(time_namespace_t *old);
time_namespace_t *nsfile_to_time(struct vfs_file *f);

#endif
