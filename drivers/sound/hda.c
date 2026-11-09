/*
 * Intel High Definition Audio, from the Intel HD Audio specification. One codec,
 * one output stream, playback only. The codec's widget graph is walked from each
 * connected output pin back to a converter, and everything on the way is powered
 * and unmuted. Programs use /dev/dsp, the OSS interface: ioctls set rate, format
 * and channels, write() plays. /sys/kernel/volume is the output level, 0 to 100.
 */
#include <kernel/pci.h>
#include <kernel/hal.h>
#include <kernel/kprintf.h>
#include <kernel/sync.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <kernel/board.h>
#include <kernel/fs/devfs.h>
#include <kernel/lib/string.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/mmu.h>
#include <kernel/sound.h>
#include <asm/mmu.h>
#include <asm/io.h>
#include <kernel/mm/vm.h>
#include <kernel/time.h>
#include <kernel/errno.h>


#define R_GCAP      0x00
#define R_GCTL      0x08
#define R_STATESTS  0x0E
#define R_INTCTL    0x20
#define R_CORBLBASE 0x40
#define R_CORBUBASE 0x44
#define R_CORBWP    0x48
#define R_CORBRP    0x4A
#define R_CORBCTL   0x4C
#define R_CORBSIZE  0x4E
#define R_RIRBLBASE 0x50
#define R_RIRBUBASE 0x54
#define R_RIRBWP    0x58
#define R_RINTCNT   0x5A
#define R_RIRBCTL   0x5C
#define R_RIRBSTS   0x5D
#define R_RIRBSIZE  0x5E

/* stream descriptor registers */
#define SD_CTL   0x00
#define SD_STS   0x03
#define SD_LPIB  0x04
#define SD_CBL   0x08
#define SD_LVI   0x0C
#define SD_FMT   0x12
#define SD_BDPL  0x18
#define SD_BDPU  0x1C

#define SD_RUN   0x02
#define SD_SRST  0x01
#define GCTL_CRST 1
#define STREAM_TAG 1

/* verbs */
#define V_GET_PARAM   0xF00
#define V_CONN_SEL    0x701
#define V_GET_CONN    0xF02
#define V_POWER       0x705
#define V_CONV_STREAM 0x706
#define V_PIN_CTL     0x707
#define V_EAPD        0x70C
#define V_PIN_CFG     0xF1C
#define V_SET_FMT     0x2     /* 4-bit verb, 16-bit payload */
#define V_SET_AMP     0x3
#define P_NODES       0x04
#define P_FN_TYPE     0x05
#define P_WCAPS       0x09
#define P_OUT_AMP     0x12
#define P_IN_AMP      0x0D
#define P_CONN_LEN    0x0E
#define W_OUT 0
#define W_MIX 2
#define W_SEL 3
#define W_PIN 4

#define CHUNK     16384u
#define NCHUNK    16u
#define RING      (CHUNK * NCHUNK)
#define MAXW      96
#define MAXDAC    4

static volatile uint8_t *regs;
static uint32_t *corb;
static uint64_t *rirb;
static int rirb_rp, corb_n, rirb_n;
static int codec = -1, afg;
static int dacs[MAXDAC], ndac;
static int ampnodes[MAXDAC * 4], namp;
static uint32_t amp_steps[MAXDAC * 4];
static volatile uint8_t *sd;
static uint8_t *ring;
static uint64_t ring_phys, bdl_phys;
static spinlock_t lock = SPINLOCK_INIT;
static uint32_t wpos;            /* next byte to write, modulo RING */
static uint64_t written, played; /* totals, bytes */
static uint64_t accepted;        /* bytes taken from the writer: its own clock never jumps */
static uint32_t frag_bytes = CHUNK, frag_count = NCHUNK - 1;   /* what the program asked for */
static uint32_t lastpos;
static int running, opened;
static uint32_t rate = 48000;
static int volume = 80;
static int chans = 2, bits16 = 1;

#define W8(o, v)   (*(volatile uint8_t  *)(regs + (o)) = (v))
#define W16(o, v)  (*(volatile uint16_t *)(regs + (o)) = (v))
#define W32(o, v)  (*(volatile uint32_t *)(regs + (o)) = (v))
#define R8(o)      (*(volatile uint8_t  *)(regs + (o)))
#define R16(o)     (*(volatile uint16_t *)(regs + (o)))
#define R32(o)     (*(volatile uint32_t *)(regs + (o)))
#define SW8(o, v)  (*(volatile uint8_t  *)(sd + (o)) = (v))
#define SW16(o, v) (*(volatile uint16_t *)(sd + (o)) = (v))
#define SW32(o, v) (*(volatile uint32_t *)(sd + (o)) = (v))
#define SR8(o)     (*(volatile uint8_t  *)(sd + (o)))
#define SR32(o)    (*(volatile uint32_t *)(sd + (o)))

/* Roughly n nanoseconds: a port 0x80 cycle takes about a microsecond on any machine,
 * where a counted loop finishes hundreds of times sooner under KVM than under TCG. */
static void spin(int n) { for (int i = 0; i < n / 1000 + 1; i++) (void)inb(0x80); }

/* One verb, one answer. Set-up and control only, polled. A codec that stops
 * answering must not cost the boot minutes: after a few misses nothing is sent. */
static int cmd_dead;

static uint32_t cmd(int nid, uint32_t verb)
{
	if (cmd_dead >= 3) return 0xFFFFFFFFu;
	uint32_t c = ((uint32_t)codec << 28) | ((uint32_t)nid << 20) | verb;
	int wp = (R16(R_CORBWP) + 1) & (corb_n - 1);
	int was = R16(R_RIRBWP) & 0xFF;
	corb[wp] = c;
	__sync_synchronize();
	W16(R_CORBWP, (uint16_t)wp);
	for (int i = 0; i < 20000; i++) {
		int now = R16(R_RIRBWP) & 0xFF;
		if (now != was) {
			rirb_rp = now;
			return (uint32_t)rirb[now & (rirb_n - 1)];
		}
		(void)inb(0x80);
	}
	cmd_dead++;
	return 0xFFFFFFFFu;
}

static uint32_t verb12(int nid, uint32_t v, uint32_t pay) { return cmd(nid, (v << 8) | pay); }
static uint32_t param(int nid, uint32_t p) { return verb12(nid, V_GET_PARAM, p); }

static int rings_init(void)
{
	void *p = buddy_alloc(0);
	if (!p) return -1;
	memset((void *)PHYS_TO_VIRT(p), 0, 4096);
	corb = (uint32_t *)PHYS_TO_VIRT(p);
	rirb = (uint64_t *)((uint8_t *)corb + 2048);
	uint64_t phys = (uint64_t)p;

	W8(R_CORBCTL, 0);
	W8(R_RIRBCTL, 0);
	spin(100000);
	uint8_t cs = R8(R_CORBSIZE), rs = R8(R_RIRBSIZE);
	corb_n = (cs & 0x40) ? 256 : (cs & 0x20) ? 16 : 2;
	rirb_n = (rs & 0x40) ? 256 : (rs & 0x20) ? 16 : 2;
	W8(R_CORBSIZE, corb_n == 256 ? 2 : corb_n == 16 ? 1 : 0);
	W8(R_RIRBSIZE, rirb_n == 256 ? 2 : rirb_n == 16 ? 1 : 0);
	W32(R_CORBLBASE, (uint32_t)phys);
	W32(R_CORBUBASE, (uint32_t)(phys >> 32));
	W32(R_RIRBLBASE, (uint32_t)(phys + 2048));
	W32(R_RIRBUBASE, (uint32_t)((phys + 2048) >> 32));
	W16(R_CORBRP, 0x8000);                   /* reset the read pointer */
	for (int i = 0; i < 100000 && !(R16(R_CORBRP) & 0x8000); i++) ;
	W16(R_CORBRP, 0);
	for (int i = 0; i < 100000 && (R16(R_CORBRP) & 0x8000); i++) ;
	W16(R_CORBWP, 0);
	W16(R_RIRBWP, 0x8000);
	W16(R_RINTCNT, 1);
	rirb_rp = 0;
	W8(R_RIRBCTL, 2);                        /* DMA on, no interrupt */
	W8(R_CORBCTL, 2);
	spin(100000);
	return 0;
}

static int conn_list(int nid, int *out, int cap)
{
	uint32_t len = param(nid, P_CONN_LEN);
	int n = (int)(len & 0x7F), lng = (len >> 7) & 1, cnt = 0;
	int per = lng ? 2 : 4;
	if (n > 32) n = 32;
	for (int i = 0; i < n && cnt < cap; i += per) {
		uint32_t r = verb12(nid, V_GET_CONN, (uint32_t)i);
		for (int j = 0; j < per && i + j < n && cnt < cap; j++) {
			int e = lng ? (int)((r >> (16 * j)) & 0xFFFF) : (int)((r >> (8 * j)) & 0xFF);
			int range = lng ? (e >> 15) : (e >> 7);
			e &= lng ? 0x7FFF : 0x7F;
			if (range && cnt) {                  /* a range from the entry before */
				for (int k = out[cnt - 1] + 1; k <= e && cnt < cap; k++) out[cnt++] = k;
			} else {
				out[cnt++] = e;
			}
		}
	}
	return cnt;
}

static void note_amp(int nid, uint32_t caps)
{
	if (namp < (int)(sizeof(ampnodes) / sizeof(ampnodes[0]))) {
		ampnodes[namp] = nid;
		amp_steps[namp++] = (caps >> 8) & 0x7F;
	}
}

static void set_amp(int nid, int input, int index, int gain, int mute)
{
	uint32_t v = ((input ? 0x4000u : 0x8000u)) | 0x3000u | ((uint32_t)index << 8) |
	             (mute ? 0x80u : 0u) | ((uint32_t)gain & 0x7F);
	cmd(nid, ((uint32_t)V_SET_AMP << 16) | v);
}

static uint32_t wcaps_of(int nid) { return param(nid, P_WCAPS); }

/* Walks towards a converter. Powers and unmutes on the way back, so a dead end
 * leaves nothing changed. */
static int path(int nid, int depth)
{
	if (depth > 6) return 0;
	uint32_t caps = wcaps_of(nid);
	int type = (caps >> 20) & 0xF;
	if (type == W_OUT) {
		int seen = 0;
		for (int i = 0; i < ndac; i++) seen |= dacs[i] == nid;
		if (!seen && ndac < MAXDAC) dacs[ndac++] = nid;
		if (caps & 4) {                          /* output amp */
			uint32_t ac = param(nid, P_OUT_AMP);
			if (!ac) ac = param(afg, P_OUT_AMP);
			note_amp(nid, ac);
			set_amp(nid, 0, 0, (int)(ac & 0x7F), 0);
		}
		verb12(nid, V_POWER, 0);
		return 1;
	}
	if (type != W_MIX && type != W_SEL && type != W_PIN) return 0;
	int list[32], n = conn_list(nid, list, 32);
	for (int i = 0; i < n; i++) {
		if (!path(list[i], depth + 1)) continue;
		if (n > 1 && type != W_MIX) verb12(nid, V_CONN_SEL, (uint32_t)i);
		verb12(nid, V_POWER, 0);
		if (type == W_MIX && (caps & 2)) set_amp(nid, 1, i, 0, 0);   /* input amp of this leg */
		if (type != W_PIN && (caps & 4)) {
			uint32_t ac = param(nid, P_OUT_AMP);
			if (!ac) ac = param(afg, P_OUT_AMP);
			note_amp(nid, ac);
			set_amp(nid, 0, 0, (int)((ac >> 8) & 0x7F), 0);
		}
		return 1;
	}
	return 0;
}

static int codec_init(void)
{
	uint32_t nodes = param(0, P_NODES);
	int start = (nodes >> 16) & 0xFF, count = nodes & 0xFF;
	afg = 0;
	for (int i = start; i < start + count; i++)
		if ((param(i, P_FN_TYPE) & 0xFF) == 1) afg = i;
	if (!afg) return -1;
	verb12(afg, V_POWER, 0);
	spin(2000000);
	nodes = param(afg, P_NODES);
	start = (nodes >> 16) & 0xFF;
	count = nodes & 0xFF;
	int pins = 0;
	for (int i = start; i < start + count && i < MAXW + start; i++) {
		uint32_t caps = wcaps_of(i);
		if (((caps >> 20) & 0xF) != W_PIN) continue;
		uint32_t cfg = verb12(i, V_PIN_CFG, 0);
		int port = (cfg >> 30) & 3, dev = (cfg >> 20) & 0xF;
		if (port == 1) continue;                 /* nothing connected */
		if (dev != 0 && dev != 1 && dev != 2) continue;   /* line out, speaker, headphone */
		if (!path(i, 0)) continue;
		verb12(i, V_POWER, 0);
		verb12(i, V_PIN_CTL, dev == 2 ? 0xC0 : 0x40);
		verb12(i, V_EAPD, 2);
		if (caps & 4) {                          /* the pin's output amp, unmuted */
			uint32_t ac = param(i, P_OUT_AMP);
			set_amp(i, 0, 0, (int)((ac >> 8) & 0x7F), 0);
		}
		pins++;
	}
	return pins ? 0 : -1;
}

static uint16_t fmt_word(void)
{
	/* base 44.1 or 48 kHz, no multiplier or divisor beyond these two */
	uint16_t f = rate == 44100 ? 0x4000 : 0;
	f |= bits16 ? 0x10 : 0x00;
	f |= (uint16_t)(chans - 1);
	return f;
}

static void stream_setup(void)
{
	SW8(SD_CTL, SR8(SD_CTL) & ~SD_RUN);
	spin(20000);
	SW8(SD_CTL, SD_SRST);
	for (int i = 0; i < 100000 && !(SR8(SD_CTL) & SD_SRST); i++) ;
	SW8(SD_CTL, 0);
	for (int i = 0; i < 100000 && (SR8(SD_CTL) & SD_SRST); i++) ;
	SW32(SD_CBL, RING);
	SW16(SD_LVI, NCHUNK - 1);
	SW16(SD_FMT, fmt_word());
	SW32(SD_BDPL, (uint32_t)bdl_phys);
	SW32(SD_BDPU, (uint32_t)(bdl_phys >> 32));
	*(volatile uint8_t *)(sd + SD_CTL + 2) = STREAM_TAG << 4;
	for (int i = 0; i < ndac; i++) {
		cmd(dacs[i], ((uint32_t)V_SET_FMT << 16) | fmt_word());
		verb12(dacs[i], V_CONV_STREAM, (uint32_t)(STREAM_TAG << 4));
	}
}

/* Bytes the hardware has taken, kept current from its position register */
static void track(void)
{
	uint32_t pos = SR32(SD_LPIB) % RING;
	uint32_t delta = (pos + RING - lastpos) % RING;
	if (delta) {                     /* what was heard is silence if played again */
		uint32_t a = lastpos, n = delta;
		if (a + n > RING) { memset(ring, 0, a + n - RING); n = RING - a; }
		memset(ring + a, 0, n);
	}
	lastpos = pos;
	played += delta;
}

static void start(void)
{
	if (running) return;
	stream_setup();
	lastpos = 0;
	played = 0;
	written = 0;
	wpos = 0;
	running = 1;
}

/* Once there is data the stream runs; if the writer falls behind the ring plays
 * zeros, which hda_write keeps clean, until close stops it. */
static void kick(void)
{
	if (running && !(SR8(SD_CTL) & SD_RUN)) SW8(SD_CTL, SD_RUN);
}

static ssize_t hda_write(vfs_file_t *f, const void *buf, size_t len)
{
	const uint8_t *u = buf;
	size_t done = 0;
	while (done < len) {
		uint64_t fl = spin_lock_irq(&lock);
		start();
		track();
		uint64_t inflight = written > played ? written - played : 0;
		if (played > written) {          /* it ran past us: start again from its position */
			written = played;
			wpos = lastpos;
			inflight = 0;
		}
		uint32_t limit = frag_bytes * frag_count;
		uint32_t room = limit - (uint32_t)(inflight > limit ? limit : inflight);
		uint32_t n = room < (uint32_t)(len - done) ? room : (uint32_t)(len - done);
		if (n > RING - wpos) n = RING - wpos;
		spin_unlock_irq(&lock, fl);
		if (n == 0) {
			if (f->flags & O_NONBLOCK) return done ? (ssize_t)done : -11;
			if (task_has_actionable_signal()) return done ? (ssize_t)done : -4;
			task_sleep_until(kernel_ticks + 1);
			continue;
		}
		if (copy_fop_from_user(ring + wpos, u + done, n) != 0) return done ? (ssize_t)done : -14;
		__sync_synchronize();
		fl = spin_lock_irq(&lock);
		wpos = (wpos + n) % RING;
		written += n;
		accepted += n;
		kick();
		spin_unlock_irq(&lock, fl);
		done += n;
	}
	return (ssize_t)len;
}

/* Until what was written has been heard */
static void drain(void)
{
	for (int i = 0; i < 4 * TIMER_HZ; i++) {
		uint64_t fl = spin_lock_irq(&lock);
		track();
		int left = running && written > played;
		spin_unlock_irq(&lock, fl);
		if (!left || task_has_actionable_signal()) break;
		task_sleep_until(kernel_ticks + 1);
	}
}

static void stop(void)
{
	uint64_t fl = spin_lock_irq(&lock);
	accepted = 0;                    /* a reset starts ALSA's pointer over at zero */
	if (running) {
		SW8(SD_CTL, SR8(SD_CTL) & ~SD_RUN);
		running = 0;
	}
	spin_unlock_irq(&lock, fl);
	memset(ring, 0, RING);
}

#define DSP_RESET    0x5000
#define DSP_SYNC     0x5001
#define DSP_SPEED    0xC0045002u
#define DSP_STEREO   0xC0045003u
#define DSP_SETFMT   0xC0045005u
#define DSP_CHANNELS 0xC0045006u
#define DSP_GETFMTS  0x8004500Bu
#define DSP_GETCAPS     0x8004500Fu
#define DSP_GETBLKSIZE  0xC0045004u
#define DSP_SETFRAGMENT 0xC004500Au
#define DSP_SETTRIGGER  0x40045010u
#define DSP_GETTRIGGER  0x80045010u
#define DSP_NONBLOCK    0x0000500Eu
#define DSP_POST        0x00005008u
#define DSP_GETODELAY   0x80045017u
#define DSP_GETOSPACE   0x8010500Cu
#define DSP_GETOPTR     0x800C5012u
#define AFMT_S16_LE  0x10

static int hda_ioctl(vfs_file_t *f, uint32_t c, uint64_t arg)
{
	(void)f;
	int v = 0;
	switch (c) {
	case DSP_RESET: stop(); return 0;
	case DSP_SYNC:  drain(); return 0;
	case DSP_SPEED:
		if (copy_from_user(&v, (void *)arg, 4)) return -14;
		stop();
		rate = v > 0 && v <= 46000 ? 44100 : 48000;
		v = (int)rate;
		return copy_to_user((void *)arg, &v, 4) ? -14 : 0;
	case DSP_STEREO:
		if (copy_from_user(&v, (void *)arg, 4)) return -14;
		stop();
		chans = v ? 2 : 1;
		return 0;
	case DSP_CHANNELS:
		if (copy_from_user(&v, (void *)arg, 4)) return -14;
		stop();
		chans = v == 1 ? 1 : 2;
		v = chans;
		return copy_to_user((void *)arg, &v, 4) ? -14 : 0;
	case DSP_SETFMT:
		v = AFMT_S16_LE;
		return copy_to_user((void *)arg, &v, 4) ? -14 : 0;
	case DSP_GETFMTS:
		v = AFMT_S16_LE;
		return copy_to_user((void *)arg, &v, 4) ? -14 : 0;
	case DSP_GETCAPS:
		v = 1 | 0x1000;                  /* revision 1, trigger accepted */
		return copy_to_user((void *)arg, &v, 4) ? -14 : 0;
	case DSP_GETBLKSIZE:
		v = (int)frag_bytes;
		return copy_to_user((void *)arg, &v, 4) ? -14 : 0;
	case DSP_SETFRAGMENT: {              /* size in the low 16 bits as log2, count above */
		if (copy_from_user(&v, (void *)arg, 4)) return -14;
		uint32_t sel = (uint32_t)v & 0xFFFF, n = (uint32_t)v >> 16;
		sel = sel < 10 ? 10 : sel > 16 ? 16 : sel;
		n = n < 2 ? 2 : n;
		if (n > (RING - CHUNK) >> sel) n = (RING - CHUNK) >> sel;
		frag_bytes = 1U << sel;
		frag_count = n;
		v = (int)(n << 16 | sel);
		return copy_to_user((void *)arg, &v, 4) ? -14 : 0;
	}
	case DSP_SETTRIGGER: case DSP_NONBLOCK: case DSP_POST:
		return 0;
	case DSP_GETTRIGGER:
		v = 2;
		return copy_to_user((void *)arg, &v, 4) ? -14 : 0;
	case DSP_GETODELAY: {
		uint64_t fl = spin_lock_irq(&lock);
		track();
		v = written > played ? (int)(written - played) : 0;
		spin_unlock_irq(&lock, fl);
		return copy_to_user((void *)arg, &v, 4) ? -14 : 0;
	}
	case DSP_GETOSPACE: {
		int o[4];
		uint32_t limit = frag_bytes * frag_count;
		uint64_t fl = spin_lock_irq(&lock);
		track();
		uint32_t used = written > played ? (uint32_t)(written - played) : 0;
		spin_unlock_irq(&lock, fl);
		uint32_t room = used >= limit ? 0 : limit - used;
		o[0] = (int)(room / frag_bytes);
		o[1] = (int)frag_count;
		o[2] = (int)frag_bytes;
		o[3] = (int)room;
		return copy_to_user((void *)arg, o, sizeof o) ? -14 : 0;
	}
	case DSP_GETOPTR: {
		/* What the program has written and been heard. It never passes what was
		 * written, also when the ring ran on with silence: ALSA's delay is that
		 * difference and Firefox asserts it is not negative. */
		int o[3];
		uint32_t limit = frag_bytes * frag_count;
		uint64_t fl = spin_lock_irq(&lock);
		track();
		uint64_t inflight = written > played ? written - played : 0;
		uint64_t heard = accepted > inflight ? accepted - inflight : 0;
		spin_unlock_irq(&lock, fl);
		o[0] = (int)heard;
		o[1] = (int)(heard / frag_bytes);
		o[2] = (int)(heard % limit);
		return copy_to_user((void *)arg, o, sizeof o) ? -14 : 0;
	}
	default:
		return -ENOTTY;
	}
}

static int hda_poll(vfs_file_t *f)
{
	(void)f;
	uint64_t fl = spin_lock_irq(&lock);
	track();
	uint32_t used = written > played ? (uint32_t)(written - played) : 0;
	spin_unlock_irq(&lock, fl);
	return used + frag_bytes <= frag_bytes * frag_count ? 4 : 0;   /* POLLOUT */
}

static int hda_open(vfs_inode_t *in, vfs_file_t *f, int flags)
{
	(void)in; (void)f; (void)flags;
	uint64_t fl = spin_lock_irq(&lock);
	int busy = opened;
	if (!busy) opened = 1;
	if (!busy) {
		accepted = 0;
		frag_bytes = CHUNK;
		frag_count = NCHUNK - 1;
	}
	spin_unlock_irq(&lock, fl);
	return busy ? -16 : 0;
}

static int hda_close(vfs_file_t *f)
{
	(void)f;
	drain();
	stop();
	opened = 0;
	return 0;
}

static vfs_fops_t hda_fops = {
	.open = hda_open, .write = hda_write, .ioctl = hda_ioctl, .close = hda_close, .poll = hda_poll,
};

int  sound_volume(void) { return volume; }
void sound_set_volume(int v)
{
	if (!regs) return;
	volume = v < 0 ? 0 : v > 100 ? 100 : v;
	for (int i = 0; i < namp; i++)
		set_amp(ampnodes[i], 0, 0, (int)(amp_steps[i] * (uint32_t)volume / 100), volume == 0);
}

void hda_init(void)
{
	uint8_t b, d, fn;
	if (pci_find_class(4, 3, 0, &b, &d, &fn) != 0 && pci_find_class(4, 3, 0x80, &b, &d, &fn) != 0)
		return;
	pci_write16(b, d, fn, 4, (uint16_t)(pci_read16(b, d, fn, 4) | 6));   /* memory, bus master */
	uint32_t bar = pci_read32(b, d, fn, 0x10);
	uint64_t base = bar & ~0xFUL;
	if ((bar & 6) == 4) base |= (uint64_t)pci_read32(b, d, fn, 0x14) << 32;
	if (!base || (bar & 1)) return;
	regs = ioremap(base, 0x4000, VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);
	if (!regs) return;

	W32(R_INTCTL, 0);                        /* polled */
	W32(R_GCTL, R32(R_GCTL) & ~GCTL_CRST);
	for (int i = 0; i < 2000000 && (R32(R_GCTL) & GCTL_CRST); i++) ;
	spin(200000);
	W32(R_GCTL, R32(R_GCTL) | GCTL_CRST);
	for (int i = 0; i < 2000000 && !(R32(R_GCTL) & GCTL_CRST); i++) ;
	spin(3000000);                           /* codecs announce themselves in 521 us */
	uint16_t present = R16(R_STATESTS);
	W16(R_STATESTS, present);
	if (!present) { kprintf(KERN_WARN "hda: no codec\n"); regs = NULL; return; }
	if (rings_init() != 0) { regs = NULL; return; }

	uint32_t vendor = 0;
	for (int c = 0; c < 15; c++) {           /* a display's HDMI codec cannot drive speakers */
		if (!(present & (1 << c))) continue;
		codec = c;
		uint32_t id = param(0, 0);
		vendor = id;
		if ((id >> 16) != 0x8086) break;
	}
	if (!vendor || vendor == 0xFFFFFFFFu || codec_init() != 0 || !ndac || cmd_dead >= 3) {
		kprintf(KERN_WARN "hda: codec %d (0x%08x) has no output path\n", codec, vendor);
		regs = NULL;
		return;
	}

	uint16_t gcap = R16(R_GCAP);
	int iss = (gcap >> 8) & 0xF;
	sd = regs + 0x80 + 0x20 * iss;           /* the first output stream */

	int order = 0;
	while ((4096u << order) < RING) order++;
	void *rp = buddy_alloc(order), *bp = buddy_alloc(0);
	if (!rp || !bp) { regs = NULL; return; }
	ring_phys = (uint64_t)rp;
	ring = (uint8_t *)PHYS_TO_VIRT(rp);
	bdl_phys = (uint64_t)bp;
	uint32_t *bdl = (uint32_t *)PHYS_TO_VIRT(bp);
	memset(ring, 0, RING);
	memset(bdl, 0, 4096);
	for (uint32_t i = 0; i < NCHUNK; i++) {
		uint64_t a = ring_phys + i * CHUNK;
		bdl[i * 4 + 0] = (uint32_t)a;
		bdl[i * 4 + 1] = (uint32_t)(a >> 32);
		bdl[i * 4 + 2] = CHUNK;
		bdl[i * 4 + 3] = 0;
	}
	sound_set_volume(volume);
	devfs_register_device_gid("dsp", S_IFCHR | 0660, DEVFS_GID_AUDIO, 0x0E03, &hda_fops);
	kprintf(KERN_INFO "hda: codec %d (0x%08x), %d output converter(s), /dev/dsp\n", codec, vendor, ndac);
}
