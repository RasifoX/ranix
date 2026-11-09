/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef KERNEL_ASESS_H
#define KERNEL_ASESS_H

#include <uapi/asess.h>
#include <kernel/nsproxy.h>
#include <kernel/proc.h>

/* Agent sessions (arastirma/TASARIM.md section 6). */

struct asess;

/* namespaces a spawned child gets, and that a member may never ask for */
#define ASESS_CLONE_NS (CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWUTS | \
                        CLONE_NEWIPC | CLONE_NEWCGROUP)
#define ASESS_CLONE_DENIED (ASESS_CLONE_NS | CLONE_NEWUSER)

/* mount data selecting the filtered session view of proc and devfs */
#define ASESS_MOUNT_OPT "asess"

#define ASESS_DENIALS_KEPT 16

typedef struct {
	uint64_t id;
	int      state;
	uid_t    uid;
	uint32_t tasks, max_tasks, denied;
	long     mem_cur, mem_max;
	char     label[ASESS_LABEL_MAX];
	struct { int nr; int pid; } recent[ASESS_DENIALS_KEPT];   /* oldest first */
	uint32_t nrecent;
} asess_view_t;

long sys_asess(unsigned int op, int sfd, void *arg, size_t size);

int  asess_syscall_allowed(process_t *p, long nr, long a0, long a1, long a2);
int  asess_file_allowed(vfs_file_t *f);

/* the fork path; asess_fork and asess_fork_cancel run under sched_lock */
int  asess_prepare_child(process_t *child, struct asess *s);
int  asess_fork(process_t *child, process_t *parent, struct asess *enter);
void asess_fork_cancel(process_t *child);
void asess_task_exit(struct asess *s);
void *asess_cgroup(struct asess *s);

uint64_t asess_id_of(process_t *p);                /* 0 outside any session */
int      asess_view(uint64_t id, asess_view_t *out);   /* 0, or -1 if absent or not the caller's to see */
uint64_t asess_nth_visible(uint32_t idx);          /* id of the idx-th session the caller may see; 0 past the end */

#endif
