/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The direction flag is the user's; the kernel must not run with it.
 *  1. Fresh anonymous pages first touched with DF set read as zero: the page
 *     fault's memset of the new page runs forwards (rep stosb with DF set
 *     zeroed the page below it, kernel heap: the fuzzer's corruption, 11-15).
 *  2. A signal handler starts with DF clear (SysV ABI; Linux clears it).
 * x86_64 only; elsewhere it prints OK. */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__x86_64__)
#define PAGES 64

static volatile int handler_df = -1;

static void on_usr1(int sig)
{
	(void)sig;
	unsigned long fl;
	__asm__ volatile("pushfq; popq %0" : "=r"(fl));
	handler_df = (fl >> 10) & 1;
}

int main(void)
{
	long pg = sysconf(_SC_PAGESIZE);
	unsigned char *m = mmap(0, (size_t)(PAGES * pg), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m == MAP_FAILED) { printf("FAIL mmap\n"); return 1; }
	/* touch each page once with DF set: one read, the fault in between */
	for (int i = 0; i < PAGES; i++) {
		unsigned char *p = m + (long)i * pg;
		unsigned char v;
		__asm__ volatile("std\n\tmovb (%1), %0\n\tcld" : "=q"(v) : "r"(p) : "memory", "cc");
		(void)v;
	}
	for (long k = 0; k < PAGES * pg; k++)
		if (m[k]) {
			printf("FAIL page %ld byte %ld is 0x%02x after a fault taken with DF set\n", k / pg, k % pg, m[k]);
			return 1;
		}
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_usr1;
	sigaction(SIGUSR1, &sa, 0);
	pid_t self = getpid();
	/* kill with DF set: the signal comes on the way out of the system call */
	long r;
	__asm__ volatile("std\n\tsyscall\n\tcld" : "=a"(r) : "a"(62L), "D"((long)self), "S"((long)SIGUSR1)
	                 : "rcx", "r11", "memory", "cc");
	if (r != 0 || handler_df != 0) {
		printf("FAIL handler ran with DF=%d (kill %ld)\n", handler_df, r);
		return 1;
	}
	printf("OK\n");
	return 0;
}
#else
int main(void) { printf("OK\n"); return 0; }
#endif
