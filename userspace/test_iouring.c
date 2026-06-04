/* io_uring: set up a ring, mmap the SQ/CQ/SQE regions, submit a NOP and a READ
 * of a file, enter, and reap both completions with correct results. Exercises
 * the ring mmap mechanics and the synchronous submit/complete path on both
 * arches. musl has no io_uring wrappers, so call the syscalls raw. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#ifndef SYS_io_uring_setup
#define SYS_io_uring_setup 425
#define SYS_io_uring_enter 426
#endif

#define IORING_OFF_SQ_RING 0x0ULL
#define IORING_OFF_CQ_RING 0x8000000ULL
#define IORING_OFF_SQES    0x10000000ULL
#define IORING_ENTER_GETEVENTS 1
#define IORING_OP_NOP  0
#define IORING_OP_READ 22

struct io_uring_sqe {
	uint8_t opcode, flags; uint16_t ioprio; int32_t fd;
	uint64_t off; uint64_t addr; uint32_t len; uint32_t rw_flags;
	uint64_t user_data; uint16_t buf_index, personality; int32_t splice_fd_in;
	uint64_t __pad2[2];
};
struct io_uring_cqe { uint64_t user_data; int32_t res; uint32_t flags; };

struct io_sqring_offsets { uint32_t head, tail, ring_mask, ring_entries, flags, dropped, array, resv1; uint64_t resv2; };
struct io_cqring_offsets { uint32_t head, tail, ring_mask, ring_entries, overflow, cqes, flags, resv1; uint64_t resv2; };
struct io_uring_params {
	uint32_t sq_entries, cq_entries, flags, sq_thread_cpu, sq_thread_idle, features, wq_fd, resv[3];
	struct io_sqring_offsets sq_off;
	struct io_cqring_offsets cq_off;
};

int main(void)
{
	struct io_uring_params p;
	memset(&p, 0, sizeof(p));
	int ring = syscall(SYS_io_uring_setup, 8, &p);
	if (ring < 0) { printf("FAIL setup %d\n", ring); return 1; }

	size_t sqring_sz = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
	size_t cqring_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);

	uint8_t *sq = mmap(0, sqring_sz, PROT_READ | PROT_WRITE, MAP_SHARED, ring, IORING_OFF_SQ_RING);
	uint8_t *cq = mmap(0, cqring_sz, PROT_READ | PROT_WRITE, MAP_SHARED, ring, IORING_OFF_CQ_RING);
	struct io_uring_sqe *sqes = mmap(0, p.sq_entries * sizeof(struct io_uring_sqe),
	                                 PROT_READ | PROT_WRITE, MAP_SHARED, ring, IORING_OFF_SQES);
	if (sq == MAP_FAILED || cq == MAP_FAILED || sqes == MAP_FAILED) { puts("FAIL mmap"); return 1; }

	uint32_t *sq_tail  = (uint32_t *)(sq + p.sq_off.tail);
	uint32_t *sq_mask  = (uint32_t *)(sq + p.sq_off.ring_mask);
	uint32_t *sq_array = (uint32_t *)(sq + p.sq_off.array);
	uint32_t *cq_head  = (uint32_t *)(cq + p.cq_off.head);
	uint32_t *cq_tail  = (uint32_t *)(cq + p.cq_off.tail);
	uint32_t *cq_mask  = (uint32_t *)(cq + p.cq_off.ring_mask);
	struct io_uring_cqe *cqes = (struct io_uring_cqe *)(cq + p.cq_off.cqes);

	/* a file to read back */
	const char *path = "/tmp/iouring_src";
	int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
	if (fd < 0) { puts("FAIL open"); return 1; }
	if (write(fd, "RANIX-IOURING", 13) != 13) { puts("FAIL prep-write"); return 1; }

	/* SQE 0: NOP */
	uint32_t t = *sq_tail;
	struct io_uring_sqe *s0 = &sqes[t & *sq_mask];
	memset(s0, 0, sizeof(*s0));
	s0->opcode = IORING_OP_NOP;
	s0->user_data = 0x1111;
	sq_array[t & *sq_mask] = t & *sq_mask;
	t++;

	/* SQE 1: READ 13 bytes of the file at offset 0 into a buffer */
	char buf[32];
	memset(buf, 0, sizeof(buf));
	struct io_uring_sqe *s1 = &sqes[t & *sq_mask];
	memset(s1, 0, sizeof(*s1));
	s1->opcode = IORING_OP_READ;
	s1->fd = fd;
	s1->off = 0;
	s1->addr = (uint64_t)(uintptr_t)buf;
	s1->len = 13;
	s1->user_data = 0x2222;
	sq_array[t & *sq_mask] = t & *sq_mask;
	t++;

	__sync_synchronize();
	*sq_tail = t;   /* publish both SQEs */

	int r = syscall(SYS_io_uring_enter, ring, 2, 2, IORING_ENTER_GETEVENTS, 0, 0);
	if (r != 2) { printf("FAIL enter r=%d\n", r); return 1; }

	/* reap exactly two completions */
	int saw_nop = 0, saw_read = 0;
	while (*cq_head != *cq_tail) {
		struct io_uring_cqe *c = &cqes[*cq_head & *cq_mask];
		if (c->user_data == 0x1111 && c->res == 0) saw_nop = 1;
		if (c->user_data == 0x2222 && c->res == 13) saw_read = 1;
		__sync_synchronize();
		*cq_head = *cq_head + 1;
	}

	if (!saw_nop)  { puts("FAIL nop-cqe"); return 1; }
	if (!saw_read) { printf("FAIL read-cqe (buf='%s')\n", buf); return 1; }
	if (memcmp(buf, "RANIX-IOURING", 13) != 0) { printf("FAIL read-data '%s'\n", buf); return 1; }

	close(fd);
	close(ring);
	puts("OK");
	return 0;
}
