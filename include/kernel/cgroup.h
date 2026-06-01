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

#endif
