/* Realtek RTL8168H / RTL8111H PCI Express gigabit Ethernet — polled driver.
 *
 * Status: runs on one real chip, DHCP on the Casper Nirvana; QEMU has no RTL8168
 * model, so nothing else exercises it. Target is that machine's NIC: PCI 10ec:8168, rev
 * 0x15, XID 0x541, PHY id 0x001cc800 (values read from Debian on that machine).
 *
 * Provenance. Original code; no GPL source was read for it or copied into it (in
 * particular not Linux r8169). Register facts and bring-up order come from:
 *   - Realtek, RTL8111B/RTL8168B Registers Datasheet Rev 1.0 (JATR-1076-21):
 *     register map, descriptors, PHYAR protocol, RMS and MTPS units. It predates
 *     the H part, so it does not cover anything H-specific.
 *   - FreeBSD sys/dev/re/if_re.c, sys/dev/rl/if_rlreg.h, sys/dev/mii/rgephy.c
 *     (BSD, Bill Paul et al.): the 8168G+ stop and start order, RXDV gate, MCU
 *     FIFO-empty bits, ASPM note.
 *   - OpenBSD sys/dev/ic/re.c, rtl81x9reg.h (BSD): HWREV mask, CRC handling, TX
 *     re-kick, PHYAR timing.
 *   - SerenityOS Kernel/Net/Realtek/RTL8168NetworkAdapter (BSD-2-Clause,
 *     Copyright (c) 2021, Idan Horowitz): PMCH bits, XID sub-version split.
 *   - IEEE 802.3 clause 22 (MII registers), PCI Express Base Specification
 *     (Device and Link Control).
 *
 * Deliberately not implemented. Serenity's configure_phy_h_*() and
 * hardware_quirks_h() (EPHY, ERI and MAC OCP tables, including its LL-share
 * sequence) are left out: that code describes itself as taken from Linux r8169,
 * so it is not a clean source under this project's licence rule. FreeBSD and
 * OpenBSD run this chip without any of it, and this driver follows their path.
 * Those tables are vendor tuning, so missing them may cost performance or power,
 * but they are not known to be needed for link.
 *
 * Firmware. Debian loads rtl_nic/rtl8168h-2.fw (976 bytes) on this NIC. This
 * driver loads no firmware at all: FreeBSD and OpenBSD re(4) load none for
 * 8168H, so it is not required for link, but what the patch fixes is unknown and
 * running without it is untested on this unit. A loader is absent on purpose: the
 * only description of the blob's format we know of is GPL code, and the root
 * filesystem is not mounted yet when this driver starts.
 *
 * Diagnostics (/proc/hwdiag, fjord doctor). Link speed comes from PHYstatus;
 * what both ends offered comes from the clause 22/40 registers ANAR, ANLPAR,
 * 1000BASE-T control and status (IEEE 802.3 28.2.4, 40.5.1). If both ends
 * offer 1000 Mb/s but the link resolved lower, the PHY fell back (downshift),
 * which on a working partner points at the cable: 1000BASE-T needs all four
 * pairs, 100BASE-TX only two. A kernel warning says so at link up. Error
 * counts come from the tally counter dump (DTCCR, the "Dump Tally Counter"
 * register of the RTL8169/8168 register datasheets; field layout as in
 * FreeBSD if_rlreg.h struct rl_stats). Realtek's PHY cable test (TDR) is not
 * implemented: no published document describing its registers is known to us,
 * and on this machine Debian's PHY driver reports cable testing unsupported.
 *
 * Interrupts. An MSI only wakes net_rx, which drains the ring through rtl_poll; its
 * ~10 ms timed poll remains as the fallback, so if the MSI never fires this is
 * the polled driver, which the 256-entry ring must ride out (about 25 k frames/s).
 * INTx stays off: the line is GSI 18, past the 8259 PICs, and x86 Ranix has no
 * IOAPIC. No RX or TX checksum offload, VLAN stripping, TSO or jumbo frames.
 */

#include <kernel/net/net.h>
#include <kernel/pci.h>
#include <kernel/sync.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/vm.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/irq.h>
#include <kernel/hwdiag.h>
#include <asm/mmu.h>
#include <asm/io.h>
#include <asm/idt.h>

/* ── MAC registers (BAR2, MMIO) ──────────────────────────── */
#define REG_IDR0        0x00    /* station address, 6 bytes */
#define REG_MAR0        0x08    /* multicast hash, 8 bytes */
#define REG_TNPDS_LO    0x20    /* TX ring base, 256-byte aligned */
#define REG_TNPDS_HI    0x24
#define REG_CMD         0x37    /* 8 bit */
#define REG_TPPOLL      0x38    /* 8 bit */
#define REG_IMR         0x3C    /* 16 bit */
#define REG_ISR         0x3E    /* 16 bit, write 1 to clear */
#define REG_TXCFG       0x40
#define REG_RXCFG       0x44
#define REG_MPC         0x4C    /* missed packet counter */
#define REG_CFG9346     0x50    /* 8 bit, config register lock */
#define REG_CFG1        0x52    /* 8 bit */
#define REG_PHYAR       0x60
#define REG_DTCCR_LO    0x10    /* tally dump address, bit 3 starts the dump */
#define REG_DTCCR_HI    0x14
#define DTCCR_CMD       0x00000008u
#define REG_PHYSTATUS   0x6C    /* 8 bit */
#define REG_PMCH        0x6F    /* 8 bit, PHY power */
#define REG_MCU         0xD3    /* 8 bit */
#define REG_RMS         0xDA    /* 16 bit, bytes; must be non-zero */
#define REG_CPLUS       0xE0    /* 16 bit */
#define REG_INTRMOD     0xE2    /* 16 bit */
#define REG_RDSAR_LO    0xE4    /* RX ring base, 256-byte aligned */
#define REG_RDSAR_HI    0xE8
#define REG_MTPS        0xEC    /* 8 bit, units of 128 bytes */
#define REG_MISC        0xF0

#define CMD_TE          0x04
#define CMD_RE          0x08
#define CMD_RESET       0x10

#define TPPOLL_NPQ      0x40    /* normal priority queue has work */

#define ISR_ROK         0x0001
#define ISR_RER         0x0002
#define ISR_RDU         0x0010  /* ring ran out of descriptors */
#define ISR_FOVW        0x0040  /* RX FIFO overflow */
#define ISR_SYSERR      0x8000
#define IMR_RX          (ISR_ROK | ISR_RER | ISR_RDU | ISR_FOVW)

#define TXCFG_QEMPTY    0x00000800u
#define TXCFG_VALUE     0x03000700u  /* standard IFG, max DMA burst */
#define TXCFG_HWREV     0x7C800000u  /* chip family bits */
#define HWREV_8168H     0x54000000u

/* FIFO threshold none, unlimited DMA, 64K, early-off V2 (FreeBSD's G+ value) */
#define RXCFG_VALUE     0x0000FF00u
#define RXCFG_APM       0x00000002u  /* accept our unicast */
#define RXCFG_AM        0x00000004u  /* accept multicast */
#define RXCFG_AB        0x00000008u  /* accept broadcast */
#define RXCFG_ACCEPT    0x0000003Fu  /* every accept-class bit */

#define CFG9346_UNLOCK  0xC0
#define CFG9346_LOCK    0x00
#define CFG1_DRVLOAD    0x20

#define PHYAR_FLAG      0x80000000u  /* write: clears when done; read: sets */

#define PHYSTATUS_FDX   0x01
#define PHYSTATUS_LINK  0x02
#define PHYSTATUS_10    0x04
#define PHYSTATUS_100   0x08
#define PHYSTATUS_1000  0x10

#define PMCH_PHY_ON     0xC0

#define MCU_RX_EMPTY    0x10
#define MCU_TX_EMPTY    0x20

#define CPLUS_TXENB     0x0001  /* FreeBSD: "XXX magic", needed with MACSTAT_DIS */
#define CPLUS_PCI_MRW   0x0008
#define CPLUS_MACSTAT   0x0080

#define MISC_RXDV_GATE  0x00080000u  /* set: chip ignores incoming frames */

/* ── MII (IEEE 802.3 clause 22) ──────────────────────────── */
#define MII_BMCR        0
#define MII_PHYID1      2
#define MII_BMSR        1
#define MII_ANAR        4
#define MII_ANLPAR      5
#define MII_GBCR        9
#define MII_GBSR        10
#define MII_PAGE        0x1F    /* Realtek page select, 0 = standard */

#define BMCR_RESTART    0x0200
#define BMCR_ANEG       0x1000
#define BMCR_RESET      0x8000
#define ANAR_10_100     0x01E1  /* 802.3 selector, 10/100 half and full */
#define GBCR_1000FD     0x0200
#define GBCR_1000HD     0x0100
#define BMSR_ANEG_DONE  0x0020
#define ANAR_100        0x0180  /* 100BASE-TX full | half, same bits in ANLPAR */
#define GBSR_MS_FAULT   0x8000  /* master/slave resolution failed */
#define GBSR_LP_1000    0x0C00  /* partner offers 1000BASE-T full | half */
#define GBSR_IDLE_ERR   0x00FF
#define PHYID1_REALTEK  0x001C

/* ── PCI (config space) ──────────────────────────────────── */
#define VENDOR_REALTEK  0x10EC
#define PCI_REV_CLASS   0x08
#define PCI_BAR2        0x18
#define PCI_BAR3        0x1C
#define PCI_CMD_INTX_OFF 0x0400

#define PM_CSR          0x04    /* from the PM capability */
#define PM_STATE_MASK   0x0003

#define PCIE_DEVCTL     0x08    /* from the PCIe capability */
#define PCIE_LINKCTL    0x10
#define DEVCTL_RELAXED  0x0010
#define DEVCTL_NOSNOOP  0x0800
#define LINKCTL_ASPM    0x0003
#define LINKCTL_ECPM    0x0100  /* clock power management */

/* ── descriptors ─────────────────────────────────────────── */
#define DESC_OWN        0x80000000u  /* chip owns it */
#define DESC_EOR        0x40000000u  /* last in ring */
#define DESC_FS         0x20000000u  /* first segment */
#define DESC_LS         0x10000000u  /* last segment */
#define RX_STAT_RES     0x00200000u  /* RX error summary */
#define RX_STAT_CRC     0x00080000u  /* FreeBSD RL_RDESC_STAT_CRCERR */
#define RX_LEN_MASK     0x00003FFFu

typedef struct {
    volatile uint32_t opts1;
    volatile uint32_t opts2;
    volatile uint32_t addr_lo;
    volatile uint32_t addr_hi;
} rtl_desc_t;

_Static_assert(sizeof(rtl_desc_t) == 16, "RTL8168 descriptors are 16 bytes");

#define RX_COUNT        256
#define TX_COUNT        64
#define BUF_SIZE        2048    /* per descriptor; also the DMA bound */
#define ETH_CRC_LEN     4
#define ETH_MIN_FRAME   60      /* without CRC */
#define ETH_HDR_LEN     14
#define TX_MAX_FRAME    1518    /* with a VLAN tag, without CRC */
#define MTPS_VALUE      16      /* 2048 bytes */

/* Bounded waits. rtl_udelay() is a counted loop, so these end whatever the chip
 * does; each step is only approximately the stated time. */
#define RESET_TRIES     10000   /* x 10 us */
#define TXQ_TRIES       1000    /* x 100 us */
#define FIFO_TRIES      3000    /* x 20 us */
#define PHY_TRIES       2000    /* x 25 us */
#define PHY_RESET_TRIES 1000    /* x 500 us */
#define TX_SLOT_TRIES   1000    /* x 1 us, under tx_lock with IRQs off */

typedef struct {
    uint8_t *virt;
    uint64_t phys;
    int      order;
} rtl_dma_t;

typedef struct {
    volatile uint8_t *regs;
    uint8_t  bus, dev, fn;

    rtl_dma_t rx_ring_mem, tx_ring_mem, rx_buf_mem, tx_buf_mem;
    rtl_desc_t *rx_ring;
    rtl_desc_t *tx_ring;

    /* dhcp.c calls dev->poll directly, outside net_poll's single-runner guard,
     * so the driver needs its own: rx_cur belongs to whoever holds poll_busy. */
    volatile uint32_t poll_busy;
    uint32_t rx_cur;

    spinlock_t tx_lock;     /* dev->tx runs from syscalls and net threads */
    uint32_t tx_cur;
    int      tx_stalled;    /* ring was full: fail fast until a slot frees */

    volatile int dead;      /* device stopped answering; stay away from it */
    int      syserr_logged;
    int      link_state;    /* for the speed log line only */

    /* /proc/hwdiag */
    spinlock_t diag_lock;   /* PHYAR and the tally buffer */
    int      adv_1000;      /* phy_init asked the PHY to offer 1000 Mb/s */
    uint32_t link_ups, link_drops;
    volatile uint64_t rx_missed, rx_desc_err, rx_desc_crc;
    rtl_dma_t tally;        /* 64-byte dump target, virt NULL if absent */
} rtl_t;

static rtl_t    g_rtl;
static netdev_t g_rtl_dev;

/* ── register and timing helpers ─────────────────────────── */

static inline uint8_t rd8(rtl_t *r, uint32_t off)
{
    return *(volatile uint8_t *)(r->regs + off);
}
static inline uint16_t rd16(rtl_t *r, uint32_t off)
{
    return *(volatile uint16_t *)(r->regs + off);
}
static inline uint32_t rd32(rtl_t *r, uint32_t off)
{
    return *(volatile uint32_t *)(r->regs + off);
}
static inline void wr8(rtl_t *r, uint32_t off, uint8_t v)
{
    *(volatile uint8_t *)(r->regs + off) = v;
}
static inline void wr16(rtl_t *r, uint32_t off, uint16_t v)
{
    *(volatile uint16_t *)(r->regs + off) = v;
}
static inline void wr32(rtl_t *r, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(r->regs + off) = v;
}

/* memcpy may use fast-string stores, which x86 does not order with later writes;
 * this makes frame data visible before the descriptor hands it to the chip. */
static inline void dma_wmb(void)
{
    __asm__ volatile("sfence" ::: "memory");
}

/* Ranix has no calibrated udelay. A port 0x80 read crosses to the legacy bus and
 * takes on the order of 1 us without needing a timer, and reading has no POST
 * display side effect. It is a rough delay, not a clock. */
static void rtl_udelay(uint32_t us)
{
    while (us--)
        (void)inb(0x80);
}

/* ── DMA memory ──────────────────────────────────────────── */

/* buddy pages are physically contiguous, page aligned (more than the 256 bytes
 * rings need) and x86 keeps them coherent, given No Snoop off in rtl_pci_tune. */
static int rtl_dma_alloc(rtl_dma_t *d, size_t bytes)
{
    int order = 0;
    while ((4096UL << order) < bytes) order++;

    void *phys = buddy_alloc(order);
    if (!phys) return -1;
    d->phys  = (uint64_t)phys;
    d->order = order;
    d->virt  = (uint8_t *)PHYS_TO_VIRT(d->phys);
    memset(d->virt, 0, 4096UL << order);
    return 0;
}

static void rtl_dma_free(rtl_dma_t *d)
{
    if (!d->virt) return;
    buddy_free(d->phys, d->order);
    d->virt = NULL;
}

static void rtl_free_all(rtl_t *r)
{
    rtl_dma_free(&r->rx_ring_mem);
    rtl_dma_free(&r->tx_ring_mem);
    rtl_dma_free(&r->rx_buf_mem);
    rtl_dma_free(&r->tx_buf_mem);
}

static inline uint64_t rx_buf_phys(rtl_t *r, uint32_t i)
{
    return r->rx_buf_mem.phys + (uint64_t)i * BUF_SIZE;
}

static inline uint64_t tx_buf_phys(rtl_t *r, uint32_t i)
{
    return r->tx_buf_mem.phys + (uint64_t)i * BUF_SIZE;
}

/* Hand RX slot i to the chip. OWN goes last: the chip may use the slot at once. */
static void rtl_rx_arm(rtl_t *r, uint32_t i)
{
    rtl_desc_t *d = &r->rx_ring[i];
    uint64_t pa = rx_buf_phys(r, i);

    d->addr_lo = (uint32_t)pa;
    d->addr_hi = (uint32_t)(pa >> 32);
    d->opts2   = 0;
    dma_wmb();
    d->opts1   = DESC_OWN | (i == RX_COUNT - 1 ? DESC_EOR : 0) | BUF_SIZE;
}

static int rtl_rings_init(rtl_t *r)
{
    if (rtl_dma_alloc(&r->rx_ring_mem, RX_COUNT * sizeof(rtl_desc_t)) != 0 ||
        rtl_dma_alloc(&r->tx_ring_mem, TX_COUNT * sizeof(rtl_desc_t)) != 0 ||
        rtl_dma_alloc(&r->rx_buf_mem,  RX_COUNT * (size_t)BUF_SIZE) != 0 ||
        rtl_dma_alloc(&r->tx_buf_mem,  TX_COUNT * (size_t)BUF_SIZE) != 0)
        return -1;

    r->rx_ring = (rtl_desc_t *)r->rx_ring_mem.virt;
    r->tx_ring = (rtl_desc_t *)r->tx_ring_mem.virt;

    for (uint32_t i = 0; i < RX_COUNT; i++)
        rtl_rx_arm(r, i);

    /* TX slots stay software-owned; rtl_tx fills opts1. Each has a fixed bounce
     * buffer, so a slot is free exactly when OWN is clear. */
    for (uint32_t i = 0; i < TX_COUNT; i++) {
        uint64_t pa = tx_buf_phys(r, i);
        r->tx_ring[i].addr_lo = (uint32_t)pa;
        r->tx_ring[i].addr_hi = (uint32_t)(pa >> 32);
        r->tx_ring[i].opts1   = (i == TX_COUNT - 1) ? DESC_EOR : 0;
    }
    return 0;
}

/* ── MII access through PHYAR ────────────────────────────── */

static int rtl_phy_read(rtl_t *r, uint32_t reg, uint16_t *out)
{
    wr32(r, REG_PHYAR, reg << 16);
    for (int i = 0; i < PHY_TRIES; i++) {
        uint32_t v = rd32(r, REG_PHYAR);
        if (v & PHYAR_FLAG) {
            rtl_udelay(20);
            *out = (uint16_t)v;
            return 0;
        }
        rtl_udelay(25);
    }
    return -1;
}

static int rtl_phy_write(rtl_t *r, uint32_t reg, uint16_t val)
{
    wr32(r, REG_PHYAR, PHYAR_FLAG | (reg << 16) | val);
    for (int i = 0; i < PHY_TRIES; i++) {
        if (!(rd32(r, REG_PHYAR) & PHYAR_FLAG)) {
            rtl_udelay(20);
            return 0;
        }
        rtl_udelay(25);
    }
    return -1;
}

/* Standard clause 22 bring-up only: wake, reset, advertise, autonegotiate. The
 * link comes up seconds later and net_link picks it up through link_check, so
 * nothing waits for it here. Failure is not fatal: the PHY starts autonegotiating
 * on its own after power-up, so the NIC may still get a link. */
static void rtl_phy_init(rtl_t *r)
{
    uint16_t id1 = 0, gbcr = 0, bmcr = 0;

    wr8(r, REG_PMCH, rd8(r, REG_PMCH) | PMCH_PHY_ON);

    if (rtl_phy_write(r, MII_PAGE, 0) != 0 ||
        rtl_phy_read(r, MII_PHYID1, &id1) != 0) {
        kprintf(KERN_WARN "rtl8168: phy not answering on phyar, keeping its defaults\n");
        return;
    }
    if (id1 != PHYID1_REALTEK)
        kprintf(KERN_WARN "rtl8168: unexpected phy id1 0x%04x, continuing\n", id1);

    /* Takes the PHY out of power down, as OpenBSD and FreeBSD do at attach.
     * Register 0x0e on page 0. */
    rtl_phy_write(r, 0x0e, 0);

    rtl_phy_write(r, MII_BMCR, BMCR_RESET);
    int i;
    for (i = 0; i < PHY_RESET_TRIES; i++) {
        rtl_udelay(500);
        if (rtl_phy_read(r, MII_BMCR, &bmcr) == 0 && !(bmcr & BMCR_RESET))
            break;
    }
    if (i == PHY_RESET_TRIES)
        kprintf(KERN_WARN "rtl8168: phy reset did not finish, continuing\n");

    /* No pause advertised: a NIC whose poller stalled must not flood the LAN
     * with PAUSE frames. */
    rtl_phy_write(r, MII_ANAR, ANAR_10_100);
    if (rtl_phy_read(r, MII_GBCR, &gbcr) == 0 &&
        rtl_phy_write(r, MII_GBCR, gbcr | GBCR_1000FD) == 0)
        r->adv_1000 = 1;
    rtl_phy_write(r, MII_BMCR, BMCR_ANEG | BMCR_RESTART);
}

/* ── chip control ────────────────────────────────────────── */

/* Stop anything firmware left running (a PXE ROM, say) before reset, following
 * FreeBSD's order for 8168G and later: no STOPREQ, gate RX, drain, disable.
 * Timeouts only warn, because the reset that follows is the real recovery. */
static void rtl_quiesce(rtl_t *r)
{
    int i;

    wr16(r, REG_IMR, 0);
    wr16(r, REG_ISR, 0xFFFF);
    wr32(r, REG_RXCFG, rd32(r, REG_RXCFG) & ~RXCFG_ACCEPT);
    wr32(r, REG_MISC, rd32(r, REG_MISC) | MISC_RXDV_GATE);
    rtl_udelay(2000);

    for (i = 0; i < TXQ_TRIES; i++) {
        if (rd32(r, REG_TXCFG) & TXCFG_QEMPTY) break;
        rtl_udelay(100);
    }
    if (i == TXQ_TRIES)
        kprintf(KERN_WARN "rtl8168: tx queue did not drain, resetting anyway\n");

    wr8(r, REG_CMD, rd8(r, REG_CMD) & ~(CMD_TE | CMD_RE));

    for (i = 0; i < FIFO_TRIES; i++) {
        if ((rd8(r, REG_MCU) & (MCU_TX_EMPTY | MCU_RX_EMPTY)) ==
            (MCU_TX_EMPTY | MCU_RX_EMPTY))
            break;
        rtl_udelay(20);
    }
    if (i == FIFO_TRIES)
        kprintf(KERN_WARN "rtl8168: fifos did not drain, resetting anyway\n");
}

static int rtl_reset(rtl_t *r)
{
    wr8(r, REG_CMD, CMD_RESET);
    for (int i = 0; i < RESET_TRIES; i++) {
        rtl_udelay(10);
        if (!(rd8(r, REG_CMD) & CMD_RESET)) return 0;
    }
    return -1;
}

static int rtl_read_mac(rtl_t *r, uint8_t *mac)
{
    uint8_t all_or = 0, all_and = 0xFF;

    for (int i = 0; i < 6; i++) {
        mac[i] = rd8(r, REG_IDR0 + i);
        all_or  |= mac[i];
        all_and &= mac[i];
    }
    if (all_or == 0 || all_and == 0xFF || (mac[0] & 1)) return -1;
    return 0;
}

/* Program a freshly reset chip and start it. Bus mastering goes on only now, with
 * valid rings loaded, and TE/RE last, as FreeBSD requires on 8168G and later. */
static int rtl_start(rtl_t *r)
{
    wr8(r, REG_CFG9346, CFG9346_UNLOCK);

    /* C+ command goes first, per OpenBSD and FreeBSD. */
    wr16(r, REG_CPLUS, CPLUS_PCI_MRW | CPLUS_MACSTAT | CPLUS_TXENB);
    (void)rd16(r, REG_CPLUS);

    wr16(r, REG_RMS, BUF_SIZE);
    wr8(r, REG_MTPS, MTPS_VALUE);
    wr16(r, REG_INTRMOD, 0);

    wr32(r, REG_TNPDS_HI, (uint32_t)(r->tx_ring_mem.phys >> 32));
    wr32(r, REG_TNPDS_LO, (uint32_t)r->tx_ring_mem.phys);
    wr32(r, REG_RDSAR_HI, (uint32_t)(r->rx_ring_mem.phys >> 32));
    wr32(r, REG_RDSAR_LO, (uint32_t)r->rx_ring_mem.phys);

    wr32(r, REG_MISC, rd32(r, REG_MISC) & ~MISC_RXDV_GATE);

    wr32(r, REG_TXCFG, TXCFG_VALUE);
    /* All multicast: nothing programs a hash, and IPv6 NDP needs solicited-node
     * multicast delivered. */
    wr32(r, REG_MAR0, 0xFFFFFFFFu);
    wr32(r, REG_MAR0 + 4, 0xFFFFFFFFu);
    wr32(r, REG_RXCFG, RXCFG_VALUE | RXCFG_APM | RXCFG_AM | RXCFG_AB);

    wr16(r, REG_IMR, 0);
    wr16(r, REG_ISR, 0xFFFF);
    wr32(r, REG_MPC, 0);
    wr8(r, REG_CFG1, rd8(r, REG_CFG1) | CFG1_DRVLOAD);

    uint16_t cmd = pci_read16(r->bus, r->dev, r->fn, PCI_COMMAND);
    pci_write16(r->bus, r->dev, r->fn, PCI_COMMAND, cmd | PCI_CMD_MASTER);

    wr8(r, REG_CMD, CMD_TE | CMD_RE);
    uint8_t on = rd8(r, REG_CMD);
    wr8(r, REG_CFG9346, CFG9346_LOCK);
    return (on & (CMD_TE | CMD_RE)) == (CMD_TE | CMD_RE) ? 0 : -1;
}

/* Undo rtl_start on a failed bring-up. The chip must stop using the rings before
 * the pages go back to the allocator, hence reset, then no bus master, then free. */
static void rtl_abort(rtl_t *r)
{
    if (rtl_reset(r) != 0)
        kprintf(KERN_WARN "rtl8168: chip ignored reset during teardown\n");
    uint16_t cmd = pci_read16(r->bus, r->dev, r->fn, PCI_COMMAND);
    pci_write16(r->bus, r->dev, r->fn, PCI_COMMAND, cmd & ~PCI_CMD_MASTER);
    (void)pci_read16(r->bus, r->dev, r->fn, PCI_COMMAND);
    rtl_free_all(r);
}

/* ── netdev operations ───────────────────────────────────── */

static int rtl_tx(netdev_t *dev, skb_t *skb)
{
    rtl_t *r = (rtl_t *)dev->priv;
    uint32_t len = skb->len;

    if (r->dead || len == 0 || len > TX_MAX_FRAME) {
        skb_free(skb);
        return -1;
    }

    uint64_t f = spin_lock_irq(&r->tx_lock);
    uint32_t i = r->tx_cur;
    rtl_desc_t *d = &r->tx_ring[i];

    if (d->opts1 & DESC_OWN) {
        wr8(r, REG_TPPOLL, TPPOLL_NPQ);
        if (!r->tx_stalled)
            for (int t = 0; t < TX_SLOT_TRIES && (d->opts1 & DESC_OWN); t++)
                rtl_udelay(1);
        if (d->opts1 & DESC_OWN) {
            if (!r->tx_stalled)
                kprintf(KERN_WARN "rtl8168: tx ring full, dropping until it drains\n");
            r->tx_stalled = 1;
            spin_unlock_irq(&r->tx_lock, f);
            skb_free(skb);
            return -1;
        }
    }
    r->tx_stalled = 0;

    uint8_t *buf = r->tx_buf_mem.virt + (size_t)i * BUF_SIZE;
    memcpy(buf, skb->data, len);
    if (len < ETH_MIN_FRAME) {
        memset(buf + len, 0, ETH_MIN_FRAME - len);  /* no stale frame on the wire */
        len = ETH_MIN_FRAME;
    }

    d->opts2 = 0;
    dma_wmb();
    d->opts1 = DESC_OWN | DESC_FS | DESC_LS |
               (i == TX_COUNT - 1 ? DESC_EOR : 0) | len;
    dma_wmb();
    wr8(r, REG_TPPOLL, TPPOLL_NPQ);

    r->tx_cur = (i + 1) % TX_COUNT;
    spin_unlock_irq(&r->tx_lock, f);

    skb_free(skb);
    return 0;
}

static void rtl_rx_frame(netdev_t *dev, rtl_t *r, uint32_t i, uint32_t st)
{
    uint32_t len = st & RX_LEN_MASK;

    /* A frame longer than one buffer spans descriptors; nothing here wants one
     * (BUF_SIZE exceeds the MTU), so each piece fails the FS|LS test and drops. */
    if (st & RX_STAT_RES) {
        atomic_fetch_add64(&r->rx_desc_err, 1);
        if (st & RX_STAT_CRC) atomic_fetch_add64(&r->rx_desc_crc, 1);
    }
    if ((st & (DESC_FS | DESC_LS)) != (DESC_FS | DESC_LS) || (st & RX_STAT_RES)) {
        netdev_rx_drop(dev);
        return;
    }
    if (len < ETH_HDR_LEN + ETH_CRC_LEN || len > BUF_SIZE) {
        netdev_rx_drop(dev);
        return;
    }
    len -= ETH_CRC_LEN;     /* the reported length includes the FCS */
    if (len > SKB_DATA_MAX) {
        netdev_rx_drop(dev);  /* drop, never deliver a truncated frame */
        return;
    }

    skb_t *skb = skb_alloc();
    if (!skb) {
        netdev_rx_drop(dev);
        return;
    }
    memcpy(skb->data, r->rx_buf_mem.virt + (size_t)i * BUF_SIZE, len);
    skb->len = (uint16_t)len;
    netdev_rx_enqueue(dev, skb);
}

static void rtl_poll(netdev_t *dev)
{
    rtl_t *r = (rtl_t *)dev->priv;
    if (!r || r->dead) return;

    if (atomic_cmpxchg32(&r->poll_busy, 0, 1) != 0) return;

    /* Interrupts are masked but ISR still latches; it doubles as a liveness probe,
     * since a device that fell off the bus reads as all ones. */
    uint16_t isr = rd16(r, REG_ISR);
    if (isr == 0xFFFF) {
        r->dead = 1;
        kprintf(KERN_ERR "rtl8168: device stopped responding, nic disabled\n");
        r->poll_busy = 0;
        return;
    }
    if (isr) {
        wr16(r, REG_ISR, isr);
        if ((isr & ISR_SYSERR) && !r->syserr_logged) {
            r->syserr_logged = 1;
            kprintf(KERN_ERR "rtl8168: pci system error reported by the chip\n");
        }
    }

    /* Frames the chip lost because the ring was full never reach a descriptor. */
    uint32_t missed = rd32(r, REG_MPC) & 0x00FFFFFFu;
    if (missed) {
        wr32(r, REG_MPC, 0);
        atomic_fetch_add64(&dev->rx_dropped, missed);
        atomic_fetch_add64(&r->rx_missed, missed);
    }

    /* At most one lap, so this ends even if frames arrive as fast as we drain. */
    for (int n = 0; n < RX_COUNT; n++) {
        uint32_t i = r->rx_cur;
        uint32_t st = r->rx_ring[i].opts1;

        if (st & DESC_OWN) break;
        __asm__ volatile("" ::: "memory");  /* read the frame after the status */
        rtl_rx_frame(dev, r, i, st);
        rtl_rx_arm(r, i);
        r->rx_cur = (i + 1) % RX_COUNT;
    }

    /* OpenBSD: PCIe parts can ignore a poll request made while a transmit is in
     * progress, stranding later frames. Re-kick if the newest one is still
     * pending. Reading tx_cur unlocked is fine: the worst case is a spare kick. */
    if (r->tx_ring[(r->tx_cur + TX_COUNT - 1) % TX_COUNT].opts1 & DESC_OWN)
        wr8(r, REG_TPPOLL, TPPOLL_NPQ);

    __asm__ volatile("" ::: "memory");
    r->poll_busy = 0;
}

/* No race with a frame arriving now: net_rx polls after this ack, so it either
 * sees the frame or the chip raises ISR again and with it a new MSI. */
static void rtl_irq(void)
{
    rtl_t *r = &g_rtl;
    uint16_t isr = rd16(r, REG_ISR);

    if (isr == 0xFFFF) return;      /* rtl_poll reports the dead device */
    wr16(r, REG_ISR, isr & IMR_RX);
    net_rx_wake();
}

/* What the two PHYs offered each other. Returns -1 if the PHY did not answer. */
typedef struct { uint16_t bmsr, anar, anlpar, gbcr, gbsr; } rtl_phyregs_t;

static int rtl_phy_snapshot(rtl_t *r, rtl_phyregs_t *pr)
{
    uint64_t f = spin_lock_irq(&r->diag_lock);
    int rc = rtl_phy_write(r, MII_PAGE, 0);
    if (rc == 0) rc = rtl_phy_read(r, MII_BMSR, &pr->bmsr);    /* latched: read twice */
    if (rc == 0) rc = rtl_phy_read(r, MII_BMSR, &pr->bmsr);
    if (rc == 0) rc = rtl_phy_read(r, MII_ANAR, &pr->anar);
    if (rc == 0) rc = rtl_phy_read(r, MII_ANLPAR, &pr->anlpar);
    if (rc == 0) rc = rtl_phy_read(r, MII_GBCR, &pr->gbcr);
    if (rc == 0) rc = rtl_phy_read(r, MII_GBSR, &pr->gbsr);
    spin_unlock_irq(&r->diag_lock, f);
    return rc;
}

static int phystatus_mbps(uint8_t st)
{
    return (st & PHYSTATUS_1000) ? 1000 : (st & PHYSTATUS_100) ? 100 :
           (st & PHYSTATUS_10) ? 10 : 0;
}

/* Both ends offered gigabit, the link came up slower: the PHY downshifted.
 * Seen on the Casper's 8168H: after the fallback the PHY has cleared the 1000
 * bits that phy_init set in its own GBCR, so what the driver asked for counts. */
static int rtl_downshifted(const rtl_t *r, const rtl_phyregs_t *pr, int mbps)
{
    int offered = r->adv_1000 || (pr->gbcr & (GBCR_1000FD | GBCR_1000HD));
    return mbps > 0 && mbps < 1000 && offered && (pr->gbsr & GBSR_LP_1000);
}

static int rtl_link_check(netdev_t *dev)
{
    rtl_t *r = (rtl_t *)dev->priv;
    if (r->dead) return 0;

    uint8_t st = rd8(r, REG_PHYSTATUS);
    int up = (st != 0xFF) && (st & PHYSTATUS_LINK);

    if (up && !r->link_state) {
        int mbps = phystatus_mbps(st);
        rtl_phyregs_t pr;
        r->link_ups++;
        kprintf(KERN_INFO "rtl8168: link %d Mb/s %s duplex\n", mbps,
                (st & PHYSTATUS_FDX) ? "full" : "half");
        if (rtl_phy_snapshot(r, &pr) == 0 && rtl_downshifted(r, &pr, mbps))
            kprintf(KERN_WARN "rtl8168: link at %d Mb/s although both ends offer 1000 Mb/s "
                    "(downshift): check the cable, 1000BASE-T needs all four pairs\n", mbps);
    }
    if (!up && r->link_state) {
        r->link_drops++;
        kprintf(KERN_WARN "rtl8168: link down\n");
    }
    /* Seen on the Casper's 8168H: rtl_start's TE/RE read back set, then the chip
     * had cleared both by the first link up, leaving every frame stuck. */
    if (up && !r->link_state)
        wr8(r, REG_CMD, CMD_TE | CMD_RE);
    r->link_state = up;
    return up;
}

/* Tally counters, DMA'd by the chip into r->tally. Returns -1 on timeout. */
static int rtl_tally_dump(rtl_t *r)
{
    uint64_t pa = r->tally.phys;
    memset(r->tally.virt, 0, 64);
    dma_wmb();
    wr32(r, REG_DTCCR_HI, (uint32_t)(pa >> 32));
    wr32(r, REG_DTCCR_LO, (uint32_t)pa | DTCCR_CMD);
    for (int i = 0; i < 1000; i++) {
        if (!(rd32(r, REG_DTCCR_LO) & DTCCR_CMD)) {
            __asm__ volatile("" ::: "memory");
            return 0;
        }
        rtl_udelay(10);
    }
    return -1;
}

static uint64_t le_get(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void rtl_diag(hw_out_t *o)
{
    rtl_t *r = &g_rtl;
    netdev_t *dev = &g_rtl_dev;

    hw_begin(o, "net", dev->name);
    hw_ks(o, "driver", "rtl8168");
    if (r->dead) {
        hw_kv(o, "available", 0);
        hw_ks(o, "reason", "device_not_responding");
        hw_end(o);
        return;
    }
    uint8_t st = rd8(r, REG_PHYSTATUS);
    int up = (st != 0xFF) && (st & PHYSTATUS_LINK);
    hw_kv(o, "available", 1);
    hw_kv(o, "link", up);
    if (up) {
        hw_kv(o, "speed", (uint64_t)phystatus_mbps(st));
        hw_ks(o, "duplex", (st & PHYSTATUS_FDX) ? "full" : "half");
    }
    hw_kv(o, "adv_1000_set", r->adv_1000);
    rtl_phyregs_t pr;
    if (rtl_phy_snapshot(r, &pr) == 0) {
        hw_kv(o, "phy", 1);
        hw_kv(o, "aneg_done", !!(pr.bmsr & BMSR_ANEG_DONE));
        hw_kv(o, "adv_1000", !!(pr.gbcr & (GBCR_1000FD | GBCR_1000HD)));
        hw_kv(o, "adv_100", !!(pr.anar & ANAR_100));
        hw_kv(o, "lp_aneg", pr.anlpar != 0);
        hw_kv(o, "lp_1000", !!(pr.gbsr & GBSR_LP_1000));
        hw_kv(o, "lp_100", !!(pr.anlpar & ANAR_100));
        hw_kv(o, "ms_fault", !!(pr.gbsr & GBSR_MS_FAULT));
        hw_kv(o, "idle_err", pr.gbsr & GBSR_IDLE_ERR);
        if (up) hw_kv(o, "downshift", rtl_downshifted(r, &pr, phystatus_mbps(st)));
    } else {
        hw_kv(o, "phy", 0);
    }
    hw_kv(o, "link_ups", r->link_ups);
    hw_kv(o, "link_drops", r->link_drops);
    hw_kv(o, "rx_packets", dev->rx_packets);
    hw_kv(o, "tx_packets", dev->tx_packets);
    hw_kv(o, "rx_missed", r->rx_missed);
    hw_kv(o, "rx_desc_err", r->rx_desc_err);
    hw_kv(o, "rx_desc_crc", r->rx_desc_crc);
    if (r->tally.virt) {
        uint64_t f = spin_lock_irq(&r->diag_lock);
        int ok = rtl_tally_dump(r) == 0;
        uint8_t t[64];
        memcpy(t, r->tally.virt, sizeof t);
        spin_unlock_irq(&r->diag_lock, f);
        hw_kv(o, "tally", ok);
        if (ok) {
            hw_kv(o, "tally_tx_ok", le_get(t + 0, 8));
            hw_kv(o, "tally_rx_ok", le_get(t + 8, 8));
            hw_kv(o, "tally_tx_err", le_get(t + 16, 8));
            hw_kv(o, "tally_rx_err", le_get(t + 24, 4));
            hw_kv(o, "tally_missed", le_get(t + 28, 2));
            hw_kv(o, "tally_align", le_get(t + 30, 2));
            hw_kv(o, "tally_tx_abort", le_get(t + 60, 2));
            hw_kv(o, "tally_tx_underrun", le_get(t + 62, 2));
        }
    } else {
        hw_kv(o, "tally", 0);
    }
    hw_end(o);
}

/* ── probe ───────────────────────────────────────────────── */

/* The minimum needed to read the chip's id: D0 and memory decode, no bus
 * mastering. This comes before the BARs are read, as leaving D3hot may reset them. */
static void rtl_pci_wake(rtl_t *r)
{
    uint8_t pm = pci_find_cap(r->bus, r->dev, r->fn, PCI_CAP_PM);
    if (pm) {
        uint16_t csr = pci_read16(r->bus, r->dev, r->fn, pm + PM_CSR);
        if (csr & PM_STATE_MASK) {
            pci_write16(r->bus, r->dev, r->fn, pm + PM_CSR, csr & ~PM_STATE_MASK);
            rtl_udelay(20000);   /* PCI PM requires 10 ms from D3hot */
        }
    }

    uint16_t cmd = pci_read16(r->bus, r->dev, r->fn, PCI_COMMAND);
    pci_write16(r->bus, r->dev, r->fn, PCI_COMMAND, cmd | PCI_CMD_MEM);
}

/* PCI settings the driver's model relies on, for a chip we have accepted. */
static void rtl_pci_tune(rtl_t *r)
{
    /* INTx off: nothing here can route it. This does not affect MSI. */
    uint16_t cmd = pci_read16(r->bus, r->dev, r->fn, PCI_COMMAND);
    pci_write16(r->bus, r->dev, r->fn, PCI_COMMAND, cmd | PCI_CMD_INTX_OFF);

    uint8_t ex = pci_find_cap(r->bus, r->dev, r->fn, PCI_CAP_PCIE);
    if (ex) {
        /* The rings live in cached memory: frames must snoop, and the OWN write-
         * back must not pass the frame data. */
        uint16_t dev = pci_read16(r->bus, r->dev, r->fn, ex + PCIE_DEVCTL);
        pci_write16(r->bus, r->dev, r->fn, ex + PCIE_DEVCTL,
                    dev & ~(DEVCTL_RELAXED | DEVCTL_NOSNOOP));
        /* FreeBSD: ASPM is implicated in TX stalls on many revisions. */
        uint16_t link = pci_read16(r->bus, r->dev, r->fn, ex + PCIE_LINKCTL);
        pci_write16(r->bus, r->dev, r->fn, ex + PCIE_LINKCTL,
                    link & ~(LINKCTL_ASPM | LINKCTL_ECPM));
    }
}

void rtl8168_init(void)
{
    rtl_t *r = &g_rtl;
    netdev_t *dev = &g_rtl_dev;

    /* 0x8161 is the same MAC family under another device id. */
    if (pci_find(VENDOR_REALTEK, 0x8168, &r->bus, &r->dev, &r->fn) != 0 &&
        pci_find(VENDOR_REALTEK, 0x8161, &r->bus, &r->dev, &r->fn) != 0)
        return;     /* no NIC: stay silent, this is the normal case in QEMU */

    rtl_pci_wake(r);

    uint32_t bar = pci_read32(r->bus, r->dev, r->fn, PCI_BAR2);
    if (bar & 1) {
        kprintf(KERN_ERR "rtl8168: bar2 is not memory (0x%x), giving up\n", bar);
        return;
    }
    uint64_t base = bar & ~0xFUL;
    if ((bar & 0x6) == 0x4)
        base |= (uint64_t)pci_read32(r->bus, r->dev, r->fn, PCI_BAR3) << 32;
    if (!base) {
        kprintf(KERN_ERR "rtl8168: bar2 is unassigned, giving up\n");
        return;
    }

    r->regs = ioremap(base, 0x1000, VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);
    if (!r->regs) {
        kprintf(KERN_WARN "rtl8168: ioremap failed\n");
        return;
    }

    /* Only one chip family is on the allow list, because the bring-up here was
     * reasoned out for that family alone. Anything else gets no reset and no bus
     * mastering, only the D0 and memory decode needed to read this register. Add
     * a family only after checking it. */
    uint32_t txcfg = rd32(r, REG_TXCFG);
    uint8_t  rev   = (uint8_t)pci_read32(r->bus, r->dev, r->fn, PCI_REV_CLASS);
    if ((txcfg & TXCFG_HWREV) != HWREV_8168H) {
        kprintf(KERN_WARN "rtl8168: unsupported chip at %02x:%02x.%u: rev 0x%02x TxConfig 0x%08x, leaving it alone\n",
                r->bus, r->dev, r->fn, rev, txcfg);
        return;
    }

    rtl_pci_tune(r);
    rtl_quiesce(r);
    if (rtl_reset(r) != 0) {
        kprintf(KERN_ERR "rtl8168: chip reset timed out, giving up\n");
        return;
    }
    if (rtl_read_mac(r, dev->mac) != 0) {
        kprintf(KERN_ERR "rtl8168: no valid mac address, giving up\n");
        return;
    }
    if (rtl_rings_init(r) != 0) {
        kprintf(KERN_ERR "rtl8168: out of memory for rings, giving up\n");
        rtl_free_all(r);
        return;
    }

    r->tx_lock = (spinlock_t)SPINLOCK_INIT;
    r->diag_lock = (spinlock_t)SPINLOCK_INIT;
    rtl_phy_init(r);

    if (rtl_start(r) != 0) {
        kprintf(KERN_ERR "rtl8168: chip did not enable tx/rx, giving up\n");
        rtl_abort(r);
        return;
    }

    memcpy(dev->name, "eth0", 5);
    dev->ip         = 0;    /* DHCP/SLAAC through the net_link watcher */
    dev->tx         = rtl_tx;
    dev->poll       = rtl_poll;
    dev->link_check = rtl_link_check;
    dev->priv       = r;
    dev->rx_lock    = (spinlock_t)SPINLOCK_INIT;
    net_register(dev);
    if (rtl_dma_alloc(&r->tally, 64) != 0)
        r->tally.virt = NULL;
    hwdiag_register(rtl_diag);

    int msi = pci_msi_enable(r->bus, r->dev, r->fn, VEC_MSI_FIRST) == 0;
    if (msi) {
        irq_register(VEC_MSI_FIRST, rtl_irq);
        wr16(r, REG_IMR, IMR_RX);
    }

    kprintf(KERN_INFO "rtl8168: rtl8168h/8111h at %02x:%02x.%u rev 0x%02x TxConfig 0x%08x, %s, no firmware patch\n",
            r->bus, r->dev, r->fn, rev, txcfg, msi ? "MSI" : "polled");
}
