#ifndef _UAPI_ASESS_H
#define _UAPI_ASESS_H

/* Agent sessions: long asess(unsigned op, int sfd, void *arg, size_t size).
 * Shared by the kernel (-nostdinc) and userspace, so base C types only. */
#define ASESS_SYSCALL   2048

#define ASESS_CREATE    1   /* arg: struct asess_attr; returns a close-on-exec sfd */
#define ASESS_SPAWN     2   /* fork; the child starts inside. Once per session */
#define ASESS_INFO      3   /* arg: struct asess_info. sfd, or -1 from inside */

#define ASESS_NET_NONE  0   /* new network namespace, loopback only */

#define ASESS_LABEL_MAX 32

/* prctl: the caller's session id, 0 outside any session */
#define PR_RANIX_ASESS_ID 0x52580001

struct asess_attr {
	unsigned int       size;       /* sizeof(struct asess_attr) */
	unsigned int       flags;      /* none defined; must be 0 */
	unsigned int       uid, gid;   /* 0: the caller's. Root must name a non-root id */
	unsigned int       net_mode;   /* ASESS_NET_NONE */
	unsigned int       nkeep;      /* entries in keep_fds */
	unsigned long long keep_fds;   /* int[nkeep]: pipes, sockets, ttys that stay open */
	unsigned long long root;       /* const char *: absolute path of a directory */
	unsigned int       max_tasks;  /* 0: ASESS_DEFAULT_TASKS */
	unsigned int       tmp_mib;    /* size of the private /tmp; 0: ASESS_DEFAULT_TMP_MIB */
	unsigned long long rw_dir;     /* const char *: a directory to write to, or 0. Its
	                                * canonical path inside root if it lies under root,
	                                * else the same path, which must exist there or in
	                                * the private /tmp. No undo: writes reach the host */
	unsigned long long mem_max;    /* bytes of memory; 0: min(RAM/2, RAM-1 GiB), at least
	                                * ASESS_MEM_FLOOR_MIB. All sessions together keep
	                                * min(1 GiB, RAM/4) free for the host */
	char               label[ASESS_LABEL_MAX];
	unsigned long long rw_src;     /* const char *: a host directory, or 0. If set, it is
	                                * bound at rw_dir's place in the view instead of
	                                * rw_dir itself, which then only names the place and
	                                * stays untouched: the staging of ranix-sess -S */
};

#define ASESS_DEFAULT_TASKS    256
#define ASESS_DEFAULT_TMP_MIB  64
#define ASESS_MEM_FLOOR_MIB    256

#define ASESS_STATE_CREATED 0
#define ASESS_STATE_RUNNING 1
#define ASESS_STATE_DEAD    2

struct asess_info {
	unsigned long long id;
	unsigned int       state, tasks, max_tasks, denied;
	unsigned long long mem_cur, mem_max;   /* bytes */
};

#endif
