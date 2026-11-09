/* SPDX-License-Identifier: EUPL-1.2 AND BSD-2-Clause */
/*
 * Broadcom GENET v5 gigabit Ethernet on the Raspberry Pi 4 (BCM2711): polled driver.
 *
 * Status: build-checked only. It has not run on hardware: the only Pi 4 is a
 * production server and must not boot Ranix, and QEMU's raspi4b has no GENET.
 *
 * Provenance (clean-room rewrite, 2026-11-09). This file replaces an earlier
 * driver that carried traces of U-Boot's GPL driver. The author of this version
 * did not read that driver's body, U-Boot, Linux, or any other GPL source. The
 * register map, reset, ring set-up, MDIO protocol, RX filter and RGMII link
 * steps come from FreeBSD sys/arm64/broadcom/genet/if_genet.c and
 * if_genetreg.h (BSD-2-Clause; FreeBSD's driver is itself derived from
 * NetBSD's bcmgenet.c, also BSD-2-Clause, by Jared McNeill). The PHY is driven
 * with plain IEEE 802.3 clause 22/28/40 registers. The board MAC address comes
 * from the VideoCore mailbox tag 0x00010003 ("Get board MAC address", Raspberry
 * Pi firmware wiki, Mailbox property interface). The register base 0xFD580000 is
 * GENET's bus address 0x7D580000 in the BCM2711 low-peripheral map, as the
 * other Pi 4 drivers in this tree use. Sources and the time each was read:
 * ranix-inceleme/ozel-CR/KAYNAKLAR.md.
 *
 * Not done, on purpose: FreeBSD's PHY driver (brgphy.c) sets the BCM54213PE's
 * RGMII clock skew; that file is BSD-4-Clause, so it was not used. This driver
 * leaves the PHY's delay settings as the firmware left them and sets only the
 * GENET side (GENET_RGMII_ID below). If received frames are corrupt on real
 * hardware, the PHY RX skew is the first thing to check.
 *
 * Copyright (c) 2026 RasifoX and the Ranix contributors (EUPL-1.2).
 *
 * Parts adapted from FreeBSD if_genet.c / if_genetreg.h, under this notice:
 *
 * Copyright (c) 2020 Michael J Karels
 * Copyright (c) 2016, 2020 Jared McNeill <jmcneill@invisible.ca>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <kernel/net/net.h>
#include <kernel/sync.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/vm.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/init.h>
#include <asm/mmu.h>
#include <drivers/mailbox/mbox.h>

#define GENET_PHYS          0xFD580000UL
#define GENET_SIZE          0x10000
#define GENET_RGMII_ID      1      /* 1: GENET adds the TX delay (rgmii-rxid wiring) */

/* ── registers (if_genetreg.h) ────────────────────────────── */

#define SYS_REV_CTRL        0x000
#define  REV_MAJOR(v)       (((v) >> 24) & 0xf)
#define  REV_MINOR(v)       (((v) >> 16) & 0xf)
#define  REV_MAJOR_V5       6
#define SYS_PORT_CTRL       0x004
#define  PORT_MODE_EXT_GPHY 3
#define SYS_RBUF_FLUSH_CTRL 0x008
#define  RBUF_FLUSH_RESET   (1u << 1)
#define EXT_RGMII_OOB_CTRL  0x08c
#define  OOB_ID_MODE_DISABLE (1u << 16)
#define  OOB_RGMII_MODE_EN  (1u << 6)
#define  OOB_OOB_DISABLE    (1u << 5)
#define  OOB_RGMII_LINK     (1u << 4)
#define INTRL2_CPU_SET_MASK   0x210
#define INTRL2_CPU_CLEAR_MASK 0x214
#define RBUF_CTRL           0x300
#define  RBUF_ALIGN_2B      (1u << 1)
#define  RBUF_64B_EN        (1u << 0)
#define RBUF_CHECK_CTRL     0x314
#define  RBUF_CHECK_EN      (1u << 0)
#define RBUF_TBUF_SIZE_CTRL 0x3b4
#define TBUF_CTRL           0x600
#define UMAC_CMD            0x808
#define  CMD_LCL_LOOP_EN    (1u << 15)
#define  CMD_SW_RESET       (1u << 13)
#define  CMD_PROMISC        (1u << 4)
#define  CMD_SPEED_M        (3u << 2)
#define  CMD_SPEED_10       (0u << 2)
#define  CMD_SPEED_100      (1u << 2)
#define  CMD_SPEED_1000     (2u << 2)
#define  CMD_RXEN           (1u << 1)
#define  CMD_TXEN           (1u << 0)
#define UMAC_MAC0           0x80c
#define UMAC_MAC1           0x810
#define UMAC_MAX_FRAME_LEN  0x814
#define UMAC_MIB_CTRL       0xd80
#define  MIB_RESET_ALL      0x7
#define MDIO_CMD            0xe14
#define  MDIO_START_BUSY    (1u << 29)
#define  MDIO_READ_FAILED   (1u << 28)
#define  MDIO_READ          (1u << 27)
#define  MDIO_WRITE         (1u << 26)
#define UMAC_MDF_CTRL       0xe50
#define UMAC_MDF_ADDR0(n)   (0xe54 + (n) * 8)
#define UMAC_MDF_ADDR1(n)   (0xe58 + (n) * 8)
#define MDF_MAX             17

#define DESC_COUNT          256
#define DESC_SIZE           12
#define QID                 16                  /* the default queue */
#define RX_BASE             0x2000
#define TX_BASE             0x4000
#define RING(base, q)       ((base) + 0xc00 + 0x40 * (q))

#define RX_WRITE_PTR_LO     (RING(RX_BASE, QID) + 0x00)
#define RX_WRITE_PTR_HI     (RING(RX_BASE, QID) + 0x04)
#define RX_PROD_INDEX       (RING(RX_BASE, QID) + 0x08)
#define RX_CONS_INDEX       (RING(RX_BASE, QID) + 0x0c)
#define RX_RING_BUF_SIZE    (RING(RX_BASE, QID) + 0x10)
#define RX_START_ADDR_LO    (RING(RX_BASE, QID) + 0x14)
#define RX_START_ADDR_HI    (RING(RX_BASE, QID) + 0x18)
#define RX_END_ADDR_LO      (RING(RX_BASE, QID) + 0x1c)
#define RX_END_ADDR_HI      (RING(RX_BASE, QID) + 0x20)
#define RX_XON_XOFF_THRES   (RING(RX_BASE, QID) + 0x28)
#define RX_READ_PTR_LO      (RING(RX_BASE, QID) + 0x2c)
#define RX_READ_PTR_HI      (RING(RX_BASE, QID) + 0x30)

#define TX_READ_PTR_LO      (RING(TX_BASE, QID) + 0x00)
#define TX_READ_PTR_HI      (RING(TX_BASE, QID) + 0x04)
#define TX_CONS_INDEX       (RING(TX_BASE, QID) + 0x08)
#define TX_PROD_INDEX       (RING(TX_BASE, QID) + 0x0c)
#define TX_RING_BUF_SIZE    (RING(TX_BASE, QID) + 0x10)
#define TX_START_ADDR_LO    (RING(TX_BASE, QID) + 0x14)
#define TX_START_ADDR_HI    (RING(TX_BASE, QID) + 0x18)
#define TX_END_ADDR_LO      (RING(TX_BASE, QID) + 0x1c)
#define TX_END_ADDR_HI      (RING(TX_BASE, QID) + 0x20)
#define TX_MBUF_DONE_THRES  (RING(TX_BASE, QID) + 0x24)
#define TX_FLOW_PERIOD      (RING(TX_BASE, QID) + 0x28)
#define TX_WRITE_PTR_LO     (RING(TX_BASE, QID) + 0x2c)
#define TX_WRITE_PTR_HI     (RING(TX_BASE, QID) + 0x30)

#define RX_DESC_STATUS(i)   (RX_BASE + DESC_SIZE * (i) + 0x0)
#define RX_DESC_ADDR_LO(i)  (RX_BASE + DESC_SIZE * (i) + 0x4)
#define RX_DESC_ADDR_HI(i)  (RX_BASE + DESC_SIZE * (i) + 0x8)
#define TX_DESC_STATUS(i)   (TX_BASE + DESC_SIZE * (i) + 0x0)
#define TX_DESC_ADDR_LO(i)  (TX_BASE + DESC_SIZE * (i) + 0x4)
#define TX_DESC_ADDR_HI(i)  (TX_BASE + DESC_SIZE * (i) + 0x8)
#define  DESC_BUFLEN(s)     (((s) >> 16) & 0xfff)
#define  DESC_EOP           (1u << 14)
#define  DESC_SOP           (1u << 13)
#define  DESC_RX_ERROR      (1u << 2)
#define  DESC_QTAG_MASK     0x1f80u
#define  DESC_CRC           (1u << 6)

#define RX_DMA_RING_CFG     (RX_BASE + 0x1040)
#define RX_DMA_CTRL         (RX_BASE + 0x1044)
#define RX_SCB_BURST_SIZE   (RX_BASE + 0x104c)
#define TX_DMA_RING_CFG     (TX_BASE + 0x1040)
#define TX_DMA_CTRL         (TX_BASE + 0x1044)
#define TX_SCB_BURST_SIZE   (TX_BASE + 0x104c)
#define  DMA_CTRL_EN        (1u << 0)
#define  DMA_CTRL_RBUF_EN   (1u << (QID + 1))

#define BUF_SIZE            2048
#define BUF_ORDER           7      /* 4 KiB << 7 = DESC_COUNT * BUF_SIZE */
#define ETHER_ALIGN         2      /* RBUF_ALIGN_2B puts two bytes before each frame */
#define ETH_MIN             60

/* IEEE 802.3 clause 22 registers */
#define MII_BMCR    0x00
#define  BMCR_ANEN      (1u << 12)
#define  BMCR_ANRESTART (1u << 9)
#define MII_BMSR    0x01
#define  BMSR_LINK      (1u << 2)
#define MII_PHYID1  0x02
#define MII_PHYID2  0x03
#define MII_ANAR    0x04
#define MII_ANLPAR  0x05
#define  AN_100FD       (1u << 8)
#define  AN_100HD       (1u << 7)
#define  AN_10FD        (1u << 6)
#define  AN_10HD        (1u << 5)
#define  AN_SEL_8023    0x0001
#define MII_GTCR    0x09            /* 1000BASE-T control (clause 40) */
#define  GTCR_ADV_1000FD (1u << 9)
#define  GTCR_ADV_1000HD (1u << 8)
#define MII_GTSR    0x0a            /* 1000BASE-T status */
#define  GTSR_LP_1000FD  (1u << 11)
#define  GTSR_LP_1000HD  (1u << 10)

/* ── state ────────────────────────────────────────────────── */

static struct {
    volatile uint8_t *regs;
    int       phy;
    uint8_t  *rx_virt, *tx_virt;
    uint64_t  rx_phys, tx_phys;
    uint32_t  rx_cons;              /* 16-bit hardware index we have consumed up to */
    uint32_t  tx_prod, tx_cons;
    spinlock_t tx_lock;
    volatile uint32_t poll_busy;    /* dhcp.c calls poll outside net_poll's guard */
    int       link;
    uint32_t  speed;
} g;

static netdev_t g_dev;

static inline uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(g.regs + off); }
static inline void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(g.regs + off) = v; }

static void udelay(uint32_t us)
{
    uint64_t freq, start, now;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(start));
    uint64_t ticks = freq * us / 1000000 + 1;
    do { __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(now)); } while (now - start < ticks);
}

/* GENET's DMA does not snoop the CPU caches on the BCM2711. */
static void dcache_clean(const void *p, size_t n)
{
    uintptr_t a = (uintptr_t)p & ~63UL, end = (uintptr_t)p + n;
    for (; a < end; a += 64) __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
static void dcache_inval(const void *p, size_t n)
{
    uintptr_t a = (uintptr_t)p & ~63UL, end = (uintptr_t)p + n;
    for (; a < end; a += 64) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ── MDIO (gen_miibus_readreg / writereg) ─────────────────── */

static int mdio_read(int phy, int reg)
{
    wr(MDIO_CMD, MDIO_READ | (uint32_t)phy << 21 | (uint32_t)reg << 16);
    wr(MDIO_CMD, rd(MDIO_CMD) | MDIO_START_BUSY);
    for (int i = 0; i < 1000; i++) {
        uint32_t v = rd(MDIO_CMD);
        if (!(v & MDIO_START_BUSY))
            return (v & MDIO_READ_FAILED) ? -1 : (int)(v & 0xffff);
        udelay(10);
    }
    return -1;
}

static void mdio_write(int phy, int reg, uint16_t val)
{
    wr(MDIO_CMD, MDIO_WRITE | (uint32_t)phy << 21 | (uint32_t)reg << 16 | val);
    wr(MDIO_CMD, rd(MDIO_CMD) | MDIO_START_BUSY);
    for (int i = 0; i < 1000 && (rd(MDIO_CMD) & MDIO_START_BUSY); i++)
        udelay(10);
}

static int phy_find(void)
{
    for (int k = 0; k < 32; k++) {
        int a = (k + 1) & 31;                   /* address 1 first, the usual spot */
        int id1 = mdio_read(a, MII_PHYID1), id2 = mdio_read(a, MII_PHYID2);
        if (id1 > 0 && id1 != 0xffff && id2 >= 0) {
            kprintf(KERN_INFO "bcmgenet: phy at %d, id %04x:%04x\n", a, id1, id2);
            return a;
        }
    }
    return -1;
}

/* Advertise everything and restart autonegotiation. No PHY reset: a reset could
 * drop the RGMII delay settings the firmware made. */
static void phy_start(void)
{
    mdio_write(g.phy, MII_ANAR, AN_100FD | AN_100HD | AN_10FD | AN_10HD | AN_SEL_8023);
    mdio_write(g.phy, MII_GTCR, GTCR_ADV_1000FD | GTCR_ADV_1000HD);
    int bmcr = mdio_read(g.phy, MII_BMCR);
    if (bmcr < 0) bmcr = 0;
    mdio_write(g.phy, MII_BMCR, (uint16_t)(bmcr | BMCR_ANEN | BMCR_ANRESTART));
}

static uint32_t phy_speed(void)
{
    int gtcr = mdio_read(g.phy, MII_GTCR), gtsr = mdio_read(g.phy, MII_GTSR);
    if (gtcr > 0 && gtsr > 0 &&
        (((gtcr & GTCR_ADV_1000FD) && (gtsr & GTSR_LP_1000FD)) ||
         ((gtcr & GTCR_ADV_1000HD) && (gtsr & GTSR_LP_1000HD))))
        return CMD_SPEED_1000;
    int anar = mdio_read(g.phy, MII_ANAR), lpar = mdio_read(g.phy, MII_ANLPAR);
    int common = (anar > 0 && lpar > 0) ? anar & lpar : 0;
    if (common & (AN_100FD | AN_100HD)) return CMD_SPEED_100;
    return CMD_SPEED_10;
}

/* ── MAC address ──────────────────────────────────────────── */

static volatile u32 __attribute__((aligned(16))) mbox_msg[8];

static int mac_from_firmware(uint8_t mac[6])
{
    mbox_msg[0] = sizeof mbox_msg;
    mbox_msg[1] = 0;
    mbox_msg[2] = 0x00010003;           /* get board MAC address */
    mbox_msg[3] = 8;                    /* value buffer */
    mbox_msg[4] = 0;
    mbox_msg[5] = 0;
    mbox_msg[6] = 0;
    mbox_msg[7] = 0;                    /* end tag */
    if (mbox_call(MBOX_CH_PROP, mbox_msg) != 0) return -1;
    uint32_t lo = mbox_msg[5], hi = mbox_msg[6];
    for (int i = 0; i < 4; i++) mac[i] = (uint8_t)(lo >> (8 * i));
    mac[4] = (uint8_t)hi;
    mac[5] = (uint8_t)(hi >> 8);
    return 0;
}

static int mac_ok(const uint8_t *m)
{
    return !(m[0] & 1) && (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]);
}

static void get_mac(uint8_t mac[6])
{
    if (mac_from_firmware(mac) == 0 && mac_ok(mac)) return;
    /* gen_get_eaddr fallback: whatever is in the MAC registers, if not in reset */
    if (!(rd(SYS_RBUF_FLUSH_CTRL) & RBUF_FLUSH_RESET)) {
        uint32_t m0 = rd(UMAC_MAC0), m1 = rd(UMAC_MAC1);
        mac[0] = (uint8_t)(m0 >> 24); mac[1] = (uint8_t)(m0 >> 16);
        mac[2] = (uint8_t)(m0 >> 8);  mac[3] = (uint8_t)m0;
        mac[4] = (uint8_t)(m1 >> 8);  mac[5] = (uint8_t)m1;
        if (mac_ok(mac)) return;
    }
    static const uint8_t laa[6] = { 0x02, 0x52, 0x41, 0x4e, 0x49, 0x58 };
    memcpy(mac, laa, 6);
    kprintf(KERN_WARN "bcmgenet: no board MAC address, using a fixed local one\n");
}

/* ── bring-up (gen_reset, gen_init_locked and helpers) ─────── */

static void core_reset(void)
{
    uint32_t v = rd(SYS_RBUF_FLUSH_CTRL);
    wr(SYS_RBUF_FLUSH_CTRL, v | RBUF_FLUSH_RESET);
    udelay(10);
    wr(SYS_RBUF_FLUSH_CTRL, v & ~RBUF_FLUSH_RESET);
    udelay(10);
    wr(SYS_RBUF_FLUSH_CTRL, 0);
    udelay(10);
    wr(UMAC_CMD, 0);
    wr(UMAC_CMD, CMD_LCL_LOOP_EN | CMD_SW_RESET);
    udelay(10);
    wr(UMAC_CMD, 0);
    wr(UMAC_MIB_CTRL, MIB_RESET_ALL);
    wr(UMAC_MIB_CTRL, 0);
}

static void dma_disable(void)
{
    wr(TX_DMA_CTRL, rd(TX_DMA_CTRL) & ~(DMA_CTRL_EN | DMA_CTRL_RBUF_EN));
    wr(RX_DMA_CTRL, rd(RX_DMA_CTRL) & ~(DMA_CTRL_EN | DMA_CTRL_RBUF_EN));
}

static void set_mdf(int n, const uint8_t *ea)
{
    wr(UMAC_MDF_ADDR0(n), (uint32_t)ea[0] << 8 | ea[1]);
    wr(UMAC_MDF_ADDR1(n), (uint32_t)ea[2] << 24 | (uint32_t)ea[3] << 16 | (uint32_t)ea[4] << 8 | ea[5]);
}

/* Exact-match filter: broadcast, us, and the multicast groups the IPv4/IPv6
 * stack needs (all-hosts, all-nodes, our solicited-node group). */
static void rx_filter(const uint8_t *mac)
{
    const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    const uint8_t v4_all[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };
    const uint8_t v6_all[6] = { 0x33, 0x33, 0x00, 0x00, 0x00, 0x01 };
    const uint8_t v6_sol[6] = { 0x33, 0x33, 0xff, mac[3], mac[4], mac[5] };
    int n = 0;
    set_mdf(n++, bcast);
    set_mdf(n++, mac);
    set_mdf(n++, v4_all);
    set_mdf(n++, v6_all);
    set_mdf(n++, v6_sol);
    wr(UMAC_CMD, rd(UMAC_CMD) & ~CMD_PROMISC);
    wr(UMAC_MDF_CTRL, ((1u << MDF_MAX) - 1) & ~((1u << (MDF_MAX - n)) - 1));
}

static void tx_ring_init(void)
{
    g.tx_prod = g.tx_cons = 0;
    wr(TX_SCB_BURST_SIZE, 0x08);
    wr(TX_READ_PTR_LO, 0);
    wr(TX_READ_PTR_HI, 0);
    wr(TX_CONS_INDEX, 0);
    wr(TX_PROD_INDEX, 0);
    wr(TX_RING_BUF_SIZE, (uint32_t)DESC_COUNT << 16 | BUF_SIZE);
    wr(TX_START_ADDR_LO, 0);
    wr(TX_START_ADDR_HI, 0);
    wr(TX_END_ADDR_LO, DESC_COUNT * DESC_SIZE / 4 - 1);
    wr(TX_END_ADDR_HI, 0);
    wr(TX_MBUF_DONE_THRES, 1);
    wr(TX_FLOW_PERIOD, 0);
    wr(TX_WRITE_PTR_LO, 0);
    wr(TX_WRITE_PTR_HI, 0);
    wr(TX_DMA_RING_CFG, 1u << QID);
    wr(TX_DMA_CTRL, rd(TX_DMA_CTRL) | DMA_CTRL_EN | DMA_CTRL_RBUF_EN);
}

static void rx_ring_init(void)
{
    g.rx_cons = 0;
    wr(RX_SCB_BURST_SIZE, 0x08);
    wr(RX_WRITE_PTR_LO, 0);
    wr(RX_WRITE_PTR_HI, 0);
    wr(RX_PROD_INDEX, 0);
    wr(RX_CONS_INDEX, 0);
    wr(RX_RING_BUF_SIZE, (uint32_t)DESC_COUNT << 16 | BUF_SIZE);
    wr(RX_START_ADDR_LO, 0);
    wr(RX_START_ADDR_HI, 0);
    wr(RX_END_ADDR_LO, DESC_COUNT * DESC_SIZE / 4 - 1);
    wr(RX_END_ADDR_HI, 0);
    wr(RX_XON_XOFF_THRES, 5u << 16 | (DESC_COUNT >> 4));
    wr(RX_READ_PTR_LO, 0);
    wr(RX_READ_PTR_HI, 0);
    wr(RX_DMA_RING_CFG, 1u << QID);
    dcache_inval(g.rx_virt, (size_t)DESC_COUNT * BUF_SIZE);
    for (uint32_t i = 0; i < DESC_COUNT; i++) {        /* each slot keeps its buffer */
        uint64_t pa = g.rx_phys + (uint64_t)i * BUF_SIZE;
        wr(RX_DESC_ADDR_LO(i), (uint32_t)pa);
        wr(RX_DESC_ADDR_HI(i), (uint32_t)(pa >> 32));
    }
    wr(RX_DMA_CTRL, rd(RX_DMA_CTRL) | DMA_CTRL_EN | DMA_CTRL_RBUF_EN);
}

/* ── data path ────────────────────────────────────────────── */

static void tx_reclaim(void)
{
    g.tx_cons = rd(TX_CONS_INDEX) & 0xffff;
}

static int genet_tx(netdev_t *dev, skb_t *skb)
{
    (void)dev;
    if (!g.link || skb->len > BUF_SIZE) { skb_free(skb); return -1; }
    uint64_t f = spin_lock_irq(&g.tx_lock);
    tx_reclaim();
    if (((g.tx_prod - g.tx_cons) & 0xffff) >= DESC_COUNT) {
        spin_unlock_irq(&g.tx_lock, f);
        skb_free(skb);
        return -1;
    }
    uint32_t i = g.tx_prod & (DESC_COUNT - 1);
    uint8_t *buf = g.tx_virt + (uint64_t)i * BUF_SIZE;
    uint32_t len = skb->len;
    memcpy(buf, skb->data, len);
    if (len < ETH_MIN) { memset(buf + len, 0, ETH_MIN - len); len = ETH_MIN; }
    dcache_clean(buf, len);
    uint64_t pa = g.tx_phys + (uint64_t)i * BUF_SIZE;
    wr(TX_DESC_ADDR_LO(i), (uint32_t)pa);
    wr(TX_DESC_ADDR_HI(i), (uint32_t)(pa >> 32));
    wr(TX_DESC_STATUS(i), len << 16 | DESC_SOP | DESC_EOP | DESC_CRC | DESC_QTAG_MASK);
    g.tx_prod = (g.tx_prod + 1) & 0xffff;
    wr(TX_PROD_INDEX, g.tx_prod);
    spin_unlock_irq(&g.tx_lock, f);
    skb_free(skb);
    return 0;
}

static void genet_poll(netdev_t *dev)
{
    if (atomic_cmpxchg32(&g.poll_busy, 0, 1) != 0) return;
    uint32_t prod = rd(RX_PROD_INDEX) & 0xffff;
    while (g.rx_cons != prod) {
        uint32_t i = g.rx_cons & (DESC_COUNT - 1);
        uint32_t st = rd(RX_DESC_STATUS(i));
        uint32_t len = DESC_BUFLEN(st);
        uint8_t *buf = g.rx_virt + (uint64_t)i * BUF_SIZE;
        if ((st & (DESC_SOP | DESC_EOP | DESC_RX_ERROR)) != (DESC_SOP | DESC_EOP) ||
            len <= ETHER_ALIGN + 14 || len - ETHER_ALIGN > SKB_DATA_MAX) {
            netdev_rx_drop(dev);
        } else {
            dcache_inval(buf, len);
            skb_t *s = skb_alloc();
            if (!s) {
                netdev_rx_drop(dev);
            } else {
                memcpy(s->data, buf + ETHER_ALIGN, len - ETHER_ALIGN);
                s->len = (uint16_t)(len - ETHER_ALIGN);
                netdev_rx_enqueue(dev, s);
            }
        }
        g.rx_cons = (g.rx_cons + 1) & 0xffff;
        wr(RX_CONS_INDEX, g.rx_cons);
    }
    __atomic_store_n(&g.poll_busy, 0, __ATOMIC_RELEASE);
}

/* gen_update_link_locked: on link up, program the RGMII block and MAC speed. */
static int genet_link_check(netdev_t *dev)
{
    (void)dev;
    (void)mdio_read(g.phy, MII_BMSR);               /* link bit latches low */
    int bmsr = mdio_read(g.phy, MII_BMSR);
    int up = bmsr > 0 && (bmsr & BMSR_LINK);
    if (up && !g.link) {
        g.speed = phy_speed();
        uint32_t v = rd(EXT_RGMII_OOB_CTRL);
        v &= ~OOB_OOB_DISABLE;
        v |= OOB_RGMII_LINK | OOB_RGMII_MODE_EN;
        if (GENET_RGMII_ID) v &= ~OOB_ID_MODE_DISABLE;
        else                v |= OOB_ID_MODE_DISABLE;
        wr(EXT_RGMII_OOB_CTRL, v);
        wr(UMAC_CMD, (rd(UMAC_CMD) & ~CMD_SPEED_M) | g.speed);
        kprintf(KERN_INFO "bcmgenet: link up, %s Mb/s\n",
                g.speed == CMD_SPEED_1000 ? "1000" : g.speed == CMD_SPEED_100 ? "100" : "10");
    } else if (!up && g.link) {
        kprintf(KERN_INFO "bcmgenet: link down\n");
    }
    g.link = up;
    return up;
}

/* ── init ─────────────────────────────────────────────────── */

void bcmgenet_init(void)
{
    g.regs = ioremap(GENET_PHYS, GENET_SIZE, VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);
    if (!g.regs) {
        kprintf(KERN_WARN "bcmgenet: ioremap failed\n");
        return;
    }
    uint32_t rev = rd(SYS_REV_CTRL);
    if (REV_MAJOR(rev) != REV_MAJOR_V5) {
        kprintf(KERN_WARN "bcmgenet: no GENET v5 here (rev 0x%x), leaving it alone\n", rev);
        return;
    }

    netdev_t *dev = &g_dev;
    get_mac(dev->mac);

    core_reset();
    dma_disable();
    wr(INTRL2_CPU_SET_MASK, 0xffffffffu);           /* polled: all interrupts masked */
    wr(INTRL2_CPU_CLEAR_MASK, 0);

    void *rx = buddy_alloc(BUF_ORDER), *tx = buddy_alloc(BUF_ORDER);   /* 256 x 2 KiB each */
    if (!rx || !tx) {
        kprintf(KERN_ERR "bcmgenet: out of memory for buffers\n");
        if (rx) buddy_free((uint64_t)rx, BUF_ORDER);
        if (tx) buddy_free((uint64_t)tx, BUF_ORDER);
        return;
    }
    g.rx_phys = (uint64_t)rx;
    g.tx_phys = (uint64_t)tx;
    g.rx_virt = (uint8_t *)PHYS_TO_VIRT(g.rx_phys);
    g.tx_virt = (uint8_t *)PHYS_TO_VIRT(g.tx_phys);
    g.tx_lock = (spinlock_t)SPINLOCK_INIT;

    g.phy = phy_find();
    if (g.phy < 0) {
        kprintf(KERN_ERR "bcmgenet: no PHY answered on MDIO, giving up\n");
        buddy_free(g.rx_phys, BUF_ORDER);
        buddy_free(g.tx_phys, BUF_ORDER);
        return;
    }

    wr(SYS_PORT_CTRL, PORT_MODE_EXT_GPHY);
    wr(UMAC_MAC0, (uint32_t)dev->mac[0] << 24 | (uint32_t)dev->mac[1] << 16 |
                  (uint32_t)dev->mac[2] << 8 | dev->mac[3]);
    wr(UMAC_MAC1, (uint32_t)dev->mac[4] << 8 | dev->mac[5]);
    rx_filter(dev->mac);
    tx_ring_init();
    rx_ring_init();

    wr(UMAC_MAX_FRAME_LEN, 1536);
    wr(RBUF_CTRL, (rd(RBUF_CTRL) | RBUF_ALIGN_2B) & ~RBUF_64B_EN);   /* no status block */
    wr(RBUF_CHECK_CTRL, rd(RBUF_CHECK_CTRL) & ~RBUF_CHECK_EN);       /* no checksum offload */
    wr(TBUF_CTRL, rd(TBUF_CTRL) & ~RBUF_64B_EN);
    wr(RBUF_TBUF_SIZE_CTRL, 1);
    wr(UMAC_CMD, rd(UMAC_CMD) | CMD_TXEN | CMD_RXEN);

    phy_start();

    memcpy(dev->name, "eth0", 5);
    dev->ip         = 0;            /* DHCP/SLAAC through the net_link watcher */
    dev->tx         = genet_tx;
    dev->poll       = genet_poll;
    dev->link_check = genet_link_check;
    dev->priv       = &g;
    dev->rx_lock    = (spinlock_t)SPINLOCK_INIT;
    net_register(dev);

    kprintf(KERN_INFO "bcmgenet: GENET v5.%u at 0x%lx, mac %02x:%02x:%02x:%02x:%02x:%02x, polled\n",
            REV_MINOR(rev), GENET_PHYS, dev->mac[0], dev->mac[1], dev->mac[2],
            dev->mac[3], dev->mac[4], dev->mac[5]);
}
