/* sigaltstack(2): a stack smaller than MINSIGSTKSZ must fail with ENOMEM
 * (the kernel gave EINVAL); a large enough one must succeed and read back.
 * The raw syscall is checked too: musl's wrapper rejects small sizes itself,
 * so only the raw call reaches the kernel's check. */
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/syscall.h>

static char stk[65536];

int main(void)
{
	stack_t ss = { .ss_sp = stk, .ss_size = 1024, .ss_flags = 0 };
	errno = 0;
	if (syscall(SYS_sigaltstack, &ss, NULL) != -1 || errno != ENOMEM) {
		printf("sigaltstack-fail raw small errno=%d\n", errno);
		return 0;
	}
	errno = 0;
	if (sigaltstack(&ss, NULL) != -1 || errno != ENOMEM) {
		printf("sigaltstack-fail small errno=%d\n", errno);
		return 0;
	}
	ss.ss_sp = NULL; ss.ss_size = sizeof(stk); errno = 0;
	if (syscall(SYS_sigaltstack, &ss, NULL) != -1 || errno != EINVAL) {
		printf("sigaltstack-fail raw null errno=%d\n", errno);
		return 0;
	}
	ss.ss_sp = stk;
	if (sigaltstack(&ss, NULL) != 0) { printf("sigaltstack-fail set errno=%d\n", errno); return 0; }
	stack_t old;
	if (sigaltstack(NULL, &old) != 0 || old.ss_sp != stk || old.ss_size != sizeof(stk)) {
		printf("sigaltstack-fail readback\n");
		return 0;
	}
	printf("OK\n");
	return 0;
}
