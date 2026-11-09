/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef CAPABILITIES_H
#define CAPABILITIES_H

#include <uapi/types.h>

/* Capability bit numbers (standard subset) */
#define CAP_CHOWN            0
#define CAP_DAC_OVERRIDE     1
#define CAP_DAC_READ_SEARCH  2
#define CAP_FOWNER           3
#define CAP_FSETID           4
#define CAP_KILL             5
#define CAP_SETGID           6
#define CAP_SETUID           7
#define CAP_SETPCAP          8
#define CAP_LINUX_IMMUTABLE  9
#define CAP_NET_BIND_SERVICE 10
#define CAP_NET_RAW          13
#define CAP_IPC_LOCK         14
#define CAP_SYSLOG           34
#define CAP_NET_ADMIN        12
#define CAP_SYS_ADMIN        21
#define CAP_SYS_PTRACE       19
#define CAP_SYS_CHROOT       18
#define CAP_MKNOD            27
#define CAP_SYS_BOOT         22
#define CAP_SYS_NICE         23
#define CAP_SYS_RESOURCE     24
#define CAP_SYS_TIME         25
#define CAP_SETFCAP          31
#define CAP_MAC_OVERRIDE     32

#define CAP_FULL  (~0ULL)  /* all capabilities */
#define CAP_NONE  (0ULL)

#define cap_has(set, cap)  (!!((set) & (1ULL << (cap))))

/* A task's capabilities act on the machine only from the initial user namespace.
 * unshare(CLONE_NEWUSER) hands any uid a full set, valid for that namespace alone. */
struct process;
int capable_task(struct process *p, int cap);
int capable_own_ns(struct process *p, int cap);
/* May p take uid/gid id it does not hold (cap = CAP_SETUID or CAP_SETGID)? */
int capable_setid(struct process *p, int cap, uint32_t id);
/* May p create the namespaces in flags (CLONE_NEW*)? */
int may_create_ns(struct process *p, unsigned long flags);
/* May the caller change t? Yes for itself, its own uid, or holding cap. */
int task_may_control(struct process *t, int cap);

/* capget/capset userspace ABI (__user_cap_header_struct / __user_cap_data_struct) */
typedef struct {
	uint32_t version;
	int      pid;
} cap_user_header_t;

typedef struct {
	uint32_t effective;
	uint32_t permitted;
	uint32_t inheritable;
} cap_user_data_t;

#define _LINUX_CAPABILITY_VERSION_3 0x20080522

#endif
