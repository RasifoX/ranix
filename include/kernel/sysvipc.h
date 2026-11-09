/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef KERNEL_SYSVIPC_H
#define KERNEL_SYSVIPC_H

#include <uapi/types.h>

/* SysV IPC objects as procfs shows them (/proc/sysvipc/{shm,sem,msg}) and the
 * limits under /proc/sys/kernel. kernel/sys/syscall_ipc.c */
enum { SYSVIPC_SHM, SYSVIPC_SEM, SYSVIPC_MSG };

typedef struct {
	int32_t  key;
	int      id;
	uint32_t mode;
	uint64_t a, b;          /* shm: size, nattch;  sem: nsems;  msg: cbytes, qnum */
	int32_t  pid1, pid2;    /* shm: cpid, lpid;    msg: lspid, lrpid */
	uint32_t uid, gid, cuid, cgid;
	int64_t  t1, t2, t3;    /* shm: atime, dtime, ctime;  sem: otime, ctime;  msg: stime, rtime, ctime */
} sysvipc_row_t;

/* Slot idx of kind in the caller's IPC namespace: 1 filled in, 0 empty, -1 past the last */
int sysvipc_row(int kind, int idx, sysvipc_row_t *r);

/* /proc/sys/kernel/<name>: shmmax shmmni shmall msgmax msgmnb msgmni, and sem's four */
uint64_t sysvipc_limit(const char *name);

#endif
