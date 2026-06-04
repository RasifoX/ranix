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
#define CAP_NET_BIND_SERVICE 10
#define CAP_NET_RAW          13
#define CAP_NET_ADMIN        12
#define CAP_SYS_ADMIN        21
#define CAP_SYS_PTRACE       19
#define CAP_SYS_CHROOT       18
#define CAP_MKNOD            27
#define CAP_SYS_BOOT         22
#define CAP_SYS_NICE         23
#define CAP_SYS_TIME         25
#define CAP_SETFCAP          31

#define CAP_FULL  (~0ULL)  /* all capabilities */
#define CAP_NONE  (0ULL)

#define cap_has(set, cap)  (!!((set) & (1ULL << (cap))))

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
