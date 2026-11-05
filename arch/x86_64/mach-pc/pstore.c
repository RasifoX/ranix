/* Persistent kernel log in a fixed physical RAM region. Why RAM, why this
 * address, and how Linux reads it back: docs/watchdog_pstore.md.
 *
 * The region holds two slots, written alternately, so a boot never overwrites the
 * log of the boot before it. scripts/ranix-lastlog.py parses the same layout.
 *
 * The direct map is write-back and a reset discards dirty cache lines, so every
 * commit pushes the new bytes to DRAM with CLFLUSH, data before header. A hang
 * therefore loses at most the line being printed. CLFLUSH rather than an uncached
 * mapping: an uncached alias of RAM the direct map also maps write-back is
 * undefined behaviour on x86.
 */

#include <uapi/types.h>
#include <kernel/board.h>
#include <kernel/cmdline.h>
#include <kernel/kprintf.h>
#include <kernel/lib/string.h>
#include <kernel/pstore.h>
#include <asm/mmu.h>

#define PSTORE_BASE     0x20000000UL
#define PSTORE_SIZE     (1UL << 20)
#define SLOT_SIZE       (PSTORE_SIZE / 2)
#define HDR_SIZE        4096UL
#define DATA_SIZE       (SLOT_SIZE - HDR_SIZE)
#define CACHELINE       64UL
#define PSTORE_VERSION  1
#define REASON_MAX      64

typedef struct {
	char     magic[8];        /* "RANIXLOG" */
	uint32_t version;
	uint32_t data_size;
	uint64_t seq;             /* boot counter; the newest slot has the highest */
	uint64_t total;           /* bytes ever logged; head = total % data_size */
	uint64_t beat_ticks;      /* uptime at the last watchdog pet */
	uint32_t state;           /* PSTORE_STATE_* */
	uint32_t data_crc;        /* CRC-32 of the stream; checkable until it wraps */
	char     reason[REASON_MAX];
	uint32_t hdr_crc;         /* CRC-32 of everything above */
} __attribute__((packed)) pstore_hdr_t;

_Static_assert(sizeof(pstore_hdr_t) <= 2 * CACHELINE, "header must fit its flush");

static const char pstore_magic[8] = { 'R','A','N','I','X','L','O','G' };

static uint64_t      ps_base;
static pstore_hdr_t *ps_hdr;
static char         *ps_data;
static uint64_t      ps_total;
static uint64_t      ps_flushed;
static uint32_t      ps_crc = 0xFFFFFFFFu;

/* Reflected CRC-32 (zlib polynomial), nibble table: small, no SSE. */
static const uint32_t crc_nibble[16] = {
	0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
	0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
	0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
	0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

static inline uint32_t crc_byte(uint32_t crc, uint8_t b)
{
	crc = crc_nibble[(crc ^ b) & 0xF] ^ (crc >> 4);
	return crc_nibble[(crc ^ (b >> 4)) & 0xF] ^ (crc >> 4);
}

static uint32_t crc_buf(const void *p, size_t n)
{
	const uint8_t *b = p;
	uint32_t crc = 0xFFFFFFFFu;
	while (n--) crc = crc_byte(crc, *b++);
	return ~crc;
}

static inline void flush_line(const void *p)
{
	__asm__ volatile("clflush %0" : : "m"(*(const char *)p));
}

static inline void flush_fence(void)
{
	__asm__ volatile("mfence" : : : "memory");
}

static uint32_t hdr_crc_of(const pstore_hdr_t *h)
{
	return crc_buf(h, __builtin_offsetof(pstore_hdr_t, hdr_crc));
}

static void hdr_publish(void)
{
	ps_hdr->total    = ps_total;
	ps_hdr->data_crc = ~ps_crc;
	ps_hdr->hdr_crc  = hdr_crc_of(ps_hdr);
	flush_line(ps_hdr);
	flush_line((const char *)ps_hdr + CACHELINE);
	flush_fence();
}

static int hdr_valid(const pstore_hdr_t *h)
{
	return memcmp(h->magic, pstore_magic, sizeof(pstore_magic)) == 0 &&
	       h->version == PSTORE_VERSION &&
	       h->data_size == DATA_SIZE &&
	       h->hdr_crc == hdr_crc_of(h);
}

struct region_ctx { uint64_t base, end; int inside; };

static void region_check(uint64_t b, uint64_t s, void *ud)
{
	struct region_ctx *c = ud;
	if (c->base >= b && c->end <= b + s) c->inside = 1;
}

static int parse_hex(const char *s, uint64_t *out)
{
	uint64_t v = 0;
	if (s[0] == '0' && s[1] == 'x') s += 2;
	if (!*s) return -1;
	for (; *s; s++) {
		int d;
		if      (*s >= '0' && *s <= '9') d = *s - '0';
		else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
		else return -1;
		v = (v << 4) | (uint64_t)d;
	}
	*out = v;
	return 0;
}

int pstore_region(uint64_t lowest_free, uint64_t *base, uint64_t *size)
{
	extern uint64_t boot_initrd_start, boot_initrd_size;
	uint64_t want = PSTORE_BASE;
	char buf[24];

	if (cmdline_get("ranix.pstore", buf, sizeof(buf))) {
		if (strcmp(buf, "off") == 0) {
			kprintf(KERN_INFO "pstore: off (ranix.pstore=off)\n");
			return -1;
		}
		if (parse_hex(buf, &want) != 0 || (want & (PSTORE_SIZE - 1))) {
			kprintf(KERN_WARN "pstore: bad ranix.pstore='%s', staying off\n", buf);
			return -1;
		}
	}

	struct region_ctx rc = { want, want + PSTORE_SIZE, 0 };
	board_foreach_mem_region(region_check, &rc);
	if (!rc.inside) {
		kprintf(KERN_WARN "pstore: 0x%lx is not usable RAM here, no persistent log\n", want);
		return -1;
	}

	uint64_t rsv_phys; uint32_t rsv_sz;
	board_get_reserved(&rsv_phys, &rsv_sz);
	int clash = want < lowest_free;
	if (boot_initrd_start && want < boot_initrd_start + boot_initrd_size &&
	    rc.end > boot_initrd_start) clash = 1;
	if (rsv_phys && want < rsv_phys + rsv_sz && rc.end > rsv_phys) clash = 1;
	if (clash) {
		kprintf(KERN_WARN "pstore: 0x%lx overlaps boot data, no persistent log\n", want);
		return -1;
	}

	ps_base = want;
	*base = want;
	*size = PSTORE_SIZE;
	return 0;
}

void pstore_cancel(void) { ps_base = 0; }

static const char *state_name(uint32_t st)
{
	switch (st) {
	case PSTORE_STATE_RUNNING:  return "no orderly end (hang, watchdog or power loss)";
	case PSTORE_STATE_REBOOT:   return "clean reboot";
	case PSTORE_STATE_POWEROFF: return "clean poweroff";
	case PSTORE_STATE_PANIC:    return "panic";
	default:                    return "unknown";
	}
}

static void report_previous(const pstore_hdr_t *h, const char *data)
{
	const char *crc;
	if (h->total > DATA_SIZE)
		crc = "wrapped, data not checked";
	else
		crc = crc_buf(data, h->total) == h->data_crc ? "data intact" : "DATA CRC MISMATCH";
	kprintf(KERN_INFO "pstore: previous boot #%lu: %s, %lu bytes, %s, last pet at %lus\n",
	        h->seq, state_name(h->state), h->total, crc, h->beat_ticks / TIMER_HZ);
	if (h->state == PSTORE_STATE_PANIC)
		kprintf(KERN_WARN "pstore: previous panic: %s\n", h->reason);
}

void pstore_attach(void)
{
	if (!ps_base) return;

	char *slot[2] = { (char *)PHYS_TO_VIRT(ps_base),
	                  (char *)PHYS_TO_VIRT(ps_base + SLOT_SIZE) };
	const pstore_hdr_t *h0 = (const pstore_hdr_t *)slot[0];
	const pstore_hdr_t *h1 = (const pstore_hdr_t *)slot[1];
	int v0 = hdr_valid(h0), v1 = hdr_valid(h1);
	int newest = -1;
	if (v0 && v1)    newest = h0->seq >= h1->seq ? 0 : 1;
	else if (v0)     newest = 0;
	else if (v1)     newest = 1;

	int use = newest == 0 ? 1 : 0;
	uint64_t seq = newest < 0 ? 1 : ((const pstore_hdr_t *)slot[newest])->seq + 1;

	if (newest >= 0)
		report_previous((const pstore_hdr_t *)slot[newest], slot[newest] + HDR_SIZE);
	else
		kprintf(KERN_INFO "pstore: no previous log (first boot, or RAM did not survive)\n");

	pstore_hdr_t *h = (pstore_hdr_t *)slot[use];
	memset(h, 0, sizeof(*h));
	memcpy(h->magic, pstore_magic, sizeof(pstore_magic));
	h->version   = PSTORE_VERSION;
	h->data_size = DATA_SIZE;
	h->seq       = seq;
	h->state     = PSTORE_STATE_RUNNING;
	ps_data = slot[use] + HDR_SIZE;

	/* Everything logged before the page allocator existed is already in the ring. */
	size_t n = kprintf_read_log(ps_data, DATA_SIZE);
	for (size_t i = 0; i < n; i++) ps_crc = crc_byte(ps_crc, (uint8_t)ps_data[i]);
	ps_total = n;
	ps_hdr = h;
	pstore_commit();

	kprintf(KERN_INFO "pstore: logging to 0x%lx slot %d, boot #%lu\n", ps_base, use, seq);
}

void pstore_putc(char c)
{
	if (!ps_hdr) return;
	ps_data[ps_total % DATA_SIZE] = c;
	ps_crc = crc_byte(ps_crc, (uint8_t)c);
	ps_total++;
}

void pstore_commit(void)
{
	if (!ps_hdr) return;

	uint64_t n = ps_total - ps_flushed;
	if (n > DATA_SIZE) n = DATA_SIZE;
	uint64_t off = (ps_total - n) % DATA_SIZE;
	while (n) {
		uint64_t step = CACHELINE - (off & (CACHELINE - 1));
		if (step > n) step = n;
		flush_line(ps_data + off);
		off = (off + step) % DATA_SIZE;
		n -= step;
	}
	flush_fence();          /* data reaches DRAM before the header admits to it */
	ps_flushed = ps_total;
	hdr_publish();
}

void pstore_beat(uint64_t ticks)
{
	if (!ps_hdr) return;
	ps_hdr->beat_ticks = ticks;
	pstore_commit();
}

void pstore_end(uint32_t state, const char *reason)
{
	if (!ps_hdr) return;
	/* A reset that follows a panic must not relabel it a clean reboot. */
	if (ps_hdr->state == PSTORE_STATE_PANIC) return;

	/* Publish the state before touching the string. reason may be a corrupt
	 * pointer, and a fault with the header half-written would leave a stale CRC
	 * and lose this whole slot, in the one case the log exists for. */
	ps_hdr->state = state;
	pstore_commit();
	if (!reason) return;

	char copy[REASON_MAX];
	size_t i = 0;
	for (; i < REASON_MAX - 1 && reason[i]; i++) copy[i] = reason[i];
	copy[i] = '\0';
	memcpy(ps_hdr->reason, copy, i + 1);
	pstore_commit();
}
