/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* DRM/KMS uAPI: /dev/dri/card0 and renderD128 as libdrm, Mesa's GBM and
 * compositors use them. Raw ioctls, no libdrm. Prints OK (or OK when the machine
 * has no DRM device). "-v SECONDS" also runs a visible page-flip animation and
 * reports the frame rate. */
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
#include <sys/stat.h>

struct drm_version { int major, minor, patch; size_t name_len; char *name; size_t date_len; char *date; size_t desc_len; char *desc; };
struct drm_get_cap { uint64_t capability, value; };
struct drm_set_client_cap { uint64_t capability, value; };
struct drm_gem_close { uint32_t handle, pad; };
struct drm_gem_flink { uint32_t handle, name; };
struct drm_gem_open { uint32_t name, handle; uint64_t size; };
struct drm_prime_handle { uint32_t handle, flags; int32_t fd; };
struct drm_mode_modeinfo {
	uint32_t clock; uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan; uint32_t vrefresh, flags, type; char name[32];
};
struct drm_mode_card_res { uint64_t fb, crtc, conn, enc; uint32_t nfb, ncrtc, nconn, nenc, minw, maxw, minh, maxh; };
struct drm_mode_crtc { uint64_t conns; uint32_t nconn, crtc_id, fb_id, x, y, gamma_size, mode_valid; struct drm_mode_modeinfo mode; };
struct drm_mode_get_encoder { uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones; };
struct drm_mode_get_connector {
	uint64_t encs, modes, props, prop_values; uint32_t nmodes, nprops, nencs;
	uint32_t encoder_id, connector_id, connector_type, connector_type_id, connection, mm_w, mm_h, subpixel, pad;
};
struct drm_mode_get_plane_res { uint64_t ids; uint32_t n; };
struct drm_mode_get_plane { uint32_t plane_id, crtc_id, fb_id, possible_crtcs, gamma_size, nfmt; uint64_t fmts; };
struct drm_mode_obj_get_properties { uint64_t props, vals; uint32_t n, obj_id, obj_type; };
struct drm_mode_property_enum { uint64_t value; char name[32]; };
struct drm_mode_get_property { uint64_t values, enums; uint32_t prop_id, flags; char name[32]; uint32_t nvalues, nenums; };
struct drm_mode_fb_cmd { uint32_t fb_id, width, height, pitch, bpp, depth, handle; };
struct drm_mode_fb_cmd2 { uint32_t fb_id, width, height, format, flags, handles[4], pitches[4], offsets[4]; uint64_t modifier[4]; };
struct drm_mode_fb_dirty_cmd { uint32_t fb_id, flags, color, nclips; uint64_t clips; };
struct drm_clip_rect { uint16_t x1, y1, x2, y2; };
struct drm_mode_cursor { uint32_t flags, crtc_id; int32_t x, y; uint32_t w, h, handle; };
struct drm_mode_crtc_page_flip { uint32_t crtc_id, fb_id, flags, reserved; uint64_t user_data; };
struct drm_mode_create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct drm_mode_map_dumb { uint32_t handle, pad; uint64_t offset; };
struct drm_event_vblank { uint32_t type, length; uint64_t user_data; uint32_t sec, usec, seq, crtc_id; };
union drm_wait_vblank {
	struct { uint32_t type, sequence; unsigned long signal; } req;
	struct { uint32_t type, sequence; long sec, usec; } rep;
};
struct drm_crtc_get_sequence { uint32_t crtc_id, active; uint64_t seq; int64_t ns; };
struct drm_syncobj_create { uint32_t handle, flags; };
struct drm_syncobj_destroy { uint32_t handle, pad; };
struct drm_syncobj_handle { uint32_t handle, flags; int32_t fd; uint32_t pad; };
struct drm_syncobj_wait { uint64_t handles; int64_t timeout; uint32_t n, flags, first, pad; uint64_t deadline; };
struct drm_syncobj_timeline_wait { uint64_t handles, points; int64_t timeout; uint32_t n, flags, first, pad; uint64_t deadline; };
struct drm_syncobj_array { uint64_t handles; uint32_t n, pad; };
struct drm_syncobj_timeline_array { uint64_t handles, points; uint32_t n, flags; };

#define D(n, t) _IOWR('d', n, t)
#define IOC_VERSION      D(0x00, struct drm_version)
#define IOC_GEM_CLOSE    _IOW('d', 0x09, struct drm_gem_close)
#define IOC_GEM_FLINK    D(0x0a, struct drm_gem_flink)
#define IOC_GEM_OPEN     D(0x0b, struct drm_gem_open)
#define IOC_GET_CAP      D(0x0c, struct drm_get_cap)
#define IOC_SET_CLIENT_CAP _IOW('d', 0x0d, struct drm_set_client_cap)
#define IOC_PRIME_H2FD   D(0x2d, struct drm_prime_handle)
#define IOC_PRIME_FD2H   D(0x2e, struct drm_prime_handle)
#define IOC_WAIT_VBLANK  D(0x3a, union drm_wait_vblank)
#define IOC_GET_SEQ      D(0x3b, struct drm_crtc_get_sequence)
#define IOC_GETRES       D(0xA0, struct drm_mode_card_res)
#define IOC_GETCRTC      D(0xA1, struct drm_mode_crtc)
#define IOC_SETCRTC      D(0xA2, struct drm_mode_crtc)
#define IOC_CURSOR       D(0xA3, struct drm_mode_cursor)
#define IOC_GETENC       D(0xA6, struct drm_mode_get_encoder)
#define IOC_GETCONN      D(0xA7, struct drm_mode_get_connector)
#define IOC_GETPROP      D(0xAA, struct drm_mode_get_property)
#define IOC_GETFB        D(0xAD, struct drm_mode_fb_cmd)
#define IOC_ADDFB        D(0xAE, struct drm_mode_fb_cmd)
#define IOC_RMFB         D(0xAF, unsigned int)
#define IOC_PAGE_FLIP    D(0xB0, struct drm_mode_crtc_page_flip)
#define IOC_DIRTYFB      D(0xB1, struct drm_mode_fb_dirty_cmd)
#define IOC_CREATE_DUMB  D(0xB2, struct drm_mode_create_dumb)
#define IOC_MAP_DUMB     D(0xB3, struct drm_mode_map_dumb)
#define IOC_DESTROY_DUMB D(0xB4, uint32_t)
#define IOC_GETPLANERES  D(0xB5, struct drm_mode_get_plane_res)
#define IOC_GETPLANE     D(0xB6, struct drm_mode_get_plane)
#define IOC_ADDFB2       D(0xB8, struct drm_mode_fb_cmd2)
#define IOC_OBJ_GETPROPS D(0xB9, struct drm_mode_obj_get_properties)
#define IOC_SO_CREATE    D(0xBF, struct drm_syncobj_create)
#define IOC_SO_DESTROY   D(0xC0, struct drm_syncobj_destroy)
#define IOC_SO_H2FD      D(0xC1, struct drm_syncobj_handle)
#define IOC_SO_FD2H      D(0xC2, struct drm_syncobj_handle)
#define IOC_SO_WAIT      D(0xC3, struct drm_syncobj_wait)
#define IOC_SO_SIGNAL    D(0xC5, struct drm_syncobj_array)
#define IOC_SO_TL_WAIT   D(0xCA, struct drm_syncobj_timeline_wait)
#define IOC_SO_QUERY     D(0xCB, struct drm_syncobj_timeline_array)
#define IOC_SO_TL_SIGNAL D(0xCD, struct drm_syncobj_timeline_array)

#define XRGB8888 0x34325258u
#define ARGB8888 0x34325241u

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
	return r < 0 ? -errno : 0;
}

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

typedef struct { uint32_t handle, pitch, fb; uint64_t size; uint32_t *map; } buf_t;

static int mkbuf(int fd, uint32_t w, uint32_t h, buf_t *b)
{
	struct drm_mode_create_dumb c = { .height = h, .width = w, .bpp = 32 };
	int e = xioctl(fd, IOC_CREATE_DUMB, &c);
	if (e) return e;
	b->handle = c.handle; b->pitch = c.pitch; b->size = c.size;
	struct drm_mode_map_dumb m = { .handle = c.handle };
	if ((e = xioctl(fd, IOC_MAP_DUMB, &m))) return e;
	b->map = mmap(0, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.offset);
	if (b->map == MAP_FAILED) return -errno;
	struct drm_mode_fb_cmd2 f = { .width = w, .height = h, .format = XRGB8888 };
	f.handles[0] = c.handle; f.pitches[0] = c.pitch;
	if ((e = xioctl(fd, IOC_ADDFB2, &f))) return e;
	b->fb = f.fb_id;
	return 0;
}

static void paint(buf_t *b, uint32_t w, uint32_t h, int frame)
{
	for (uint32_t y = 0; y < h; y++) {
		uint32_t *row = b->map + y * (b->pitch / 4);
		for (uint32_t x = 0; x < w; x++) {
			uint32_t bar = ((x + (uint32_t)frame * 8) / 64) & 7;
			row[x] = (bar & 1 ? 0xC00000u : 0) | (bar & 2 ? 0xC000u : 0) | (bar & 4 ? 0xC0u : 0) | (y * 255 / h);
		}
	}
}

/* wait for one event; returns its type, 0 on timeout */
static uint32_t wait_event(int fd, int ms, struct drm_event_vblank *out)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	if (poll(&p, 1, ms) != 1) return 0;
	char buf[256];
	ssize_t n = read(fd, buf, sizeof(buf));
	if (n < (ssize_t)sizeof(*out)) return 0;
	memcpy(out, buf, sizeof(*out));
	return out->type;
}

static void test_sysfs(int fd)
{
	struct stat st;
	CHECK(fstat(fd, &st) == 0 && S_ISCHR(st.st_mode), "fstat");
	unsigned maj = (unsigned)((st.st_rdev >> 8) & 0xfff), mn = (unsigned)((st.st_rdev & 0xff) | ((st.st_rdev >> 12) & 0xfff00));
	CHECK(maj == 226 && mn == 0, "rdev %u:%u", maj, mn);
	char p[128], l[128];
	snprintf(p, sizeof(p), "/sys/dev/char/%u:%u/device/drm", maj, mn);
	CHECK(stat(p, &st) == 0 && S_ISDIR(st.st_mode), "%s", p);
	snprintf(p, sizeof(p), "/sys/dev/char/%u:%u/device/subsystem", maj, mn);
	ssize_t n = readlink(p, l, sizeof(l) - 1);
	CHECK(n > 0, "readlink subsystem");
	if (n > 0) { l[n] = 0; CHECK(strstr(l, "/pci") || strstr(l, "/platform"), "subsystem -> %s", l); }
	if (n > 0 && strstr(l, "/pci")) {
		snprintf(p, sizeof(p), "/sys/dev/char/%u:%u/device/uevent", maj, mn);
		FILE *f = fopen(p, "r");
		char line[128]; int slot = 0;
		while (f && fgets(line, sizeof(line), f)) if (!strncmp(line, "PCI_SLOT_NAME=", 14)) slot = 1;
		if (f) fclose(f);
		CHECK(slot, "uevent PCI_SLOT_NAME");
		snprintf(p, sizeof(p), "/sys/dev/char/%u:%u/device/vendor", maj, mn);
		f = fopen(p, "r");
		unsigned v = 0;
		CHECK(f && fscanf(f, "%x", &v) == 1 && v, "vendor");
		if (f) fclose(f);
	}
	CHECK(stat("/sys/class/drm/renderD128/device/drm/card0", &st) == 0, "class/drm/renderD128");
}

int main(int argc, char **argv)
{
	int visual = argc > 2 && !strcmp(argv[1], "-v") ? atoi(argv[2]) : 0;
	int hold = argc > 2 && !strcmp(argv[1], "-h") ? atoi(argv[2]) : 0;   /* show a still image, then exit */
	int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) { fprintf(stderr, "no /dev/dri/card0: skipped\n"); puts("OK"); return 0; }

	char name[32] = {0};
	struct drm_version v = { .name_len = sizeof(name) - 1, .name = name };
	CHECK(xioctl(fd, IOC_VERSION, &v) == 0 && name[0], "VERSION");
	struct drm_get_cap c = { .capability = 1 };
	CHECK(xioctl(fd, IOC_GET_CAP, &c) == 0 && c.value == 1, "CAP_DUMB_BUFFER");
	c.capability = 5;
	CHECK(xioctl(fd, IOC_GET_CAP, &c) == 0 && c.value == 3, "CAP_PRIME");
	c.capability = 0x13;
	CHECK(xioctl(fd, IOC_GET_CAP, &c) == 0 && c.value == 1, "CAP_SYNCOBJ");
	c.capability = 0x7777;
	CHECK(xioctl(fd, IOC_GET_CAP, &c) == -EINVAL, "unknown cap");
	struct drm_set_client_cap cc = { 2, 1 };
	CHECK(xioctl(fd, IOC_SET_CLIENT_CAP, &cc) == 0, "UNIVERSAL_PLANES");
	cc.capability = 3;
	CHECK(xioctl(fd, IOC_SET_CLIENT_CAP, &cc) == -EOPNOTSUPP, "ATOMIC refused");
	test_sysfs(fd);

	/* resources, connector, encoder, CRTC */
	struct drm_mode_card_res r = {0};
	CHECK(xioctl(fd, IOC_GETRES, &r) == 0 && r.ncrtc == 1 && r.nconn >= 1 && r.nenc >= 1, "GETRESOURCES");
	uint32_t crtc_id = 0, conn_id = 0, enc_id = 0;
	r.crtc = (uintptr_t)&crtc_id; r.conn = (uintptr_t)&conn_id; r.enc = (uintptr_t)&enc_id;
	r.ncrtc = r.nconn = r.nenc = 1; r.nfb = 0;
	CHECK(xioctl(fd, IOC_GETRES, &r) == 0 && crtc_id && conn_id && enc_id, "GETRESOURCES ids");
	struct drm_mode_get_connector gc = { .connector_id = conn_id };
	CHECK(xioctl(fd, IOC_GETCONN, &gc) == 0 && gc.nmodes >= 1 && gc.connection == 1, "GETCONNECTOR");
	struct drm_mode_modeinfo mode;
	uint32_t props[8]; uint64_t pvals[8]; uint32_t encs[4];
	gc.nmodes = 1; gc.modes = (uintptr_t)&mode;
	gc.nprops = gc.nprops > 8 ? 8 : gc.nprops; gc.props = (uintptr_t)props; gc.prop_values = (uintptr_t)pvals;
	gc.nencs = 1; gc.encs = (uintptr_t)encs;
	CHECK(xioctl(fd, IOC_GETCONN, &gc) == 0 && mode.hdisplay && mode.vdisplay && encs[0] == enc_id, "GETCONNECTOR 2");
	struct drm_mode_get_encoder ge = { .encoder_id = enc_id };
	CHECK(xioctl(fd, IOC_GETENC, &ge) == 0 && ge.possible_crtcs == 1, "GETENCODER");
	struct drm_mode_crtc gcr = { .crtc_id = crtc_id };
	CHECK(xioctl(fd, IOC_GETCRTC, &gcr) == 0, "GETCRTC");
	uint32_t W = mode.hdisplay, H = mode.vdisplay;

	/* planes and the type property */
	uint32_t planes[4] = {0};
	struct drm_mode_get_plane_res pr = { .ids = (uintptr_t)planes, .n = 4 };
	CHECK(xioctl(fd, IOC_GETPLANERES, &pr) == 0 && pr.n == 2, "GETPLANERESOURCES %u", pr.n);
	uint32_t fmts[16];
	struct drm_mode_get_plane gp = { .plane_id = planes[0], .nfmt = 16, .fmts = (uintptr_t)fmts };
	CHECK(xioctl(fd, IOC_GETPLANE, &gp) == 0 && gp.nfmt >= 1 && fmts[0] == XRGB8888, "GETPLANE");
	struct drm_mode_obj_get_properties op = { .props = (uintptr_t)props, .vals = (uintptr_t)pvals, .n = 8,
	                                          .obj_id = planes[0], .obj_type = 0xeeeeeeee };
	CHECK(xioctl(fd, IOC_OBJ_GETPROPS, &op) == 0 && op.n == 1 && pvals[0] == 1, "plane props");
	struct drm_mode_property_enum en[4];
	struct drm_mode_get_property gpr = { .prop_id = props[0], .nenums = 4, .enums = (uintptr_t)en, .nvalues = 4, .values = (uintptr_t)pvals };
	CHECK(xioctl(fd, IOC_GETPROP, &gpr) == 0 && !strcmp(gpr.name, "type") && gpr.nenums == 3 && !strcmp(en[1].name, "Primary"), "type property");

	/* two dumb buffers, framebuffers, mode set */
	buf_t A = {0}, B = {0};
	CHECK(mkbuf(fd, W, H, &A) == 0 && mkbuf(fd, W, H, &B) == 0, "dumb buffers");
	if (fails) { printf("FAIL %d\n", fails); return 1; }
	paint(&A, W, H, 0);
	paint(&B, W, H, 1);
	struct drm_mode_fb_cmd legacy = { .width = W, .height = H, .pitch = A.pitch, .bpp = 32, .depth = 24, .handle = A.handle };
	CHECK(xioctl(fd, IOC_ADDFB, &legacy) == 0 && legacy.fb_id, "ADDFB");
	struct drm_mode_fb_cmd gf = { .fb_id = A.fb };
	CHECK(xioctl(fd, IOC_GETFB, &gf) == 0 && gf.width == W && gf.pitch == A.pitch && gf.handle, "GETFB");
	struct drm_gem_close gcl = { .handle = gf.handle };
	xioctl(fd, IOC_GEM_CLOSE, &gcl);
	struct drm_mode_crtc sc = { .crtc_id = crtc_id, .fb_id = A.fb, .conns = (uintptr_t)&conn_id, .nconn = 1, .mode_valid = 1, .mode = mode };
	CHECK(xioctl(fd, IOC_SETCRTC, &sc) == 0, "SETCRTC");
	CHECK(xioctl(fd, IOC_GETCRTC, &gcr) == 0 && gcr.fb_id == A.fb && gcr.mode_valid, "GETCRTC after set");
	if (hold) {
		if (argc > 3) {                            /* a flat colour, or "s" for stripes per page */
			for (uint32_t y = 0; y < H; y++)
				for (uint32_t x = 0; x < W; x++)
					A.map[y * (A.pitch / 4) + x] = argv[3][0] == 's' ? (uint32_t)((y * A.pitch + x * 4) / 4096) * 0x010307u
					                             : argv[3][0] == 'S' ? ((uint32_t)((y * A.pitch + x * 4) / 4096) * 0x010307u) & 0xFFFFFFu
					                                                : (uint32_t)strtoul(argv[3], 0, 16);
			struct drm_mode_fb_dirty_cmd d0 = { .fb_id = A.fb };
			xioctl(fd, IOC_DIRTYFB, &d0);
		}
		sleep((unsigned)hold); close(fd); return 0;
	}

	/* page flips with events: completions at vblank, in order */
	struct drm_event_vblank ev;
	buf_t *bufs[2] = { &A, &B };
	int flips = visual ? visual * 60 : getenv("DRM_FLIPS") ? atoi(getenv("DRM_FLIPS")) : 30, cur = 0;
	uint32_t lastseq = 0;
	double t0 = now_ms(), worst = 0, prev = t0, worst_ioctl = 0, sum_ioctl = 0;
	for (int i = 0; i < flips; i++) {
		buf_t *nb = bufs[cur ^ 1];
		if (visual) paint(nb, W, H, i);
		else if (getenv("DRM_FLIPS")) nb->map[(i * 97) % (W * H / 2)] ^= 0xffffff;
		struct drm_mode_crtc_page_flip pf = { .crtc_id = crtc_id, .fb_id = nb->fb, .flags = 1, .user_data = 0x1000 + (uint64_t)i };
		double ti = now_ms();
		int e = xioctl(fd, IOC_PAGE_FLIP, &pf);
		ti = now_ms() - ti;
		if (ti > worst_ioctl) worst_ioctl = ti;
		sum_ioctl += ti;
		if (e) { CHECK(0, "PAGE_FLIP %d: %d", i, e); break; }
		uint32_t t = wait_event(fd, 1000, &ev);
		if (t != 2 || ev.user_data != 0x1000 + (uint64_t)i) { CHECK(0, "flip event %d type %u", i, t); break; }
		CHECK(i == 0 || ev.seq > lastseq, "sequence %u after %u", ev.seq, lastseq);
		lastseq = ev.seq;
		double tn = now_ms();
		if (tn - prev > worst) worst = tn - prev;
		prev = tn;
		cur ^= 1;
	}
	double per = (now_ms() - t0) / flips;
	fprintf(stderr, "%d flips, %.2f ms/flip (%.1f fps), worst %.1f ms; PAGE_FLIP ioctl avg %.2f worst %.2f ms\n",
	        flips, per, 1000 / per, worst, sum_ioctl / flips, worst_ioctl);
	CHECK(per > 4 && per < 100, "flip pacing %.2f ms", per);

	/* vblank wait and sequence */
	union drm_wait_vblank wv = { .req = { .type = 1, .sequence = 1 } };
	double tw = now_ms();
	CHECK(xioctl(fd, IOC_WAIT_VBLANK, &wv) == 0, "WAIT_VBLANK");
	tw = now_ms() - tw;
	CHECK(tw < 60, "WAIT_VBLANK took %.1f ms", tw);
	struct drm_crtc_get_sequence gs = { .crtc_id = crtc_id };
	CHECK(xioctl(fd, IOC_GET_SEQ, &gs) == 0 && gs.seq >= wv.rep.sequence && gs.active, "CRTC_GET_SEQUENCE");
	wv.req.type = 1 | 0x4000000; wv.req.sequence = 2; wv.req.signal = 0x77;
	CHECK(xioctl(fd, IOC_WAIT_VBLANK, &wv) == 0, "WAIT_VBLANK event");
	CHECK(wait_event(fd, 1000, &ev) == 1 && ev.user_data == 0x77, "vblank event");

	/* cursor and dirty rectangles */
	buf_t C = {0};
	struct drm_mode_create_dumb cd = { .height = 64, .width = 64, .bpp = 32 };
	CHECK(xioctl(fd, IOC_CREATE_DUMB, &cd) == 0, "cursor dumb");
	struct drm_mode_map_dumb md = { .handle = cd.handle };
	xioctl(fd, IOC_MAP_DUMB, &md);
	C.map = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)md.offset);
	if (C.map != MAP_FAILED) for (int i = 0; i < 64 * 64; i++) C.map[i] = (i % 64 < 64 - i / 64) ? 0xFFFFFFFFu : 0;
	struct drm_mode_cursor cu = { .flags = 1, .crtc_id = crtc_id, .w = 64, .h = 64, .handle = cd.handle };
	CHECK(xioctl(fd, IOC_CURSOR, &cu) == 0, "CURSOR set");
	cu.flags = 2; cu.x = (int)W / 2; cu.y = (int)H / 2;
	CHECK(xioctl(fd, IOC_CURSOR, &cu) == 0, "CURSOR move");
	struct drm_clip_rect clip = { 0, 0, 64, 64 };
	struct drm_mode_fb_dirty_cmd dc = { .fb_id = bufs[cur]->fb, .nclips = 1, .clips = (uintptr_t)&clip };
	CHECK(xioctl(fd, IOC_DIRTYFB, &dc) == 0, "DIRTYFB");
	if (visual) sleep(1);
	cu.flags = 1; cu.handle = 0;
	CHECK(xioctl(fd, IOC_CURSOR, &cu) == 0, "CURSOR hide");

	/* PRIME: export, map the dma-buf, import back and into a second file */
	struct drm_prime_handle ph = { .handle = A.handle, .flags = O_CLOEXEC | O_RDWR };
	CHECK(xioctl(fd, IOC_PRIME_H2FD, &ph) == 0 && ph.fd >= 0, "PRIME export");
	if (ph.fd >= 0) {
		CHECK(lseek(ph.fd, 0, SEEK_END) == (off_t)A.size, "dma-buf size");
		uint32_t *m = mmap(0, A.size, PROT_READ | PROT_WRITE, MAP_SHARED, ph.fd, 0);
		CHECK(m != MAP_FAILED, "dma-buf mmap");
		if (m != MAP_FAILED) {
			CHECK(m[0] == A.map[0] && m[A.size / 4 - 1] == A.map[A.size / 4 - 1], "dma-buf contents");
			m[5] = 0x12345678;
			CHECK(A.map[5] == 0x12345678, "dma-buf shared");
			munmap(m, A.size);
		}
		struct drm_prime_handle ih = { .fd = ph.fd };
		CHECK(xioctl(fd, IOC_PRIME_FD2H, &ih) == 0 && ih.handle == A.handle, "PRIME import same file");
		int fd2 = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
		CHECK(fd2 >= 0, "open renderD128");
		if (fd2 >= 0) {
			struct drm_prime_handle ih2 = { .fd = ph.fd };
			CHECK(xioctl(fd2, IOC_PRIME_FD2H, &ih2) == 0 && ih2.handle, "PRIME import render node");
			struct drm_mode_card_res r2 = {0};
			CHECK(xioctl(fd2, IOC_GETRES, &r2) == -EACCES, "render node refuses KMS");
			struct drm_version v2 = {0};
			CHECK(xioctl(fd2, IOC_VERSION, &v2) == 0 && v2.name_len, "render VERSION");
			struct drm_gem_close g2 = { .handle = ih2.handle };
			CHECK(xioctl(fd2, IOC_GEM_CLOSE, &g2) == 0, "GEM_CLOSE");
			close(fd2);
		}
		close(ph.fd);
	}

	/* flink names */
	struct drm_gem_flink fl = { .handle = B.handle };
	CHECK(xioctl(fd, IOC_GEM_FLINK, &fl) == 0 && fl.name, "FLINK");
	int fd3 = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	struct drm_gem_open go = { .name = fl.name };
	CHECK(fd3 >= 0 && xioctl(fd3, IOC_GEM_OPEN, &go) == 0 && go.handle && go.size == B.size, "GEM_OPEN");

	/* syncobj */
	struct drm_syncobj_create so = {0};
	CHECK(xioctl(fd, IOC_SO_CREATE, &so) == 0 && so.handle, "SYNCOBJ_CREATE");
	struct drm_syncobj_wait sw = { .handles = (uintptr_t)&so.handle, .n = 1, .timeout = 0, .flags = 2 };
	CHECK(xioctl(fd, IOC_SO_WAIT, &sw) == -ETIME, "unsignaled wait");
	struct drm_syncobj_array sa = { .handles = (uintptr_t)&so.handle, .n = 1 };
	CHECK(xioctl(fd, IOC_SO_SIGNAL, &sa) == 0, "SYNCOBJ_SIGNAL");
	sw.flags = 0;
	CHECK(xioctl(fd, IOC_SO_WAIT, &sw) == 0, "signaled wait");
	struct drm_syncobj_handle sh = { .handle = so.handle };
	CHECK(xioctl(fd, IOC_SO_H2FD, &sh) == 0 && sh.fd >= 0, "SYNCOBJ_HANDLE_TO_FD");
	struct drm_syncobj_handle sh2 = { .fd = sh.fd };
	CHECK(fd3 >= 0 && xioctl(fd3, IOC_SO_FD2H, &sh2) == 0 && sh2.handle, "SYNCOBJ_FD_TO_HANDLE");
	close(sh.fd);
	struct drm_syncobj_handle ex = { .handle = so.handle, .flags = 1 };
	CHECK(xioctl(fd, IOC_SO_H2FD, &ex) == 0 && ex.fd >= 0, "export sync_file");
	if (ex.fd >= 0) {
		struct pollfd p = { .fd = ex.fd, .events = POLLIN };
		CHECK(poll(&p, 1, 0) == 1, "sync_file signaled");
		close(ex.fd);
	}
	uint64_t pt = 5;
	struct drm_syncobj_timeline_array ta = { .handles = (uintptr_t)&so.handle, .points = (uintptr_t)&pt, .n = 1 };
	CHECK(xioctl(fd, IOC_SO_TL_SIGNAL, &ta) == 0, "TIMELINE_SIGNAL");
	pt = 0;
	CHECK(xioctl(fd, IOC_SO_QUERY, &ta) == 0 && pt == 5, "QUERY %llu", (unsigned long long)pt);
	pt = 5;
	struct drm_syncobj_timeline_wait tw2 = { .handles = (uintptr_t)&so.handle, .points = (uintptr_t)&pt, .n = 1 };
	CHECK(xioctl(fd, IOC_SO_TL_WAIT, &tw2) == 0, "TIMELINE_WAIT");
	struct drm_syncobj_destroy sd = { .handle = so.handle };
	CHECK(xioctl(fd, IOC_SO_DESTROY, &sd) == 0, "SYNCOBJ_DESTROY");
	if (fd3 >= 0) close(fd3);

	/* removing the scanout framebuffer turns the CRTC off */
	unsigned int rm = bufs[cur]->fb;
	CHECK(xioctl(fd, IOC_RMFB, &rm) == 0, "RMFB");
	CHECK(xioctl(fd, IOC_GETCRTC, &gcr) == 0 && !gcr.fb_id, "CRTC off after RMFB");
	munmap(A.map, A.size);
	munmap(B.map, B.size);
	uint32_t h = A.handle;
	CHECK(xioctl(fd, IOC_DESTROY_DUMB, &h) == 0, "DESTROY_DUMB");
	CHECK(xioctl(fd, IOC_DESTROY_DUMB, &h) == -EINVAL, "DESTROY_DUMB twice");
	close(fd);
	if (fails) { printf("FAIL %d\n", fails); return 1; }
	puts("OK");
	return 0;
}
