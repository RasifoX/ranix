/* io_uring(7) — Linux 5.1+ async I/O submission/completion rings.
 *
 * SQ ring, CQ ring and the SQE array are kernel-allocated pages that userspace
 * mmaps via the ring fd. Both sides share the pages through an UNCACHED mapping:
 * userspace's mmap goes through the file mmap fop (which sys_mmap maps NOCACHE),
 * and the kernel touches the same physical pages through an ioremap() alias with
 * the same attribute. Matching attributes keep the lock-free head/tail protocol
 * coherent on aarch64 (Normal-NC vs cached aliasing is otherwise not coherent)
 * without per-access cache maintenance.
 *
 * Submission is synchronous-at-enter: io_uring_enter() drains the SQ, runs each
 * op against the existing vfs/socket paths, and posts the CQE before returning.
 * The ring mechanics are real; true async offload to a worker is follow-up. */

#include <kernel/io_uring.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <kernel/hal.h>
#include <kernel/sync.h>
#include <kernel/kprintf.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/vm.h>
#include <kernel/mm/mmu.h>
#include <kernel/lib/string.h>
#include <kernel/board.h>

#define IORING_MAX_ENTRIES 4096

/* One ordered struct per ring fd. The three regions are laid out so the SQ and
 * CQ rings can share a page (FEAT_SINGLE_MMAP); the SQE array is its own page. */
typedef struct io_ring {
	uint32_t sq_entries;
	uint32_t cq_entries;
	uint32_t sq_mask;
	uint32_t cq_mask;

	void    *ring_phys;   /* buddy block: SQ ring header + array, then CQ ring */
	void    *sqes_phys;   /* buddy block: io_uring_sqe[sq_entries] */
	uint32_t ring_order;
	uint32_t sqes_order;

	uint8_t *ring;        /* uncached kernel alias of ring_phys */
	struct io_uring_sqe *sqes; /* uncached kernel alias of sqes_phys */

	/* pointers into *ring (the layout userspace sees via the offsets struct) */
	volatile uint32_t *sq_head, *sq_tail, *sq_dropped, *sq_array, *sq_flags;
	volatile uint32_t *cq_head, *cq_tail, *cq_overflow, *cq_flags;
	struct io_uring_cqe *cqes;

	uint32_t sq_local_head; /* last SQ index the kernel consumed */

	spinlock_t   lock;      /* serializes enter() on this ring */
	wait_queue_t cq_wait;   /* waiters in io_uring_enter(GETEVENTS) */
} io_ring_t;

/* round x up to a power of two (x in 1..IORING_MAX_ENTRIES) */
static uint32_t roundup_pow2(uint32_t x)
{
	uint32_t p = 1;
	while (p < x) p <<= 1;
	return p;
}

static int order_for(size_t bytes)
{
	int order = 0;
	size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	while ((1u << order) < pages) order++;
	return order;
}

static void io_ring_free(io_ring_t *r)
{
	if (!r) return;
	if (r->ring_phys) buddy_free((uint64_t)r->ring_phys, r->ring_order);
	if (r->sqes_phys) buddy_free((uint64_t)r->sqes_phys, r->sqes_order);
	/* The ioremap windows are never reclaimed (matches ioremap's bump model). */
	kfree(r);
}

static int io_ring_close(vfs_file_t *f)
{
	io_ring_free((io_ring_t *)f->priv);
	f->priv = NULL;
	return 0;
}

/* mmap fop: hand back the physical page backing the requested ring offset so
 * sys_mmap can map it into userspace. */
static uint64_t io_ring_mmap(vfs_file_t *f, off_t off)
{
	io_ring_t *r = (io_ring_t *)f->priv;
	if (!r) return 0;
	uint64_t o = (uint64_t)off;
	if (o >= IORING_OFF_SQES) {
		uint64_t d = o - IORING_OFF_SQES;
		if (d >= ((uint64_t)1 << (r->sqes_order + PAGE_SHIFT))) return 0;
		return (uint64_t)r->sqes_phys + d;
	}
	/* SQ and CQ rings live in the same block; CQ just sits at a higher offset
	 * inside it but userspace addresses it via IORING_OFF_CQ_RING. Both map to
	 * the start of ring_phys (single-mmap), offset applied by the cq_off. */
	uint64_t d = (o >= IORING_OFF_CQ_RING) ? (o - IORING_OFF_CQ_RING) : o;
	if (d >= ((uint64_t)1 << (r->ring_order + PAGE_SHIFT))) return 0;
	return (uint64_t)r->ring_phys + d;
}

static vfs_fops_t io_ring_fops = {
	.close = io_ring_close,
	.mmap  = io_ring_mmap,
};

static vfs_iops_t io_ring_iops = { 0 };

long io_uring_setup(uint32_t entries, struct io_uring_params *uparams)
{
	struct io_uring_params p;
	if (copy_from_user(&p, uparams, sizeof(p)) != 0) return -14;
	if (entries == 0 || entries > IORING_MAX_ENTRIES) return -22;
	/* No flags supported yet (no SQPOLL/IOPOLL/CQSIZE); reject the rest so a
	 * caller relying on them fails loudly rather than silently degrading. */
	if (p.flags != 0) return -22;
	for (int i = 0; i < 4; i++) if (((uint32_t *)&p.resv)[i]) return -22;

	uint32_t sq = roundup_pow2(entries);
	uint32_t cq = sq * 2;   /* Linux default: CQ twice the SQ */

	io_ring_t *r = kmalloc(sizeof(*r));
	if (!r) return -12;
	memset(r, 0, sizeof(*r));
	r->sq_entries = sq;
	r->cq_entries = cq;
	r->sq_mask    = sq - 1;
	r->cq_mask    = cq - 1;
	r->lock       = (spinlock_t)SPINLOCK_INIT;
	wait_queue_init(&r->cq_wait);

	/* Ring block layout (single mmap region):
	 *   [0]            sq_head,sq_tail,sq_ring_mask,sq_ring_entries,sq_flags,
	 *                  sq_dropped   (the SQ header — we place fields by hand)
	 *   [cq base]      cq_head,cq_tail,...               (CQ header)
	 *   sq array       uint32_t[sq]                      (SQ index → SQE slot)
	 *   cqes           io_uring_cqe[cq] */
	uint32_t sq_off_head     = 0;
	uint32_t sq_off_tail     = 64;
	uint32_t sq_off_mask     = 128;
	uint32_t sq_off_entries  = 132;
	uint32_t sq_off_flags    = 136;
	uint32_t sq_off_dropped  = 140;
	uint32_t sq_off_array    = 256;
	uint32_t cq_base         = sq_off_array + sq * sizeof(uint32_t);
	cq_base = (cq_base + 63) & ~63u;
	uint32_t cq_off_head     = cq_base + 0;
	uint32_t cq_off_tail     = cq_base + 64;
	uint32_t cq_off_mask     = cq_base + 128;
	uint32_t cq_off_entries  = cq_base + 132;
	uint32_t cq_off_overflow = cq_base + 136;
	uint32_t cq_off_flags    = cq_base + 140;
	uint32_t cq_off_cqes     = cq_base + 256;
	size_t   ring_bytes      = cq_off_cqes + cq * sizeof(struct io_uring_cqe);

	r->ring_order = order_for(ring_bytes);
	r->sqes_order = order_for(sq * sizeof(struct io_uring_sqe));
	r->ring_phys  = buddy_alloc(r->ring_order);
	r->sqes_phys  = buddy_alloc(r->sqes_order);
	if (!r->ring_phys || !r->sqes_phys) { io_ring_free(r); return -12; }

	memset((void *)PHYS_TO_VIRT((uint64_t)r->ring_phys), 0, (size_t)PAGE_SIZE << r->ring_order);
	memset((void *)PHYS_TO_VIRT((uint64_t)r->sqes_phys), 0, (size_t)PAGE_SIZE << r->sqes_order);

	r->ring = ioremap((uint64_t)r->ring_phys, (size_t)PAGE_SIZE << r->ring_order,
	                  VM_READ | VM_WRITE | VM_NOCACHE);
	r->sqes = ioremap((uint64_t)r->sqes_phys, (size_t)PAGE_SIZE << r->sqes_order,
	                  VM_READ | VM_WRITE | VM_NOCACHE);
	if (!r->ring || !r->sqes) { io_ring_free(r); return -12; }

	r->sq_head    = (volatile uint32_t *)(r->ring + sq_off_head);
	r->sq_tail    = (volatile uint32_t *)(r->ring + sq_off_tail);
	r->sq_dropped = (volatile uint32_t *)(r->ring + sq_off_dropped);
	r->sq_flags   = (volatile uint32_t *)(r->ring + sq_off_flags);
	r->sq_array   = (volatile uint32_t *)(r->ring + sq_off_array);
	r->cq_head    = (volatile uint32_t *)(r->ring + cq_off_head);
	r->cq_tail    = (volatile uint32_t *)(r->ring + cq_off_tail);
	r->cq_overflow= (volatile uint32_t *)(r->ring + cq_off_overflow);
	r->cq_flags   = (volatile uint32_t *)(r->ring + cq_off_flags);
	r->cqes       = (struct io_uring_cqe *)(r->ring + cq_off_cqes);
	*(volatile uint32_t *)(r->ring + sq_off_mask)    = r->sq_mask;
	*(volatile uint32_t *)(r->ring + sq_off_entries) = sq;
	*(volatile uint32_t *)(r->ring + cq_off_mask)    = r->cq_mask;
	*(volatile uint32_t *)(r->ring + cq_off_entries) = cq;

	vfs_inode_t *inode = kmalloc(sizeof(*inode));
	vfs_file_t  *file  = kmalloc(sizeof(*file));
	if (!inode || !file) { kfree(inode); kfree(file); io_ring_free(r); return -12; }
	memset(inode, 0, sizeof(*inode));
	inode->mode = S_IFREG | 0600;
	inode->fops = &io_ring_fops;
	inode->iops = &io_ring_iops;
	/* cached=0: fput() frees this inode when the last fd ref drops, which is
	 * also where io_ring_close() reclaims the ring pages. */
	memset(file, 0, sizeof(*file));
	file->flags    = O_RDWR;
	file->refcount = 0;
	file->priv     = r;
	file->inode    = inode;

	process_t *proc = task_current();
	int fd = fd_alloc(proc->fds, file);
	if (fd < 0) { kfree(file); kfree(inode); io_ring_free(r); return -24; }

	p.sq_entries = sq;
	p.cq_entries = cq;
	p.features   = IORING_FEAT_SINGLE_MMAP;
	p.sq_off = (struct io_sqring_offsets){
		.head = sq_off_head, .tail = sq_off_tail, .ring_mask = sq_off_mask,
		.ring_entries = sq_off_entries, .flags = sq_off_flags,
		.dropped = sq_off_dropped, .array = sq_off_array };
	p.cq_off = (struct io_cqring_offsets){
		.head = cq_off_head, .tail = cq_off_tail, .ring_mask = cq_off_mask,
		.ring_entries = cq_off_entries, .overflow = cq_off_overflow,
		.cqes = cq_off_cqes, .flags = cq_off_flags };
	if (copy_to_user(uparams, &p, sizeof(p)) != 0) { vfs_close(fd); return -14; }
	return fd;
}

/* Post one completion. The CQ has cq_entries slots; if userspace hasn't reaped
 * and it's full, bump the overflow counter (Linux semantics) and drop. */
static void cqe_post(io_ring_t *r, uint64_t user_data, int32_t res)
{
	uint32_t tail = *r->cq_tail;
	uint32_t head = *r->cq_head;
	if (tail - head >= r->cq_entries) { (*r->cq_overflow)++; return; }
	struct io_uring_cqe *c = &r->cqes[tail & r->cq_mask];
	c->user_data = user_data;
	c->res       = res;
	c->flags     = 0;
	__sync_synchronize();      /* fill the CQE before publishing the new tail */
	*r->cq_tail = tail + 1;
}

/* Run one SQE synchronously against the file/socket layer and return the result
 * to place in res. user_data is copied straight to the CQE by the caller. */
static int32_t sqe_execute(const struct io_uring_sqe *s)
{
	process_t *p = task_current();

	switch (s->opcode) {
	case IORING_OP_NOP:
		return 0;

	case IORING_OP_READ:
	case IORING_OP_RECV: {
		void *buf = (void *)(uintptr_t)s->addr;
		if (!access_ok(buf, s->len)) return -14;
		vfs_file_t *f = fget(p->fds, s->fd);
		if (!f) return -9;
		int32_t res;
		if (!f->inode->fops || !f->inode->fops->read) { res = -22; }
		else {
			vfs_file_t tmp = *f;
			/* off==-1 means "use the file's current position" (Linux) */
			if (s->opcode == IORING_OP_READ && s->off != (uint64_t)-1) tmp.pos = (off_t)s->off;
			res = (int32_t)f->inode->fops->read(&tmp, buf, s->len);
			if (s->opcode == IORING_OP_READ && s->off == (uint64_t)-1 && res > 0) f->pos = tmp.pos;
		}
		fput(f);
		return res;
	}

	case IORING_OP_WRITE:
	case IORING_OP_SEND: {
		const void *buf = (const void *)(uintptr_t)s->addr;
		if (!access_ok(buf, s->len)) return -14;
		vfs_file_t *f = fget(p->fds, s->fd);
		if (!f) return -9;
		int32_t res;
		if (!f->inode->fops || !f->inode->fops->write) { res = -22; }
		else {
			vfs_file_t tmp = *f;
			if (s->opcode == IORING_OP_WRITE && s->off != (uint64_t)-1) tmp.pos = (off_t)s->off;
			res = (int32_t)f->inode->fops->write(&tmp, buf, s->len);
			if (s->opcode == IORING_OP_WRITE && s->off == (uint64_t)-1 && res > 0) f->pos = tmp.pos;
		}
		fput(f);
		return res;
	}

	case IORING_OP_READV:
	case IORING_OP_WRITEV: {
		const struct { void *base; size_t len; } *uiov =
			(const void *)(uintptr_t)s->addr;
		if (!access_ok(uiov, (size_t)s->len * sizeof(*uiov))) return -14;
		vfs_file_t *f = fget(p->fds, s->fd);
		if (!f) return -9;
		int is_write = (s->opcode == IORING_OP_WRITEV);
		vfs_fops_t *fo = f->inode->fops;
		int32_t total = 0, res = 0;
		if (!fo || (is_write ? !fo->write : !fo->read)) { fput(f); return -22; }
		vfs_file_t tmp = *f;
		if (s->off != (uint64_t)-1) tmp.pos = (off_t)s->off;
		for (uint32_t i = 0; i < s->len; i++) {
			struct { void *base; size_t len; } iov;
			if (copy_from_user(&iov, &uiov[i], sizeof(iov)) != 0) { res = -14; break; }
			if (!iov.len) continue;
			if (!access_ok(iov.base, iov.len)) { res = -14; break; }
			ssize_t r = is_write ? fo->write(&tmp, iov.base, iov.len)
			                     : fo->read(&tmp, iov.base, iov.len);
			if (r < 0) { res = (int32_t)r; break; }
			total += (int32_t)r;
			if (r == 0 || (size_t)r < iov.len) break;
		}
		fput(f);
		return res < 0 ? res : total;
	}

	case IORING_OP_FSYNC:
		return 0;   /* ramfs/tmpfs are memory-backed: durable by definition */

	default:
		return -22; /* EINVAL: unsupported opcode */
	}
}

/* Drain SQEs that userspace published (sq_tail advanced past our local head),
 * execute each and post its CQE. Returns the number submitted. */
static uint32_t submit_sqes(io_ring_t *r, uint32_t to_submit)
{
	uint32_t tail = *r->sq_tail;
	__sync_synchronize();   /* read the SQEs userspace wrote before the tail */
	uint32_t submitted = 0;
	while (submitted < to_submit && r->sq_local_head != tail) {
		uint32_t idx = r->sq_array[r->sq_local_head & r->sq_mask];
		if (idx >= r->sq_entries) { (*r->sq_dropped)++; r->sq_local_head++; continue; }
		struct io_uring_sqe s = r->sqes[idx];   /* snapshot out of the shared page */
		int32_t res = sqe_execute(&s);
		cqe_post(r, s.user_data, res);
		r->sq_local_head++;
		submitted++;
	}
	*r->sq_head = r->sq_local_head;   /* publish consumption to userspace */
	return submitted;
}

long io_uring_enter(int fd, uint32_t to_submit, uint32_t min_complete,
                    uint32_t flags, const void *argp, size_t argsz)
{
	(void)argp; (void)argsz;
	process_t *p = task_current();
	vfs_file_t *f = fget(p->fds, fd);
	if (!f) return -9;
	if (f->inode->fops != &io_ring_fops) { fput(f); return -22; }
	io_ring_t *r = (io_ring_t *)f->priv;

	uint64_t lf = spin_lock_irq(&r->lock);
	uint32_t submitted = submit_sqes(r, to_submit);
	spin_unlock_irq(&r->lock, lf);
	if (submitted) wake_up(&r->cq_wait);   /* let a sibling enter()-waiter re-check */

	/* Submission is synchronous, so completions are already posted; GETEVENTS
	 * only ever has to wait if min_complete exceeds what we just produced. */
	if (flags & IORING_ENTER_GETEVENTS) {
		while (min_complete && (*r->cq_tail - *r->cq_head) < min_complete) {
			if (task_has_actionable_signal()) { fput(f); return -4; }
			uint64_t wf = spin_lock_irq(&r->cq_wait.lock);
			if ((*r->cq_tail - *r->cq_head) >= min_complete) {
				spin_unlock_irq(&r->cq_wait.lock, wf);
				break;
			}
			sleep_on_locked(&r->cq_wait, &r->cq_wait.lock, wf);
		}
	}

	fput(f);
	return (long)submitted;
}

long io_uring_register(int fd, unsigned int opcode, void *arg, unsigned int nr_args)
{
	(void)arg; (void)nr_args;
	process_t *p = task_current();
	vfs_file_t *f = fget(p->fds, fd);
	if (!f) return -9;
	if (f->inode->fops != &io_ring_fops) { fput(f); return -22; }
	fput(f);
	/* Buffer/file pre-registration is an optimization; accept the calls but do
	 * not pin anything yet (ops resolve fds/buffers per-SQE). */
	switch (opcode) {
	case IORING_REGISTER_BUFFERS:
	case IORING_UNREGISTER_BUFFERS:
	case IORING_REGISTER_FILES:
	case IORING_UNREGISTER_FILES:
		return 0;
	default:
		return -22;
	}
}
