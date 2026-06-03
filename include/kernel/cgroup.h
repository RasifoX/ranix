#ifndef CGROUP_H
#define CGROUP_H

struct process;
struct vfs_mount;

/* process lifecycle hooks (see kernel/cgroup.c) */
int  cgroup_can_fork(struct process *parent);      /* charges on success; -1 if at limit */
void cgroup_fork_attach(struct process *child, struct process *parent);
void cgroup_cancel_fork(struct process *parent);
void cgroup_exit(struct process *p);

void cgroup_register(void);
int  cgroup_mount(struct vfs_mount *mnt, void *data);

/* memory controller (cg = a process's ->cgroup, void to keep cgroup_t opaque) */
int  cgroup_charge_mem(void *cg, long n);   /* 0 ok, -1 over memory.max */
void cgroup_uncharge_mem(void *cg, long n);
int  cgroup_is_in_subtree(void *cg, void *ancestor);  /* 1 if cg == ancestor or below */
void cgroup_oom_kill(void *over_cg);        /* SIGKILL biggest task in subtree (sched.c) */
int  cgroup_frozen(void *cg);               /* 1 if cg or an ancestor is frozen */
int  cgroup_cpu_weight(void *cg);           /* cpu.weight (1..10000, 100 = baseline) */

/* cgroup namespace */
struct cgroup_namespace;
extern struct cgroup_namespace init_cgroup_ns;
struct cgroup_namespace *current_cgroup_ns(void);
struct cgroup_namespace *cgroup_ns_get(struct cgroup_namespace *ns);
void                     cgroup_ns_put(struct cgroup_namespace *ns);
struct cgroup_namespace *cgroup_ns_create(void);
unsigned long            cgroup_ns_inum(struct cgroup_namespace *ns);
int  cgroup_proc_path(struct process *p, char *buf, int sz);

struct vfs_file;
struct cgroup_namespace *nsfile_to_cgroup(struct vfs_file *f);

#endif
