/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * drivers/sound/hda.c against a model controller and a model Realtek ALC269VC,
 * the codec in the Casper (widget graph as Linux's codec dump shows it):
 *   DAC 0x02, 0x03 (out amp 0x57 steps of 0.75 dB, 0 dB at the top)
 *   mixer 0x0c <- 0x02, 0x0b      mixer 0x0d <- 0x03, 0x0b
 *   0x0b: the analog loopback, fed by the microphone pins
 *   pin 0x14 speaker (fixed) <- 0x0c, 0x0d     pin 0x21 headphone jack <- 0x0c, 0x0d
 *   pin 0x18 mic jack, 0x12 internal mic, 0x15 and 0x17 not connected
 * The model starts with every mixer input unmuted: what a codec may come up with.
 * Time is kernel_ticks (1 ms); while the stream runs, every tick the model DMA
 * plays 192 bytes (48 kHz, 16-bit stereo) from the ring and counts what it heard.
 * Run: tests/hda_host/run.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "../../drivers/sound/hda.c"

volatile uint64_t kernel_ticks;
static uint8_t R[0x4000] __attribute__((aligned(4096)));
static vfs_fops_t *dsp;
static void (*thread_fn)(void);
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- the codec ---- */
#define N 0x25
static struct {
	uint32_t wcaps, cfg, pincap, outamp, inamp;
	int conn[8], nconn, sel, pinctl, eapd, power;
	int og, om, ig[8], im[8];
} W[N];
static int hp_present;

static void node(int nid, int type, uint32_t extra, const int *conn, int n)
{
	W[nid].wcaps = (uint32_t)type << 20 | extra | (n ? 0x100 : 0) | 1;
	for (int i = 0; i < n; i++) W[nid].conn[i] = conn[i];
	W[nid].nconn = n;
}

static void codec_build(void)
{
	static const int m0c[] = {0x02, 0x0b}, m0d[] = {0x03, 0x0b}, pin[] = {0x0c, 0x0d}, lb[] = {0x18, 0x12};
	node(0x02, 0, 0x4, NULL, 0); W[0x02].outamp = 2u << 16 | 0x57u << 8 | 0x57; W[0x02].og = 0x57;
	node(0x03, 0, 0x4, NULL, 0); W[0x03].outamp = 2u << 16 | 0x57u << 8 | 0x57; W[0x03].og = 0x57;
	node(0x0b, 2, 0x2, lb, 2); W[0x0b].inamp = 0x80051f17u;
	node(0x0c, 2, 0x2, m0c, 2); W[0x0c].inamp = 0x80000000u;
	node(0x0d, 2, 0x2, m0d, 2); W[0x0d].inamp = 0x80000000u;
	node(0x14, 4, 0x4, pin, 2); W[0x14].cfg = 0x90170110; W[0x14].outamp = 0x80000000u; W[0x14].om = 1;
	node(0x21, 4, 0x4, pin, 2); W[0x21].cfg = 0x0321101f; W[0x21].outamp = 0x80000000u; W[0x21].om = 1;
	W[0x21].pincap = 0x1c;                   /* out, headphone drive, presence detect */
	node(0x18, 4, 0x2, NULL, 0); W[0x18].cfg = 0x03a11020;
	node(0x12, 4, 0x2, NULL, 0); W[0x12].cfg = 0x90a60130;
	node(0x15, 4, 0x4, pin, 2); W[0x15].cfg = 0x411111f0;
	node(0x17, 4, 0x4, pin, 2); W[0x17].cfg = 0x411111f0;
}

static uint32_t verb(uint32_t c)
{
	int nid = (int)(c >> 20) & 0x7F;
	uint32_t v = c & 0xFFFFF;
	if (nid >= N) return 0;
	if ((v >> 16) == 0x3) {                  /* set amp */
		int out = (v >> 15) & 1, in = (v >> 14) & 1, idx = (v >> 8) & 0xF, mute = (v >> 7) & 1, g = v & 0x7F;
		if (out) { W[nid].og = g; W[nid].om = mute; }
		if (in) { W[nid].ig[idx] = g; W[nid].im[idx] = mute; }
		return 0;
	}
	if ((v >> 16) == 0x2) return 0;          /* format */
	uint32_t vb = v >> 8, pay = v & 0xFF;
	switch (vb) {
	case 0xF00:
		switch (pay) {
		case 0x00: return nid == 0 ? 0x10ec0269 : 0;
		case 0x04: return nid == 0 ? (1u << 16 | 1) : nid == 1 ? (2u << 16 | (N - 2)) : 0;
		case 0x05: return nid == 1 ? 1 : 0;
		case 0x09: return W[nid].wcaps;
		case 0x0C: return W[nid].pincap;
		case 0x0D: return W[nid].inamp;
		case 0x0E: return (uint32_t)W[nid].nconn;
		case 0x12: return W[nid].outamp;
		}
		return 0;
	case 0xF02: {
		uint32_t r = 0;
		for (int j = 0; j < 4 && (int)pay + j < W[nid].nconn; j++) r |= (uint32_t)W[nid].conn[pay + j] << (8 * j);
		return r;
	}
	case 0xF1C: return W[nid].cfg;
	case 0xF09: return nid == 0x21 && hp_present ? 0x80000000u : 0;
	case 0x701: W[nid].sel = (int)pay; return 0;
	case 0x705: W[nid].power = (int)pay; return 0;
	case 0x707: W[nid].pinctl = (int)pay; return 0;
	case 0x70C: W[nid].eapd = (int)pay; return 0;
	}
	return 0;
}

/* ---- the controller: registers in memory, CORB/RIRB served on each port 0x80 read ---- */
static int corb_done, rirb_wp;
#define REG16(o) (*(volatile uint16_t *)(R + (o)))
#define REG32(o) (*(volatile uint32_t *)(R + (o)))

uint8_t inb(uint16_t port)
{
	if (port != 0x80 || !REG32(0x40)) return 0;
	uint32_t *cq = (uint32_t *)(uintptr_t)((uint64_t)REG32(0x44) << 32 | REG32(0x40));
	uint64_t *rq = (uint64_t *)(uintptr_t)((uint64_t)REG32(0x54) << 32 | REG32(0x50));
	if (REG16(0x58) & 0x8000) { REG16(0x58) = 0; rirb_wp = 0; }
	while (corb_done != (REG16(0x48) & 0xFF)) {
		corb_done = (corb_done + 1) & 0xFF;
		rirb_wp = (rirb_wp + 1) & 0xFF;
		rq[rirb_wp] = verb(cq[corb_done]);
		REG16(0x58) = (uint16_t)rirb_wp;
	}
	return 0;
}

/* ---- the stream: what the DMA played ---- */
static uint64_t heard_nonzero, heard;
static volatile uint8_t *SD(void) { return R + 0x80 + 0x20 * ((REG16(0) >> 8) & 0xF); }

static void dma_tick(void)
{
	volatile uint8_t *s = SD();
	if (!(s[0] & 2)) return;
	uint32_t cbl = *(volatile uint32_t *)(s + 8), pos = *(volatile uint32_t *)(s + 4);
	uint32_t *bdl = (uint32_t *)(uintptr_t)((uint64_t)*(volatile uint32_t *)(s + 0x1C) << 32 | *(volatile uint32_t *)(s + 0x18));
	uint8_t *ring0 = (uint8_t *)(uintptr_t)((uint64_t)bdl[1] << 32 | bdl[0]);
	for (int i = 0; i < 192; i++) {
		heard++;
		if (ring0[(pos + (uint32_t)i) % cbl]) heard_nonzero++;
	}
	*(volatile uint32_t *)(s + 4) = (pos + 192) % cbl;
}

void task_sleep_until(uint64_t deadline)
{
	while (kernel_ticks < deadline) { kernel_ticks++; dma_tick(); }
}

/* the watch thread, one pass per HDA_WATCH_MS; a driver without one has nothing to run */
static void advance(int ms)
{
	for (int t = 0; t < ms; t++) {
		kernel_ticks++;
		dma_tick();
#ifdef HDA_WATCH_MS
		if (kernel_ticks % HDA_WATCH_MS == 0) hda_watch_once();
#endif
	}
}

/* ---- the rest of the kernel ---- */
void kprintf(const char *fmt, ...) { va_list a; va_start(a, fmt); if (getenv("V")) vprintf(fmt, a); va_end(a); }
int pci_find_class(uint8_t c, uint8_t s, uint8_t p, uint8_t *b, uint8_t *d, uint8_t *f)
{ (void)c; (void)s; (void)p; *b = 0; *d = 0x1f; *f = 3; return 0; }
uint16_t pci_read16(uint8_t b, uint8_t d, uint8_t f, uint8_t r) { (void)b; (void)d; (void)f; (void)r; return 0; }
uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t r) { (void)b; (void)d; (void)f; return r == 0x10 ? 0xDF000000u : 0; }
void pci_write16(uint8_t b, uint8_t d, uint8_t f, uint8_t r, uint16_t v) { (void)b; (void)d; (void)f; (void)r; (void)v; }
void *ioremap(uint64_t p, size_t n, uint32_t fl) { (void)p; (void)n; (void)fl; return R; }
void *buddy_alloc(int order) { return aligned_alloc(4096, (size_t)4096 << order); }
int devfs_register_device_gid(const char *n, uint32_t m, int g, uint32_t dv, vfs_fops_t *f)
{ (void)n; (void)m; (void)g; (void)dv; dsp = f; return 0; }
pid_t proc_create(const char *n, void (*fn)(void)) { (void)n; thread_fn = fn; return 1; }
int task_has_actionable_signal(void) { return 0; }
int copy_fop_from_user(void *d, const void *s, size_t n) { memcpy(d, s, n); return 0; }
int copy_from_user(void *d, const void *s, size_t n) { memcpy(d, s, n); return 0; }
int copy_to_user(void *d, const void *s, size_t n) { memcpy(d, s, n); return 0; }
static hwdiag_fn diag;
void hwdiag_register(hwdiag_fn fn) { diag = fn; }
void hw_begin(hw_out_t *o, const char *c, const char *n) { (void)o; (void)c; (void)n; }
void hw_kv(hw_out_t *o, const char *k, uint64_t v) { (void)o; (void)k; (void)v; }
void hw_end(hw_out_t *o) { (void)o; }
const char *cmdline_get(const char *k, char *b, size_t n) { (void)k; (void)b; (void)n; return NULL; }

int main(void)
{
	codec_build();
	REG16(0x00) = 0x4401;                    /* GCAP: 4 input streams first */
	REG16(0x0E) = 1;                         /* STATESTS: codec 0 */
	R[0x4E] = 0x42; R[0x5E] = 0x42;          /* CORB/RIRB can be 256 */
	hda_init();
	CHECK(dsp != NULL, "/dev/dsp registered");
	if (!dsp) { printf("FAIL %d\n", fails); return 1; }
	CHECK(W[0x14].pinctl == 0x40 && W[0x21].pinctl == 0xC0, "speaker out, headphone out+drive (%x %x)",
	      W[0x14].pinctl, W[0x21].pinctl);
	CHECK(W[0x14].eapd == 2, "speaker EAPD on");
	CHECK(!W[0x14].om && !W[0x21].om, "pin amps unmuted");
	CHECK(!W[0x0c].im[0], "mixer 0x0c: DAC input unmuted");
	CHECK(W[0x0c].im[1], "mixer 0x0c: loopback input (microphones) muted");
	CHECK(W[0x15].pinctl == 0 && W[0x18].pinctl == 0, "unconnected pin and mic pin left alone");

	/* volume: the curve of other systems, on the DAC; 0 mutes */
	sound_set_volume(100);
	CHECK(W[0x02].og == 0x57 && !W[0x02].om, "100: 0 dB (%d)", W[0x02].og);
	sound_set_volume(50);
	CHECK(W[0x02].og == 0x57 - 24, "50: -18 dB, step %d (want %d)", W[0x02].og, 0x57 - 24);
	sound_set_volume(15);
	CHECK(W[0x02].og == 0x57 - 66, "15: -49.4 dB, step %d (want %d)", W[0x02].og, 0x57 - 66);
	sound_set_volume(0);
	CHECK(W[0x02].om, "0: muted");
	sound_set_volume(63);

	/* a writer that stops: what it wrote is heard once, then silence */
	vfs_inode_t in = {0};
	vfs_file_t f = {O_NONBLOCK};
	CHECK(dsp->open(&in, &f, 0) == 0, "open");
	static uint8_t pcm[32768];
	memset(pcm, 0x11, sizeof pcm);
	CHECK(dsp->write(&f, pcm, sizeof pcm) == (ssize_t)sizeof pcm, "write 32 KB");
	advance(3000);                           /* three seconds, nobody calls */
	CHECK(heard_nonzero == sizeof pcm, "heard %llu bytes of sound for 32768 written (a replay if more)",
	      (unsigned long long)heard_nonzero);
	CHECK(!(SD()[0] & 2), "a stream with nothing to play stops");
	uint64_t h0 = heard;
	advance(1000);
	CHECK(heard == h0, "and plays nothing more");

	/* SETTRIGGER: 0 holds output (ALSA's stop/pause), PCM_ENABLE_OUTPUT goes on */
	heard_nonzero = 0;
	CHECK(dsp->write(&f, pcm, sizeof pcm) == (ssize_t)sizeof pcm, "write again");
	advance(40);
	int v = 0;
	dsp->ioctl(&f, 0x40045010u, (uint64_t)(uintptr_t)&v);
	uint64_t before = heard_nonzero;
	advance(500);
	CHECK(heard_nonzero == before && before > 0, "held: nothing plays (%llu -> %llu)",
	      (unsigned long long)before, (unsigned long long)heard_nonzero);
	v = 2;
	dsp->ioctl(&f, 0x40045010u, (uint64_t)(uintptr_t)&v);
	advance(1000);
	CHECK(heard_nonzero == sizeof pcm, "released: the rest plays, once (%llu)", (unsigned long long)heard_nonzero);
	dsp->close(&f);

	/* headphones in: speakers off; out: on again */
	hp_present = 1;
	advance(1000);
	CHECK(W[0x14].pinctl == 0, "headphones in: speaker pin off (%x)", W[0x14].pinctl);
	hp_present = 0;
	advance(1000);
	CHECK(W[0x14].pinctl == 0x40, "headphones out: speaker pin on (%x)", W[0x14].pinctl);
	CHECK(thread_fn != NULL, "the watch thread is started");
	CHECK(diag != NULL, "/proc/hwdiag has a sound line");

	printf(fails ? "FAIL %d\n" : "OK\n", fails);
	return fails != 0;
}
