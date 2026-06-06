#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>

/* A tiny leaf function whose machine code we copy elsewhere to try to run. */
static int retval(void) { return 0x5a; }
typedef int (*fn_t)(void);

/* Run `body` in a child; return 1 if the child died on a fault signal
 * (SIGSEGV/SIGILL/SIGBUS) — i.e. the page was non-executable as required. */
static int faults_in_child(void (*body)(void))
{
	pid_t pid = fork();
	if (pid == 0) { body(); _exit(0); }   /* 0 = ran without faulting (bad) */
	int st = 0;
	waitpid(pid, &st, 0);
	if (WIFSIGNALED(st)) {
		int s = WTERMSIG(st);
		return (s == SIGSEGV || s == SIGILL || s == SIGBUS);
	}
	return 0;
}

static void exec_heap(void)
{
	unsigned char *data = malloc(4096);
	memcpy(data, (void *)retval, 64);
	fn_t f = (fn_t)(void *)data;
	volatile int r = f();
	(void)r;
}

static void exec_stack(void)
{
	unsigned char buf[256] __attribute__((aligned(16)));
	memcpy(buf, (void *)retval, 64);
	fn_t f = (fn_t)(void *)buf;
	volatile int r = f();
	(void)r;
}

int main(void)
{
	/* 1. Executing a heap (data) page must fault — NX data. */
	if (!faults_in_child(exec_heap)) { puts("FAIL: heap executed"); return 1; }

	/* 2. Executing a stack page must fault — NX stack. */
	if (!faults_in_child(exec_stack)) { puts("FAIL: stack executed"); return 1; }

	/* 3. mprotect(W|X) must be rejected — W^X policy. */
	void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
	               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m == MAP_FAILED) { puts("mmap-failed"); return 1; }
	if (mprotect(m, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0 || errno != EACCES) {
		puts("FAIL: mprotect W+X allowed");
		return 1;
	}

	/* 4. mmap(W|X) must be rejected too. */
	if (mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
	         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != MAP_FAILED) {
		puts("FAIL: mmap W+X allowed");
		return 1;
	}

	/* 5. An R+X mapping actually executes — the code path still works. */
	memcpy(m, (void *)retval, 64);
	if (mprotect(m, 4096, PROT_READ | PROT_EXEC) != 0) {
		puts("FAIL: mprotect R+X rejected");
		return 1;
	}
	__builtin___clear_cache((char *)m, (char *)m + 64);
	if (((fn_t)m)() != 0x5a) { puts("FAIL: R+X wrong result"); return 1; }

	puts("OK");
	return 0;
}
