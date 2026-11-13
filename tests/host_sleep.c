/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Host test: the pure half of suspend (kernel/lib/sleep_math.c).
 *   - _Sx_ packages found in AML without an interpreter, including one inside
 *     If (SSx) with SSx a constant Name, and the real Casper DSDT if given;
 *   - how long the machine slept: the counter when it kept counting (s2idle),
 *     the RTC when it was reset (S3) or disagrees;
 *   - the clocks across a sleep: MONOTONIC stands still, BOOTTIME and REALTIME
 *     move by the sleep;
 *   - /sys/power/state, mem_sleep and rtc wakealarm parsing.
 *   cc -DHOST_TEST -I. -Iinclude tests/host_sleep.c kernel/lib/sleep_math.c [CASPER_DSDT.aml] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/sleep.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

/* a table header (36 bytes) then the body */
static uint32_t table(uint8_t *out, const uint8_t *body, uint32_t n)
{
	memset(out, 0, 36);
	memcpy(out, "DSDT", 4);
	memcpy(out + 36, body, n);
	uint32_t len = 36 + n;
	memcpy(out + 4, &len, 4);
	return len;
}

static void aml_cases(void)
{
	uint8_t t[256];
	uint8_t a, b;

	/* Name (_S3_, Package (4) { 5, 0, 0, 0 }) */
	static const uint8_t plain[] = { 0x08, '_', 'S', '3', '_', 0x12, 0x07, 0x04, 0x0A, 0x05, 0x00, 0x00, 0x00 };
	uint32_t n = table(t, plain, sizeof plain);
	CHECK(acpi_sleep_type(t, n, 3, &a, &b) == SLEEP_TYPE_OK && a == 5 && b == 0, "plain _S3_ (a=%u b=%u)", a, b);
	CHECK(acpi_sleep_type(t, n, 5, &a, &b) == SLEEP_TYPE_NONE, "no _S5_ in a table with only _S3_");

	/* One and Zero as values, two-byte PkgLength, root prefix */
	static const uint8_t root[] = { 0x08, '\\', '_', 'S', '1', '_', 0x12, 0x40, 0x00, 0x04, 0x01, 0x0A, 0x02, 0x00, 0x00 };
	n = table(t, root, sizeof root);
	CHECK(acpi_sleep_type(t, n, 1, &a, &b) == SLEEP_TYPE_OK && a == 1 && b == 2, "\\_S1_ with One, 2-byte PkgLength (a=%u b=%u)", a, b);

	/* a reference that is not a Name: skipped, the real one after it found */
	static const uint8_t ref[] = { 0x70, '_', 'S', '3', '_', 0x60,
	                               0x08, '_', 'S', '3', '_', 0x12, 0x07, 0x04, 0x0A, 0x05, 0x0A, 0x05, 0x00 };
	n = table(t, ref, sizeof ref);
	CHECK(acpi_sleep_type(t, n, 3, &a, &b) == SLEEP_TYPE_OK && a == 5 && b == 5, "reference skipped (a=%u b=%u)", a, b);

	/* If (SS3) { Name (_S3_ ...) } with Name (SS3, One) / Zero / a field */
	static const uint8_t cond1[] = { 0x08, 'S', 'S', '3', '_', 0x01,
	                                 0xA0, 0x12, 'S', 'S', '3', '_',
	                                 0x08, '_', 'S', '3', '_', 0x12, 0x07, 0x04, 0x0A, 0x05, 0x00, 0x00, 0x00 };
	n = table(t, cond1, sizeof cond1);
	CHECK(acpi_sleep_type(t, n, 3, &a, &b) == SLEEP_TYPE_OK && a == 5, "If (SS3) with SS3 = One");
	uint8_t cond0[sizeof cond1];
	memcpy(cond0, cond1, sizeof cond1);
	cond0[5] = 0x00;                                  /* Name (SS3, Zero) */
	n = table(t, cond0, sizeof cond0);
	CHECK(acpi_sleep_type(t, n, 3, &a, &b) == SLEEP_TYPE_NONE, "If (SS3) with SS3 = Zero: no S3");
	uint8_t condf[sizeof cond1];
	memcpy(condf, cond1, sizeof cond1);
	memcpy(condf + 1, "XX3_", 4);                     /* SS3 not a constant here (a field, say) */
	n = table(t, condf, sizeof condf);
	CHECK(acpi_sleep_type(t, n, 3, &a, &b) == SLEEP_TYPE_COND && a == 5, "If (SS3) unknown: conditional");

	/* truncated package at the end of the table: no read past it */
	static const uint8_t cut[] = { 0x08, '_', 'S', '3', '_', 0x12, 0x07, 0x04, 0x0A };
	n = table(t, cut, sizeof cut);
	CHECK(acpi_sleep_type(t, n, 3, &a, &b) == SLEEP_TYPE_NONE, "truncated package");
	CHECK(acpi_sleep_type(t, n, 9, &a, &b) == SLEEP_TYPE_NONE, "state 9");
}

static void casper(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) { printf("skip: no Casper DSDT at %s\n", path); return; }
	static uint8_t d[4 << 20];
	uint32_t n = (uint32_t)fread(d, 1, sizeof d, f);
	fclose(f);
	uint8_t a, b;
	CHECK(acpi_sleep_type(d, n, 3, &a, &b) == SLEEP_TYPE_OK && a == 5 && b == 0, "Casper _S3_ = 5 (a=%u)", a);
	CHECK(acpi_sleep_type(d, n, 4, &a, &b) == SLEEP_TYPE_OK && a == 6, "Casper _S4_ = 6 (a=%u)", a);
	CHECK(acpi_sleep_type(d, n, 5, &a, &b) == SLEEP_TYPE_OK && a == 7, "Casper _S5_ = 7 (a=%u)", a);
	CHECK(acpi_sleep_type(d, n, 1, &a, &b) == SLEEP_TYPE_NONE, "Casper _S1_ under If (SS1), SS1 = Zero: none");
}

#define S 1000000000ULL

static void length_cases(void)
{
	/* s2idle, TSC kept counting: the counter, to the ns */
	CHECK(sleep_length_ns(SLEEP_S2IDLE, 100 * S, 105 * S + 123, 1000, 1005) == 5 * S + 123, "s2idle counter");
	/* the counter went back (reset): the RTC */
	CHECK(sleep_length_ns(SLEEP_S2IDLE, 100 * S, 3 * S, 1000, 1007) == 7 * S, "counter reset -> rtc");
	/* the counter says 1 s, the RTC 60 s (TSC stopped in a deep state): the RTC */
	CHECK(sleep_length_ns(SLEEP_S2IDLE, 100 * S, 101 * S, 1000, 1060) == 60 * S, "counter disagrees -> rtc");
	/* within 2 s of each other: the counter (the RTC only has seconds) */
	CHECK(sleep_length_ns(SLEEP_S2IDLE, 100 * S, 101 * S + 900000000ULL, 1000, 1001) == S + 900000000ULL, "agree -> counter");
	/* S3: always the RTC, even if the counter looks continuous */
	CHECK(sleep_length_ns(SLEEP_DEEP, 100 * S, 200 * S, 1000, 1004) == 4 * S, "deep -> rtc");
	/* RTC unreadable (0) and counter reset: nothing known, 0 */
	CHECK(sleep_length_ns(SLEEP_DEEP, 100 * S, 3 * S, 0, 0) == 0, "nothing known -> 0");
	/* RTC going back (someone set it during the sleep): never negative */
	CHECK(sleep_length_ns(SLEEP_DEEP, 0, 0, 1000, 900) == 0, "rtc back -> 0");
}

static void clock_cases(void)
{
	sleep_clock_t c;
	memset(&c, 0, sizeof c);
	/* before: monotonic 50 s, boot offset 0, realtime offset 1.7e9 s */
	c.rt_off_sec = 1700000000; c.rt_off_nsec = 250000000;
	sleep_clock_resume(&c, 50 * S + 5, 7 * S + 900000000ULL);
	/* the counter is re-anchored at the monotonic time of the entry */
	CHECK(c.anchor_ns == 50 * S + 5, "anchor = monotonic at entry");
	CHECK(c.boot_off_sec == 7 && c.boot_off_nsec == 900000000, "boot offset += sleep (%lld.%09lld)",
	      (long long)c.boot_off_sec, (long long)c.boot_off_nsec);
	CHECK(c.rt_off_sec == 1700000008 && c.rt_off_nsec == 150000000, "realtime offset += sleep, carried (%lld.%09lld)",
	      (long long)c.rt_off_sec, (long long)c.rt_off_nsec);
	CHECK(c.slept_total_ns == 7 * S + 900000000ULL && c.count == 1, "totals");
	sleep_clock_resume(&c, 60 * S, 2 * S + 200000000ULL);
	CHECK(c.boot_off_sec == 10 && c.boot_off_nsec == 100000000, "second sleep adds (%lld.%09lld)",
	      (long long)c.boot_off_sec, (long long)c.boot_off_nsec);
	CHECK(c.count == 2, "count");
}

static void parse_cases(void)
{
	CHECK(sleep_parse_state("freeze\n", 7, SLEEP_S2IDLE) == SLEEP_S2IDLE, "freeze");
	CHECK(sleep_parse_state("mem", 3, SLEEP_S2IDLE) == SLEEP_S2IDLE, "mem -> mem_sleep default s2idle");
	CHECK(sleep_parse_state("mem\n", 4, SLEEP_DEEP) == SLEEP_DEEP, "mem -> deep when chosen");
	CHECK(sleep_parse_state("disk", 4, SLEEP_S2IDLE) < 0, "disk refused");
	CHECK(sleep_parse_state("standby", 7, SLEEP_S2IDLE) < 0, "standby refused");
	CHECK(sleep_parse_state("memx", 4, SLEEP_S2IDLE) < 0, "memx refused");
	CHECK(sleep_parse_mem_sleep("deep\n", 5) == SLEEP_DEEP, "mem_sleep deep");
	CHECK(sleep_parse_mem_sleep("s2idle", 6) == SLEEP_S2IDLE, "mem_sleep s2idle");
	CHECK(sleep_parse_mem_sleep("shallow", 7) < 0, "mem_sleep shallow refused");

	char b[64];
	CHECK(sleep_format_mem_sleep(b, sizeof b, SLEEP_S2IDLE, 0) > 0 && !strcmp(b, "[s2idle]\n"), "mem_sleep text s2idle only: %s", b);
	CHECK(sleep_format_mem_sleep(b, sizeof b, SLEEP_S2IDLE, 1) > 0 && !strcmp(b, "[s2idle] deep\n"), "mem_sleep text both: %s", b);
	CHECK(sleep_format_mem_sleep(b, sizeof b, SLEEP_DEEP, 1) > 0 && !strcmp(b, "s2idle [deep]\n"), "mem_sleep text deep: %s", b);

	int64_t at;
	CHECK(wakealarm_parse("+5\n", 3, 1000, &at) == 0 && at == 1005, "+5");
	CHECK(wakealarm_parse("1700000000", 10, 1000, &at) == 0 && at == 1700000000, "absolute");
	CHECK(wakealarm_parse("0\n", 2, 1000, &at) == 0 && at == 0, "0 clears");
	CHECK(wakealarm_parse("+", 1, 1000, &at) < 0, "+ alone");
	CHECK(wakealarm_parse("12x", 3, 1000, &at) < 0, "junk");
	CHECK(wakealarm_parse("999", 3, 1000, &at) < 0, "in the past");
	CHECK(wakealarm_parse("+99999999999999999999", 21, 1000, &at) < 0, "overflow");
}

int main(int argc, char **argv)
{
	aml_cases();
	casper(argc > 1 ? argv[1] : "tests/data/casper-dsdt.aml");
	length_cases();
	clock_cases();
	parse_cases();
	printf("[host_sleep] %d/%d\n", checks - fails, checks);
	return fails ? 1 : 0;
}
