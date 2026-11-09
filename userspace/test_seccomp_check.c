/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A seccomp program is checked when it is installed, the way Linux's
 * bpf_check_classic + seccomp_check_filter check it, and refused with EINVAL:
 * - an opcode outside the seccomp list (LD|H|ABS, LDX|W|ABS, MOD, junk);
 * - a jump (JA, or either branch of a conditional) past the last instruction;
 * - a load outside seccomp_data, or not 4-byte aligned;
 * - DIV by constant 0, a constant shift of 32 or more, a scratch cell >= 16,
 *   a scratch cell read before every path to it has stored it;
 * - a last instruction that is not RET; an empty program.
 * And the whole Linux list runs: ALU (K and X), NEG, TAX/TXA, ST/LD MEM,
 * LDX LEN, JEQ X; the test program turns getppid into errno 77.
 * Every case runs in its own child, since an accepted filter stays for good.
 * Prints OK, or FAIL with the first case that differs. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

struct sock_filter { unsigned short code; unsigned char jt, jf; unsigned int k; };
struct sock_fprog { unsigned short len; struct sock_filter *filter; };

#define I(c, t, f, k)  { (c), (t), (f), (unsigned)(k) }
#define LD_NR          I(0x20, 0, 0, 0)
#define RET(r)         I(0x06, 0, 0, (r))
#define ALLOW          0x7fff0000u
#define ERRNO(e)       (0x00050000u | (e))

/* child: 0 = refused with EINVAL, 1 = accepted, 2 = refused otherwise */
static int try_install(const struct sock_filter *f, unsigned short n)
{
	struct sock_fprog p = { n, (struct sock_filter *)f };
	pid_t c = fork();
	if (c == 0) {
		if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) _exit(3);
		long r = syscall(SYS_seccomp, 1, 0, &p);
		_exit(r == 0 ? 1 : errno == EINVAL ? 0 : 2);
	}
	int st = -1;
	waitpid(c, &st, 0);
	if (WIFEXITED(st)) return WEXITSTATUS(st);
	return 10 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);   /* an accepted bad program killed it */
}

static int fails;
#define BAD(name, ...) do { \
	struct sock_filter f_[] = { __VA_ARGS__ }; \
	int r_ = try_install(f_, sizeof(f_) / sizeof(f_[0])); \
	if (r_ != 0) { printf("FAIL: %s: %s (%d)\n", name, r_ == 1 || r_ >= 10 ? "accepted" : "not EINVAL", r_); fails++; } \
} while (0)

static void refused(void)
{
	BAD("LD|H|ABS (not in the seccomp list)", I(0x28, 0, 0, 0), RET(ALLOW));
	BAD("LDX|W|ABS (not in the seccomp list)", I(0x21, 0, 0, 0), RET(ALLOW));
	BAD("ALU|MOD|K (not in the seccomp list)", LD_NR, I(0x94, 0, 0, 3), RET(ALLOW));
	BAD("opcode 0xff", I(0xff, 0, 0, 0), RET(ALLOW));
	BAD("JA past the end", I(0x05, 0, 0, 1), RET(ALLOW));
	BAD("JA k=0xffffffff", I(0x05, 0, 0, 0xffffffffu), RET(ALLOW));
	BAD("JEQ jt past the end", LD_NR, I(0x15, 5, 0, 0), RET(ALLOW));
	BAD("JEQ jf past the end", LD_NR, I(0x15, 0, 1, 0), RET(ALLOW));
	BAD("JSET X jt past the end", LD_NR, I(0x4d, 1, 0, 0), RET(ALLOW));
	BAD("LD|W|ABS at 64 (past seccomp_data)", I(0x20, 0, 0, 64), RET(ALLOW));
	BAD("LD|W|ABS at 0xffffffff", I(0x20, 0, 0, 0xffffffffu), RET(ALLOW));
	BAD("LD|W|ABS at 2 (unaligned)", I(0x20, 0, 0, 2), RET(ALLOW));
	BAD("DIV|K by 0", LD_NR, I(0x34, 0, 0, 0), RET(ALLOW));
	BAD("LSH|K by 32", LD_NR, I(0x64, 0, 0, 32), RET(ALLOW));
	BAD("RSH|K by 33", LD_NR, I(0x74, 0, 0, 33), RET(ALLOW));
	BAD("ST to cell 16", LD_NR, I(0x02, 0, 0, 16), RET(ALLOW));
	BAD("LD|MEM from cell 16", I(0x60, 0, 0, 16), RET(ALLOW));
	BAD("LD|MEM before any ST", I(0x60, 0, 0, 0), RET(ALLOW));
	/* stored on one branch only: the other path reaches the load unset */
	BAD("LD|MEM stored on one path", LD_NR, I(0x15, 0, 1, 1), I(0x02, 0, 0, 2),
	    I(0x60, 0, 0, 2), RET(ALLOW));
	BAD("no RET at the end", LD_NR);
	BAD("RET then a load", RET(ALLOW), LD_NR);

	struct sock_fprog empty = { 0, NULL };
	pid_t c = fork();
	if (c == 0) {
		prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
		_exit(syscall(SYS_seccomp, 1, 0, &empty) == -1 && errno == EINVAL ? 0 : 1);
	}
	int st = -1;
	waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) { puts("FAIL: empty program accepted"); fails++; }
}

/* every instruction kind Linux allows, in one program that must still work */
static void accepted(void)
{
	uint32_t v = SYS_getppid;
	v += 5; v *= 3; v -= 1; v <<= 4; v >>= 2; v |= 0x100; v ^= 0x55; v &= 0xffff; v /= 3; v = -v;
	struct sock_filter f[] = {
		/*  0 */ LD_NR,
		/*  1 */ I(0x02, 0, 0, 3),               /* ST M[3] */
		/*  2 */ I(0x15, 0, 19, SYS_getppid),    /* other calls: ALLOW */
		/*  3 */ I(0x04, 0, 0, 5),               /* ADD K */
		/*  4 */ I(0x24, 0, 0, 3),               /* MUL K */
		/*  5 */ I(0x14, 0, 0, 1),               /* SUB K */
		/*  6 */ I(0x64, 0, 0, 4),               /* LSH K */
		/*  7 */ I(0x74, 0, 0, 2),               /* RSH K */
		/*  8 */ I(0x44, 0, 0, 0x100),           /* OR K */
		/*  9 */ I(0xa4, 0, 0, 0x55),            /* XOR K */
		/* 10 */ I(0x54, 0, 0, 0xffff),          /* AND K */
		/* 11 */ I(0x34, 0, 0, 3),               /* DIV K */
		/* 12 */ I(0x84, 0, 0, 0),               /* NEG */
		/* 13 */ I(0x07, 0, 0, 0),               /* TAX */
		/* 14 */ I(0x00, 0, 0, v),               /* LD IMM */
		/* 15 */ I(0x1d, 0, 6, 0),               /* JEQ X */
		/* 16 */ I(0x81, 0, 0, 0),               /* LDX LEN */
		/* 17 */ I(0x87, 0, 0, 0),               /* TXA */
		/* 18 */ I(0x15, 0, 3, 64),              /* sizeof(seccomp_data) */
		/* 19 */ I(0x60, 0, 0, 3),               /* LD M[3] */
		/* 20 */ I(0x15, 0, 1, SYS_getppid),
		/* 21 */ RET(ERRNO(77)),
		/* 22 */ RET(ALLOW),
	};
	struct sock_fprog p = { sizeof(f) / sizeof(f[0]), f };
	pid_t c = fork();
	if (c == 0) {
		if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) _exit(3);
		if (syscall(SYS_seccomp, 1, 0, &p)) _exit(4);
		long r = syscall(SYS_getppid);
		if (r != -1 || errno != 77) _exit(5);
		if (syscall(SYS_getpid) <= 0) _exit(6);
		_exit(0);
	}
	int st = -1;
	waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) {
		printf("FAIL: full-list program: status %x (4 refused, 5 wrong result)\n", st);
		fails++;
	}
	/* a scratch cell stored on both paths, and RET A, are fine */
	struct sock_filter g[] = {
		LD_NR, I(0x15, 0, 2, 1), I(0x02, 0, 0, 2), I(0x05, 0, 0, 1), I(0x02, 0, 0, 2),
		I(0x60, 0, 0, 2), I(0x00, 0, 0, ALLOW), I(0x16, 0, 0, 0),
	};
	int r = try_install(g, sizeof(g) / sizeof(g[0]));
	if (r != 1) { printf("FAIL: valid scratch/RET A program: %d\n", r); fails++; }
}

int main(void)
{
	refused();
	accepted();
	if (!fails) puts("OK");
	return fails != 0;
}
