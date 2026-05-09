#include <kernel/block.h>
#include <kernel/board.h>
#include <kernel/mm/vm.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <drivers/mailbox/mbox.h>

/*
 * BCM2711 EMMC2 (Arasan SDHCI 3.0) driver.
 * Read-only; uses PIO (Buffer Data Port), no DMA.
 *
 * Register layout: standard SDHCI 3.0 spec, offset from base 0xFE340000.
 */

/* ── SDHCI register offsets ────────────────��─────────────────── */
#define SDHCI_ARG2          0x00
#define SDHCI_BLKSIZECNT    0x04
#define SDHCI_ARG1          0x08
#define SDHCI_CMDTM         0x0C
#define SDHCI_RESP0         0x10
#define SDHCI_RESP1         0x14
#define SDHCI_RESP2         0x18
#define SDHCI_RESP3         0x1C
#define SDHCI_DATA          0x20
#define SDHCI_STATUS        0x24
#define SDHCI_CONTROL0      0x28
#define SDHCI_CONTROL1      0x2C
#define SDHCI_INTERRUPT     0x30
#define SDHCI_IRPT_MASK     0x34
#define SDHCI_IRPT_EN       0x38
#define SDHCI_CONTROL2      0x3C

/* CMDTM bits */
#define CMD_INDEX(n)        ((uint32_t)(n) << 24)
#define CMD_DATA_PRESENT    (1u << 21)
#define CMD_IXCHK_EN        (1u << 20)
#define CMD_CRCCHK_EN       (1u << 19)
#define CMD_RSPNS_NONE      (0u << 16)
#define CMD_RSPNS_136       (1u << 16)
#define CMD_RSPNS_48        (2u << 16)
#define CMD_RSPNS_48B       (3u << 16)
#define CMD_TM_DAT_DIR_CH   (1u << 4)   /* card→host */
#define CMD_TM_BLKCNT_EN    (1u << 1)
#define CMD_TM_MULTI_BLK    (1u << 5)

/* STATUS bits */
#define STATUS_CMD_INHIBIT  (1u << 0)
#define STATUS_DAT_INHIBIT  (1u << 1)

/* INTERRUPT bits */
#define INT_CMD_DONE        (1u << 0)
#define INT_DATA_DONE       (1u << 1)
#define INT_READ_READY      (1u << 5)
#define INT_ERROR           (1u << 15)
#define INT_ALL             0xFFFFFFFFu

/* CONTROL0 bits — register at 0x28 contains Host Control (byte 0) + Power Control (byte 1) */
#define C0_4BIT             (1u << 1)
/* Power Control byte (bits [15:8]): voltage = 3.3V (111b), bus power on (bit 0) */
#define C0_PWR_ON_33V       (0x0Fu << 8)

/* CONTROL1 bits */
#define C1_CLK_INTLEN       (1u << 0)
#define C1_CLK_STABLE       (1u << 1)
#define C1_CLK_EN           (1u << 2)
#define C1_SRST_HC          (1u << 24)
#define C1_SRST_CMD         (1u << 25)
#define C1_SRST_DATA        (1u << 26)
#define C1_TOUNIT_MAX       (0xEu << 16)

/* ── driver state ────────────────────────────────────────────── */

typedef struct {
	uint64_t base;
	uint32_t rca;
	int      hc;
	uint32_t base_clk;
} pi4_sd_t;

static pi4_sd_t       g_sd;
static block_device_t g_sd_dev;

/* ── register accessors ───────────────────��──────────────────── */

static inline uint32_t sd_read(pi4_sd_t *s, uint32_t reg)
{
	return *(volatile uint32_t *)(s->base + reg);
}

static inline void sd_write(pi4_sd_t *s, uint32_t reg, uint32_t val)
{
	*(volatile uint32_t *)(s->base + reg) = val;
}

static int sd_wait(pi4_sd_t *s, uint32_t reg, uint32_t mask, uint32_t val, uint32_t limit)
{
	for (uint32_t i = 0; i < limit; i++) {
		if ((sd_read(s, reg) & mask) == val) return 0;
		for (volatile int d = 0; d < 100; d++) {}
	}
	return -1;
}

/* ── clock ───────────────────────────────────────────────────── */

static void sd_set_clock(pi4_sd_t *s, uint32_t hz)
{
	sd_write(s, SDHCI_CONTROL1, sd_read(s, SDHCI_CONTROL1) & ~(C1_CLK_EN | C1_CLK_INTLEN));
	for (volatile int i = 0; i < 10000; i++) {}

	uint32_t div = s->base_clk / (2 * hz);
	if (div == 0) div = 1;
	if (div > 0x3FF) div = 0x3FF;
	uint32_t hi = (div & 0x300) >> 2;
	uint32_t lo = (div & 0x0FF) << 8;

	uint32_t c1 = (sd_read(s, SDHCI_CONTROL1) & 0xFFFF003F) | lo | hi | C1_CLK_INTLEN | C1_TOUNIT_MAX;
	sd_write(s, SDHCI_CONTROL1, c1);

	if (sd_wait(s, SDHCI_CONTROL1, C1_CLK_STABLE, C1_CLK_STABLE, 100000) != 0)
		kprintf("sd: clock stable timeout\n");

	sd_write(s, SDHCI_CONTROL1, sd_read(s, SDHCI_CONTROL1) | C1_CLK_EN);
	for (volatile int i = 0; i < 10000; i++) {}
}

/* ── command ─────────────────────────────────────────────────── */

static void sd_reset_cmd(pi4_sd_t *s)
{
	sd_write(s, SDHCI_INTERRUPT, INT_ALL);
	sd_write(s, SDHCI_CONTROL1, sd_read(s, SDHCI_CONTROL1) | C1_SRST_CMD);
	sd_wait(s, SDHCI_CONTROL1, C1_SRST_CMD, 0, 100000);
	sd_wait(s, SDHCI_STATUS, STATUS_CMD_INHIBIT, 0, 100000);
}

static int sd_cmd(pi4_sd_t *s, uint32_t cmd, uint32_t arg)
{
	if (sd_wait(s, SDHCI_STATUS, STATUS_CMD_INHIBIT, 0, 500000) != 0) {
		kprintf("sd: cmd inhibit timeout cmd=0x%x\n", cmd >> 24);
		return -1;
	}

	sd_write(s, SDHCI_INTERRUPT, INT_ALL);
	sd_write(s, SDHCI_ARG1, arg);
	sd_write(s, SDHCI_CMDTM, cmd);

	/* No response: wait for CMD line to free rather than polling INT_CMD_DONE,
	 * since BCM2711 EMMC2 may not assert CMD_DONE for no-response commands. */
	if (((cmd >> 16) & 0x3) == 0) {
		sd_wait(s, SDHCI_STATUS, STATUS_CMD_INHIBIT, 0, 500000);
		sd_write(s, SDHCI_INTERRUPT, INT_ALL);
		return 0;
	}

	for (uint32_t i = 0; i < 500000; i++) {
		uint32_t intr = sd_read(s, SDHCI_INTERRUPT);
		if (intr & INT_CMD_DONE) {
			sd_write(s, SDHCI_INTERRUPT, INT_CMD_DONE);
			return 0;
		}
		if (intr & INT_ERROR) {
			sd_reset_cmd(s);
			return -1;
		}
		for (volatile int d = 0; d < 100; d++) {}
	}
	sd_reset_cmd(s);
	return -1;
}

/* ── card init ───────────────────────────────────────────────── */

static int sd_card_init(pi4_sd_t *s)
{
	/* CMD0: reset */
	sd_cmd(s, CMD_INDEX(0) | CMD_RSPNS_NONE, 0);

	/* CMD8: voltage check */
	sd_cmd(s, CMD_INDEX(8) | CMD_RSPNS_48 | CMD_IXCHK_EN | CMD_CRCCHK_EN, 0x000001AA);

	/* ACMD41: poll until ready */
	int hc = 0;
	for (int i = 0; i < 1000; i++) {
		sd_cmd(s, CMD_INDEX(55) | CMD_RSPNS_48 | CMD_IXCHK_EN | CMD_CRCCHK_EN, 0);
		if (sd_cmd(s, CMD_INDEX(41) | CMD_RSPNS_48, 0x40FF8000) != 0)
			continue;
		uint32_t ocr = sd_read(s, SDHCI_RESP0);
		if (ocr & 0x80000000u) { hc = (ocr & 0x40000000u) ? 1 : 0; break; }
		for (volatile int d = 0; d < 100000; d++) {}
	}
	s->hc = hc;
	kprintf("sd: card ready: %s\n", hc ? "SDHC/SDXC" : "SDSC");

	/* CMD2: CID */
	sd_cmd(s, CMD_INDEX(2) | CMD_RSPNS_136 | CMD_CRCCHK_EN, 0);

	/* CMD3: RCA */
	sd_cmd(s, CMD_INDEX(3) | CMD_RSPNS_48 | CMD_IXCHK_EN | CMD_CRCCHK_EN, 0);
	s->rca = sd_read(s, SDHCI_RESP0) & 0xFFFF0000u;

	/* CMD7: select */
	sd_cmd(s, CMD_INDEX(7) | CMD_RSPNS_48B | CMD_IXCHK_EN | CMD_CRCCHK_EN, s->rca);
	sd_wait(s, SDHCI_STATUS, STATUS_DAT_INHIBIT, 0, 500000);

	/* ACMD6: 4-bit bus */
	sd_cmd(s, CMD_INDEX(55) | CMD_RSPNS_48 | CMD_IXCHK_EN | CMD_CRCCHK_EN, s->rca);
	sd_cmd(s, CMD_INDEX(6)  | CMD_RSPNS_48 | CMD_IXCHK_EN | CMD_CRCCHK_EN, 2);
	sd_write(s, SDHCI_CONTROL0, sd_read(s, SDHCI_CONTROL0) | C0_4BIT);

	/* CMD16: set block length (SDSC only) */
	if (!hc)
		sd_cmd(s, CMD_INDEX(16) | CMD_RSPNS_48 | CMD_IXCHK_EN | CMD_CRCCHK_EN, 512);

	sd_set_clock(s, 25000000);
	return 0;
}

/* ── read ──────────────────────────────────────────────────���─── */

static int pi4_sd_read(block_device_t *dev, uint64_t lba, void *buf, uint64_t count)
{
	pi4_sd_t *s = dev->priv;
	for (uint64_t i = 0; i < count; i++) {
		uint64_t addr = s->hc ? (lba + i) : (lba + i) * 512;

		sd_write(s, SDHCI_BLKSIZECNT, (1u << 16) | 512u);
		if (sd_wait(s, SDHCI_STATUS, STATUS_DAT_INHIBIT, 0, 500000) != 0) return -1;
		sd_write(s, SDHCI_INTERRUPT, INT_ALL);
		sd_write(s, SDHCI_ARG1, (uint32_t)addr);
		sd_write(s, SDHCI_CMDTM,
		         CMD_INDEX(17) | CMD_RSPNS_48 | CMD_IXCHK_EN | CMD_CRCCHK_EN
		         | CMD_DATA_PRESENT | CMD_TM_DAT_DIR_CH);

		if (sd_wait(s, SDHCI_INTERRUPT, INT_CMD_DONE | INT_ERROR, INT_CMD_DONE, 500000) != 0) return -1;
		sd_write(s, SDHCI_INTERRUPT, INT_CMD_DONE);

		if (sd_wait(s, SDHCI_INTERRUPT, INT_READ_READY | INT_ERROR, INT_READ_READY, 500000) != 0) return -1;
		sd_write(s, SDHCI_INTERRUPT, INT_READ_READY);

		uint32_t *dst = (uint32_t *)((uint8_t *)buf + i * 512);
		for (int w = 0; w < 128; w++)
			dst[w] = sd_read(s, SDHCI_DATA);

		if (sd_wait(s, SDHCI_INTERRUPT, INT_DATA_DONE | INT_ERROR, INT_DATA_DONE, 500000) != 0) return -1;
		sd_write(s, SDHCI_INTERRUPT, INT_DATA_DONE);
	}
	return 0;
}

static int pi4_sd_write(block_device_t *dev, uint64_t lba, const void *buf, uint64_t count)
{
	(void)dev; (void)lba; (void)buf; (void)count;
	return -1;
}

/* ── init ────────────────────────────────────────────────────── */

void pi4_sd_init(void)
{
	board_config_t *cfg = board_get_config();
	if (cfg->sd_base == 0) return;

#if defined(__aarch64__)
	g_sd.base = (uint64_t)ioremap(cfg->sd_base, 0x1000,
	                               VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);
	if (!g_sd.base) { kprintf("sd: ioremap failed\n"); return; }

	/* request 100 MHz for EMMC2 */
	mbox_set_clock(MBOX_CLK_EMMC2, 100000000);
	g_sd.base_clk = mbox_get_clock(MBOX_CLK_EMMC2);
	if (g_sd.base_clk == 0) g_sd.base_clk = 100000000;
	kprintf("sd: EMMC2 base_clk=%u Hz\n", g_sd.base_clk);

	/* full reset */
	sd_write(&g_sd, SDHCI_CONTROL1,
	         sd_read(&g_sd, SDHCI_CONTROL1) | C1_SRST_HC | C1_SRST_CMD | C1_SRST_DATA);
	if (sd_wait(&g_sd, SDHCI_CONTROL1, C1_SRST_HC | C1_SRST_CMD | C1_SRST_DATA, 0, 1000000) != 0)
		kprintf("sd: reset timeout\n");

	sd_write(&g_sd, SDHCI_IRPT_MASK, INT_ALL);
	sd_write(&g_sd, SDHCI_IRPT_EN,   0);
	sd_write(&g_sd, SDHCI_INTERRUPT,  INT_ALL);

	/* SRST_HC cleared the Power Control register (SD bus power off).
	 * Re-enable 3.3 V bus power before driving CMD/DAT lines. */
	sd_write(&g_sd, SDHCI_CONTROL0, C0_PWR_ON_33V);
	for (volatile int i = 0; i < 200000; i++) {}   /* ≥1 ms power-up */

	sd_set_clock(&g_sd, 400000);

	if (sd_card_init(&g_sd) != 0) { kprintf("sd: init failed\n"); return; }

	memset(&g_sd_dev, 0, sizeof(g_sd_dev));
	memcpy(g_sd_dev.name, "sd0", 4);
	g_sd_dev.size_blocks = 0;
	g_sd_dev.read        = pi4_sd_read;
	g_sd_dev.write       = pi4_sd_write;
	g_sd_dev.priv        = &g_sd;

	if (block_device_register(&g_sd_dev) == 0)
		kprintf("sd: EMMC2 ready\n");
#endif
}
