#include <kernel/csprng.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>

extern volatile uint64_t kernel_ticks;

/* ── arch entropy source ─────────────────────────────────────
 * A free-running hardware counter (rdtsc / cntvct) gives per-call jitter; on
 * x86 RDSEED/RDRAND give a real hardware RNG when present. */

static uint64_t hw_counter(void)
{
#if defined(__x86_64__)
	uint32_t lo, hi;
	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
	uint64_t c;
	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(c));
	return c;
#else
	return kernel_ticks;
#endif
}

#if defined(__x86_64__)
static int x86_has_rdseed = -1, x86_has_rdrand = -1;

static void x86_probe_rng(void)
{
	uint32_t a, b, c, d;
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
	x86_has_rdrand = (c >> 30) & 1;
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
	x86_has_rdseed = (b >> 18) & 1;
}

static int hw_random64(uint64_t *out)
{
	if (x86_has_rdseed < 0) x86_probe_rng();
	if (x86_has_rdseed) {
		for (int i = 0; i < 64; i++) {
			uint64_t v; uint8_t ok;
			__asm__ volatile("rdseed %0; setc %1" : "=r"(v), "=qm"(ok));
			if (ok) { *out = v; return 1; }
		}
	}
	if (x86_has_rdrand) {
		for (int i = 0; i < 64; i++) {
			uint64_t v; uint8_t ok;
			__asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
			if (ok) { *out = v; return 1; }
		}
	}
	return 0;
}
#else
static int hw_random64(uint64_t *out) { (void)out; return 0; }
#endif

/* ── ChaCha20 core ───────────────────────────────────────────── */

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QR(a, b, c, d) \
	a += b; d ^= a; d = ROTL32(d, 16); \
	c += d; b ^= c; b = ROTL32(b, 12); \
	a += b; d ^= a; d = ROTL32(d, 8);  \
	c += d; b ^= c; b = ROTL32(b, 7)

static void chacha20_block(const uint32_t in[16], uint32_t out[16])
{
	for (int i = 0; i < 16; i++) out[i] = in[i];
	for (int r = 0; r < 10; r++) {
		QR(out[0], out[4], out[8],  out[12]);
		QR(out[1], out[5], out[9],  out[13]);
		QR(out[2], out[6], out[10], out[14]);
		QR(out[3], out[7], out[11], out[15]);
		QR(out[0], out[5], out[10], out[15]);
		QR(out[1], out[6], out[11], out[12]);
		QR(out[2], out[7], out[8],  out[13]);
		QR(out[3], out[4], out[9],  out[14]);
	}
	for (int i = 0; i < 16; i++) out[i] += in[i];
}

/* ── DRBG state ──────────────────────────────────────────────── */

static uint32_t   drbg_state[16];   /* chacha "constant|key|counter|nonce" */
static spinlock_t drbg_lock = SPINLOCK_INIT;
static uint64_t   drbg_reseed_at;   /* kernel_ticks of the next forced reseed */
static int        drbg_ready;

static const uint32_t SIGMA[4] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574 };

/* fold whatever entropy we can scrape into the key words */
static void drbg_stir(void)
{
	uint64_t s[4];
	for (int i = 0; i < 4; i++) {
		uint64_t h;
		if (!hw_random64(&h)) h = hw_counter() ^ (kernel_ticks << 16);
		/* mix the counter every word so successive RDSEED failures still differ */
		h ^= hw_counter();
		s[i] = h;
	}
	const uint32_t *w = (const uint32_t *)s;
	for (int i = 0; i < 8; i++) drbg_state[4 + i] ^= w[i];     /* key */
	drbg_state[14] ^= (uint32_t)kernel_ticks;
	drbg_state[15] ^= (uint32_t)(hw_counter() >> 17);
}

void csprng_init(void)
{
	memcpy(drbg_state, SIGMA, sizeof(SIGMA));
	for (int i = 4; i < 16; i++) drbg_state[i] = 0;
	drbg_state[12] = 0;                                        /* block counter */
	for (int i = 0; i < 4; i++) drbg_stir();                  /* whiten the key */
	drbg_reseed_at = kernel_ticks + 8192;
	drbg_ready = 1;
}

void csprng_add_entropy(const void *data, size_t len)
{
	if (!data || !len) return;
	const uint8_t *p = data;
	uint64_t fl = spin_lock_irq(&drbg_lock);
	uint8_t *key = (uint8_t *)&drbg_state[4];                  /* 32-byte key region */
	for (size_t i = 0; i < len; i++) key[i & 31] ^= p[i];
	spin_unlock_irq(&drbg_lock, fl);
}

void csprng_bytes(void *out, size_t len)
{
	uint8_t *o = out;
	uint64_t fl = spin_lock_irq(&drbg_lock);
	if (!drbg_ready) { spin_unlock_irq(&drbg_lock, fl); csprng_init(); fl = spin_lock_irq(&drbg_lock); }
	if ((int64_t)(kernel_ticks - drbg_reseed_at) >= 0) {
		drbg_stir();
		drbg_reseed_at = kernel_ticks + 8192;
	}
	while (len) {
		uint32_t ks[16];
		chacha20_block(drbg_state, ks);
		if (++drbg_state[12] == 0) drbg_state[13]++;          /* 64-bit counter */
		size_t chunk = len < 64 ? len : 64;
		memcpy(o, ks, chunk);
		o += chunk; len -= chunk;
		/* forward secrecy: re-key from the first 32 bytes of fresh keystream */
		uint32_t rk[16];
		chacha20_block(drbg_state, rk);
		if (++drbg_state[12] == 0) drbg_state[13]++;
		memcpy(&drbg_state[4], rk, 32);
	}
	spin_unlock_irq(&drbg_lock, fl);
}
