/* A dynamically linked probe, for loading through ld.so by hand; see
 * test_asess_inherit. Prints its session id, and its answer to the gate. */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "../include/uapi/asess.h"

int main(void)
{
	long id = syscall(SYS_prctl, PR_RANIX_ASESS_ID, 0, 0, 0, 0);
	int r = unshare(CLONE_NEWUSER);
	printf("id=%ld unshare=%d errno=%d euid=%d\n", id, r, errno, (int)geteuid());
	return 0;
}
