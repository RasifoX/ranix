/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The kernel must treat every user pointer as hostile: a kernel address, an
 * unmapped one, a read-only one. Each case runs in a forked child, so a failure is
 * a line of output and a kernel bug is a panic that stops the run. First failure
 * or OK. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#define KTEXT ((void *)0xffffffff80100000ULL)   /* kernel image */
#define KMAP  ((void *)0xffff800000100000ULL)   /* direct map */
#define BAD   ((void *)0x10000ULL)              /* never mapped */

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s errno=%d\n", __LINE__, #c, errno); _exit(1); } } while (0)
/* a child that returns from f passed; killed by want (0: no signal expected) */
static void in_child(const char *name, void (*f)(void), int want)
{
	pid_t p = fork();
	if (p == 0) { alarm(30); f(); _exit(0); }
	int st = 0;
	waitpid(p, &st, 0);
	if (want ? !(WIFSIGNALED(st) && WTERMSIG(st) == want) : !(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
		printf("FAIL %s status=%#x\n", name, st);
		fails++;
	}
}

static void t_fstat(void)
{
	int fd = open("/", O_RDONLY);
	errno = 0; CHECK(syscall(SYS_fstat, fd, KTEXT) == -1 && errno == EFAULT);
	errno = 0; CHECK(syscall(SYS_fstat, fd, BAD) == -1 && errno == EFAULT);
}

static void t_wait(void)
{
	pid_t p = fork();
	if (p == 0) _exit(7);
	usleep(20000);
	errno = 0; CHECK(syscall(SYS_wait4, p, KTEXT, 0, 0) == -1 && errno == EFAULT);
	p = fork();
	if (p == 0) _exit(7);
	usleep(20000);
	errno = 0; CHECK(syscall(SYS_wait4, p, BAD, 0, 0) == -1 && errno == EFAULT);
}

static int thr(void *a) { (void)a; syscall(SYS_exit, 0); return 0; }
static void t_settid(void)
{
	char *st = malloc(65536);
	/* the tid store must not land in the kernel; the call itself may succeed */
	clone(thr, st + 65536, CLONE_VM | CLONE_THREAD | CLONE_SIGHAND | CLONE_PARENT_SETTID, NULL, KMAP);
	usleep(50000);
}

static void t_settid_clear(void)
{
	syscall(SYS_set_tid_address, KMAP);   /* written at exit */
}

static void t_exec(void)
{
	char *av[] = { "x", KTEXT, NULL }, *av2[] = { "x", BAD, NULL };
	char *ok[] = { "/bin/true", NULL };
	errno = 0; CHECK(syscall(SYS_execve, KTEXT, ok, NULL) == -1 && errno == EFAULT);
	errno = 0; CHECK(syscall(SYS_execve, BAD, ok, NULL) == -1 && errno == EFAULT);
	errno = 0; CHECK(syscall(SYS_execve, "/bin/true", KTEXT, NULL) == -1 && errno == EFAULT);
	errno = 0; CHECK(syscall(SYS_execve, "/bin/true", BAD, NULL) == -1 && errno == EFAULT);
	errno = 0; CHECK(syscall(SYS_execve, "/bin/true", av, NULL) == -1 && errno == EFAULT);
	errno = 0; CHECK(syscall(SYS_execve, "/bin/true", av2, NULL) == -1 && errno == EFAULT);
	errno = 0; CHECK(syscall(SYS_execve, "/bin/true", ok, av) == -1 && errno == EFAULT);
	errno = 0; CHECK(syscall(SYS_execveat, AT_FDCWD, KTEXT, ok, NULL, AT_EMPTY_PATH) == -1 && errno == EFAULT);
}

/* a path of any length must not outgrow the kernel's buffer for it */
static void t_longpath(void)
{
	static char lp[4090];
	struct statfs sf;
	memset(lp, 'A', sizeof(lp) - 1);
	lp[0] = '/';
	errno = 0; CHECK(statfs(lp, &sf) == -1);
	errno = 0; CHECK(getxattr(lp, "user.x", NULL, 0) == -1);
	errno = 0; CHECK(setxattr(lp, "user.x", "", 0, 0) == -1);
	errno = 0; CHECK(listxattr(lp, NULL, 0) == -1);
	errno = 0; CHECK(removexattr(lp, "user.x") == -1);
	errno = 0; CHECK(syscall(SYS_mknod, lp, 0100644, 0) == -1);
}

static void t_counts(void)
{
	struct pollfd pf = { 0, POLLIN, 0 };
	/* nfds*8 wraps to 8: one element copied, 2^61 walked */
	errno = 0; CHECK(syscall(SYS_poll, &pf, (1ULL << 61) + 1, 0) == -1 && errno == EINVAL);
	errno = 0; CHECK(fcntl(0, F_DUPFD, -100000) == -1 && errno == EINVAL);
	errno = 0; CHECK(syscall(SYS_shmget, 0, ~0ULL - 8192, 0600) == -1);
	errno = 0; CHECK(syscall(SYS_mmap, 0, ~0ULL - 100, 3, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == -1);
}

static void t_arch(void)
{
	errno = 0; CHECK(syscall(SYS_arch_prctl, 0x1002 /*SET_FS*/, 0x8000000000000000ULL) == -1 && errno == EPERM);
	errno = 0; CHECK(syscall(SYS_arch_prctl, 0x1001 /*SET_GS*/, 0xdead000000000000ULL) == -1 && errno == EPERM);
	char *st = malloc(65536);
	(void)st;
	errno = 0; CHECK(syscall(SYS_clone, CLONE_VM | CLONE_THREAD | CLONE_SIGHAND | CLONE_SETTLS, st + 65536, 0, 0, 0xdead000000000000ULL) == -1);
	errno = 0; CHECK(syscall(SYS_clone, CLONE_VM | CLONE_THREAD | CLONE_SIGHAND, 0xdead000000000000ULL, 0, 0, 0) == -1);
}

/* rt_sigreturn with sp in the kernel, then in a hole: SIGSEGV, not a panic or a
 * dump of kernel memory into the registers */
static void t_sigret_k(void)
{
	__asm__ volatile("movq %0, %%rsp; movq $15, %%rax; syscall" :: "r"(KTEXT) : "rax", "rcx", "r11", "memory");
}
static void t_sigret_b(void)
{
	__asm__ volatile("movq %0, %%rsp; movq $15, %%rax; syscall" :: "r"(BAD) : "rax", "rcx", "r11", "memory");
}

/* a frame holding a non-canonical rip reaches iretq in ring 0 */
static void t_sigret_rip(void)
{
	static uint64_t fr[128] __attribute__((aligned(16)));
	memset(fr, 0, sizeof(fr));
	fr[1] = 0xdead000000000000ULL;      /* rip */
	fr[2] = (uint64_t)(fr + 64);
	__asm__ volatile("movq %0, %%rsp; movq $15, %%rax; syscall" :: "r"(fr) : "rax", "rcx", "r11", "memory");
}

/* reserved MXCSR bits make the fxrstor on the way out #GP in ring 0 */
static void t_sigret_fx(void)
{
	static uint64_t fr[128] __attribute__((aligned(16)));
	static char stack[4096] __attribute__((aligned(16)));
	memset(fr, 0, sizeof(fr));
	fr[1] = (uint64_t)&_exit;
	fr[2] = (uint64_t)(stack + 2048);
	*(uint32_t *)((char *)fr + 160 + 24) = 0xffffffff;   /* MXCSR */
	__asm__ volatile("movq %0, %%rsp; movq $15, %%rax; syscall" :: "r"(fr) : "rax", "rcx", "r11", "memory");
}

/* TF and DF live across a sigreturn into a syscall instruction: the kernel must
 * not start its entry path with them set */
__asm__(".text\nfl_tgt: syscall\n mov $231, %eax\n xor %edi, %edi\n syscall\n");
extern char fl_tgt[];
static void t_entry_flags(void)
{
	static uint64_t fr[128] __attribute__((aligned(16)));
	static char stack[4096] __attribute__((aligned(16)));
	signal(SIGTRAP, SIG_IGN);
	memset(fr, 0, sizeof(fr));
	fr[1] = (uint64_t)fl_tgt;
	fr[2] = (uint64_t)(stack + 2048);
	fr[3] = 0x500;                /* TF | DF */
	fr[4] = SYS_getpid;
	__asm__ volatile("movq %0, %%rsp; movq $15, %%rax; syscall" :: "r"(fr) : "rax", "rcx", "r11", "memory");
}

/* A syscall that reschedules can resume a kernel task; it must not do so on a GS
 * base chosen here. The base is the fuzzer's own 0x40: on low RAM a per-CPU read
 * through it returns BIOS vectors. */
static void t_user_gs(void)
{
	pid_t p = fork();
	if (p == 0) { alarm(40); for (;;) sched_yield(); }   /* outlives us if our alarm fires */
	CHECK(syscall(SYS_arch_prctl, 0x1001 /*SET_GS*/, 0x40UL) == 0);
	time_t end = time(NULL) + 10;   /* slow under emulation, and our alarm is 30 s */
	for (int i = 0; i < 200000 && time(NULL) < end; i++) sched_yield();
	for (int i = 0; i < 200; i++) usleep(1000);
	kill(p, SIGKILL);
	waitpid(p, NULL, 0);
}

static void sigh(int s) { (void)s; }
static void t_handler_rip(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = (void (*)(int))0xdead000000000000ULL;
	sigaction(SIGUSR1, &sa, NULL);
	raise(SIGUSR1);
}

/* the frame goes on a read-only stack: mapped, so the old check passed it */
static void t_ro_stack(void)
{
	static char *ro;
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigh;
	sigaction(SIGUSR1, &sa, NULL);
	ro = mmap(NULL, 8192, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	(void)*(volatile char *)ro;
	(void)*(volatile char *)(ro + 4096);
	register long rax __asm__("rax") = SYS_tkill;
	register long rdi __asm__("rdi") = syscall(SYS_gettid);
	register long rsi __asm__("rsi") = SIGUSR1;
	__asm__ volatile("movq %1, %%rsp; syscall" : "+r"(rax) : "r"(ro + 8000), "r"(rdi), "r"(rsi) : "rcx", "r11", "memory");
}

static void t_remote(void)
{
	char c[8];
	struct iovec l = { c, 8 }, r = { BAD, 8 };
	errno = 0; CHECK(process_vm_readv(getpid(), &l, 1, &r, 1, 0) == -1 && errno == EFAULT);
	r.iov_base = KTEXT;
	errno = 0; CHECK(process_vm_readv(getpid(), &l, 1, &r, 1, 0) == -1 && errno == EFAULT);
	int fd = open("/proc/self/mem", O_RDONLY);
	CHECK(fd >= 0);
	lseek(fd, 0x10000, SEEK_SET);
	errno = 0; CHECK(read(fd, c, 8) == -1 && errno == EIO);
	/* readable from the other side of a fork, writable into read-only text */
	static const char text[] __attribute__((section(".rodata"))) = "constant";
	r.iov_base = (void *)text;
	l.iov_base = "CHANGED!";
	(void)!process_vm_writev(getpid(), &l, 1, &r, 1, 0);
}

static void t_ptrace(void)
{
	char *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	memset(m, 0x41, 4096);
	pid_t p = fork();
	if (p == 0) {
		ptrace(PTRACE_TRACEME, 0, 0, 0);
		raise(SIGSTOP);
		for (;;) pause();
	}
	int st;
	waitpid(p, &st, WUNTRACED);
	/* a word running off the end of the mapping */
	errno = 0;
	long v = ptrace(PTRACE_PEEKDATA, p, m + 4092, 0);
	CHECK(v == -1 && errno != 0);
	errno = 0; ptrace(PTRACE_PEEKDATA, p, KTEXT, 0); CHECK(errno != 0);
	/* breakpoint-style write into read-only text, and COW isolation from us */
	CHECK(ptrace(PTRACE_POKEDATA, p, (void *)t_ptrace, 0x9090909090909090UL) == 0);
	CHECK(ptrace(PTRACE_POKEDATA, p, m, 0x4242424242424242UL) == 0);
	CHECK(*m == 0x41);
	errno = 0;
	CHECK(ptrace(PTRACE_PEEKDATA, p, m, 0) == 0x4242424242424242L);
	kill(p, SIGKILL);
	waitpid(p, &st, 0);
}

static void t_mq(void)
{
	struct mq_attr a = { 0, 8, 64, 0 };
	char name[32];
	snprintf(name, sizeof(name), "/uacc%d", getpid());
	mqd_t q = mq_open(name, O_RDWR | O_CREAT | O_NONBLOCK, 0600, &a);
	CHECK(q >= 0);
	errno = 0; CHECK(syscall(SYS_mq_timedsend, q, KTEXT, 16, 0, 0) == -1 && errno == EFAULT);
	CHECK(mq_send(q, "x", 1, 0) == 0);
	errno = 0; CHECK(syscall(SYS_mq_timedreceive, q, KMAP, 64, 0, 0) == -1 && errno == EFAULT);
	mq_close(q);
	mq_unlink(name);
}

static void t_netlink(void)
{
	int s = socket(AF_NETLINK, SOCK_RAW, 0);
	CHECK(s >= 0);
	struct iovec v = { KTEXT, 64 };
	struct msghdr m;
	memset(&m, 0, sizeof(m));
	m.msg_iov = &v; m.msg_iovlen = 1;
	errno = 0; CHECK(sendmsg(s, &m, 0) == -1 && errno == EFAULT);
	errno = 0; CHECK(recvmsg(s, &m, MSG_DONTWAIT) == -1 && errno == EFAULT);
	errno = 0; CHECK(sendto(s, KTEXT, 64, 0, NULL, 0) == -1 && errno == EFAULT);
	errno = 0; CHECK(recvfrom(s, KMAP, 64, MSG_DONTWAIT, NULL, NULL) == -1 && errno == EFAULT);
}

/* a header naming the kernel half maps a user page into every address space */
static void t_elf(void)
{
	struct { uint32_t type, flags; uint64_t off, vaddr, paddr, filesz, memsz, align; } ph;
	unsigned char eh[64];
	const char *fn = "/tmp/.uacc-elf";
	for (int k = 0; k < 3; k++) {
		memset(eh, 0, sizeof(eh));
		memcpy(eh, "\177ELF\2\1\1", 7);
		eh[16] = 2; eh[18] = 62; eh[20] = 1;
		*(uint64_t *)(eh + 24) = k == 2 ? 0xdead000000000000ULL : 0x400000ULL;
		*(uint64_t *)(eh + 32) = 64;
		*(uint16_t *)(eh + 52) = 64;
		*(uint16_t *)(eh + 54) = sizeof(ph);
		*(uint16_t *)(eh + 56) = 1;
		memset(&ph, 0, sizeof(ph));
		ph.type = 1; ph.flags = 7; ph.filesz = 0; ph.memsz = 4096; ph.align = 4096;
		ph.vaddr = k == 0 ? 0xffffffff80100000ULL : k == 1 ? 0xfffffffffffff000ULL : 0x400000ULL;
		unlink(fn);
		int fd = open(fn, O_WRONLY | O_CREAT, 0755);
		CHECK(fd >= 0);
		CHECK(write(fd, eh, 64) == 64 && write(fd, &ph, sizeof(ph)) == sizeof(ph));
		close(fd);
		char *av[] = { (char *)fn, NULL };
		errno = 0; execve(fn, av, NULL);
		CHECK(errno == ENOEXEC);
	}
	unlink(fn);
}

static void t_unix(void)
{
	struct sockaddr_un a, b;
	memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b));
	a.sun_family = b.sun_family = AF_UNIX;
	strcpy(a.sun_path + 1, "uacc-a"); strcpy(b.sun_path + 1, "uacc-b");
	socklen_t la = 2 + 1 + 6;
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(s >= 0 && bind(s, (void *)&a, la) == 0);
	/* a second name would outlive the socket in the registry */
	errno = 0; CHECK(bind(s, (void *)&b, la) == -1 && errno == EINVAL);
	CHECK(listen(s, 4) == 0);
	int c = socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(connect(c, (void *)&a, la) == 0);
	errno = 0; CHECK(connect(c, (void *)&a, la) == -1 && errno == EISCONN);
	/* a TCP socket must not take a unix address, nor the reverse */
	int t = socket(AF_INET, SOCK_STREAM, 0);
	errno = 0; CHECK(connect(t, (void *)&a, la) == -1 && errno == EAFNOSUPPORT);
	errno = 0; CHECK(bind(t, (void *)&a, la) == -1 && errno == EAFNOSUPPORT);
	/* short addrlen: nothing but the family reaches the kernel */
	int u = socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(bind(u, (void *)&b, 2) == -1);
	close(s);
	int n = socket(AF_UNIX, SOCK_STREAM, 0);
	errno = 0; CHECK(connect(n, (void *)&a, la) == -1 && errno == ECONNREFUSED);
}

/* each loops for ever without its bound; the alarm in in_child is what fails */
static void t_hangs(void)
{
	void *top = (void *)0x100000000ULL;
	errno = 0; CHECK(syscall(SYS_madvise, 0, 1ULL << 62, MADV_DONTNEED) == -1 && errno == ENOMEM);
	errno = 0; CHECK(syscall(SYS_mremap, top, 4096, 1ULL << 62, 0) == -1);
	errno = 0; CHECK(syscall(SYS_mprotect, top, 1ULL << 38, PROT_READ) == -1 && errno == ENOMEM);
	errno = 0; CHECK(syscall(SYS_munmap, 0x700000000000ULL, 1ULL << 46) == -1);
	CHECK(syscall(SYS_brk, 0x7000000000ULL) != 0x7000000000L);
	int z = open("/dev/zero", O_RDONLY), n = open("/dev/null", O_WRONLY);
	CHECK(z >= 0 && n >= 0);
	long r = syscall(SYS_sendfile, n, z, NULL, ~0ULL >> 1);
	CHECK(r == 0x7ffff000L);
	int p = syscall(SYS_futex, top, 3 /*REQUEUE*/, 0, 0x7fffffffUL, top, 0);
	CHECK(p == -1 || p == 0);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	in_child("fstat", t_fstat, 0);
	in_child("wait", t_wait, 0);
	in_child("settid", t_settid, 0);
	in_child("cleartid", t_settid_clear, 0);
	in_child("exec", t_exec, 0);
	in_child("longpath", t_longpath, 0);
	in_child("counts", t_counts, 0);
	in_child("arch", t_arch, 0);
	in_child("sigret-k", t_sigret_k, SIGSEGV);
	in_child("sigret-b", t_sigret_b, SIGSEGV);
	in_child("sigret-rip", t_sigret_rip, SIGSEGV);
	in_child("sigret-fx", t_sigret_fx, 0);
	in_child("entry-flags", t_entry_flags, 0);
	in_child("user-gs", t_user_gs, 0);
	in_child("handler-rip", t_handler_rip, SIGSEGV);
	in_child("ro-stack", t_ro_stack, SIGSEGV);
	in_child("remote", t_remote, 0);
	in_child("ptrace", t_ptrace, 0);
	in_child("mq", t_mq, 0);
	in_child("netlink", t_netlink, 0);
	in_child("elf", t_elf, 0);
	in_child("unix", t_unix, 0);
	in_child("hangs", t_hangs, 0);
	puts(fails ? "FAILED" : "OK");
	return 0;
}
