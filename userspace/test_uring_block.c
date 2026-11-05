/* An SQE runs a read or write on any file, so it may sleep: on an empty pipe here,
 * and the ring must not keep a lock or the CPU's interrupts while it does. Also
 * SQEs keep read(2)'s access-mode gate, and SEND works on a socket. OK or FAIL. */
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define IORING_OFF_SQ_RING 0x0ULL
#define IORING_OFF_CQ_RING 0x8000000ULL
#define IORING_OFF_SQES    0x10000000ULL
#define OP_READ  22
#define OP_WRITE 23
#define OP_SEND  26

struct sqe {
	uint8_t opcode, flags; uint16_t ioprio; int32_t fd;
	uint64_t off, addr; uint32_t len, rw_flags;
	uint64_t user_data; uint16_t buf_index, personality; int32_t splice_fd_in;
	uint64_t pad[2];
};
struct cqe { uint64_t user_data; int32_t res; uint32_t flags; };
struct sq_off { uint32_t head, tail, mask, entries, flags, dropped, array, resv1; uint64_t resv2; };
struct cq_off { uint32_t head, tail, mask, entries, overflow, cqes, flags, resv1; uint64_t resv2; };
struct params {
	uint32_t sq_entries, cq_entries, flags, cpu, idle, features, wq_fd, resv[3];
	struct sq_off s; struct cq_off c;
};

static int ring;
static struct params p;
static uint8_t *sq, *cq;
static struct sqe *sqes;

static void ring_up(void)
{
	memset(&p, 0, sizeof(p));
	ring = (int)syscall(SYS_io_uring_setup, 8, &p);
	sq = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ring, IORING_OFF_SQ_RING);
	cq = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ring, IORING_OFF_CQ_RING);
	sqes = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ring, IORING_OFF_SQES);
}

/* one SQE, one completion's result */
static uint64_t next_off = (uint64_t)-1;
static int submit(int op, int fd, void *buf, uint32_t len)
{
	uint32_t t = *(uint32_t *)(sq + p.s.tail), i = t & *(uint32_t *)(sq + p.s.mask);
	memset(&sqes[i], 0, sizeof(sqes[i]));
	sqes[i].opcode = (uint8_t)op; sqes[i].fd = fd; sqes[i].addr = (uint64_t)buf;
	sqes[i].len = len; sqes[i].off = next_off; next_off = (uint64_t)-1;
	((uint32_t *)(sq + p.s.array))[i] = i;
	*(uint32_t *)(sq + p.s.tail) = t + 1;
	syscall(SYS_io_uring_enter, ring, 1, 1, 1, 0, 0);
	uint32_t h = *(uint32_t *)(cq + p.c.head);
	struct cqe *c = (struct cqe *)(cq + p.c.cqes) + (h & *(uint32_t *)(cq + p.c.mask));
	*(uint32_t *)(cq + p.c.head) = h + 1;
	return c->res;
}

int main(void)
{
	char b[8];
	ring_up();
	if (ring < 0 || sq == MAP_FAILED) { puts("FAIL setup"); return 0; }

	int fd = open("/tmp/.uring-wo", O_CREAT | O_TRUNC | O_WRONLY, 0600);
	if (submit(OP_READ, fd, b, 1) != -9) { puts("FAIL read on O_WRONLY"); return 0; }
	close(fd);
	fd = open("/tmp/.uring-wo", O_RDONLY);
	if (submit(OP_WRITE, fd, b, 1) != -9) { puts("FAIL write on O_RDONLY"); return 0; }
	close(fd);
	unlink("/tmp/.uring-wo");

	/* lseek keeps a file position non-negative, and file ops rely on it */
	fd = open("/proc/self/status", O_RDONLY);
	next_off = (uint64_t)-4096;
	if (submit(OP_READ, fd, b, 8) != -22) { puts("FAIL negative offset"); return 0; }
	close(fd);

	int sv[2];
	socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
	if (submit(OP_SEND, sv[0], "x", 1) != 1) { puts("FAIL send on socket"); return 0; }

	/* A ring must cost no kernel address space: each used to spend 0.5 MiB of a
	 * 1 GiB window nothing gives back, and past it mappings landed on kernel text. */
	for (int i = 0; i < 3000; i++) {
		struct params q;
		memset(&q, 0, sizeof(q));
		int r = (int)syscall(SYS_io_uring_setup, 4096, &q);
		if (r < 0) { printf("FAIL setup %d\n", i); return 0; }
		close(r);
	}
	{
		/* and the rings still work afterwards */
		ring_up();
		if (submit(0 /* NOP */, -1, NULL, 0) != 0) { puts("FAIL nop"); return 0; }
	}

	/* waiting for more completions than exist: only a signal ends this */
	pid_t w = fork();
	if (w == 0) {
		ring_up();
		syscall(SYS_io_uring_enter, ring, 0, 1, 1 /* GETEVENTS */, 0, 0);
		_exit(3);
	}
	usleep(100000);
	kill(w, SIGKILL);
	{
		int st, gone = 0;
		for (int i = 0; i < 300 && !gone; i++) { gone = waitpid(w, &st, WNOHANG) == w; usleep(10000); }
		if (!gone) { puts("FAIL GETEVENTS wait not killable"); return 0; }
	}

	/* several children parked in a blocking SQE; the machine keeps time and they die */
	pid_t kids[4];
	for (int k = 0; k < 4; k++) {
		kids[k] = fork();
		if (kids[k] == 0) {
			int pf[2];
			pipe(pf);
			ring_up();
			submit(OP_READ, pf[0], b, 1);
			_exit(3);
		}
	}
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	usleep(300000);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
	if (ms > 3000) { printf("FAIL stalled %ld ms\n", ms); return 0; }
	for (int k = 0; k < 4; k++) {
		kill(kids[k], SIGKILL);
		int st, gone = 0;
		for (int i = 0; i < 300 && !gone; i++) { gone = waitpid(kids[k], &st, WNOHANG) == kids[k]; usleep(10000); }
		if (!gone) { puts("FAIL child not killable"); return 0; }
	}
	puts("OK");
	return 0;
}
