#include <uapi/types.h>
#include <kernel/kprintf.h>
#include <kernel/lib/string.h>
#include <kernel/smp.h>
#include <asm/mmu.h>

struct rsdp {
	char     signature[8];      /* "RSD PTR " */
	uint8_t  checksum;
	char     oemid[6];
	uint8_t  revision;          /* 0 = ACPI 1.0, >=2 = ACPI 2.0+ */
	uint32_t rsdt_addr;
	uint32_t length;
	uint64_t xsdt_addr;
	uint8_t  ext_checksum;
	uint8_t  reserved[3];
} __attribute__((packed));

struct sdt_header {
	char     signature[4];
	uint32_t length;
	uint8_t  revision;
	uint8_t  checksum;
	char     oemid[6];
	char     oem_table_id[8];
	uint32_t oem_revision;
	uint32_t creator_id;
	uint32_t creator_revision;
} __attribute__((packed));

struct madt {
	struct sdt_header h;
	uint32_t local_apic_addr;
	uint32_t flags;
	uint8_t  entries[];
} __attribute__((packed));

#define MADT_LAPIC    0   /* Processor Local APIC */
#define MADT_X2APIC   9   /* Processor Local x2APIC */

uint64_t mb2_get_rsdp(void);

/* APIC IDs of detected CPUs, logical index -> physical APIC id. Index 0 is the
 * BSP. Filled by acpi_detect_cpus; arch_smp_boot_cpu/IPI use these as the LAPIC
 * destination rather than assuming apic_id == logical index. */
uint32_t cpu_apic_id[MAX_CPUS];
static int acpi_ncpus = 0;

static uint8_t sum_bytes(const void *p, int n)
{
	const uint8_t *b = p;
	uint8_t s = 0;
	for (int i = 0; i < n; i++) s += b[i];
	return s;
}

static struct rsdp *find_rsdp(void)
{
	uint64_t phys = mb2_get_rsdp();
	if (phys) {
		struct rsdp *r = (struct rsdp *)PHYS_TO_VIRT(phys);
		if (memcmp(r->signature, "RSD PTR ", 8) == 0)
			return r;
	}

	/* Fallback: scan the EBDA (first 1 KB) and the BIOS ROM area on 16-byte
	 * boundaries — the legacy locations the spec mandates. */
	uint64_t ebda = (uint64_t)(*(uint16_t *)PHYS_TO_VIRT(0x40EUL)) << 4;
	uint64_t ranges[2][2] = { { ebda, ebda + 1024 }, { 0xE0000, 0x100000 } };
	for (int rg = 0; rg < 2; rg++) {
		if (ranges[rg][0] == 0) continue;
		for (uint64_t a = ranges[rg][0]; a < ranges[rg][1]; a += 16) {
			struct rsdp *r = (struct rsdp *)PHYS_TO_VIRT(a);
			if (memcmp(r->signature, "RSD PTR ", 8) == 0 &&
			    sum_bytes(r, 20) == 0)
				return r;
		}
	}
	return 0;
}

static void parse_madt(struct madt *m)
{
	uint32_t off = sizeof(struct sdt_header) + 8;   /* skip lapic_addr + flags */
	while (off + 2 <= m->h.length) {
		uint8_t *e = (uint8_t *)m + off;
		uint8_t type = e[0], len = e[1];
		if (len < 2) break;

		if (type == MADT_LAPIC) {
			uint8_t  apic_id = e[3];
			uint32_t flags   = *(uint32_t *)(e + 4);
			if ((flags & 1) && acpi_ncpus < (int)(sizeof(cpu_apic_id) / sizeof(cpu_apic_id[0])))
				cpu_apic_id[acpi_ncpus++] = apic_id;
		} else if (type == MADT_X2APIC) {
			uint32_t x2id  = *(uint32_t *)(e + 4);
			uint32_t flags = *(uint32_t *)(e + 8);
			if ((flags & 1) && acpi_ncpus < (int)(sizeof(cpu_apic_id) / sizeof(cpu_apic_id[0])))
				cpu_apic_id[acpi_ncpus++] = x2id;
		}
		off += len;
	}
}

static void *find_table(struct rsdp *r, const char *sig)
{
	int use_xsdt = (r->revision >= 2 && r->xsdt_addr);
	struct sdt_header *root =
		(struct sdt_header *)PHYS_TO_VIRT(use_xsdt ? r->xsdt_addr : r->rsdt_addr);

	int n = (root->length - sizeof(struct sdt_header)) / (use_xsdt ? 8 : 4);
	uint8_t *ptrs = (uint8_t *)root + sizeof(struct sdt_header);

	for (int i = 0; i < n; i++) {
		uint64_t pa;
		if (use_xsdt) memcpy(&pa, ptrs + i * 8, 8);
		else { uint32_t p32; memcpy(&p32, ptrs + i * 4, 4); pa = p32; }

		struct sdt_header *t = (struct sdt_header *)PHYS_TO_VIRT(pa);
		if (memcmp(t->signature, sig, 4) == 0)
			return t;
	}
	return 0;
}

/* ── HPET: high-resolution monotonic counter (a Linux clocksource) ──────────
 * The HPET ACPI table gives an MMIO base; CAP[63:32] is the tick period in
 * femtoseconds, MAIN_COUNTER (0xF0) a free-running 64-bit count. We enable it
 * and expose hpet_now_ns() so x86 clock_gettime/mono_time get ns resolution
 * instead of the 10 ms tick (aarch64 already has cntvct). */
static volatile uint8_t *hpet_base;
static uint64_t          hpet_period_fs;

static void hpet_init_from(struct rsdp *r)
{
	void *t = find_table(r, "HPET");
	if (!t) return;
	uint64_t base = *(uint64_t *)((uint8_t *)t + 44);   /* GAS address @ offset 44 */
	if (!base) return;
	hpet_base = (volatile uint8_t *)PHYS_TO_VIRT(base);
	hpet_period_fs = (*(volatile uint64_t *)hpet_base) >> 32;
	if (!hpet_period_fs || hpet_period_fs > 100000000ULL) { hpet_base = 0; return; }
	*(volatile uint64_t *)(hpet_base + 0x10) |= 1;      /* ENABLE_CNF */
}

uint64_t hpet_now_ns(void)
{
	if (!hpet_base) return 0;
	uint64_t cnt = *(volatile uint64_t *)(hpet_base + 0xF0);
	/* ns = cnt * period_fs / 1e6, split to avoid 64-bit overflow */
	return (cnt / 1000000ULL) * hpet_period_fs + ((cnt % 1000000ULL) * hpet_period_fs) / 1000000ULL;
}

/* Returns the number of enabled CPUs from the ACPI MADT, 0 if no MADT. */
int acpi_detect_cpus(void)
{
	if (acpi_ncpus) return acpi_ncpus;

	struct rsdp *r = find_rsdp();
	if (!r) return 0;

	hpet_init_from(r);   /* independent of the MADT */

	struct madt *m = find_table(r, "APIC");
	if (!m) return 0;

	parse_madt(m);
	return acpi_ncpus;
}
