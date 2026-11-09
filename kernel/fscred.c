/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Filesystem credentials and the permission rule every file check uses
 * (Linux generic_permission, capable_wrt_inode_uidgid, inode_owner_or_capable).
 * See include/kernel/fscred.h for why uid 0 alone is no longer enough. */
#include <kernel/fscred.h>
#include <kernel/proc.h>
#include <kernel/capabilities.h>
#include <kernel/user_namespace.h>
#include <kernel/vfs.h>
#include <kernel/errno.h>

/* the capabilities that follow the fsuid (Linux CAP_FS_MASK) */
#define CAP_FS_SET ((1ULL << CAP_CHOWN) | (1ULL << CAP_DAC_OVERRIDE) | \
                    (1ULL << CAP_DAC_READ_SEARCH) | (1ULL << CAP_FOWNER) | \
                    (1ULL << CAP_FSETID) | (1ULL << CAP_LINUX_IMMUTABLE) | \
                    (1ULL << CAP_MKNOD) | (1ULL << CAP_MAC_OVERRIDE))

uid_t task_fsuid(const struct process *p)
{
	const process_t *t = (const process_t *)p;
	if (!t) return 0;
	return t->fs_uid_set ? t->fsuid : t->euid;
}

gid_t task_fsgid(const struct process *p)
{
	const process_t *t = (const process_t *)p;
	if (!t) return 0;
	return t->fs_gid_set ? t->fsgid : t->egid;
}

void fs_cred_task(struct process *p, fs_cred_t *c)
{
	process_t *t = (process_t *)p;
	c->uid  = task_fsuid(p);
	c->gid  = task_fsgid(p);
	c->caps = t ? t->cap_effective : CAP_FULL;
	c->task = p;
	c->ns   = t ? t->user_ns : NULL;
}

void fs_cred_access(struct process *p, fs_cred_t *c)
{
	process_t *t = (process_t *)p;
	fs_cred_task(p, c);
	if (!t) return;
	c->uid  = t->uid;
	c->gid  = t->gid;
	c->caps = t->uid == 0 ? t->cap_permitted : 0;
}

int fs_in_group(const fs_cred_t *c, gid_t g)
{
	const process_t *t = (const process_t *)c->task;
	if (c->gid == g) return 1;
	for (int i = 0; t && i < t->ngroups; i++)
		if (t->groups[i] == g) return 1;
	return 0;
}

int fs_capable(const fs_cred_t *c, uid_t iuid, gid_t igid, int cap)
{
	if (!cap_has(c->caps, cap)) return 0;
	if (!c->ns || c->ns == &init_user_ns) return 1;
	return kuid_has_mapping(c->ns, iuid) && kgid_has_mapping(c->ns, igid);
}

int fs_permission(const fs_cred_t *c, mode_t mode, uid_t iuid, gid_t igid, int mask)
{
	int shift;
	mask &= FS_MAY_READ | FS_MAY_WRITE | FS_MAY_EXEC;
	if (c->uid == iuid) shift = 6;   /* the owner gets the owner bits only */
	else shift = fs_in_group(c, igid) ? 3 : 0;
	if ((((mode & 0777) >> shift) & (unsigned)mask) == (unsigned)mask) return 0;

	if (S_ISDIR(mode)) {
		/* directories: read and search by CAP_DAC_READ_SEARCH, anything by DAC_OVERRIDE */
		if (!(mask & FS_MAY_WRITE) && fs_capable(c, iuid, igid, CAP_DAC_READ_SEARCH)) return 0;
		return fs_capable(c, iuid, igid, CAP_DAC_OVERRIDE) ? 0 : -EACCES;
	}
	if (mask == FS_MAY_READ && fs_capable(c, iuid, igid, CAP_DAC_READ_SEARCH)) return 0;
	/* a file nobody may execute stays so for root too */
	if ((!(mask & FS_MAY_EXEC) || (mode & 0111)) && fs_capable(c, iuid, igid, CAP_DAC_OVERRIDE))
		return 0;
	return -EACCES;
}

int fs_owner_or_capable(uid_t iuid, gid_t igid)
{
	fs_cred_t c;
	fs_cred_task((struct process *)task_current(), &c);
	return c.uid == iuid || fs_capable(&c, iuid, igid, CAP_FOWNER);
}

void cred_fsuid_fixup(struct process *p, uid_t old_fsuid)
{
	process_t *t = (process_t *)p;
	uid_t now = task_fsuid(p);
	if (old_fsuid == 0 && now != 0)
		t->cap_effective &= ~CAP_FS_SET;
	else if (old_fsuid != 0 && now == 0)
		t->cap_effective |= t->cap_permitted & CAP_FS_SET;
}
