#ifndef PID_NAMESPACE_H
#define PID_NAMESPACE_H

#include <uapi/types.h>

/*
 * PID namespaces (Linux-faithful). A process has a number (nr) in its own pid
 * namespace and in every ancestor namespace, recorded as a vector of
 * (namespace, nr) pairs — pidnrs[0] is always the initial namespace, where the
 * nr equals the global pid (the proctab index). getpid/getppid/kill/waitpid
 * translate through the *caller's* namespace; a process is invisible to
 * namespaces that are not its ancestors.
 *
 * The first process in a new pid namespace is nr 1 — the namespace's child
 * reaper (init); orphans reparent to it, and its death tears the namespace
 * down.
 */
#define PIDNS_MAX_LEVEL 8

struct process;

typedef struct pid_namespace {
	int                   refcount;
	unsigned long         inum;
	struct pid_namespace *parent;
	int                   level;     /* initial ns = 0 */
	int                   last_nr;   /* ns-local nr allocator */
	struct process       *reaper;    /* nr 1 of this ns */
	struct process      **map;       /* nr -> process (NULL for init ns: uses proctab) */
	int                   map_cap;
	int                   dead;      /* reaper exited: ns is being torn down */
} pid_namespace_t;

extern pid_namespace_t init_pid_ns;

pid_namespace_t *current_pid_ns(void);
pid_namespace_t *pid_ns_get(pid_namespace_t *ns);
void             pid_ns_put(pid_namespace_t *ns);
pid_namespace_t *pid_ns_create_child(pid_namespace_t *parent); /* refcount 1 */

/* attach p to namespace ns and all ancestors, assigning a nr in each; pidnrs[0]
 * uses the already-assigned global pid. Returns 0 on success. */
int  pidns_attach(struct process *p, pid_namespace_t *ns, int global_pid);
void pidns_detach(struct process *p);  /* remove from each ns's map */

int  task_pid_nr_ns(struct process *p, pid_namespace_t *ns); /* 0 if not visible */
int  task_vpid(struct process *p);                            /* nr in p's own ns */
struct process *pidns_find(pid_namespace_t *ns, int nr);      /* task by ns-local nr */

struct vfs_file;
pid_namespace_t *nsfile_to_pid(struct vfs_file *f);

#endif
