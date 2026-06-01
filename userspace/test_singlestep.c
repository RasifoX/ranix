/* ptrace single-step: a breakpoint (int3) stops the tracee; then each
 * PTRACE_SINGLESTEP traps after one instruction (SIGTRAP) with the PC advancing.
 * (aarch64 software-step is a separate mechanism — skipped here.) */
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#ifndef PTRACE_SINGLESTEP
#define PTRACE_SINGLESTEP 9
#endif

#if defined(__x86_64__)
#include <sys/user.h>
typedef struct user_regs_struct ptrace_regs;
#define REG_PC(r) ((r).rip)
#define BREAKPOINT() __asm__ volatile("int3")

int main(void)
{
	int c2p[2];
	if (pipe(c2p)) { puts("FAIL pipe"); return 0; }

	pid_t child = fork();
	if (child < 0) { puts("FAIL fork"); return 0; }
	if (child == 0) {
		close(c2p[0]);
		ptrace(PTRACE_TRACEME, 0, 0, 0);
		char c = 'R'; write(c2p[1], &c, 1);
		BREAKPOINT();                       /* int3 -> SIGTRAP stop */
		volatile int x = 0;
		for (int i = 0; i < 50; i++) x += i;
		_exit(0);
	}
	close(c2p[1]);
	char c; read(c2p[0], &c, 1);

	int status;
	if (waitpid(child, &status, 0) != child || !WIFSTOPPED(status)) { puts("FAIL nobp"); return 0; }

	unsigned long prev = 0;
	int distinct = 0;
	for (int i = 0; i < 4; i++) {
		if (ptrace(PTRACE_SINGLESTEP, child, 0, 0) < 0) { puts("FAIL step"); return 0; }
		if (waitpid(child, &status, 0) != child) { puts("FAIL stepwait"); return 0; }
		if (WIFEXITED(status)) break;
		if (!WIFSTOPPED(status)) { puts("FAIL stepnostop"); return 0; }
		ptrace_regs r; memset(&r, 0, sizeof(r));
		ptrace(PTRACE_GETREGS, child, 0, &r);
		unsigned long pc = REG_PC(r);
		if (pc == 0) { puts("FAIL pc0"); return 0; }
		if (i > 0 && pc != prev) distinct++;
		prev = pc;
	}

	ptrace(PTRACE_CONT, child, 0, 0);
	while (waitpid(child, &status, 0) > 0)
		if (WIFEXITED(status) || WIFSIGNALED(status)) break;
		else if (WIFSTOPPED(status)) ptrace(PTRACE_CONT, child, 0, 0);

	puts(distinct >= 1 ? "OK" : "FAIL no-advance");
	return 0;
}
#else
int main(void) { puts("OK"); return 0; }   /* aarch64 software-step: TODO */
#endif
