#ifndef IO_URING_H
#define IO_URING_H

#include <uapi/types.h>
#include <kernel/vfs.h>

/* Linux io_uring(7) ABI — struct layouts and constants are fixed by the kernel
 * UAPI so liburing/tokio/etc. interoperate unchanged. */

struct io_uring_sqe {
	uint8_t  opcode;
	uint8_t  flags;
	uint16_t ioprio;
	int32_t  fd;
	union { uint64_t off; uint64_t addr2; };
	union { uint64_t addr; };
	uint32_t len;
	union {
		uint32_t rw_flags;
		uint32_t fsync_flags;
		uint32_t msg_flags;
		uint32_t accept_flags;
	};
	uint64_t user_data;
	union { uint16_t buf_index; uint16_t buf_group; };
	uint16_t personality;
	int32_t  splice_fd_in;
	uint64_t __pad2[2];
};

struct io_uring_cqe {
	uint64_t user_data;
	int32_t  res;
	uint32_t flags;
};

/* sqe->opcode */
#define IORING_OP_NOP        0
#define IORING_OP_READV      1
#define IORING_OP_WRITEV     2
#define IORING_OP_FSYNC      3
#define IORING_OP_READ_FIXED 4
#define IORING_OP_WRITE_FIXED 5
#define IORING_OP_RECVMSG    10
#define IORING_OP_SENDMSG    9
#define IORING_OP_ACCEPT     13
#define IORING_OP_READ       22
#define IORING_OP_WRITE      23
#define IORING_OP_SEND       26
#define IORING_OP_RECV       27

/* io_uring_enter() flags */
#define IORING_ENTER_GETEVENTS 1

/* mmap offsets — userspace mmaps these regions of the ring fd */
#define IORING_OFF_SQ_RING 0x0ULL
#define IORING_OFF_CQ_RING 0x8000000ULL
#define IORING_OFF_SQES    0x10000000ULL

struct io_sqring_offsets {
	uint32_t head, tail, ring_mask, ring_entries, flags, dropped, array;
	uint32_t resv1; uint64_t resv2;
};

struct io_cqring_offsets {
	uint32_t head, tail, ring_mask, ring_entries, overflow, cqes, flags;
	uint32_t resv1; uint64_t resv2;
};

struct io_uring_params {
	uint32_t sq_entries;
	uint32_t cq_entries;
	uint32_t flags;
	uint32_t sq_thread_cpu;
	uint32_t sq_thread_idle;
	uint32_t features;
	uint32_t wq_fd;
	uint32_t resv[3];
	struct io_sqring_offsets sq_off;
	struct io_cqring_offsets cq_off;
};

#define IORING_FEAT_SINGLE_MMAP 1

/* io_uring_register() opcodes (only buffers/files stubs implemented) */
#define IORING_REGISTER_BUFFERS   0
#define IORING_UNREGISTER_BUFFERS 1
#define IORING_REGISTER_FILES     2
#define IORING_UNREGISTER_FILES   3

long io_uring_setup(uint32_t entries, struct io_uring_params *params);
long io_uring_enter(int fd, uint32_t to_submit, uint32_t min_complete,
                    uint32_t flags, const void *argp, size_t argsz);
long io_uring_register(int fd, unsigned int opcode, void *arg, unsigned int nr_args);

#endif
