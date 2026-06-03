/* Broadcom GENET v5 Ethernet — Raspberry Pi 4 (BCM2711) built-in gigabit NIC.
 *
 * Written from documentation (U-Boot drivers/net/bcmgenet.c + Linux
 * drivers/net/ethernet/broadcom/genet + BCM2711 peripherals) WITHOUT access to
 * the hardware — it cannot be exercised in QEMU (which has no GENET model), only
 * on a real Pi 4. Polling model (no IRQ) to match Ranix's netdev poll() path.
 *
 * Hardware notes / the parts most likely to need tuning on real silicon:
 *   - DMA addresses: the descriptor takes the buffer's CPU physical address
 *     (VIRT_TO_PHYS). BCM2711's GENET dma-ranges are 1:1 for our low buffers, so
 *     no bus offset is applied (matches U-Boot). If RX/TX never moves, this is
 *     the first suspect.
 *   - Cache coherency: GENET DMA is not coherent with the CPU D-cache, so TX
 *     buffers are cleaned (dc cvac) before the descriptor is posted and RX
 *     buffers invalidated (dc ivac) before they are read.
 *   - PHY (BCM54213PE @ MDIO addr 1): link/speed are read over MDIO; the speed
 *     decode from the aux-status register is the other likely tuning point.
 */
#include <kernel/net/net.h>
#include <kernel/net/eth.h>
#include <kernel/hal.h>
#include <kernel/lib/string.h>
#include <asm/mmu.h>

extern void kprintf(const char *, ...);

/* ── register map (offsets from the GENET MMIO base) ─────────────────────── */
#define GENET_PHYS              0xFD580000UL
#define GENET_SIZE              0x10000UL

#define SYS_REV_CTRL            0x0000
#define SYS_PORT_CTRL           0x0004
#define SYS_RBUF_FLUSH_CTRL     0x0008
#define SYS_TBUF_FLUSH_CTRL     0x000c
#define PORT_MODE_EXT_GPHY      3

#define EXT_RGMII_OOB_CTRL      0x008c   /* GENET_EXT_OFF 0x0080 + 0x0c */
#define RGMII_LINK              (1u << 4)
#define OOB_DISABLE             (1u << 5)
#define RGMII_MODE_EN           (1u << 6)
#define ID_MODE_DIS             (1u << 16)

#define RBUF_CTRL               0x0300
#define RBUF_TBUF_SIZE_CTRL     0x03b4
#define RBUF_ALIGN_2B           (1u << 1)

#define UMAC_CMD                0x0808
#define UMAC_MAC0               0x080c
#define UMAC_MAC1               0x0810
#define UMAC_MAX_FRAME_LEN      0x0814
#define UMAC_MIB_CTRL           0x0d80
#define MDIO_CMD                0x0e14
#define CMD_TX_EN               (1u << 0)
#define CMD_RX_EN               (1u << 1)
#define CMD_SPEED_SHIFT         2
#define UMAC_SPEED_10           0
#define UMAC_SPEED_100          1
#define UMAC_SPEED_1000         2
#define CMD_SW_RESET            (1u << 13)
#define CMD_LCL_LOOP_EN         (1u << 15)
#define MIB_RESET_RX            (1u << 0)
#define MIB_RESET_RUNT          (1u << 1)
#define MIB_RESET_TX            (1u << 2)

#define MDIO_START_BUSY         (1u << 29)
#define MDIO_READ_FAIL          (1u << 28)
#define MDIO_RD                 (2u << 26)
#define MDIO_WR                 (1u << 26)
#define MDIO_PMD_SHIFT          21
#define MDIO_REG_SHIFT          16

/* DMA: the descriptor ring lives in GENET MMIO; only packet buffers are in RAM.
 * The register-block offsets assume the fixed 256-descriptor hardware layout. */
#define RX_OFF                  0x2000
#define TX_OFF                  0x4000
#define HW_DESCS                256
#define DMA_DESC_SIZE           12
#define DEFAULT_Q               0x10
#define DMA_RING_SIZE           0x40
#define DMA_RINGS_SIZE          (DMA_RING_SIZE * (DEFAULT_Q + 1))

#define RDMA_REG_OFF            (RX_OFF + HW_DESCS * DMA_DESC_SIZE)   /* 0x2c00 */
#define TDMA_REG_OFF            (TX_OFF + HW_DESCS * DMA_DESC_SIZE)   /* 0x4c00 */
#define RDMA_RING_BASE          (RDMA_REG_OFF + DEFAULT_Q * DMA_RING_SIZE)
#define TDMA_RING_BASE          (TDMA_REG_OFF + DEFAULT_Q * DMA_RING_SIZE)
#define RDMA_CTL_BASE           (RDMA_REG_OFF + DMA_RINGS_SIZE)
#define TDMA_CTL_BASE           (TDMA_REG_OFF + DMA_RINGS_SIZE)

/* per-ring registers (relative to the ring base). NOTE: prod/cons sit at
 * SWAPPED offsets between the TX and RX rings — TDMA has CONS@0x08/PROD@0x0c,
 * RDMA has PROD@0x08/CONS@0x0c. Using one shared pair silently posts TX to the
 * wrong register and the NIC never fetches the descriptor (no packets go out). */
#define RDMA_PROD_INDEX         0x08
#define RDMA_CONS_INDEX         0x0c
#define TDMA_CONS_INDEX         0x08
#define TDMA_PROD_INDEX         0x0c
#define DMA_RING_BUF_SIZE       0x10
#define DMA_START_ADDR          0x14
#define DMA_END_ADDR            0x1c
#define DMA_MBUF_DONE_THRESH    0x24
#define DMA_XON_XOFF_THRESH     0x28   /* RDMA */
#define DMA_FLOW_PERIOD         0x28   /* TDMA */
#define DMA_WRITE_PTR           0x00
#define DMA_READ_PTR            0x2c
/* ring-control block registers (relative to *_CTL_BASE) */
#define DMA_RING_CFG            0x00
#define DMA_CTRL                0x04
#define DMA_SCB_BURST_SIZE      0x0c

#define DMA_EN                  (1u << 0)
#define DMA_RING_BUF_EN_SHIFT   0x01
#define DMA_MAX_BURST_LENGTH    0x08
#define DMA_RING_SIZE_SHIFT     16
#define DMA_BUFLENGTH_SHIFT     16
#define DMA_BUFLENGTH_MASK      0x0fff

/* descriptor length/status word */
#define DMA_OWN                 0x8000
#define DMA_EOP                 0x4000
#define DMA_SOP                 0x2000
#define DMA_TX_APPEND_CRC       0x0040
#define DMA_TX_QTAG_SHIFT       7

#define RX_DESCS                32
#define TX_DESCS                32
#define RX_BUF_LENGTH           2048
#define RX_BUF_OFFSET           2     /* RBUF_ALIGN_2B: 2 pad bytes before the frame */
#define DMA_FC_THRESH_HI        (RX_DESCS >> 4)
#define DMA_FC_THRESH_LO        5
#define DMA_FC_THRESH_VALUE     ((DMA_FC_THRESH_LO << 16) | DMA_FC_THRESH_HI)

/* ── DMA buffers (static BSS → contiguous physical, kernel direct map) ────── */
static uint8_t rx_buf[RX_DESCS][RX_BUF_LENGTH] __attribute__((aligned(64)));
static uint8_t tx_buf[RX_BUF_LENGTH]           __attribute__((aligned(64)));

static uint64_t genet_base;     /* virtual MMIO base */
static uint32_t rx_cons, rx_idx;
static uint32_t tx_prod, tx_idx;
static netdev_t g_genet;

/* ── MMIO + cache + delay helpers ────────────────────────────────────────── */
static inline uint32_t gr(uint32_t off)        { return reg_read32(genet_base + off); }
static inline void     gw(uint32_t off, uint32_t v) { reg_write32(genet_base + off, v); }

static void dcache_clean(const void *p, unsigned long n)
{
    unsigned long a = (unsigned long)p & ~63UL, end = (unsigned long)p + n;
    for (; a < end; a += 64) __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
static void dcache_inval(void *p, unsigned long n)
{
    unsigned long a = (unsigned long)p & ~63UL, end = (unsigned long)p + n;
    for (; a < end; a += 64) __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
static void genet_udelay(int us)
{
    /* coarse busy-wait; init delays only need a minimum, overshoot is harmless */
    volatile uint64_t n = (uint64_t)us * 200;
    while (n--) __asm__ volatile("nop");
}

/* ── MDIO (clause-22) ────────────────────────────────────────────────────── */
static int mdio_wait(void)
{
    for (int i = 0; i < 100000; i++) {
        if (!(gr(MDIO_CMD) & MDIO_START_BUSY)) return 0;
        genet_udelay(1);
    }
    return -1;
}
static uint16_t mdio_read(int phy, int reg)
{
    gw(MDIO_CMD, MDIO_RD | (phy << MDIO_PMD_SHIFT) | (reg << MDIO_REG_SHIFT));
    gw(MDIO_CMD, gr(MDIO_CMD) | MDIO_START_BUSY);
    if (mdio_wait() != 0) return 0xffff;
    return (uint16_t)(gr(MDIO_CMD) & 0xffff);
}
static void mdio_write(int phy, int reg, uint16_t val)
{
    gw(MDIO_CMD, MDIO_WR | (phy << MDIO_PMD_SHIFT) | (reg << MDIO_REG_SHIFT) | val);
    gw(MDIO_CMD, gr(MDIO_CMD) | MDIO_START_BUSY);
    mdio_wait();
}

#define PHY_ADDR 1   /* BCM54213PE on the Pi 4 sits at MDIO address 1 */

/* decode the negotiated speed from the BCM auxiliary status summary (reg 0x19),
 * bits [10:8] = HCD speed */
static int phy_speed(void)
{
    uint16_t aux = mdio_read(PHY_ADDR, 0x19);
    switch ((aux >> 8) & 7) {
    case 7: case 6: return UMAC_SPEED_1000;  /* 1000 FD/HD */
    case 5: case 4: return UMAC_SPEED_100;   /* 100 FD/HD */
    default:        return UMAC_SPEED_10;
    }
}

static int phy_link_up(void) { return (mdio_read(PHY_ADDR, 0x01) & (1 << 2)) ? 1 : 0; }

/* return UMAC speed code, after waiting for link; -1 = no link */
static int phy_bringup(void)
{
    /* restart autonegotiation (BMCR bit 9 | enable bit 12) */
    mdio_write(PHY_ADDR, 0x00, (1 << 12) | (1 << 9));
    int up = 0;
    for (int i = 0; i < 3000; i++) {       /* wait for link, up to ~3s */
        if (phy_link_up()) { up = 1; break; }
        genet_udelay(1000);
    }
    if (!up) return -1;
    return phy_speed();
}

static void umac_set_speed(int speed)
{
    uint32_t cmd = gr(UMAC_CMD) & ~(3u << CMD_SPEED_SHIFT);
    gw(UMAC_CMD, cmd | ((uint32_t)speed << CMD_SPEED_SHIFT) | CMD_TX_EN | CMD_RX_EN);
}

/* netdev carrier poll (Linux-style hotplug): the link watcher calls this ~1 Hz.
 * On a down→up edge it re-reads the negotiated speed and re-applies it so a
 * cable replugged into a different-speed port still works. */
static int genet_link_check(netdev_t *dev)
{
    int up = phy_link_up();
    if (up && !dev->link_up) umac_set_speed(phy_speed());
    return up;
}

/* ── MAC + DMA init ──────────────────────────────────────────────────────── */
static void umac_reset(void)
{
    uint32_t reg = gr(SYS_RBUF_FLUSH_CTRL);
    gw(SYS_RBUF_FLUSH_CTRL, reg | (1u << 1)); genet_udelay(10);
    gw(SYS_RBUF_FLUSH_CTRL, reg & ~(1u << 1)); genet_udelay(10);
    gw(SYS_RBUF_FLUSH_CTRL, 0); genet_udelay(10);

    gw(UMAC_CMD, 0);
    gw(UMAC_CMD, CMD_SW_RESET | CMD_LCL_LOOP_EN); genet_udelay(2);
    gw(UMAC_CMD, 0);

    gw(UMAC_MIB_CTRL, MIB_RESET_RX | MIB_RESET_TX | MIB_RESET_RUNT);
    gw(UMAC_MIB_CTRL, 0);

    gw(UMAC_MAX_FRAME_LEN, RX_BUF_LENGTH);
    gw(RBUF_CTRL, gr(RBUF_CTRL) | RBUF_ALIGN_2B);
    gw(RBUF_TBUF_SIZE_CTRL, 1);
}

static void set_mac(const uint8_t mac[6])
{
    gw(UMAC_MAC0, (mac[0] << 24) | (mac[1] << 16) | (mac[2] << 8) | mac[3]);
    gw(UMAC_MAC1, (mac[4] << 8) | mac[5]);
}

static void dma_init(void)
{
    /* RX descriptors: point each at its static buffer, hand ownership to HW */
    for (int i = 0; i < RX_DESCS; i++) {
        uint64_t pa = VIRT_TO_PHYS((uint64_t)rx_buf[i]);
        uint32_t d  = RX_OFF + i * DMA_DESC_SIZE;
        dcache_inval(rx_buf[i], RX_BUF_LENGTH);
        gw(d + 0x04, (uint32_t)pa);
        gw(d + 0x08, (uint32_t)(pa >> 32));
        gw(d + 0x00, (RX_BUF_LENGTH << DMA_BUFLENGTH_SHIFT) | DMA_OWN);
    }

    /* RX ring */
    gw(RDMA_CTL_BASE + DMA_SCB_BURST_SIZE, DMA_MAX_BURST_LENGTH);
    gw(RDMA_RING_BASE + DMA_START_ADDR, 0);
    gw(RDMA_RING_BASE + DMA_READ_PTR, 0);
    gw(RDMA_RING_BASE + DMA_WRITE_PTR, 0);
    gw(RDMA_RING_BASE + DMA_END_ADDR, RX_DESCS * DMA_DESC_SIZE / 4 - 1);
    rx_cons = gr(RDMA_RING_BASE + RDMA_PROD_INDEX);
    gw(RDMA_RING_BASE + RDMA_CONS_INDEX, rx_cons);
    rx_idx = rx_cons & (RX_DESCS - 1);
    gw(RDMA_RING_BASE + DMA_RING_BUF_SIZE,
       (RX_DESCS << DMA_RING_SIZE_SHIFT) | RX_BUF_LENGTH);
    gw(RDMA_RING_BASE + DMA_XON_XOFF_THRESH, DMA_FC_THRESH_VALUE);
    gw(RDMA_CTL_BASE + DMA_RING_CFG, 1u << DEFAULT_Q);

    /* TX ring */
    gw(TDMA_CTL_BASE + DMA_SCB_BURST_SIZE, DMA_MAX_BURST_LENGTH);
    gw(TDMA_RING_BASE + DMA_START_ADDR, 0);
    gw(TDMA_RING_BASE + DMA_READ_PTR, 0);
    gw(TDMA_RING_BASE + DMA_WRITE_PTR, 0);
    gw(TDMA_RING_BASE + DMA_END_ADDR, TX_DESCS * DMA_DESC_SIZE / 4 - 1);
    tx_prod = gr(TDMA_RING_BASE + TDMA_CONS_INDEX);
    gw(TDMA_RING_BASE + TDMA_PROD_INDEX, tx_prod);
    tx_idx = tx_prod & (TX_DESCS - 1);
    gw(TDMA_RING_BASE + DMA_MBUF_DONE_THRESH, 1);
    gw(TDMA_RING_BASE + DMA_FLOW_PERIOD, 0);
    gw(TDMA_RING_BASE + DMA_RING_BUF_SIZE,
       (TX_DESCS << DMA_RING_SIZE_SHIFT) | RX_BUF_LENGTH);
    gw(TDMA_CTL_BASE + DMA_RING_CFG, 1u << DEFAULT_Q);

    uint32_t dma_ctrl = (1u << (DEFAULT_Q + DMA_RING_BUF_EN_SHIFT)) | DMA_EN;
    gw(TDMA_CTL_BASE + DMA_CTRL, dma_ctrl);
    gw(RDMA_CTL_BASE + DMA_CTRL, gr(RDMA_CTL_BASE + DMA_CTRL) | dma_ctrl);
}

/* ── netdev tx / poll ────────────────────────────────────────────────────── */
static int genet_tx(netdev_t *dev, skb_t *skb)
{
    uint16_t len = skb->len;
    if (!dev->link_up)        { skb_free(skb); return -1; }  /* no carrier */
    if (len > RX_BUF_LENGTH)  { skb_free(skb); return -1; }
    memcpy(tx_buf, skb->data, len);
    dcache_clean(tx_buf, len);

    uint32_t d = TX_OFF + tx_idx * DMA_DESC_SIZE;
    uint64_t pa = VIRT_TO_PHYS((uint64_t)tx_buf);
    uint32_t ls = (len << DMA_BUFLENGTH_SHIFT) | (0x3f << DMA_TX_QTAG_SHIFT) |
                  DMA_TX_APPEND_CRC | DMA_SOP | DMA_EOP;
    gw(d + 0x04, (uint32_t)pa);
    gw(d + 0x08, (uint32_t)(pa >> 32));
    gw(d + 0x00, ls);

    tx_idx = (tx_idx + 1) & (TX_DESCS - 1);
    tx_prod = (tx_prod + 1) & 0xffff;
    gw(TDMA_RING_BASE + TDMA_PROD_INDEX, tx_prod);

    /* wait for the descriptor to drain (poll model) */
    for (int i = 0; i < 100000; i++) {
        if ((gr(TDMA_RING_BASE + TDMA_CONS_INDEX) & 0xffff) == tx_prod) break;
        genet_udelay(1);
    }
    { static int n = 0; if (n < 6) { n++;
        uint32_t c = gr(TDMA_RING_BASE + TDMA_CONS_INDEX) & 0xffff;
        kprintf("genet tx#%d len=%u prod=%u cons=%u %s\n", n, len, tx_prod, c,
                c == tx_prod ? "sent" : "TIMEOUT"); } }
    skb_free(skb);
    return (int)len;
}

static void genet_poll(netdev_t *dev)
{
    uint32_t prod = gr(RDMA_RING_BASE + RDMA_PROD_INDEX) & 0xffff;
    { static int n = 0; if (prod != (rx_cons & 0xffff) && n < 6) { n++;
        kprintf("genet rx prod=%u cons=%u\n", prod, rx_cons & 0xffff); } }
    while (prod != (rx_cons & 0xffff)) {
        uint32_t d  = RX_OFF + rx_idx * DMA_DESC_SIZE;
        uint32_t ls = gr(d + 0x00);
        uint32_t len = (ls >> DMA_BUFLENGTH_SHIFT) & DMA_BUFLENGTH_MASK;

        if (len > RX_BUF_OFFSET && len <= RX_BUF_LENGTH) {
            dcache_inval(rx_buf[rx_idx], RX_BUF_LENGTH);
            uint32_t plen = len - RX_BUF_OFFSET;
            skb_t *skb = skb_alloc();
            if (skb) {
                if (plen > SKB_DATA_MAX) plen = SKB_DATA_MAX;
                memcpy(skb->data, rx_buf[rx_idx] + RX_BUF_OFFSET, plen);
                skb->len = (uint16_t)plen;
                netdev_rx_enqueue(dev, skb);
            }
        }

        /* recycle: hand the descriptor back to HW, advance consumer */
        dcache_inval(rx_buf[rx_idx], RX_BUF_LENGTH);
        gw(d + 0x00, (RX_BUF_LENGTH << DMA_BUFLENGTH_SHIFT) | DMA_OWN);
        rx_cons = (rx_cons + 1) & 0xffff;
        gw(RDMA_RING_BASE + RDMA_CONS_INDEX, rx_cons);
        rx_idx = (rx_idx + 1) & (RX_DESCS - 1);
    }
}

/* ── probe / register ────────────────────────────────────────────────────── */
void bcmgenet_init(void)
{
    genet_base = PHYS_TO_VIRT(GENET_PHYS);

    uint32_t rev = gr(SYS_REV_CTRL);
    kprintf("bcmgenet: GENET rev 0x%x @ %p\n", rev, (void *)genet_base);

    memset(&g_genet, 0, sizeof(g_genet));
    memcpy(g_genet.name, "eth0", 5);
    /* MAC: the real Pi MAC comes from VideoCore/OTP via the firmware (DT
     * "local-mac-address"); until that is wired, use a fixed Raspberry-Pi OUI
     * (dc:a6:32) locally-administered address. TODO: read DT local-mac-address. */
    uint8_t mac[6] = { 0xdc, 0xa6, 0x32, 0x00, 0x00, 0x01 };
    memcpy(g_genet.mac, mac, 6);

    gw(SYS_PORT_CTRL, PORT_MODE_EXT_GPHY);
    umac_reset();
    set_mac(mac);

    /* RGMII out-of-band: drive link, enable RGMII, disable the internal delay
     * (the PHY provides it on the Pi 4) */
    uint32_t oob = gr(EXT_RGMII_OOB_CTRL);
    oob &= ~OOB_DISABLE;
    oob |= RGMII_LINK | RGMII_MODE_EN | ID_MODE_DIS;
    gw(EXT_RGMII_OOB_CTRL, oob);

    int speed = phy_bringup();
    if (speed < 0) {
        kprintf("bcmgenet: no link (PHY autoneg timeout) — NIC not registered\n");
        return;
    }
    gw(UMAC_CMD, (uint32_t)speed << CMD_SPEED_SHIFT);

    dma_init();
    gw(UMAC_CMD, gr(UMAC_CMD) | CMD_TX_EN | CMD_RX_EN);

    g_genet.tx         = genet_tx;
    g_genet.poll       = genet_poll;
    g_genet.link_check = genet_link_check;   /* Linux-style carrier hotplug */
    g_genet.link_up    = 1;                  /* link is up (we just brought it up) */
    g_genet.ip         = 0;   /* configured by DHCP/SLAAC */
    net_register(&g_genet);
    kprintf("bcmgenet: eth0 up, speed=%d mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
            speed, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
