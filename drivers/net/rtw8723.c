/* SPDX-License-Identifier: EUPL-1.2 AND ISC AND BSD-2-Clause */
/*
 * Realtek RTL8723BE (PCI 10ec:b723) Wi-Fi: the kernel half.
 *
 * Ranix splits this card in two. The kernel keeps what needs privilege or DMA:
 * power-on, the 8051 firmware upload, the TX/RX DMA rings, and a register
 * window. Radio tuning, scanning, 802.11 and WPA2 run in user space
 * (fjord-wifi), so a mistake there costs a restart of a program, not of the
 * machine.
 *
 *   /dev/rtw0   ioctl RTW_LOAD   {u64 ptr, u64 len}: power on, firmware, queues
 *               ioctl RTW_RINGS  u32[3] out: physical RX, MGQ, BEQ ring bases
 *               ioctl RTW_PEEK / RTW_POKE {u32 addr, width, val}
 *               ioctl RTW_STOP   power off
 *               ioctl RTW_LINK   u8[6] mac: carrier on for wlan0; 0: carrier off
 *               ioctl RTW_RXDBG  u32[8] out: ring state, for "fjord-wifi rxdbg";
 *                                [4] MGQ, [5] BEQ: frames pushed << 16 | handed back,
 *                                [6] MGQ_DESA read back (the chip's current descriptor)
 *               read   before RTW_RINGS: efuse summary text ("mac_hi=... mac_lo=...")
 *                      after:  one received frame, as the RX descriptor (32 bytes)
 *                              followed by the DMA buffer (driver info, shift, frame)
 *               write  [queue: 0 = management, 1 = best effort][40-byte TX
 *                      descriptor words 0..9][802.11 frame]; the kernel adds the
 *                      buffer and next-descriptor addresses in words 10..13
 *               poll   readable when a received frame is waiting
 *   /dev/wlan0  Ethernet frames between the network stack and fjord-wifi:
 *               read = frames the stack wants sent, write = frames received.
 *
 * Provenance (clean-room rewrite, 2026-11-09). This file replaces an earlier
 * driver that was found to follow Linux rtlwifi (GPL-2.0). The author of this
 * version did not read that driver's body, Linux, or any other GPL source.
 * Register names, bit values and sequences come only from FreeBSD rtwn(4):
 *   sys/dev/rtwn/rtl8723b/{r23b_init.c,r23b_fw.c,r23b_reg.h,r23b.h,r23b_rom.c,
 *     r23b_rom_image.h}, rtl8723b/usb/r23bu_init.c       (BSD-2-Clause)
 *   sys/dev/rtwn/rtl8192e/{r92e_init.c,r92e_fw.c}         (BSD-2-Clause)
 *   sys/dev/rtwn/{if_rtwn.c,if_rtwn_fw.c,if_rtwn_efuse.c,if_rtwnreg.h},
 *   sys/dev/rtwn/pci/{rtwn_pci_attach.c,rtwn_pci_rx.c,rtwn_pci_tx.c},
 *   sys/dev/rtwn/rtl8188e/pci/{r88ee_init.c,r88ee_attach.c}, rtl8188e/r88e_rom_image.h,
 *   sys/dev/rtwn/rtl8192c/{r92c_reg.h,r92c_fw.c}, rtl8192c/pci/{r92ce_reg.h,
 *     r92ce_tx_desc.h,r92ce_tx.c}                         (ISC)
 * FreeBSD drives the 8723B only over USB; the PCIe pieces (ring layout, queue
 * kick, PCIe power-on extras, MAC address at efuse 0xd0) are taken from its
 * RTL8188EE and RTL8192CE code, the closest PCIe parts of the same family.
 * The PCI id comes from pci.ids (BSD-3-Clause option). Every source and the time
 * it was read is listed in ranix-inceleme/ozel-CR/KAYNAKLAR.md.
 *
 * Firmware: rtlwifi/rtl8723befw_36.bin from linux-firmware, under Realtek's own
 * binary redistribution licence (LICENCE.rtlwifi_firmware.txt), not the GPL. It
 * is read by fjord-wifi from /lib/firmware and handed to RTW_LOAD.
 *
 * Copyright (c) 2026 RasifoX and the Ranix contributors (EUPL-1.2).
 *
 * Parts adapted from FreeBSD rtwn(4), under these notices:
 *
 * Copyright (c) 2010 Damien Bergamini <damien.bergamini@free.fr>
 * Copyright (c) 2014 Kevin Lo <kevlo@FreeBSD.org>
 * Copyright (c) 2015 Stefan Sperling <stsp@openbsd.org>
 * Copyright (c) 2015-2016 Andriy Voskoboinyk <avos@FreeBSD.org>
 * Copyright (c) 2017-2018 Farhan Khan <khanzf@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Copyright (c) 2017 Kevin Lo <kevlo@FreeBSD.org>
 * Copyright (c) 2026 Ahmad Khalifa <vexeduxr@FreeBSD.org>
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <kernel/net/net.h>
#include <kernel/pci.h>
#include <kernel/sync.h>
#include <kernel/tick.h>
#include <kernel/hal.h>
#include <kernel/errno.h>
#include <kernel/vfs.h>
#include <kernel/fs/devfs.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/vm.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/init.h>
#include <asm/mmu.h>
#include <asm/io.h>

/* ── user interface ───────────────────────────────────────── */

#define RTW_LOAD   0x7701
#define RTW_PEEK   0x7702
#define RTW_POKE   0x7703
#define RTW_RINGS  0x7704
#define RTW_STOP   0x7705
#define RTW_LINK   0x7706
#define RTW_RXDBG  0x7707

struct rtw_fw_arg { uint64_t ptr, len; };
struct rtw_reg    { uint32_t addr, width, val; };

/* ── registers (names as in FreeBSD rtwn) ─────────────────── */

#define R_SYS_ISO_CTRL     0x000
#define R_SYS_FUNC_EN      0x002
#define R_APS_FSMCO        0x004
#define R_SYS_CLKR         0x008
#define R_AFE_MISC         0x010
#define R_RSV_CTRL         0x01c
#define R_RF_CTRL          0x01f
#define R_LDOA15_CTRL      0x020
#define R_EFUSE_CTRL       0x030
#define R_EFUSE_TEST       0x034
#define R_PWR_DATA         0x038
#define R_LEDCFG0          0x04c
#define R_BB_PAD_CTRL      0x064
#define R_MCUFWDL          0x080
#define R_HIMR             0x0b0       /* 88E-family interrupt mask */
#define R_HIMRE            0x0b8
#define R_EFUSE_ACCESS     0x0cf
#define R_CR               0x100
#define R_PBP              0x104
#define R_TRXDMA_CTRL      0x10c
#define R_TRXFF_BNDY       0x114
#define R_MCUTST_1         0x1c0
#define R_HMETFR           0x1cc
#define R_RQPN             0x200
#define R_TDECTRL          0x208
#define R_RQPN_NPQ         0x214
#define R_AUTO_LLT         0x224
#define R_PCIE_CTRL_REG    0x300       /* low byte: TX queue kick; +1: DMA stop bits */
#define R_HISR             0x0b4       /* 88E-family interrupt status (read only here) */
#define R_INT_MIG          0x304
#define R_MGQ_DESA         0x318
#define R_BEQ_DESA         0x330
#define R_RX_DESA          0x340
#define R_TXPKTBUF_BCNQ_BDNY     0x424
#define R_TXPKTBUF_MGQ_BDNY      0x425
#define R_TXPKTBUF_WMAC_LBK_BF_HD 0x45d
#define R_TXPAUSE          0x522
#define R_RCR              0x608
#define R_RX_DRVINFO_SZ    0x60f
#define R_GNT_BT           0x764
#define R_ANT_SEL          0x948

#define SYS_ISO_IP2MAC     0x0020
#define SYS_FUNC_BBRSTB    0x0001
#define SYS_FUNC_BB_GLB_RST 0x0002
#define SYS_FUNC_CPUEN     0x0400
#define SYS_FUNC_ELDR      0x1000
#define SYS_CLKR_ANA8M     0x0002
#define SYS_CLKR_LOADER_EN 0x0020

#define APS_APFM_ONMAC     0x00000100
#define APS_APFM_OFF       0x00000200
#define APS_APFM_RSM       0x00000400
#define APS_AFSM_HSUS      0x00000800
#define APS_AFSM_PCIE      0x00001000
#define APS_APDM_HPDN      0x00008000
#define APS_RDY_MACON      0x00010000
#define APS_SUS_HOST       0x00020000

#define LDOA15_EN          0x01
#define EFUSE_CTRL_VALID   0x80000000u
#define EFUSE_TEST_SEL_M   0x00000300u  /* 0 = Wi-Fi bank */
#define EFUSE_TEST_PGMEN   0x0800
#define EFUSE_ACCESS_ON    0x69
#define EFUSE_ACCESS_OFF   0x00

#define FWDL_EN            0x00000001u
#define FWDL_RDY           0x00000002u
#define FWDL_CHKSUM_RPT    0x00000004u
#define FWDL_WINTINI_RDY   0x00000040u
#define FWDL_RAM_DL_SEL    0x00000080u
#define FWDL_PAGE_M        0x00070000u
#define FWDL_ROM_DLEN      0x00080000u
#define FW_START_ADDR      0x1000
#define FW_PAGE_SIZE       4096
#define FW_HDR_SIZE        32
#define FW_SIG_8723B       0x530       /* header signature >> 4 */
#define FW_MAX_SIZE        0x8000

#define CR_HCI_TXDMA_EN    0x0001
#define CR_HCI_RXDMA_EN    0x0002
#define CR_TXDMA_EN        0x0004
#define CR_RXDMA_EN        0x0008
#define CR_PROTOCOL_EN     0x0010
#define CR_SCHEDULE_EN     0x0020
#define CR_MACTXEN         0x0040
#define CR_MACRXEN         0x0080
#define CR_CALTMR_EN       0x0400

#define AUTO_LLT_INIT      0x00010000u
#define RQPN_LD            0x80000000u
#define TX_QUEUE_ALL       0x7f

/* 8723B queue pages (r23b.h) and the PCIe queue map used for 88EE. */
#define PUBQ_PAGES         231
#define HPQ_PAGES          12
#define LPQ_PAGES          2
#define NPQ_PAGES          2
#define TX_PAGE_COUNT      247
#define RX_DMA_SIZE        0x3f80
#define PCIE_QMAP          0xe771

#define RCR_APM            0x00000002u  /* frames to our address */
#define RCR_AM             0x00000004u  /* multicast */
#define RCR_AB             0x00000008u  /* broadcast */
#define RCR_APP_PHYSTS     0x10000000u
#define RCR_HTC_LOC_CTRL   0x00004000u
#define RCR_AMF            0x00002000u  /* management */
#define RCR_ADF            0x00000800u  /* data */
#define RCR_APP_ICV        0x20000000u
#define RCR_APP_MIC        0x40000000u

/* r23bu_power_on btcoex and antenna bits (r23b_reg.h) */
#define PWR_DATA_RFE_CTRL_EN  0x0800
#define LEDCFG0_DPDT_SEL_EN   0x00800000u
#define BB_PAD_DPDT_SEL_DATA  0x01

/* ── efuse map (8723B common layout; PCIe part as in RTL8188EE) ── */

#define EFUSE_PHYS_LEN     512
#define EFUSE_MAP_LEN      512
#define ROM_CRYSTALCAP     0xb9
#define ROM_THERMAL        0xba
#define ROM_PCI_MAC        0xd0

/* ── rings ────────────────────────────────────────────────── */

#define RX_COUNT    64
#define RX_BUF      4096
#define TX_COUNT    32
#define TX_BUF      4096
#define RX_DESC_SZ  32          /* rtwn_rx_stat_pci */
#define TX_DESC_SZ  64          /* ring stride, as r92ce_tx_desc */
/*
 * TX descriptor words. The 8723B control block is 40 bytes, words 0-9: FreeBSD
 * r23bu_attach.c sets txdesc_len = sizeof(struct r12a_tx_desc) and fills it with
 * r12a_fill_tx_desc (rtl8812a/r12a_tx_desc.h: txdw0-6, a reserved word 7, txdw8
 * HWSEQ_EN, txdw9 SEQ). fjord-wifi builds exactly that block (QSEL in txdw1, DRVRATE
 * in txdw3, rate in txdw4). The PCIe tail follows the control block the way
 * r92ce_tx_desc follows its 32-byte one: buffer address, its high half, next
 * descriptor, its high half. The first version of this file used the r92ce offsets
 * (buffer at word 8), which overwrote txdw8/txdw9 and left the chip reading its
 * frame from word 10, the next descriptor's address.
 */
#define TXD_CTRL_WORDS  10
#define TXD_BUF         10
#define TXD_BUF64       11
#define TXD_NEXT        12
#define TXD_NEXT64      13

#define RXDW0_PKTLEN_M  0x00003fffu
#define RXDW0_INFOSZ_S  16
#define RXDW0_SHIFT_S   24
#define RXDW0_EOR       0x40000000u
#define RXDW0_OWN       0x80000000u
#define TXDW0_OWN       0x80000000u

enum { Q_MGT = 0, Q_BE = 1, NQ = 2 };
/* rtwn PCI queue numbers: the kick bit in R_PCIE_CTRL_REG */
static const uint8_t q_kick[NQ] = { 6 /* MGNT */, 1 /* BE */ };
static const uint16_t q_desa[NQ] = { R_MGQ_DESA, R_BEQ_DESA };

typedef struct {
    uint8_t *virt;
    uint64_t phys;
    int      order;
} dmamem_t;

typedef struct {
    dmamem_t desc, buf;
    uint32_t cur;
    spinlock_t lock;
    /* diagnostics, reset by RTW_RINGS: frames given to the chip, descriptors it
     * handed back (OWN cleared), writes refused because the ring was full */
    uint32_t tail, inflight, pushed, done, full;
} txq_t;

#define WLAN_TXQ_MAX 64

static struct {
    volatile uint8_t *regs;
    uint32_t regs_len;
    uint8_t  bus, dev, fn;

    uint8_t  rom[EFUSE_MAP_LEN];
    int      rom_ok;
    uint32_t rom_used;          /* physical efuse bytes in use */

    int      loaded;            /* firmware running */
    int      rings_up;
    volatile int rx_waiting;    /* a poller sleeps on /dev/rtw0 */

    dmamem_t rx_desc, rx_buf;
    uint32_t rx_cur;
    uint32_t rx_restarts;
    spinlock_t rx_lock;
    txq_t    tx[NQ];

    /* wlan0 */
    netdev_t nd;
    volatile int link;
    skb_t   *txq[WLAN_TXQ_MAX];
    uint32_t txq_head, txq_n;
    spinlock_t txq_lock;
} g;

/* ── helpers ──────────────────────────────────────────────── */

static inline uint8_t  rd1(uint32_t a) { return *(volatile uint8_t  *)(g.regs + a); }
static inline uint16_t rd2(uint32_t a) { return *(volatile uint16_t *)(g.regs + a); }
static inline uint32_t rd4(uint32_t a) { return *(volatile uint32_t *)(g.regs + a); }
static inline void wr1(uint32_t a, uint8_t v)  { *(volatile uint8_t  *)(g.regs + a) = v; }
static inline void wr2(uint32_t a, uint16_t v) { *(volatile uint16_t *)(g.regs + a) = v; }
static inline void wr4(uint32_t a, uint32_t v) { *(volatile uint32_t *)(g.regs + a) = v; }

static void set1(uint32_t a, uint8_t clr, uint8_t set)    { wr1(a, (uint8_t)((rd1(a) & ~clr) | set)); }
static void set2(uint32_t a, uint16_t clr, uint16_t set)  { wr2(a, (uint16_t)((rd2(a) & ~clr) | set)); }
static void set4(uint32_t a, uint32_t clr, uint32_t set)  { wr4(a, (rd4(a) & ~clr) | set); }
/* rtwn_setbits_1_shift: the byte at a + shift, with 32-bit masks shifted down */
static void set1s(uint32_t a, uint32_t clr, uint32_t set, int shift)
{
    set1(a + (uint32_t)shift, (uint8_t)(clr >> (shift * 8)), (uint8_t)(set >> (shift * 8)));
}

/* No calibrated delay in Ranix: a port 0x80 read takes about 1 us (see rtl8168.c). */
static void udelay(uint32_t us) { while (us--) (void)inb(0x80); }
static void mdelay(uint32_t ms) { while (ms--) udelay(1000); }

/* memcpy may use string stores that x86 does not order with later writes. */
static inline void dma_wmb(void) { __asm__ volatile("sfence" ::: "memory"); }
static inline void cbarrier(void) { __asm__ volatile("" ::: "memory"); }

/* DMA memory below 4 GiB: the descriptor-base registers fjord-wifi writes are 32-bit. */
static int dma_alloc(dmamem_t *d, size_t bytes)
{
    int order = 0;
    while ((4096UL << order) < bytes) order++;
    /* The buddy allocator has no low zone and on an 8 GiB machine hands out blocks
     * above 4 GiB first; keep those aside (up to 64) until a low one comes. */
    void *held[64];
    int nheld = 0, ok = 0;
    for (int tries = 0; tries < 64; tries++) {
        void *p = buddy_alloc(order);
        if (!p) break;
        if ((uint64_t)p + (4096UL << order) <= 0x100000000UL) {
            d->phys = (uint64_t)p;
            ok = 1;
            break;
        }
        held[nheld++] = p;
    }
    while (nheld) buddy_free((uint64_t)held[--nheld], order);
    if (!ok) return -1;
    d->order = order;
    d->virt  = (uint8_t *)PHYS_TO_VIRT(d->phys);
    memset(d->virt, 0, 4096UL << order);
    return 0;
}

/* ── efuse (rtwn_efuse_read, r23b_efuse_preread) ─────────── */

static int efuse_byte(uint32_t addr, uint8_t *out)
{
    uint32_t r = rd4(R_EFUSE_CTRL);
    r = (r & ~0x0003ff00u) | ((addr << 8) & 0x0003ff00u);
    r &= ~EFUSE_CTRL_VALID;
    wr4(R_EFUSE_CTRL, r);
    for (int i = 0; i < 100; i++) {
        r = rd4(R_EFUSE_CTRL);
        if (r & EFUSE_CTRL_VALID) { *out = (uint8_t)r; return 0; }
        udelay(10);
    }
    return -1;
}

/* The physical efuse is a list of records: a header names an 8-byte block of the
 * logical map and a mask of which 2-byte words follow (a set bit means absent). */
static int efuse_read(void)
{
    memset(g.rom, 0xff, sizeof g.rom);
    set4(R_EFUSE_TEST, EFUSE_TEST_SEL_M, 0);           /* Wi-Fi bank */
    set2(R_EFUSE_TEST, EFUSE_TEST_PGMEN, 0);
    wr1(R_EFUSE_ACCESS, EFUSE_ACCESS_ON);
    uint16_t f = rd2(R_SYS_FUNC_EN);
    if (!(f & SYS_FUNC_ELDR)) wr2(R_SYS_FUNC_EN, f | SYS_FUNC_ELDR);
    uint16_t c = rd2(R_SYS_CLKR);
    if ((c & (SYS_CLKR_LOADER_EN | SYS_CLKR_ANA8M)) != (SYS_CLKR_LOADER_EN | SYS_CLKR_ANA8M))
        wr2(R_SYS_CLKR, c | SYS_CLKR_LOADER_EN | SYS_CLKR_ANA8M);

    uint32_t a = 0;
    uint8_t h, h2, b;
    int err = 0;
    while (a < EFUSE_PHYS_LEN) {
        if (efuse_byte(a++, &h)) { err = -1; break; }
        if (h == 0xff) break;
        uint32_t off;
        if ((h & 0x1f) == 0x0f) {                    /* extended header */
            if (efuse_byte(a++, &h2)) { err = -1; break; }
            if ((h2 & 0x0f) == 0x0f) continue;
            off = ((h2 & 0xf0u) >> 1) | (h >> 5);
            h = h2;
        } else {
            off = h >> 4;
        }
        for (int w = 0; w < 4; w++) {
            if (h & (1 << w)) continue;
            uint32_t m = off * 8 + (uint32_t)w * 2;
            for (int k = 0; k < 2; k++) {
                if (efuse_byte(a++, &b)) { err = -1; break; }
                if (m + (uint32_t)k < EFUSE_MAP_LEN) g.rom[m + (uint32_t)k] = b;
            }
            if (err) break;
        }
        if (err) break;
    }
    wr1(R_EFUSE_ACCESS, EFUSE_ACCESS_OFF);
    g.rom_used = a;
    g.rom_ok = !err;
    return err;
}

static int mac_valid(const uint8_t *m)
{
    int ff = 1, zero = 1;
    for (int i = 0; i < 6; i++) { if (m[i] != 0xff) ff = 0; if (m[i]) zero = 0; }
    return !ff && !zero && !(m[0] & 1);
}

/* ── power (r23bu_power_on/off plus the r88ee PCIe steps) ─── */

static int wait_set4(uint32_t reg, uint32_t bit, int want, int tries, int us)
{
    for (int i = 0; i < tries; i++) {
        if (!!(rd4(reg) & bit) == want) return 0;
        udelay((uint32_t)us);
    }
    return -1;
}

static int power_on(void)
{
    wr1(R_RSV_CTRL, 0);                                         /* unlock ISO/CLK/power */
    set1s(R_APS_FSMCO, APS_AFSM_HSUS | APS_APDM_HPDN, 0, 1);   /* leave suspend */
    set1s(R_APS_FSMCO, APS_AFSM_HSUS | APS_AFSM_PCIE, 0, 1);
    set1(R_LDOA15_CTRL, 0, LDOA15_EN);
    set1(0x067, 0x10, 0);                                       /* BT_GPS_SEL pins off */
    mdelay(1);
    set1(R_SYS_ISO_CTRL, SYS_ISO_IP2MAC, 0);                    /* analog to digital */
    set1s(R_APS_FSMCO, APS_APFM_RSM | APS_AFSM_HSUS | APS_AFSM_PCIE, 0, 1);
    if (wait_set4(R_APS_FSMCO, APS_SUS_HOST, 1, 5000, 10)) {
        kprintf(KERN_ERR "rtw8723: timeout waiting for chip power\n");
        return -ETIMEDOUT;
    }
    set1s(R_APS_FSMCO, 0, APS_RDY_MACON, 2);                    /* release WLON reset */
    set1s(R_APS_FSMCO, APS_APDM_HPDN, 0, 1);
    set1s(R_APS_FSMCO, APS_AFSM_HSUS | APS_AFSM_PCIE, 0, 1);
    set1s(R_APS_FSMCO, 0, APS_APFM_ONMAC, 1);                   /* MAC on */
    if (wait_set4(R_APS_FSMCO, APS_APFM_ONMAC, 0, 5000, 10)) {
        kprintf(KERN_ERR "rtw8723: timeout waiting for MAC power\n");
        return -ETIMEDOUT;
    }
    set1(R_AFE_MISC, 0, 0x40);
    set1(R_PCIE_CTRL_REG + 2, 0, 0x04);                         /* r88ee_power_on */

    wr1(R_CR, 0);
    set2(R_CR, 0, CR_HCI_TXDMA_EN | CR_TXDMA_EN | CR_HCI_RXDMA_EN | CR_RXDMA_EN |
                  CR_PROTOCOL_EN | CR_SCHEDULE_EN | CR_CALTMR_EN);   /* no ENSEC: software crypto */

    /* btcoex/antenna power-on, as r23bu_power_on */
    wr1(R_BB_PAD_CTRL + 3, 0x20);
    set2(R_SYS_FUNC_EN, 0, SYS_FUNC_BBRSTB | SYS_FUNC_BB_GLB_RST);
    wr1(R_GNT_BT + 1, 0x18);
    wr1(0x76e, 0x4);
    wr4(R_ANT_SEL, 0);                                          /* S0 */
    set2(R_PWR_DATA, 0, PWR_DATA_RFE_CTRL_EN);
    set4(R_LEDCFG0, 0, LEDCFG0_DPDT_SEL_EN);
    set1(R_BB_PAD_CTRL, BB_PAD_DPDT_SEL_DATA, 0);

    wr4(R_INT_MIG, 0);
    wr4(R_MCUTST_1, 0);
    return 0;
}

static void power_off(void)
{
    wr1(R_PCIE_CTRL_REG + 1, 0xff);                             /* DMA stop bits (r88ee_power_off) */
    wr4(R_CR, 0);                                               /* stop RX/TX */
    wr1(R_TXPAUSE, TX_QUEUE_ALL);
    set1(R_SYS_FUNC_EN, SYS_FUNC_BBRSTB, 0);
    udelay(1);
    set1(R_SYS_FUNC_EN, SYS_FUNC_BB_GLB_RST, 0);
    wr1(R_CR, CR_HCI_TXDMA_EN | CR_HCI_RXDMA_EN);
    set1s(R_SYS_FUNC_EN, SYS_FUNC_CPUEN, 0, 1);                 /* 8051 reset */
    wr1(R_MCUFWDL, 0);
    wr1(R_RF_CTRL, 0);
    set1s(R_APS_FSMCO, 0, APS_RDY_MACON, 2);
    set1s(R_APS_FSMCO, 0, APS_APFM_OFF, 1);                     /* MAC off by state machine */
    if (wait_set4(R_APS_FSMCO, APS_APFM_OFF, 0, 10, 5000))
        kprintf(KERN_WARN "rtw8723: MAC did not turn off\n");
    set1(R_AFE_MISC, 0x40, 0);
    set1(R_SYS_ISO_CTRL, 0, SYS_ISO_IP2MAC);
    set1(R_LDOA15_CTRL, LDOA15_EN, 0);
    set1s(R_APS_FSMCO, APS_AFSM_PCIE, APS_AFSM_HSUS, 1);        /* suspend */
}

/* ── firmware (rtwn_load_firmware, r23b_fw_*, r92c/r92e_fw_reset) ── */

static void fw_reset_8051(void)
{
    wr1(R_HMETFR + 3, 0x20);                                    /* ask the 8051 to reset */
    for (int i = 0; i < 100; i++) {
        if (!(rd2(R_SYS_FUNC_EN) & SYS_FUNC_CPUEN)) return;
        udelay(50);
    }
    set1s(R_SYS_FUNC_EN, SYS_FUNC_CPUEN, 0, 1);
}

static int fw_load(const uint8_t *fw, size_t len)
{
    if (len >= FW_HDR_SIZE && ((fw[0] | fw[1] << 8) >> 4) == FW_SIG_8723B) {
        kprintf(KERN_INFO "rtw8723: firmware v%u.%u\n", fw[4] | fw[5] << 8, fw[6] | fw[7] << 8);
        fw += FW_HDR_SIZE;
        len -= FW_HDR_SIZE;
    }
    if (rd1(R_MCUFWDL) & FWDL_RAM_DL_SEL) {                    /* firmware already in RAM */
        wr1(R_MCUFWDL, 0);
        fw_reset_8051();
    }

    set1s(R_SYS_FUNC_EN, 0, SYS_FUNC_CPUEN, 1);                 /* 8051 on */
    set1(R_MCUFWDL, 0, FWDL_EN);
    for (int i = 0; i <= 100 && !(rd1(R_MCUFWDL) & FWDL_EN); i++) {
        wr1(R_MCUFWDL, rd1(R_MCUFWDL) | FWDL_EN);
        udelay(1000);
    }
    set1s(R_MCUFWDL, FWDL_ROM_DLEN, 0, 2);

    int ok = 0;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        set1(R_MCUFWDL, 0, FWDL_CHKSUM_RPT);
        for (size_t off = 0; off < len; off += FW_PAGE_SIZE) {
            size_t n = len - off < FW_PAGE_SIZE ? len - off : FW_PAGE_SIZE;
            set4(R_MCUFWDL, FWDL_PAGE_M, (uint32_t)(off / FW_PAGE_SIZE) << 16);
            for (size_t i = 0; i < n; i++)
                wr1(FW_START_ADDR + (uint32_t)i, fw[off + i]);
        }
        for (int i = 0; i < 25; i++) {
            if (rd4(R_MCUFWDL) & FWDL_CHKSUM_RPT) { ok = 1; break; }
            mdelay(10);
        }
    }
    if (!ok) {
        kprintf(KERN_ERR "rtw8723: firmware checksum not reported\n");
        return -EIO;
    }

    set1(R_MCUFWDL, FWDL_EN, 0);
    set4(R_MCUFWDL, FWDL_WINTINI_RDY, FWDL_RDY);
    /* restart the 8051 through the MCU IO wrapper */
    set1(R_RSV_CTRL + 1, 0x01, 0);
    set1s(R_SYS_FUNC_EN, SYS_FUNC_CPUEN, 0, 1);
    set1(R_RSV_CTRL + 1, 0, 0x01);
    set1s(R_SYS_FUNC_EN, 0, SYS_FUNC_CPUEN, 1);
    for (int i = 0; i < 20; i++) {
        if (rd4(R_MCUFWDL) & FWDL_WINTINI_RDY) return 0;
        mdelay(10);
    }
    kprintf(KERN_ERR "rtw8723: firmware did not report ready\n");
    return -ETIMEDOUT;
}

/* ── TX buffer pages and queues (rtwn_dma_init) ──────────── */

static int queues_init(void)
{
    set4(R_AUTO_LLT, 0, AUTO_LLT_INIT);                         /* hardware builds the LLT */
    if (wait_set4(R_AUTO_LLT, AUTO_LLT_INIT, 0, 1000, 1)) {
        kprintf(KERN_ERR "rtw8723: LLT init timed out\n");
        return -ETIMEDOUT;
    }
    wr1(R_RQPN_NPQ, NPQ_PAGES);
    wr4(R_RQPN, (uint32_t)PUBQ_PAGES << 16 | (uint32_t)LPQ_PAGES << 8 | HPQ_PAGES | RQPN_LD);
    uint8_t bnd = TX_PAGE_COUNT + 1;
    wr1(R_TXPKTBUF_BCNQ_BDNY, bnd);
    wr1(R_TXPKTBUF_MGQ_BDNY, bnd);
    wr1(R_TXPKTBUF_WMAC_LBK_BF_HD, bnd);
    wr1(R_TRXFF_BNDY, bnd);
    wr1(R_TDECTRL + 1, bnd);
    set2(R_TRXDMA_CTRL, 0xfff0, PCIE_QMAP);
    wr2(R_TRXFF_BNDY + 2, RX_DMA_SIZE - 1);
    wr1(R_PBP, 0x22);                                           /* 256-byte RX and TX pages */
    wr4(R_HIMR, 0);                                             /* polled */
    wr4(R_HIMRE, 0);
    return 0;
}

/* ── rings ────────────────────────────────────────────────── */

static inline volatile uint32_t *rx_desc(uint32_t i)
{
    return (volatile uint32_t *)(g.rx_desc.virt + i * RX_DESC_SZ);
}
static inline volatile uint32_t *tx_desc(int q, uint32_t i)
{
    return (volatile uint32_t *)(g.tx[q].desc.virt + i * TX_DESC_SZ);
}

/* rtwn_pci_setup_rx_desc: buffer address and size first, OWN last. */
static void rx_arm(uint32_t i)
{
    volatile uint32_t *d = rx_desc(i);
    for (int w = 1; w < 8; w++) d[w] = 0;
    d[6] = (uint32_t)(g.rx_buf.phys + (uint64_t)i * RX_BUF);
    d[0] = RX_BUF | (i == RX_COUNT - 1 ? RXDW0_EOR : 0);
    dma_wmb();
    d[0] |= RXDW0_OWN;
}

/* Ring memory is allocated once (at probe) and never freed: the card may still
 * hold any address it was ever given. Each piece is checked on its own. The first
 * version tested only rx_desc, so after a partial failure the next RTW_RINGS
 * "succeeded" with rx_buf and the TX buffers at physical 0, and the card wrote
 * received frames over low memory: the resets of 2026-11-09/10. */
static int rings_ready(void)
{
    if (!g.rx_desc.virt || !g.rx_buf.virt) return 0;
    for (int q = 0; q < NQ; q++)
        if (!g.tx[q].desc.virt || !g.tx[q].buf.virt) return 0;
    return 1;
}

static int rings_alloc(void)
{
    int err = 0;
    if (!g.rx_desc.virt && dma_alloc(&g.rx_desc, RX_COUNT * RX_DESC_SZ)) err = -1;
    if (!g.rx_buf.virt  && dma_alloc(&g.rx_buf, (size_t)RX_COUNT * RX_BUF)) err = -1;
    for (int q = 0; q < NQ; q++) {
        if (!g.tx[q].desc.virt && dma_alloc(&g.tx[q].desc, TX_COUNT * TX_DESC_SZ)) err = -1;
        if (!g.tx[q].buf.virt  && dma_alloc(&g.tx[q].buf, (size_t)TX_COUNT * TX_BUF)) err = -1;
    }
    return err || !rings_ready() ? -1 : 0;
}

/* DMA stop bits on, MAC TX/RX off (r88ee_power_off order). */
static void dma_quiesce(void)
{
    if (!g.regs) return;
    wr1(R_PCIE_CTRL_REG + 1, 0xff);
    set1(R_CR, CR_MACRXEN | CR_MACTXEN, 0);
}

static void rings_reset(void)
{
    for (uint32_t i = 0; i < RX_COUNT; i++) rx_arm(i);
    g.rx_cur = 0;
    for (int q = 0; q < NQ; q++) {
        memset(g.tx[q].desc.virt, 0, TX_COUNT * TX_DESC_SZ);
        for (uint32_t i = 0; i < TX_COUNT; i++) {        /* r92ce_setup_tx_desc: a chain */
            uint64_t next = g.tx[q].desc.phys + ((i + 1) % TX_COUNT) * TX_DESC_SZ;
            tx_desc(q, i)[TXD_NEXT]   = (uint32_t)next;
            tx_desc(q, i)[TXD_NEXT64] = (uint32_t)(next >> 32);
        }
        g.tx[q].cur = 0;
        g.tx[q].tail = g.tx[q].inflight = 0;
        g.tx[q].pushed = g.tx[q].done = g.tx[q].full = 0;
    }
    dma_wmb();
}

static int rings_start(uint32_t base[3])
{
    g.rings_up = 0;
    dma_quiesce();
    if (rings_alloc()) {
        kprintf(KERN_ERR "rtw8723: no DMA memory below 4 GiB for rings\n");
        return -ENOMEM;     /* card stays stopped; descriptor registers untouched */
    }
    uint32_t pcie_ctrl = rd4(R_PCIE_CTRL_REG);
    rings_reset();
    wr4(R_RX_DESA, (uint32_t)g.rx_desc.phys);
    for (int q = 0; q < NQ; q++) wr4(q_desa[q], (uint32_t)g.tx[q].desc.phys);
    wr1(R_RX_DRVINFO_SZ, 4);                                    /* 32-byte PHY status */
    wr4(R_RCR, RCR_APM | RCR_AM | RCR_AB | RCR_ADF | RCR_AMF | RCR_HTC_LOC_CTRL |
               RCR_APP_PHYSTS | RCR_APP_ICV | RCR_APP_MIC);
    wr1(R_TXPAUSE, 0);
    wr1(R_PCIE_CTRL_REG + 1, 0);                                /* DMA stop bits off (r88ee_post_init) */
    set1(R_CR, 0, CR_MACRXEN | CR_MACTXEN);
    kprintf(KERN_INFO "rtw8723: rings rx %08x mgq %08x beq %08x, tx desc 40+tail (buf @40, next @48), "
            "pcie_ctrl was %08x\n", (uint32_t)g.rx_desc.phys, (uint32_t)g.tx[Q_MGT].desc.phys,
            (uint32_t)g.tx[Q_BE].desc.phys, pcie_ctrl);
    base[0] = (uint32_t)g.rx_desc.phys;
    base[1] = (uint32_t)g.tx[Q_MGT].desc.phys;
    base[2] = (uint32_t)g.tx[Q_BE].desc.phys;
    g.rx_restarts++;
    g.rings_up = 1;
    return 0;
}

/* One received frame: descriptor, then driver info + shift + frame from the buffer.
 * The copy to user space happens outside the lock, from a heap bounce buffer. */
static ssize_t rx_pop(void *ubuf, size_t len)
{
    uint8_t *frame = kmalloc(RX_DESC_SZ + RX_BUF);
    if (!frame) return -ENOMEM;
    uint64_t f = spin_lock_irq(&g.rx_lock);
    uint32_t i = g.rx_cur;
    volatile uint32_t *d = rx_desc(i);
    uint32_t w0 = d[0];
    if (w0 & RXDW0_OWN) {
        spin_unlock_irq(&g.rx_lock, f);
        kfree(frame);
        return -EAGAIN;
    }
    cbarrier();
    uint32_t data = (w0 & RXDW0_PKTLEN_M) + ((w0 >> RXDW0_INFOSZ_S) & 0xf) * 8 + ((w0 >> RXDW0_SHIFT_S) & 3);
    if (data > RX_BUF) data = RX_BUF;
    for (int w = 0; w < 8; w++) ((uint32_t *)frame)[w] = d[w];
    memcpy(frame + RX_DESC_SZ, g.rx_buf.virt + (uint64_t)i * RX_BUF, data);
    rx_arm(i);
    g.rx_cur = (i + 1) % RX_COUNT;
    spin_unlock_irq(&g.rx_lock, f);

    size_t n = RX_DESC_SZ + data;
    if (n > len) n = len;
    long bad = copy_fop_to_user(ubuf, frame, n);
    kfree(frame);
    return bad ? -EFAULT : (ssize_t)n;
}

/* Descriptors the chip has handed back since the last call (caller holds t->lock). */
static void tx_reclaim(int q)
{
    txq_t *t = &g.tx[q];
    while (t->inflight && !(tx_desc(q, t->tail)[0] & TXDW0_OWN)) {
        t->tail = (t->tail + 1) % TX_COUNT;
        t->inflight--;
        t->done++;
    }
}

static ssize_t tx_push(const void *ubuf, size_t len)
{
    if (len < 1 + 40 + 24 || len > 1 + 40 + TX_BUF) return -EINVAL;
    uint8_t *b = kmalloc(len);
    if (!b) return -ENOMEM;
    if (copy_fop_from_user(b, ubuf, len)) { kfree(b); return -EFAULT; }
    int q = b[0];
    uint32_t w[TXD_CTRL_WORDS];
    memcpy(w, b + 1, sizeof w);
    uint32_t flen = (uint32_t)len - 41;
    /* The chip reads pktlen and txbufsize bytes from the buffer: never past the frame. */
    if (q >= NQ || (w[0] & 0xffff) > flen || (w[7] & 0xffff) > flen) { kfree(b); return -EINVAL; }

    txq_t *t = &g.tx[q];
    uint64_t f = spin_lock_irq(&t->lock);
    tx_reclaim(q);
    uint32_t slot = t->cur;
    volatile uint32_t *d = tx_desc(q, slot);
    if (d[0] & TXDW0_OWN) {
        t->full++;
        spin_unlock_irq(&t->lock, f);
        kfree(b);
        return -EAGAIN;
    }
    uint64_t pa = t->buf.phys + (uint64_t)slot * TX_BUF;
    memcpy(t->buf.virt + (uint64_t)slot * TX_BUF, b + 41, flen);
    for (int k = 1; k < TXD_CTRL_WORDS; k++) d[k] = w[k];
    d[TXD_BUF]   = (uint32_t)pa;                                /* r92ce_tx_postsetup */
    d[TXD_BUF64] = (uint32_t)(pa >> 32);
    dma_wmb();
    d[0] = w[0] | TXDW0_OWN;
    dma_wmb();
    /* Observation only: where the chip's descriptor pointer stands before this kick
     * (the DESA registers read back the current descriptor), and the status bits. */
    uint32_t desa = rd4(q_desa[q]), hisr = rd4(R_HISR);
    wr2(R_PCIE_CTRL_REG, (uint16_t)(1u << q_kick[q]));
    t->cur = (slot + 1) % TX_COUNT;
    t->inflight++;
    uint32_t n = ++t->pushed, done = t->done, full = t->full;
    spin_unlock_irq(&t->lock, f);
    kfree(b);

    /* The first frames after RTW_RINGS, then every power of two: a few lines per
     * session, enough to see whether descriptors come back and how the pointer moves. */
    if (n <= 4 || !(n & (n - 1)))
        kprintf(KERN_INFO "rtw8723: tx q%d #%u done %u full %u slot %u w0 %08x w1 %08x w3 %08x "
                "w4 %08x w7 %08x buf %08x desa %08x hisr %08x\n",
                q, n, done, full, slot, w[0], w[1], w[3], w[4], w[7], (uint32_t)pa, desa, hisr);
    return (ssize_t)len;
}

/* Timer tick: wake a poller once the head descriptor is ours. One memory read. */
static void rtw_tick(void)
{
    if (!g.rx_waiting || !g.rings_up) return;
    if (!(rx_desc(g.rx_cur)[0] & RXDW0_OWN)) {
        g.rx_waiting = 0;
        poll_notify();
    }
}

/* ── /dev/rtw0 ────────────────────────────────────────────── */

static char *put_hex(char *p, uint32_t v, int digits)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) *p++ = hx[(v >> (i * 4)) & 0xf];
    return p;
}
static char *put_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

static ssize_t efuse_text(void *ubuf, size_t len)
{
    char t[160], *p = t;
    const uint8_t *m = g.rom + ROM_PCI_MAC;
    p = put_str(p, "rtw8723: efuse ");
    p = put_str(p, g.rom_ok ? "ok" : "partial");
    p = put_str(p, " used=0x"); p = put_hex(p, g.rom_used, 3);
    p = put_str(p, " mac_hi="); p = put_hex(p, (uint32_t)m[0] << 24 | m[1] << 16 | m[2] << 8 | m[3], 8);
    p = put_str(p, " mac_lo="); p = put_hex(p, (uint32_t)m[4] << 8 | m[5], 4);
    p = put_str(p, " xtal=");   p = put_hex(p, g.rom[ROM_CRYSTALCAP], 2);
    p = put_str(p, " thermal="); p = put_hex(p, g.rom[ROM_THERMAL], 2);
    p = put_str(p, g.loaded ? " fw=up\n" : " fw=down\n");
    size_t n = (size_t)(p - t);
    if (n > len) n = len;
    return copy_fop_to_user(ubuf, t, n) ? -EFAULT : (ssize_t)n;
}

static ssize_t rtw_read(vfs_file_t *f, void *buf, size_t len)
{
    (void)f;
    if (!g.rings_up) return efuse_text(buf, len);
    return rx_pop(buf, len);
}

static ssize_t rtw_write(vfs_file_t *f, const void *buf, size_t len)
{
    (void)f;
    if (!g.rings_up) return -EIO;
    return tx_push(buf, len);
}

static int rtw_poll(vfs_file_t *f)
{
    (void)f;
    if (!g.rings_up) return 1;
    if (!(rx_desc(g.rx_cur)[0] & RXDW0_OWN)) return 1;
    g.rx_waiting = 1;
    return 0;
}

static void wlan_txq_flush(void);

static int rtw_ioctl(vfs_file_t *f, uint32_t cmd, uint64_t arg)
{
    (void)f;
    void *u = (void *)arg;
    switch (cmd) {
    case RTW_PEEK:
    case RTW_POKE: {
        struct rtw_reg r;
        if (copy_from_user(&r, u, sizeof r)) return -EFAULT;
        if ((r.width != 1 && r.width != 2 && r.width != 4) || r.addr % r.width ||
            r.addr + r.width > g.regs_len)
            return -EINVAL;
        if (cmd == RTW_POKE) {
            if (r.width == 1) wr1(r.addr, (uint8_t)r.val);
            else if (r.width == 2) wr2(r.addr, (uint16_t)r.val);
            else wr4(r.addr, r.val);
            return 0;
        }
        r.val = r.width == 1 ? rd1(r.addr) : r.width == 2 ? rd2(r.addr) : rd4(r.addr);
        return copy_to_user(u, &r, sizeof r) ? -EFAULT : 0;
    }
    case RTW_LOAD: {
        struct rtw_fw_arg a;
        if (copy_from_user(&a, u, sizeof a)) return -EFAULT;
        if (a.len < FW_HDR_SIZE || a.len > FW_MAX_SIZE) return -EINVAL;
        uint8_t *fw = kmalloc(a.len);
        if (!fw) return -ENOMEM;
        if (copy_from_user(fw, (void *)a.ptr, a.len)) { kfree(fw); return -EFAULT; }
        g.rings_up = 0;
        g.loaded = 0;
        dma_quiesce();                              /* a reload must not leave DMA running */
        if (!g.rom_ok) efuse_read();                /* read at boot; retry only if that failed */
        int e = power_on();
        if (!e) e = fw_load(fw, a.len);
        if (!e) e = queues_init();
        kfree(fw);
        if (e) return e;
        g.loaded = 1;
        kprintf(KERN_INFO "rtw8723: powered on, firmware running\n");
        return 0;
    }
    case RTW_RINGS: {
        if (!g.loaded) return -EIO;
        uint32_t base[3];
        int e = rings_start(base);
        if (e) return e;
        return copy_to_user(u, base, sizeof base) ? -EFAULT : 0;
    }
    case RTW_STOP:
        g.link = 0;
        wlan_txq_flush();
        g.rings_up = 0;
        g.loaded = 0;
        power_off();
        return 0;
    case RTW_LINK:
        if (!arg) {
            g.link = 0;
            wlan_txq_flush();
            return 0;
        }
        if (copy_from_user(g.nd.mac, u, 6)) return -EFAULT;
        g.link = 1;
        return 0;
    case RTW_RXDBG: {
        uint32_t o[8] = {0};
        if (g.rings_up) {
            uint32_t hw = g.rx_cur, k = 0;          /* first slot the chip still owns */
            while (k < RX_COUNT && !(rx_desc(hw)[0] & RXDW0_OWN)) { hw = (hw + 1) % RX_COUNT; k++; }
            o[0] = g.rx_cur;
            o[1] = hw;
            o[2] = rx_desc(hw)[0];
            o[3] = rx_desc(hw)[6];
        }
        for (int q = 0; q < NQ && g.rings_up; q++) {   /* TX: pushed << 16 | done */
            uint64_t fl = spin_lock_irq(&g.tx[q].lock);
            tx_reclaim(q);
            o[4 + q] = (g.tx[q].pushed & 0xffff) << 16 | (g.tx[q].done & 0xffff);
            spin_unlock_irq(&g.tx[q].lock, fl);
        }
        if (g.rings_up) o[6] = rd4(R_MGQ_DESA);
        o[7] = g.rx_restarts;
        return copy_to_user(u, o, sizeof o) ? -EFAULT : 0;
    }
    }
    return -ENOTTY;
}

static vfs_fops_t rtw_fops = {
    .read  = rtw_read,
    .write = rtw_write,
    .ioctl = rtw_ioctl,
    .poll  = rtw_poll,
};

/* ── wlan0: Ethernet frames between the stack and fjord-wifi ── */

static void wlan_txq_flush(void)
{
    uint64_t f = spin_lock_irq(&g.txq_lock);
    while (g.txq_n) {
        skb_free(g.txq[g.txq_head]);
        g.txq_head = (g.txq_head + 1) % WLAN_TXQ_MAX;
        g.txq_n--;
    }
    spin_unlock_irq(&g.txq_lock, f);
}

static int wlan_tx(netdev_t *dev, skb_t *skb)
{
    (void)dev;
    if (!g.link) { skb_free(skb); return -1; }
    uint64_t f = spin_lock_irq(&g.txq_lock);
    if (g.txq_n == WLAN_TXQ_MAX) {
        spin_unlock_irq(&g.txq_lock, f);
        skb_free(skb);
        return -1;
    }
    g.txq[(g.txq_head + g.txq_n) % WLAN_TXQ_MAX] = skb;
    g.txq_n++;
    spin_unlock_irq(&g.txq_lock, f);
    poll_notify();
    return 0;
}

static void wlan_poll(netdev_t *dev) { (void)dev; }   /* frames arrive by write() */
static int  wlan_link(netdev_t *dev) { (void)dev; return g.link; }

static ssize_t wlan_read(vfs_file_t *f, void *buf, size_t len)
{
    (void)f;
    uint64_t fl = spin_lock_irq(&g.txq_lock);
    if (!g.txq_n) { spin_unlock_irq(&g.txq_lock, fl); return -EAGAIN; }
    skb_t *s = g.txq[g.txq_head];
    g.txq_head = (g.txq_head + 1) % WLAN_TXQ_MAX;
    g.txq_n--;
    spin_unlock_irq(&g.txq_lock, fl);
    size_t n = s->len < len ? s->len : len;
    long bad = copy_fop_to_user(buf, s->data, n);
    skb_free(s);
    return bad ? -EFAULT : (ssize_t)n;
}

static ssize_t wlan_write(vfs_file_t *f, const void *buf, size_t len)
{
    (void)f;
    if (len < 14 || len > SKB_DATA_MAX) return -EINVAL;
    skb_t *s = skb_alloc();
    if (!s) { netdev_rx_drop(&g.nd); return -ENOMEM; }
    if (copy_fop_from_user(s->data, buf, len)) { skb_free(s); return -EFAULT; }
    s->len = (uint16_t)len;
    netdev_rx_enqueue(&g.nd, s);
    return (ssize_t)len;
}

static int wlan_fpoll(vfs_file_t *f)
{
    (void)f;
    return g.txq_n != 0;
}

static vfs_fops_t wlan_fops = {
    .read  = wlan_read,
    .write = wlan_write,
    .poll  = wlan_fpoll,
};

/* ── probe ────────────────────────────────────────────────── */

#define VENDOR_REALTEK 0x10ec
#define DEVICE_8723BE  0xb723
#define CFG_BAR2       0x18
#define CFG_BAR3       0x1c
#define PCI_CMD_INTX_OFF 0x0400     /* PCI command register, interrupt disable */

/* Standard PCI BAR sizing, with memory decode off while the BAR holds all ones. */
static uint32_t bar_size(uint8_t b, uint8_t d, uint8_t fn)
{
    uint16_t cmd = pci_read16(b, d, fn, PCI_COMMAND);
    pci_write16(b, d, fn, PCI_COMMAND, cmd & ~PCI_CMD_MEM);
    uint32_t orig = pci_read32(b, d, fn, CFG_BAR2);
    pci_write32(b, d, fn, CFG_BAR2, 0xffffffffu);
    uint32_t sz = pci_read32(b, d, fn, CFG_BAR2) & ~0xfu;
    pci_write32(b, d, fn, CFG_BAR2, orig);
    pci_write16(b, d, fn, PCI_COMMAND, cmd);
    return sz ? ~sz + 1 : 0;
}

void rtw8723_init(void)
{
    if (pci_find(VENDOR_REALTEK, DEVICE_8723BE, &g.bus, &g.dev, &g.fn) != 0)
        return;     /* no card: the normal case in QEMU */

    /* D0 (PCI PM spec: PMCSR at cap + 4, bits 1:0) in case the last OS left D3hot.
     * Leaving D3hot may reset config space, so the BARs are put back if they changed. */
    uint8_t pm = pci_find_cap(g.bus, g.dev, g.fn, PCI_CAP_PM);
    if (pm) {
        uint16_t pmcsr = pci_read16(g.bus, g.dev, g.fn, (uint8_t)(pm + 4));
        if (pmcsr & 3) {
            uint32_t b2 = pci_read32(g.bus, g.dev, g.fn, CFG_BAR2);
            uint32_t b3 = pci_read32(g.bus, g.dev, g.fn, CFG_BAR3);
            pci_write16(g.bus, g.dev, g.fn, (uint8_t)(pm + 4), pmcsr & ~3u);
            mdelay(10);
            if (pci_read32(g.bus, g.dev, g.fn, CFG_BAR2) != b2) {
                pci_write32(g.bus, g.dev, g.fn, CFG_BAR2, b2);
                pci_write32(g.bus, g.dev, g.fn, CFG_BAR3, b3);
            }
            kprintf(KERN_INFO "rtw8723: was in D%u, now D0\n", pmcsr & 3);
        }
    }

    uint32_t bar = pci_read32(g.bus, g.dev, g.fn, CFG_BAR2);
    if (bar & 1) {
        kprintf(KERN_ERR "rtw8723: bar2 is not memory (0x%x)\n", bar);
        return;
    }
    uint64_t base = bar & ~0xfUL;
    if ((bar & 0x6) == 0x4)
        base |= (uint64_t)pci_read32(g.bus, g.dev, g.fn, CFG_BAR3) << 32;
    uint32_t size = bar_size(g.bus, g.dev, g.fn);
    if (!base || size < 0x2000) {
        kprintf(KERN_ERR "rtw8723: bar2 unusable (base 0x%lx size 0x%x)\n", base, size);
        return;
    }
    if (size > 0x10000) size = 0x10000;

    /* Memory decode first; bus mastering only after the card is told to stop DMA.
     * A warm reboot may not reset the card, and the last OS's rings point into
     * memory that is now ours. Polled driver: legacy INTx stays off. */
    uint16_t cmd0 = pci_read16(g.bus, g.dev, g.fn, PCI_COMMAND);
    pci_write16(g.bus, g.dev, g.fn, PCI_COMMAND, (uint16_t)((cmd0 | PCI_CMD_MEM | PCI_CMD_INTX_OFF) & ~PCI_CMD_MASTER));

    /* ASPM off, as rtwn_pci_attach does (PCIe Link Control bits 1:0). */
    uint8_t pcie = pci_find_cap(g.bus, g.dev, g.fn, PCI_CAP_PCIE);
    if (pcie) {
        uint16_t lc = pci_read16(g.bus, g.dev, g.fn, (uint8_t)(pcie + 0x10));
        pci_write16(g.bus, g.dev, g.fn, (uint8_t)(pcie + 0x10), lc & ~3u);
    }

    g.regs = ioremap(base, size, VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);
    if (!g.regs) {
        kprintf(KERN_ERR "rtw8723: ioremap failed\n");
        return;
    }
    g.regs_len = size;

    /* What the card looks like on arrival (observation), then stop its DMA. */
    kprintf(KERN_INFO "rtw8723: on arrival cmd %04x cr %04x pcie_ctrl %08x rx_desa %08x mgq_desa %08x\n",
            cmd0, rd2(R_CR), rd4(R_PCIE_CTRL_REG), rd4(R_RX_DESA), rd4(R_MGQ_DESA));
    wr1(R_PCIE_CTRL_REG + 1, 0xff);                             /* r88ee_power_off */
    wr1(R_TXPAUSE, TX_QUEUE_ALL);
    set1(R_CR, CR_MACRXEN | CR_MACTXEN, 0);
    pci_write16(g.bus, g.dev, g.fn, PCI_COMMAND,
                (uint16_t)(cmd0 | PCI_CMD_MEM | PCI_CMD_MASTER | PCI_CMD_INTX_OFF));

    g.rx_lock = (spinlock_t)SPINLOCK_INIT;
    g.txq_lock = (spinlock_t)SPINLOCK_INIT;
    for (int q = 0; q < NQ; q++) g.tx[q].lock = (spinlock_t)SPINLOCK_INIT;
    /* Early in boot memory below 4 GiB is plentiful; later it may not be. */
    if (rings_alloc())
        kprintf(KERN_WARN "rtw8723: ring memory not all allocated at probe, will retry at RTW_RINGS\n");

    efuse_read();
    const uint8_t *mac = g.rom + ROM_PCI_MAC;
    if (mac_valid(mac))
        memcpy(g.nd.mac, mac, 6);

    memcpy(g.nd.name, "wlan0", 6);
    g.nd.ip         = 0;            /* DHCP through the link watcher once fjord-wifi joins */
    g.nd.tx         = wlan_tx;
    g.nd.poll       = wlan_poll;
    g.nd.link_check = wlan_link;
    g.nd.priv       = &g;
    g.nd.rx_lock    = (spinlock_t)SPINLOCK_INIT;
    net_register(&g.nd);

    devfs_register_device("rtw0",  S_IFCHR | 0600, 0x0F10, &rtw_fops);
    devfs_register_device("wlan0", S_IFCHR | 0600, 0x0F11, &wlan_fops);
    tick_register(rtw_tick);

    kprintf(KERN_INFO "rtw8723: rtl8723be at %02x:%02x.%u, bar2 0x%lx/0x%x, efuse %s, "
            "mac %02x:%02x:%02x:%02x:%02x:%02x\n",
            g.bus, g.dev, g.fn, base, size, g.rom_ok ? "ok" : "partial",
            g.nd.mac[0], g.nd.mac[1], g.nd.mac[2], g.nd.mac[3], g.nd.mac[4], g.nd.mac[5]);
}
