/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Inside a user namespace, making another namespace still takes CAP_SYS_ADMIN
 * there (LTP clone304; test_nscaps covers the machine-wide side): with it dropped, unshare(CLONE_NEWUTS) and
 * clone3(CLONE_NEWPID) are EPERM, and so is clone3 asking for a pid (set_tid). */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

struct cap_hdr { uint32_t version; int pid; };
struct cap_data { uint32_t effective, permitted, inheritable; };
struct clone_args_v2 { uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls,
                       set_tid, set_tid_size, cgroup; };

static int bad;

static void want_eperm(const char *what, long r, int is_clone)
{
	if (r == 0 && is_clone) _exit(0);   /* a clone3 child that should not exist */
	if (r == 0) { printf("%s: succeeded, want EPERM\n", what); bad++; return; }
	if (r > 0) { waitpid((pid_t)r, NULL, 0); printf("%s: succeeded, want EPERM\n", what); bad++; }
	else if (errno != EPERM) { printf("%s: errno %d, want EPERM\n", what, errno); bad++; }
}

static int child(void)
{
	if (unshare(CLONE_NEWUSER)) { printf("unshare(CLONE_NEWUSER): errno %d\n", errno); return 1; }
	if (unshare(CLONE_NEWUTS)) { printf("with CAP_SYS_ADMIN: unshare(CLONE_NEWUTS) errno %d\n", errno); bad++; }
	struct cap_hdr h = { 0x20080522, 0 };
	struct cap_data d[2];
	memset(d, 0, sizeof(d));
	syscall(SYS_capget, &h, d);
	d[0].effective &= ~(1u << 21);   /* CAP_SYS_ADMIN */
	if (syscall(SYS_capset, &h, d)) { printf("capset: errno %d\n", errno); return 1; }
	errno = 0; want_eperm("unshare(CLONE_NEWUTS)", unshare(CLONE_NEWUTS), 0);
	struct clone_args_v2 a;
	memset(&a, 0, sizeof(a));
	a.flags = CLONE_NEWPID; a.exit_signal = SIGCHLD;
	errno = 0; want_eperm("clone3(CLONE_NEWPID)", syscall(SYS_clone3, &a, sizeof(a)), 1);
	pid_t tid = 1;
	memset(&a, 0, sizeof(a));
	a.exit_signal = SIGCHLD; a.set_tid = (uint64_t)(uintptr_t)&tid; a.set_tid_size = 1;
	errno = 0; want_eperm("clone3 with set_tid", syscall(SYS_clone3, &a, sizeof(a)), 1);
	return bad;
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);   /* the child ends with _exit */
	pid_t c = fork();
	if (c == 0) _exit(child());
	int st;
	waitpid(c, &st, 0);
	puts(WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "OK" : "FAIL");
	return 0;
}
