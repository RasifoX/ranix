/* SCHED_FIFO policy via raw syscalls (musl's sched_* wrappers can short-circuit
 * in libc; we want to exercise the kernel path). */
#include <stdio.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

struct sp { int sched_priority; };

int main(void)
{
	if (syscall(SYS_sched_get_priority_max, SCHED_FIFO) != 99) { puts("FAIL max"); return 1; }
	if (syscall(SYS_sched_get_priority_min, SCHED_FIFO) != 1)  { puts("FAIL min"); return 1; }

	struct sp pr = { 50 };
	if (syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &pr) != 0) { puts("FAIL set"); return 1; }
	if (syscall(SYS_sched_getscheduler, 0) != SCHED_FIFO)         { puts("FAIL get"); return 1; }

	struct sp g = { 0 };
	if (syscall(SYS_sched_getparam, 0, &g) != 0 || g.sched_priority != 50) { puts("FAIL param"); return 1; }

	puts("OK");
	return 0;
}
