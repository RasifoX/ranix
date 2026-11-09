/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* seccomp_data as a filter sees it, checked the way Chromium's policy compiler
 * checks it: every 64-bit field is two 32-bit loads (lo word at the lower
 * offset), a 32-bit argument passes when its hi word is 0 or 0xffffffff, flags
 * are tested with JSET/AND masks. If the kernel fills only nr and arch, every
 * argument check reads garbage and allowed calls land in the policy's trap.
 * - args[0..5] lo and hi words match the registers of a raw syscall;
 * - instruction_pointer is the address just past the syscall instruction;
 * - Chromium-style rules: socketpair domain == AF_UNIX (32-bit arg), mmap
 *   flags within PRIVATE|ANONYMOUS|STACK|FIXED|NORESERVE (JSET on ~mask);
 * - negative: the same filter refuses AF_INET and an unlisted mmap flag.
 * Prints OK, or FAIL with what was wrong. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

struct sock_filter { unsigned short code; unsigned char jt, jf; unsigned int k; };
struct sock_fprog { unsigned short len; struct sock_filter *filter; };

#define LD(off)        { 0x20, 0, 0, (off) }
#define JEQ(k, t, f)   { 0x15, (t), (f), (unsigned)(k) }
#define JSET(k, t, f)  { 0x45, (t), (f), (unsigned)(k) }
#define AND(k)         { 0x54, 0, 0, (unsigned)(k) }
#define RET(r)         { 0x06, 0, 0, (r) }
#define ERRNO(e)       (0x00050000u | (e))
#define ALLOW          0x7fff0000u

#define ARG_LO(n) (16 + 8 * (n))
#define ARG_HI(n) (20 + 8 * (n))

static int install(struct sock_filter *f, unsigned short n)
{
	struct sock_fprog p = { n, f };
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return -1;
	return prctl(PR_SET_SECCOMP, 2, &p, 0, 0);
}

/* getpriority with six known 64-bit values; returns where the call came from */
extern char after_call[];
__attribute__((noinline)) static long call6(long *ret)
{
	register long r10 __asm__("r10") = 0x4400000044L;
	register long r8 __asm__("r8") = (long)0xa5000055a5000055UL;
	register long r9 __asm__("r9") = (long)0xffffffff00000066UL;
	long rax = SYS_getpriority;
	__asm__ volatile("syscall\n.globl after_call\nafter_call:"
	                 : "+a"(rax)
	                 : "D"(0x1100000011L), "S"(0x22L), "d"((long)0x8000003380000033UL),
	                   "r"(r10), "r"(r8), "r"(r9)
	                 : "rcx", "r11", "memory");
	*ret = rax;
	return 0;
}

/* errno 10+i when word i of args (0 lo, 1 hi, ... 11) or ip (12 lo, 13 hi) differs */
static const char *args_and_ip(void)
{
	static const uint32_t want[12] = { 0x11, 0x11, 0x22, 0, 0x80000033, 0x80000033,
	                                   0x44, 0x44, 0xa5000055, 0xa5000055, 0x66, 0xffffffff };
	uint64_t ip = (uintptr_t)after_call;
	struct sock_filter f[64];
	unsigned n = 0;
	f[n++] = (struct sock_filter)LD(0);
	f[n++] = (struct sock_filter)JEQ(SYS_getpriority, 1, 0);
	f[n++] = (struct sock_filter)RET(ALLOW);
	for (unsigned w = 0; w < 12; w++) {
		f[n++] = (struct sock_filter)LD(16 + 4 * w);
		f[n++] = (struct sock_filter)JEQ(want[w], 1, 0);
		f[n++] = (struct sock_filter)RET(ERRNO(10 + w));
	}
	f[n++] = (struct sock_filter)LD(8);
	f[n++] = (struct sock_filter)JEQ((uint32_t)ip, 1, 0);
	f[n++] = (struct sock_filter)RET(ERRNO(22));
	f[n++] = (struct sock_filter)LD(12);
	f[n++] = (struct sock_filter)JEQ((uint32_t)(ip >> 32), 1, 0);
	f[n++] = (struct sock_filter)RET(ERRNO(23));
	f[n++] = (struct sock_filter)RET(ERRNO(99));
	if (install(f, n)) return "install (args)";
	long r;
	call6(&r);
	if (r == -99) return NULL;
	static char why[64];
	if (r <= -10 && r >= -21)
		snprintf(why, sizeof why, "args[%ld] %s word is wrong", (-r - 10) / 2, (-r - 10) % 2 ? "hi" : "lo");
	else if (r == -22 || r == -23)
		snprintf(why, sizeof why, "instruction_pointer %s word is wrong", r == -22 ? "lo" : "hi");
	else
		snprintf(why, sizeof why, "getpriority returned %ld", r);
	return why;
}

/* Chromium baseline: socketpair only AF_UNIX (a 32-bit argument: hi word 0 or
 * ~0), mmap flags only within the allowed mask; anything else is refused. */
static const char *chromium_rules(void)
{
	const uint32_t okflags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK | MAP_FIXED | MAP_NORESERVE;
	struct sock_filter f[] = {
		LD(0),
		JEQ(SYS_socketpair, 0, 5),
		LD(ARG_HI(0)),                 /* 32-bit arg: hi word 0 or 0xffffffff */
		JEQ(0, 1, 0),
		JEQ(0xffffffffu, 0, 8),
		LD(ARG_LO(0)),
		JEQ(AF_UNIX, 7, 6),            /* -> allow : refuse */
		JEQ(SYS_mmap, 0, 6),
		LD(ARG_HI(3)),
		JEQ(0, 0, 3),
		LD(ARG_LO(3)),
		JSET(~okflags, 1, 0),
		RET(ALLOW),
		RET(ERRNO(EACCES)),           /* not EPERM: musl turns an anonymous mmap's EPERM into ENOMEM */
		RET(ALLOW),
	};
	/* index 13 refuses, 12 and 14 allow: socketpair lands on 13 or 14, a call
	 * that is neither socketpair nor mmap on 14 */
	if (install(f, sizeof f / sizeof f[0])) return "install (rules)";
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv)) return "socketpair(AF_UNIX) refused";
	close(sv[0]); close(sv[1]);
	if (socketpair(AF_INET, SOCK_STREAM, 0, sv) == 0 || errno != EACCES)
		return "socketpair(AF_INET) was not refused";
	void *m = mmap(0, 0x201000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (m == MAP_FAILED) return "mmap(PRIVATE|ANONYMOUS|STACK) refused";
	munmap(m, 0x201000);
	m = mmap(0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (m != MAP_FAILED || errno != EACCES) return "mmap with MAP_POPULATE was not refused";
	return NULL;
}

static const char *in_child(const char *(*fn)(void))
{
	int p[2];
	static char buf[96];
	if (pipe(p)) return "pipe";
	pid_t c = fork();
	if (c == 0) {
		const char *e = fn();
		int n = snprintf(buf, sizeof buf, "%s", e ? e : "");
		(void)!write(p[1], buf, n);
		_exit(e ? 1 : 0);
	}
	close(p[1]);
	int n = read(p[0], buf, sizeof buf - 1);
	close(p[0]);
	buf[n > 0 ? n : 0] = 0;
	int st;
	waitpid(c, &st, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return NULL;
	if (!buf[0]) snprintf(buf, sizeof buf, "child status %#x", st);
	return buf;
}

int main(void)
{
	const char *e = in_child(args_and_ip);
	if (!e) e = in_child(chromium_rules);
	if (e) { printf("FAIL: %s\n", e); return 1; }
	printf("OK\n");
	return 0;
}
