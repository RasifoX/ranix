/* i915 uAPI on Ranix: GETPARAM, QUERY, GEM create/pwrite/pread/mmap_offset,
 * contexts with an engine map, EXECBUFFER2 (softpin and relocations) running
 * MI_STORE_DWORD on the render engine, GEM_WAIT/BUSY, fences. Raw ioctls, no
 * libdrm. Prints OK, or OK when the machine has no i915 device. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>

struct drm_version { int major, minor, patch; size_t name_len; char *name; size_t date_len; char *date; size_t desc_len; char *desc; };
struct getparam { int32_t param; uint32_t pad; uint64_t value; };
struct gem_create { uint64_t size; uint32_t handle, pad; };
struct gem_rw { uint32_t handle, pad; uint64_t offset, size, data_ptr; };
struct mmap_offset { uint32_t handle, pad; uint64_t offset, flags, extensions; };
struct gem_wait { uint32_t handle, flags; int64_t timeout_ns; };
struct gem_busy { uint32_t handle, busy; };
struct gem_close { uint32_t handle, pad; };
struct user_ext { uint64_t next; uint32_t name, flags; uint32_t rsvd[4]; };
struct ctx_param { uint32_t ctx_id, size; uint64_t param, value; };
struct ctx_create_ext { uint32_t ctx_id, flags; uint64_t extensions; };
struct ctx_setparam_ext { struct user_ext base; struct ctx_param param; };
struct ctx_destroy { uint32_t ctx_id, pad; };
struct engines { uint64_t extensions; uint16_t cls, inst; };
struct execbuf2 { uint64_t buffers_ptr; uint32_t count, start, len, dr1, dr4, ncliprects; uint64_t cliprects, flags, rsvd1, rsvd2; };
struct exec_obj { uint32_t handle, nrelocs; uint64_t relocs, alignment, offset, flags, rsvd1, rsvd2; };
struct reloc { uint32_t target, delta; uint64_t offset, presumed; uint32_t rd, wr; };
struct query_item { uint64_t id; int32_t length; uint32_t flags; uint64_t data; };
struct query { uint32_t n, flags; uint64_t items; };
struct exec_fence { uint32_t handle, flags; };
struct syncobj_create { uint32_t handle, flags; };
struct syncobj_wait { uint64_t handles; int64_t timeout; uint32_t n, flags, first, pad; uint64_t deadline; };

#define IOC_VERSION     _IOWR('d', 0x00, struct drm_version)
#define IOC_GEM_CLOSE   _IOW('d', 0x09, struct gem_close)
#define IOC_SO_CREATE   _IOWR('d', 0xBF, struct syncobj_create)
#define IOC_SO_WAIT     _IOWR('d', 0xC3, struct syncobj_wait)
#define I(nr, t)        _IOWR('d', 0x40 + (nr), t)
#define IOC_GETPARAM    I(0x06, struct getparam)
#define IOC_BUSY        I(0x17, struct gem_busy)
#define IOC_CREATE      I(0x1b, struct gem_create)
#define IOC_PREAD       _IOW('d', 0x40 + 0x1c, struct gem_rw)
#define IOC_PWRITE      _IOW('d', 0x40 + 0x1d, struct gem_rw)
#define IOC_MMAP_OFFSET I(0x24, struct mmap_offset)
#define IOC_EXECBUF     _IOWR('d', 0x40 + 0x29, struct execbuf2)
#define IOC_WAIT        I(0x2c, struct gem_wait)
#define IOC_CTX_CREATE  I(0x2d, struct ctx_create_ext)
#define IOC_CTX_DESTROY _IOW('d', 0x40 + 0x2e, struct ctx_destroy)
#define IOC_CTX_GETP    I(0x34, struct ctx_param)
#define IOC_QUERY       I(0x39, struct query)

#define MI_STORE_DWORD  ((0x20u << 23) | 2)
#define MI_BB_END       (0x0Au << 23)
#define MI_LRI_1        ((0x22u << 23) | 1)          /* MI_LOAD_REGISTER_IMM, one register */
#define MI_SRM          ((0x24u << 23) | 2)          /* MI_STORE_REGISTER_MEM, 64-bit address */
#define CS_GPR0         0x2600                       /* render CS GPR 0: saved with the context */

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

static int xioctl(int fd, unsigned long r, void *a)
{
	int e;
	do e = ioctl(fd, r, a); while (e < 0 && errno == EINTR);
	return e < 0 ? -errno : 0;
}

static uint32_t create(int fd, uint64_t size)
{
	struct gem_create c = { .size = size };
	return xioctl(fd, IOC_CREATE, &c) ? 0 : c.handle;
}

static void *map(int fd, uint32_t h, size_t size)
{
	struct mmap_offset m = { .handle = h, .flags = 2 /* WB */ };
	if (xioctl(fd, IOC_MMAP_OFFSET, &m)) return MAP_FAILED;
	return mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.offset);
}

static int64_t getparam(int fd, int p)
{
	int v = -1;
	struct getparam g = { .param = p, .value = (uintptr_t)&v };
	int e = xioctl(fd, IOC_GETPARAM, &g);
	return e ? e : v;
}

static void wait_obj(int fd, uint32_t h)
{
	struct gem_wait w = { .handle = h, .timeout_ns = 2000000000LL };
	CHECK(xioctl(fd, IOC_WAIT, &w) == 0, "GEM_WAIT");
}

/* 200 small batches, each waited for: the mean round trip in microseconds */
static double round_trip(int fd, struct execbuf2 *eb, uint32_t *bm, uint32_t *tm, uint32_t T)
{
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < 200 && !fails; i++) {
		bm[3] = 0x1000 + i;
		CHECK(xioctl(fd, IOC_EXECBUF, eb) == 0, "execbuf %d", i);
		wait_obj(fd, T);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	CHECK(tm[0x40] == 0x1000 + 199, "last of 200: 0x%x", tm[0x40]);
	return ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e3 / 200;
}

/* ranix_gt (root only): the interrupt line; 0 when not readable */
static int gt_irq_state(int *on, unsigned long *count, unsigned *spin_us)
{
	char st[2048] = {0};
	int f = open("/sys/class/drm/ranix_gt", O_RDONLY);
	if (f < 0) return 0;
	ssize_t k = read(f, st, sizeof st - 1); (void)k;
	close(f);
	const char *p = strstr(st, "irq: ");
	if (!p) return 0;
	unsigned vec = 0;
	*on = !strncmp(p, "irq: on", 7);
	const char *q = strstr(p, " vector ");
	return q && sscanf(q, " vector %u count %lu empty %*u spin_us %u", &vec, count, spin_us) == 3;
}

/* With the engine's interrupt on, each request raises at least one interrupt and
 * a waiter wakes well inside a tick (a lost interrupt would cost ~1000 us on the
 * tick fallback). Reads ranix_gt only: changes no setting, so a kill is harmless. */
static void irq_checks(int fd, struct execbuf2 *eb, uint32_t *bm, uint32_t *tm, uint32_t T)
{
	int on = 0; unsigned long c0 = 0, c1 = 0; unsigned spin = 0;
	if (!gt_irq_state(&on, &c0, &spin)) return;          /* not root, or an older kernel */
	if (!on) { fprintf(stderr, "engine interrupt not on: polled\n"); return; }
	double us = round_trip(fd, eb, bm, tm, T);
	gt_irq_state(&on, &c1, &spin);
	fprintf(stderr, "interrupt: %lu for 200 requests, round trip %.1f us (waiter polls %u us)\n", c1 - c0, us, spin);
	CHECK(c1 - c0 >= 200, "interrupts: %lu for 200 requests", c1 - c0);
	CHECK(us < 600, "interrupt wake %.1f us", us);
}

/* ranix_gt changes engine settings: nobody but root may write it */
/* Two slots of one engine map are two hardware contexts, as in i915: a register
 * set on slot 1 does not show on slot 0. Mesa's render and compute batches use
 * two slots and each relies on the state it left there. */
static void engine_slots_separate(int fd, uint32_t T, uint32_t *tm)
{
	struct engines2 { uint64_t extensions; uint16_t c0, i0, c1, i1; } en = { 0, 0, 0, 0, 0 };
	struct ctx_setparam_ext sp = { .param = { .size = sizeof(en), .param = 0xa, .value = (uintptr_t)&en } };
	struct ctx_create_ext cc = { .flags = 1, .extensions = (uintptr_t)&sp };
	CHECK(xioctl(fd, IOC_CTX_CREATE, &cc) == 0, "CONTEXT_CREATE_EXT [rcs0, rcs0]");
	uint32_t B = create(fd, 4096), *bm = map(fd, B, 4096);
	CHECK(B && bm != MAP_FAILED, "slot batch object");
	if (!B || bm == MAP_FAILED || fails) return;
	uint32_t prog[3][5] = {
		{ MI_LRI_1, CS_GPR0, 0x11111111, MI_BB_END, 0 },                   /* slot 0 */
		{ MI_LRI_1, CS_GPR0, 0x22222222, MI_BB_END, 0 },                   /* slot 1 */
		{ MI_SRM, CS_GPR0, 0x200000 + 0x200, 0, MI_BB_END },               /* slot 0 again */
	};
	tm[0x80] = 0;
	struct exec_obj o[2] = {
		{ .handle = T, .offset = 0x200000, .flags = (1 << 4) | (1 << 3) | (1 << 2) },
		{ .handle = B, .offset = 0x400000, .flags = (1 << 4) | (1 << 3) },
	};
	for (int i = 0; i < 3; i++) {
		memcpy(bm + i * 16, prog[i], sizeof prog[i]);
		struct execbuf2 eb = { .buffers_ptr = (uintptr_t)o, .count = 2, .start = i * 64,
		                       .flags = i == 1 ? 1 : 0, .rsvd1 = cc.ctx_id };
		int e = xioctl(fd, IOC_EXECBUF, &eb);
		CHECK(e == 0, "execbuf on slot %d: %d", i == 1, e);
		wait_obj(fd, T);
	}
	CHECK(tm[0x80] == 0x11111111, "slot 0 sees 0x%x in GPR0, want its own 0x11111111", tm[0x80]);
	struct ctx_destroy cd = { .ctx_id = cc.ctx_id };
	CHECK(xioctl(fd, IOC_CTX_DESTROY, &cd) == 0, "destroy [rcs0, rcs0]");
	struct gem_close gc = { .handle = B };
	munmap(bm, 4096);
	CHECK(xioctl(fd, IOC_GEM_CLOSE, &gc) == 0, "close slot batch");
}

static void gt_node_closed_to_users(void)
{
	if (geteuid() != 0 || access("/sys/class/drm/ranix_gt", F_OK)) return;
	pid_t p = fork();
	if (p == 0) {
		if (setgid(65534) || setuid(65534)) _exit(2);
		int f = open("/sys/class/drm/ranix_gt", O_WRONLY);
		int wrote = f >= 0 && write(f, "irq off", 7) == 7;
		_exit(f < 0 && !wrote ? 0 : 1);
	}
	int st = -1;
	if (p > 0) waitpid(p, &st, 0);
	CHECK(p > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0, "ranix_gt writable by uid 65534 (status %d)", st);
}

int main(void)
{
	gt_node_closed_to_users();
	int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	char name[16] = {0};
	struct drm_version v = { .name_len = sizeof(name) - 1, .name = name };
	if (fd < 0 || xioctl(fd, IOC_VERSION, &v) || strcmp(name, "i915")) {
		fprintf(stderr, "no i915 device: skipped\n");
		if (fails) { printf("FAIL %d\n", fails); return 1; }
		puts("OK");
		return 0;
	}

	/* GETPARAM */
	CHECK(getparam(fd, 4) > 0, "CHIPSET_ID");
	CHECK(getparam(fd, 37) == 1, "HAS_EXEC_SOFTPIN");
	CHECK(getparam(fd, 50) == 1, "HAS_CONTEXT_ISOLATION");
	CHECK(getparam(fd, 9999) == -EINVAL, "unknown param");

	/* QUERY: engine info, memory regions (two passes: size, then data) */
	struct query_item qi[2] = { { .id = 2 }, { .id = 4 } };
	struct query q = { .n = 2, .items = (uintptr_t)qi };
	CHECK(xioctl(fd, IOC_QUERY, &q) == 0 && qi[0].length > 0 && qi[1].length > 0, "QUERY sizes");
	uint8_t eng[256], mem[256];
	qi[0].data = (uintptr_t)eng; qi[1].data = (uintptr_t)mem;
	CHECK(qi[0].length <= 256 && qi[1].length <= 256 && xioctl(fd, IOC_QUERY, &q) == 0, "QUERY data");
	CHECK(*(uint32_t *)eng == 1 && *(uint32_t *)mem == 1, "one render engine, one region");

	/* objects */
	uint32_t T = create(fd, 4096), B = create(fd, 4096);
	CHECK(T && B, "GEM_CREATE");
	uint32_t pat[4] = { 1, 2, 3, 4 }, back[4] = {0};
	struct gem_rw w = { .handle = T, .offset = 64, .size = 16, .data_ptr = (uintptr_t)pat };
	CHECK(xioctl(fd, IOC_PWRITE, &w) == 0, "PWRITE");
	struct gem_rw r = { .handle = T, .offset = 64, .size = 16, .data_ptr = (uintptr_t)back };
	CHECK(xioctl(fd, IOC_PREAD, &r) == 0 && !memcmp(pat, back, 16), "PREAD");
	uint32_t *tm = map(fd, T, 4096), *bm = map(fd, B, 4096);
	CHECK(tm != MAP_FAILED && bm != MAP_FAILED && tm[16] == 1, "MMAP_OFFSET");
	if (fails) { printf("FAIL %d\n", fails); return 1; }

	/* a context with an engine map: [render] */
	struct engines en = { 0, 0, 0 };
	struct ctx_setparam_ext sp = { .base = { .name = 0 }, .param = { .size = sizeof(en), .param = 0xa, .value = (uintptr_t)&en } };
	struct ctx_create_ext cc = { .flags = 1, .extensions = (uintptr_t)&sp };
	CHECK(xioctl(fd, IOC_CTX_CREATE, &cc) == 0 && cc.ctx_id, "CONTEXT_CREATE_EXT engines");
	struct ctx_param gp = { .ctx_id = cc.ctx_id, .param = 0x3 };
	CHECK(xioctl(fd, IOC_CTX_GETP, &gp) == 0 && gp.value == 1ULL << 48, "GTT_SIZE");

	/* softpinned: the batch stores a dword into T */
	bm[0] = MI_STORE_DWORD; bm[1] = 0x200000 + 0x100; bm[2] = 0; bm[3] = 0xC0FFEE; bm[4] = MI_BB_END;
	struct exec_obj o[2] = {
		{ .handle = T, .offset = 0x200000, .flags = (1 << 4) | (1 << 3) | (1 << 2) },
		{ .handle = B, .offset = 0x300000, .flags = (1 << 4) | (1 << 3) },
	};
	struct execbuf2 eb = { .buffers_ptr = (uintptr_t)o, .count = 2, .flags = 0, .rsvd1 = cc.ctx_id };
	int e = xioctl(fd, IOC_EXECBUF, &eb);
	CHECK(e == 0, "EXECBUFFER2 softpin: %d", e);
	if (!e) { wait_obj(fd, T); CHECK(tm[0x40] == 0xC0FFEE, "GPU wrote 0x%x", tm[0x40]); }
	struct gem_busy bz = { .handle = T };
	CHECK(xioctl(fd, IOC_BUSY, &bz) == 0 && bz.busy == 0, "BUSY after wait");

	engine_slots_separate(fd, T, tm);

	/* relocation into a kernel-placed object, default context */
	uint32_t T2 = create(fd, 4096), B2 = create(fd, 4096);
	uint32_t *t2m = map(fd, T2, 4096), *b2m = map(fd, B2, 4096);
	b2m[0] = MI_STORE_DWORD; b2m[1] = 0; b2m[2] = 0; b2m[3] = 0xBEEF; b2m[4] = MI_BB_END;
	struct reloc rl = { .target = T2, .delta = 8, .offset = 4 };
	struct exec_obj o2[2] = { { .handle = T2, .flags = 1 << 2 }, { .handle = B2, .nrelocs = 1, .relocs = (uintptr_t)&rl } };
	struct execbuf2 eb2 = { .buffers_ptr = (uintptr_t)o2, .count = 2, .flags = 1 /* RENDER */ };
	e = xioctl(fd, IOC_EXECBUF, &eb2);
	CHECK(e == 0 && o2[0].offset && o2[0].offset < (1ULL << 32), "EXECBUFFER2 reloc: %d off 0x%llx", e, (unsigned long long)o2[0].offset);
	if (!e) { wait_obj(fd, T2); CHECK(t2m[2] == 0xBEEF, "reloc write 0x%x", t2m[2]); }

	/* many requests back to back, then a syncobj signaled through the fence array */
	double us = round_trip(fd, &eb, bm, tm, T);
	fprintf(stderr, "execbuf+wait round trip %.1f us\n", us);
	irq_checks(fd, &eb, bm, tm, T);
	/* GGTT audit (root): our window holds only scratch or RAM pages, nothing refused */
	{
		int af = open("/sys/class/drm/ranix_gt", O_RDWR);
		if (af >= 0 && write(af, "audit", 5) == 5) {
			char st[2048] = {0};
			lseek(af, 0, SEEK_SET);
			ssize_t k = read(af, st, sizeof st - 1); (void)k;
			const char *a = strstr(st, "audit: ");
			unsigned long ws = 0, wr_ = 0, bad = 1, refused = 1;
			if (a) {
				const char *b = strstr(a, " BAD "), *r = strstr(a, "refused maps ");
				sscanf(a, "audit: window scratch %lu ram %lu", &ws, &wr_);
				if (b) bad = strtoul(b + 5, NULL, 10);
				if (r) refused = strtoul(r + 13, NULL, 10);
			}
			CHECK(a && ws > 0 && bad == 0 && refused == 0, "ggtt audit: %.200s", a ? a : st);
		}
		if (af >= 0) close(af);
	}
	/* the frequency governor (ranix_gt "rps:" line): its request stays within its limits */
	{
		char st[2048] = {0};
		int sf = open("/sys/class/drm/ranix_gt", O_RDONLY);
		if (sf >= 0) { ssize_t k = read(sf, st, sizeof st - 1); (void)k; close(sf); }
		const char *r = strstr(st, "rps: on");
		if (r) {
			unsigned req = 0, lo = 0, hi = 0, rp1 = 0, rp0 = 0;
			int got = sscanf(r, "rps: on req_MHz %u min_MHz %u max_MHz %u rp1_MHz %u rp0_MHz %u", &req, &lo, &hi, &rp1, &rp0);
			CHECK(got == 5 && lo <= req && req <= hi && lo <= rp1 && rp1 <= rp0 && hi <= rp0, "rps line: %.120s", r);
			fprintf(stderr, "GT request %u MHz (%u-%u, RP1 %u)\n", req, lo, rp0, rp1);
		}
	}
	struct syncobj_create so = {0};
	CHECK(xioctl(fd, IOC_SO_CREATE, &so) == 0, "SYNCOBJ_CREATE");
	struct exec_fence ef = { .handle = so.handle, .flags = 2 /* SIGNAL */ };
	eb.flags = (1 << 19) | (1 << 17);           /* FENCE_ARRAY | FENCE_OUT */
	eb.ncliprects = 1; eb.cliprects = (uintptr_t)&ef;
	CHECK(xioctl(fd, IOC_EXECBUF, &eb) == 0, "execbuf with fences");
	struct syncobj_wait sw = { .handles = (uintptr_t)&so.handle, .n = 1 };
	CHECK(xioctl(fd, IOC_SO_WAIT, &sw) == 0, "syncobj signaled");
	int ofd = (int)(eb.rsvd2 >> 32);
	struct pollfd pf = { .fd = ofd, .events = POLLIN };
	CHECK(ofd > 0 && poll(&pf, 1, 0) == 1, "out fence");
	if (ofd > 0) close(ofd);

	struct ctx_destroy cd = { .ctx_id = cc.ctx_id };
	CHECK(xioctl(fd, IOC_CTX_DESTROY, &cd) == 0, "CONTEXT_DESTROY");
	CHECK(xioctl(fd, IOC_CTX_DESTROY, &cd) == -ENOENT, "destroy twice");
	struct gem_close gc = { .handle = T };
	CHECK(xioctl(fd, IOC_GEM_CLOSE, &gc) == 0, "GEM_CLOSE");
	close(fd);
	if (fails) { printf("FAIL %d\n", fails); return 1; }
	puts("OK");
	return 0;
}
