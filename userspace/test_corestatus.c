/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A death that dumps core says so: WCOREDUMP in the wait status and
 * CLD_DUMPED from waitid (LTP abort01, waitpid01). With RLIMIT_CORE below a
 * page nothing is dumped and the bit stays clear, as on Linux. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static int bad;

static pid_t aborting_child(rlim_t core)
{
	pid_t k = fork();
	if (k == 0) {
		struct rlimit r = { core, core == 0 ? 0 : RLIM_INFINITY };
		setrlimit(RLIMIT_CORE, &r);
		abort();
	}
	return k;
}

int main(void)
{
	chdir("/tmp");
	int st = 0;
	waitpid(aborting_child(RLIM_INFINITY), &st, 0);
	if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGABRT || !WCOREDUMP(st)) {
		printf("abort with cores on: status 0x%x, want SIGABRT with WCOREDUMP\n", st); bad++;
	}
	waitpid(aborting_child(0), &st, 0);
	if (!WIFSIGNALED(st) || WCOREDUMP(st)) {
		printf("abort with RLIMIT_CORE 0: status 0x%x, want no WCOREDUMP\n", st); bad++;
	}
	siginfo_t si;
	pid_t k = aborting_child(RLIM_INFINITY);
	if (waitid(P_PID, (id_t)k, &si, WEXITED) != 0 || si.si_code != CLD_DUMPED || si.si_status != SIGABRT) {
		printf("waitid: si_code %d si_status %d, want CLD_DUMPED and SIGABRT\n", si.si_code, si.si_status); bad++;
	}
	unlink("/tmp/core");
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
