/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* KMS on a virtio-gpu (QEMU -device virtio-gpu-pci): the card must be the
 * virtio_gpu back end, not the firmware framebuffer. Checks, modetest style
 * with raw ioctls: several modes with the host's size preferred, SETCRTC to
 * another mode, page flips of dumb buffers completing with events while the
 * buffer itself is scanned out (no copy), the cursor on the host's cursor
 * plane, DIRTYFB, an unknown size refused, and back. The back end's state is
 * read from /sys/class/drm/ranix_flip (root).
 *   test_virtio_gpu [WxH]   WxH: the size the host was told (xres/yres)
 * Prints OK, or OK when the machine has no virtio-gpu. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

struct drm_version { int major, minor, patch; size_t name_len; char *name; size_t date_len; char *date; size_t desc_len; char *desc; };
struct drm_mode_modeinfo {
	uint32_t clock; uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan; uint32_t vrefresh, flags, type; char name[32];
};
struct drm_mode_card_res { uint64_t fb, crtc, conn, enc; uint32_t nfb, ncrtc, nconn, nenc, minw, maxw, minh, maxh; };
struct drm_mode_crtc { uint64_t conns; uint32_t nconn, crtc_id, fb_id, x, y, gamma_size, mode_valid; struct drm_mode_modeinfo mode; };
struct drm_mode_get_connector {
	uint64_t encs, modes, props, prop_values; uint32_t nmodes, nprops, nencs;
	uint32_t encoder_id, connector_id, connector_type, connector_type_id, connection, mm_w, mm_h, subpixel, pad;
};
struct drm_mode_fb_cmd2 { uint32_t fb_id, width, height, format, flags, handles[4], pitches[4], offsets[4]; uint64_t modifier[4]; };
struct drm_mode_fb_dirty_cmd { uint32_t fb_id, flags, color, nclips; uint64_t clips; };
struct drm_clip_rect { uint16_t x1, y1, x2, y2; };
struct drm_mode_cursor { uint32_t flags, crtc_id; int32_t x, y; uint32_t w, h, handle; };
struct drm_mode_crtc_page_flip { uint32_t crtc_id, fb_id, flags, reserved; uint64_t user_data; };
struct drm_mode_create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct drm_mode_map_dumb { uint32_t handle, pad; uint64_t offset; };
struct drm_event_vblank { uint32_t type, length; uint64_t user_data; uint32_t sec, usec, seq, crtc_id; };

#define D(n, t) _IOWR('d', n, t)
#define IOC_VERSION      D(0x00, struct drm_version)
#define IOC_GETRES       D(0xA0, struct drm_mode_card_res)
#define IOC_GETCRTC      D(0xA1, struct drm_mode_crtc)
#define IOC_SETCRTC      D(0xA2, struct drm_mode_crtc)
#define IOC_CURSOR       D(0xA3, struct drm_mode_cursor)
#define IOC_GETCONN      D(0xA7, struct drm_mode_get_connector)
#define IOC_RMFB         D(0xAF, unsigned int)
#define IOC_PAGE_FLIP    D(0xB0, struct drm_mode_crtc_page_flip)
#define IOC_DIRTYFB      D(0xB1, struct drm_mode_fb_dirty_cmd)
#define IOC_CREATE_DUMB  D(0xB2, struct drm_mode_create_dumb)
#define IOC_MAP_DUMB     D(0xB3, struct drm_mode_map_dumb)
#define IOC_ADDFB2       D(0xB8, struct drm_mode_fb_cmd2)

#define XRGB8888 0x34325258u
#define MODE_TYPE_PREFERRED 8
#define CONNECTOR_VIRTUAL 15

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

/* is there a virtio-gpu (1af4:1050) on the PCI bus */
static int have_virtio_gpu(void)
{
	DIR *d = opendir("/sys/bus/pci/devices");
	struct dirent *e;
	int found = 0;
	while (d && !found && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char p[128]; unsigned v = 0, dv = 0;
		snprintf(p, sizeof(p), "/sys/bus/pci/devices/%s/vendor", e->d_name);
		FILE *f = fopen(p, "r");
		if (f) { if (fscanf(f, "%x", &v) != 1) v = 0; fclose(f); }
		snprintf(p, sizeof(p), "/sys/bus/pci/devices/%s/device", e->d_name);
		f = fopen(p, "r");
		if (f) { if (fscanf(f, "%x", &dv) != 1) dv = 0; fclose(f); }
		found = v == 0x1af4 && dv == 0x1050;
	}
	if (d) closedir(d);
	return found;
}

/* the back end's state line ("scan=1 ... virtio-gpu mode=WxH ...") */
static void state(char *o, size_t n)
{
	o[0] = 0;
	int fd = open("/sys/class/drm/ranix_flip", O_RDONLY);
	if (fd < 0) return;
	ssize_t r = read(fd, o, n - 1);
	close(fd);
	o[r > 0 ? r : 0] = 0;
}

static uint64_t state_num(const char *key)
{
	char s[512];
	state(s, sizeof(s));
	const char *p = strstr(s, key);
	return p ? strtoull(p + strlen(key), NULL, 10) : ~0ULL;
}

static int state_has(const char *what)
{
	char s[512];
	state(s, sizeof(s));
	return strstr(s, what) != NULL;
}

typedef struct { uint32_t handle, pitch, fb; uint64_t size; uint32_t *map; } buf_t;

static int mkbuf(int fd, uint32_t w, uint32_t h, uint32_t colour, buf_t *b)
{
	struct drm_mode_create_dumb c = { .height = h, .width = w, .bpp = 32 };
	int e = xioctl(fd, IOC_CREATE_DUMB, &c);
	if (e) return e;
	b->handle = c.handle; b->pitch = c.pitch; b->size = c.size;
	struct drm_mode_map_dumb m = { .handle = c.handle };
	if ((e = xioctl(fd, IOC_MAP_DUMB, &m))) return e;
	b->map = mmap(0, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.offset);
	if (b->map == MAP_FAILED) return -errno;
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) b->map[y * (b->pitch / 4) + x] = colour ^ (x * 255 / w);
	struct drm_mode_fb_cmd2 f = { .width = w, .height = h, .format = XRGB8888 };
	f.handles[0] = c.handle; f.pitches[0] = c.pitch;
	if ((e = xioctl(fd, IOC_ADDFB2, &f))) return e;
	b->fb = f.fb_id;
	return 0;
}

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

static int setcrtc(int fd, uint32_t crtc, uint32_t conn, uint32_t fb, const struct drm_mode_modeinfo *m)
{
	struct drm_mode_crtc sc = { .crtc_id = crtc, .fb_id = fb, .conns = (uintptr_t)&conn, .nconn = 1, .mode_valid = 1, .mode = *m };
	return xioctl(fd, IOC_SETCRTC, &sc);
}

int main(int argc, char **argv)
{
	unsigned want_w = 0, want_h = 0;
	if (argc > 1 && sscanf(argv[1], "%ux%u", &want_w, &want_h) != 2) want_w = want_h = 0;
	if (!have_virtio_gpu()) { fprintf(stderr, "no virtio-gpu: skipped\n"); puts("OK"); return 0; }
	int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	CHECK(fd >= 0, "open card0: %d", errno);
	if (fd < 0) { printf("FAIL %d\n", fails); return 1; }

	char name[32] = {0};
	struct drm_version v = { .name_len = sizeof(name) - 1, .name = name };
	CHECK(xioctl(fd, IOC_VERSION, &v) == 0 && !strcmp(name, "virtio_gpu"), "driver '%s', want virtio_gpu", name);
	if (fails) { printf("FAIL %d\n", fails); return 1; }

	/* resources and modes */
	struct drm_mode_card_res r = {0};
	uint32_t crtc = 0, conn = 0, enc = 0;
	r.crtc = (uintptr_t)&crtc; r.conn = (uintptr_t)&conn; r.enc = (uintptr_t)&enc;
	r.ncrtc = r.nconn = r.nenc = 1;
	CHECK(xioctl(fd, IOC_GETRES, &r) == 0 && crtc && conn, "GETRESOURCES");
	struct drm_mode_get_connector gc = { .connector_id = conn };
	CHECK(xioctl(fd, IOC_GETCONN, &gc) == 0, "GETCONNECTOR");
	uint32_t n = gc.nmodes;
	CHECK(n > 1 && n <= 32, "%u modes, want several", n);
	if (fails) { printf("FAIL %d\n", fails); return 1; }
	struct drm_mode_modeinfo modes[32];
	gc.modes = (uintptr_t)modes; gc.nprops = 0; gc.nencs = 0;
	CHECK(xioctl(fd, IOC_GETCONN, &gc) == 0 && gc.nmodes == n, "GETCONNECTOR modes");
	CHECK(gc.connector_type == CONNECTOR_VIRTUAL && gc.connection == 1, "connector type %u", gc.connector_type);
	CHECK(modes[0].type & MODE_TYPE_PREFERRED, "first mode not preferred");
	if (want_w) CHECK(modes[0].hdisplay == want_w && modes[0].vdisplay == want_h, "preferred %ux%u, host said %ux%u",
	                  modes[0].hdisplay, modes[0].vdisplay, want_w, want_h);
	for (uint32_t i = 0; i < n; i++) {
		CHECK(modes[i].hdisplay && modes[i].vdisplay && modes[i].vrefresh == 60, "mode %u", i);
		for (uint32_t j = 0; j < i; j++)
			CHECK(modes[i].hdisplay != modes[j].hdisplay || modes[i].vdisplay != modes[j].vdisplay, "mode %u twice", i);
	}
	struct drm_mode_crtc g = { .crtc_id = crtc };
	CHECK(xioctl(fd, IOC_GETCRTC, &g) == 0, "GETCRTC");
	/* another size: 1024x768 when listed, else the last */
	uint32_t alt = n - 1;
	for (uint32_t i = 1; i < n; i++) if (modes[i].hdisplay == 1024 && modes[i].vdisplay == 768) alt = i;
	struct drm_mode_modeinfo *am = &modes[alt];
	CHECK(am->hdisplay != g.mode.hdisplay || am->vdisplay != g.mode.vdisplay, "no other mode");
	uint32_t W = am->hdisplay, H = am->vdisplay;
	fprintf(stderr, "%u modes, preferred %s, switching to %s\n", n, modes[0].name, am->name);

	/* SETCRTC to that mode */
	buf_t A = {0}, B = {0};
	CHECK(mkbuf(fd, W, H, 0x203040, &A) == 0 && mkbuf(fd, W, H, 0x806020, &B) == 0, "dumb buffers %ux%u", W, H);
	if (fails) { printf("FAIL %d\n", fails); return 1; }
	CHECK(setcrtc(fd, crtc, conn, A.fb, am) == 0, "SETCRTC %ux%u", W, H);
	CHECK(xioctl(fd, IOC_GETCRTC, &g) == 0 && g.fb_id == A.fb && g.mode.hdisplay == W && g.mode.vdisplay == H,
	      "GETCRTC %ux%u", g.mode.hdisplay, g.mode.vdisplay);
	char want[64];
	snprintf(want, sizeof(want), "mode=%ux%u ", W, H);
	CHECK(state_has(want), "back end not at %s", want);
	CHECK(state_has("scan=1 ") && state_has("(client)"), "buffer not scanned out directly");

	/* page flips with events: the buffer itself goes on screen each time */
	uint64_t f0 = state_num("flips=");
	buf_t *bufs[2] = { &A, &B };
	int cur = 0, flips = 30;
	uint32_t lastseq = 0;
	struct drm_event_vblank ev;
	double t0 = now_ms();
	for (int i = 0; i < flips; i++) {
		struct drm_mode_crtc_page_flip pf = { .crtc_id = crtc, .fb_id = bufs[cur ^ 1]->fb, .flags = 1, .user_data = 0x2000 + (uint64_t)i };
		int e = xioctl(fd, IOC_PAGE_FLIP, &pf);
		if (e) { CHECK(0, "PAGE_FLIP %d: %d", i, e); break; }
		uint32_t t = wait_event(fd, 1000, &ev);
		if (t != 2 || ev.user_data != 0x2000 + (uint64_t)i) { CHECK(0, "flip event %d type %u", i, t); break; }
		CHECK(i == 0 || ev.seq > lastseq, "sequence %u after %u", ev.seq, lastseq);
		lastseq = ev.seq;
		cur ^= 1;
	}
	double per = (now_ms() - t0) / flips;
	fprintf(stderr, "%d flips, %.2f ms/flip\n", flips, per);
	CHECK(per > 4 && per < 100, "flip pacing %.2f ms", per);
	uint64_t f1 = state_num("flips=");
	CHECK(f1 != ~0ULL && f1 - f0 >= (uint64_t)flips, "back end flips %llu -> %llu", (unsigned long long)f0, (unsigned long long)f1);

	/* the cursor on the host's cursor plane: the buffer stays scanned out */
	struct drm_mode_create_dumb cd = { .height = 64, .width = 64, .bpp = 32 };
	CHECK(xioctl(fd, IOC_CREATE_DUMB, &cd) == 0, "cursor dumb");
	struct drm_mode_map_dumb md = { .handle = cd.handle };
	xioctl(fd, IOC_MAP_DUMB, &md);
	uint32_t *cm = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)md.offset);
	if (cm != MAP_FAILED) for (int i = 0; i < 64 * 64; i++) cm[i] = (i % 64 < 64 - i / 64) ? 0xFFFFFFFFu : 0;
	struct drm_mode_cursor cu = { .flags = 1, .crtc_id = crtc, .w = 64, .h = 64, .handle = cd.handle };
	CHECK(xioctl(fd, IOC_CURSOR, &cu) == 0, "CURSOR set");
	CHECK(state_has("cursor=hw"), "cursor not on the cursor plane");
	cu.flags = 2; cu.x = (int)W / 2; cu.y = (int)H / 2;
	CHECK(xioctl(fd, IOC_CURSOR, &cu) == 0, "CURSOR move");
	CHECK(state_has("scan=1 "), "a cursor forced the copy path");
	struct drm_mode_crtc_page_flip pf = { .crtc_id = crtc, .fb_id = bufs[cur ^ 1]->fb, .flags = 1, .user_data = 0x77 };
	CHECK(xioctl(fd, IOC_PAGE_FLIP, &pf) == 0 && wait_event(fd, 1000, &ev) == 2 && ev.user_data == 0x77, "flip with cursor");
	cur ^= 1;
	/* too big for the host's 64x64 cursor: KMS draws it (copy), then back */
	struct drm_mode_create_dumb cb = { .height = 128, .width = 128, .bpp = 32 };
	CHECK(xioctl(fd, IOC_CREATE_DUMB, &cb) == 0, "big cursor dumb");
	cu.flags = 1; cu.w = 128; cu.h = 128; cu.handle = cb.handle;
	CHECK(xioctl(fd, IOC_CURSOR, &cu) == 0, "CURSOR 128");
	CHECK(state_has("cursor=off") && state_has("scan=0 "), "128x128 cursor not drawn by KMS");
	cu.handle = 0; cu.w = cu.h = 0;
	CHECK(xioctl(fd, IOC_CURSOR, &cu) == 0, "CURSOR hide");
	CHECK(state_has("scan=1 "), "no direct scanout after the cursor went");

	/* DIRTYFB on the front buffer sends the rectangle to the host */
	uint64_t d0 = state_num("damages=");
	struct drm_clip_rect clip = { 10, 10, 74, 74 };
	struct drm_mode_fb_dirty_cmd dc = { .fb_id = bufs[cur]->fb, .nclips = 1, .clips = (uintptr_t)&clip };
	CHECK(xioctl(fd, IOC_DIRTYFB, &dc) == 0, "DIRTYFB");
	CHECK(state_num("damages=") == d0 + 1, "DIRTYFB not sent");

	/* a size the connector does not list is refused; the preferred one works */
	struct drm_mode_modeinfo bad = *am;
	bad.hdisplay = 1234; bad.vdisplay = 567;
	CHECK(setcrtc(fd, crtc, conn, A.fb, &bad) == -EINVAL, "1234x567 accepted");
	buf_t P = {0};
	CHECK(mkbuf(fd, modes[0].hdisplay, modes[0].vdisplay, 0x102030, &P) == 0, "preferred buffer");
	CHECK(setcrtc(fd, crtc, conn, P.fb, &modes[0]) == 0, "SETCRTC preferred");
	snprintf(want, sizeof(want), "mode=%ux%u ", modes[0].hdisplay, modes[0].vdisplay);
	CHECK(state_has(want), "back end not back at %s", want);
	unsigned int rm = P.fb;
	CHECK(xioctl(fd, IOC_RMFB, &rm) == 0, "RMFB");
	CHECK(xioctl(fd, IOC_GETCRTC, &g) == 0 && !g.fb_id, "CRTC off after RMFB");
	CHECK(state_has("(shadow)") && state_num("err=") == 0, "after RMFB: not on the shadow or host errors");
	close(fd);
	if (fails) { printf("FAIL %d\n", fails); return 1; }
	puts("OK");
	return 0;
}
