/* getcpu(2), personality(2), setfsuid/setfsgid(2) — the misc compat batch. */
#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <sys/syscall.h>

#ifdef __x86_64__
#define NR_setfsuid   122
#define NR_setfsgid   123
#define NR_personality 135
#define NR_getcpu     309
#else
#define NR_setfsuid   151
#define NR_setfsgid   152
#define NR_personality 92
#define NR_getcpu     168
#endif

#define ADDR_NO_RANDOMIZE 0x0040000

static int fail = 0;
static void check(const char *n, int ok)
{
	if (!ok) { printf("  FAIL: %s\n", n); fail = 1; }
}

int main(void)
{
	/* getcpu: a valid cpu index and node 0 */
	unsigned cpu = 0xdead, node = 0xbeef;
	long r = syscall(NR_getcpu, &cpu, &node, (void *)0);
	check("getcpu-ret", r == 0);
	check("getcpu-cpu", cpu < 256);
	check("getcpu-node", node == 0);

	/* personality: query returns current, set returns previous, then sticks */
	long cur = syscall(NR_personality, 0xffffffffu);
	check("persona-query", cur >= 0);
	long prev = syscall(NR_personality, ADDR_NO_RANDOMIZE);
	check("persona-set-ret", prev == cur);
	long now = syscall(NR_personality, 0xffffffffu);
	check("persona-stuck", now == ADDR_NO_RANDOMIZE);
	syscall(NR_personality, 0);   /* restore */

	/* setfsuid returns the *previous* fsuid (== euid initially); a second call
	 * confirms the first stuck, and restores fsuid to euid. */
	uid_t eu = geteuid();
	long pf = syscall(NR_setfsuid, 1000);
	check("setfsuid-prev", pf == (long)eu);
	long pf2 = syscall(NR_setfsuid, eu);
	check("setfsuid-now", pf2 == 1000);
	gid_t eg = getegid();
	long pg = syscall(NR_setfsgid, 1000);
	check("setfsgid-prev", pg == (long)eg);
	long pg2 = syscall(NR_setfsgid, eg);
	check("setfsgid-now", pg2 == 1000);

	printf(fail ? "FAIL\n" : "OK\n");
	return fail;
}
