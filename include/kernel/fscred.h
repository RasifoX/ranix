/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef FSCRED_H
#define FSCRED_H

/* Filesystem credentials: who a file access is checked for. As on Linux, a
 * file is checked against the fsuid/fsgid (the effective ids, unless setfsuid
 * moved them alone), and only capabilities override its mode bits:
 * CAP_DAC_OVERRIDE, CAP_DAC_READ_SEARCH, CAP_FOWNER, CAP_CHOWN. uid 0 by
 * itself overrides nothing, so root that dropped a capability is held to the
 * mode bits like anyone else. */

#include <uapi/types.h>

struct process;
struct user_namespace;

#define FS_MAY_EXEC  1   /* = X_OK, the mode's x bit */
#define FS_MAY_WRITE 2
#define FS_MAY_READ  4

typedef struct {
	uid_t                  uid;
	gid_t                  gid;
	uint64_t               caps;   /* the capabilities this check may use */
	struct process        *task;   /* whose supplementary groups; NULL: none */
	struct user_namespace *ns;     /* caps reach only inodes this ns maps */
} fs_cred_t;

uid_t task_fsuid(const struct process *p);   /* NULL (kernel thread): 0 */
gid_t task_fsgid(const struct process *p);
/* the task's own: fsuid, fsgid, effective capabilities. NULL: a kernel thread, all caps */
void  fs_cred_task(struct process *p, fs_cred_t *c);
/* access(2) without AT_EACCESS: real ids; capabilities only for a real uid 0
 * (Linux access_override_creds) */
void  fs_cred_access(struct process *p, fs_cred_t *c);
int   fs_in_group(const fs_cred_t *c, gid_t g);
/* Linux capable_wrt_inode_uidgid: cap, and the inode's ids named in c->ns */
int   fs_capable(const fs_cred_t *c, uid_t iuid, gid_t igid, int cap);
/* Linux generic_permission: mask (FS_MAY_*) against mode, then the DAC
 * capabilities. 0 or -EACCES. */
int   fs_permission(const fs_cred_t *c, mode_t mode, uid_t iuid, gid_t igid, int mask);
/* the current task owns the inode or holds CAP_FOWNER over it */
int   fs_owner_or_capable(uid_t iuid, gid_t igid);
/* setfsuid moved the fsuid away from or back to 0: the filesystem
 * capabilities leave or return with it (Linux LSM_SETID_FS) */
void  cred_fsuid_fixup(struct process *p, uid_t old_fsuid);

#endif
