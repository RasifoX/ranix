#ifndef CGROUP_H
#define CGROUP_H

#include <kernel/mm/vm.h>

struct process;
struct vfs_mount;

/* process lifecycle hooks (see kernel/cgroup.c) */
int  cgroup_can_fork(void *cg);       /* the new task's cgroup; charges on success, -1 if at limit */
void cgroup_fork_attach(struct process *child, void *cg);
void cgroup_cancel_fork(void *cg);
void cgroup_exit(struct process *p);

void cgroup_register(void);
int  cgroup_mount(struct vfs_mount *mnt, void *data);

/* memory controller (cg = a process's ->cgroup, void to keep cgroup_t opaque).
 * cg only places a space's first charge; after that the space's cgroup holds. */
int  cgroup_charge_mem(vm_space_t *vm, void *cg, long n);   /* 0 ok, -1 over memory.max */
void cgroup_uncharge_mem(vm_space_t *vm, long n);
void cgroup_charge_table(vm_space_t *vm);   /* one page-table page; cannot fail */
int  cgroup_charge_file(void **owner, long have, long n);   /* see kernel/cgroup.c */
void *cgroup_current(void);
int  cgroup_charge_to(void *owner, long n, int force);
void cgroup_file_uncharge(void *owner, long n);   /* pages gone, file stays */
void cgroup_file_release(void *owner, long n);   /* the pages freed with the file; owner may be NULL */
void cgroup_vm_release(vm_space_t *vm);     /* last reference gone */
void *cgroup_session_create(const char *name, long mem_max, long pids_max, long avail, long reserve);
void cgroup_put(void *cg);
void cgroup_mem_usage(void *cg, long *cur, long *max);
int  cgroup_migrate(void *from, void *to, long n, vm_space_t *vm);  /* 0, -1 over pids.max */
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
