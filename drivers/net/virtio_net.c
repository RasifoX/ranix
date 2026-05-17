#include <kernel/net/net.h>
#include <kernel/fdt.h>
#include <kernel/mm/vm.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <asm/mmu.h>

/*
 * virtio-net MMIO (legacy transport, spec 0.9/1.0 MMIO).
 * One device per MMIO node found in FDT with DeviceID=1.
 */

/* ── MMIO register offsets (32-bit each) ─────────────────── */
#define VNET_MAGIC       0x000
#define VNET_VERSION     0x004
#define VNET_DEVICE_ID   0x008
#define VNET_VENDOR_ID   0x00C
#define VNET_HOST_FEAT   0x010
#define VNET_HOST_SEL    0x014
#define VNET_GUEST_FEAT  0x020
#define VNET_GUEST_SEL   0x024
#define VNET_GUEST_PG_SZ 0x028
#define VNET_QUEUE_SEL   0x030
#define VNET_QUEUE_MAX   0x034
#define VNET_QUEUE_NUM   0x038
#define VNET_QUEUE_ALIGN 0x03C
#define VNET_QUEUE_PFN   0x040
#define VNET_QUEUE_NTFY  0x050
#define VNET_IRQ_STATUS  0x060
#define VNET_IRQ_ACK     0x064
#define VNET_STATUS      0x070
#define VNET_CONFIG      0x100

#define VNET_MAGIC_VAL   0x74726976u
#define VNET_DEV_NET     1

#define VIRTIO_STATUS_ACK     0x01
#define VIRTIO_STATUS_DRIVER  0x02
#define VIRTIO_STATUS_OK      0x04
#define VIRTIO_STATUS_FAILED  0x80

/* ── vring ───────────────────────────────────────────────── */
#define VNET_Q  64   /* queue depth, power-of-two */

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) vring_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VNET_Q];
} __attribute__((packed)) vring_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) vring_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    vring_used_elem_t ring[VNET_Q];
} __attribute__((packed)) vring_used_t;

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

/* ── virtio-net header (prepended to each packet) ────────── */
typedef struct {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
} __attribute__((packed)) virtio_net_hdr_t;

#define VNET_HDR_SZ  sizeof(virtio_net_hdr_t)
#define VNET_BUF_SZ  (VNET_HDR_SZ + 1514)

/* ── driver state ────────────────────────────────────────── */

typedef struct {
    volatile uint32_t *regs;

    /* RX vring — static, page-aligned */
    vring_desc_t  *rx_desc;
    vring_avail_t *rx_avail;
    vring_used_t  *rx_used;
    uint8_t        rx_bufs[VNET_Q][VNET_BUF_SZ];
    uint16_t       rx_last;

    /* TX vring */
    vring_desc_t  *tx_desc;
    vring_avail_t *tx_avail;
    vring_used_t  *tx_used;
    uint8_t        tx_buf[VNET_BUF_SZ];    /* one TX in flight at a time */
    uint16_t       tx_last;
} vnet_priv_t;

static vnet_priv_t g_priv;
static netdev_t    g_dev;
static int         g_init_done;

/* ── register accessors ──────────────────────────────────── */

static inline uint32_t vr(vnet_priv_t *p, uint32_t off)
{
    return p->regs[off / 4];
}
static inline void vw(vnet_priv_t *p, uint32_t off, uint32_t val)
{
    p->regs[off / 4] = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ── vring layout helpers ────────────────────────────────── */

/* vring size: desc table + avail + pad + used */
static uint32_t vring_size(uint32_t qsz)
{
    uint32_t d = qsz * sizeof(vring_desc_t);
    uint32_t a = sizeof(uint16_t) * (3 + qsz);
    uint32_t pa = ((d + a + 4095) & ~4095u);
    uint32_t u = sizeof(uint16_t) * 3 + qsz * sizeof(vring_used_elem_t);
    return pa + u;
}

static void setup_queue(vnet_priv_t *p, int qidx,
                        vring_desc_t **desc_out,
                        vring_avail_t **avail_out,
                        vring_used_t **used_out)
{
    vw(p, VNET_QUEUE_SEL, (uint32_t)qidx);
    uint32_t qsz = vr(p, VNET_QUEUE_MAX);
    if (qsz > VNET_Q) qsz = VNET_Q;

    /* allocate contiguous pages for vring */
    uint32_t sz = vring_size(qsz);
    uint32_t pages = (sz + 4095) / 4096;
    /* find order */
    int ord = 0; while ((1u << ord) < pages) ord++;
    void *phys = buddy_alloc(ord);
    if (!phys) { kprintf("virtio-net: OOM for queue %d\n", qidx); return; }
    void *virt = (void *)PHYS_TO_VIRT(phys);
    memset(virt, 0, (size_t)pages * 4096);

    vring_desc_t  *desc  = (vring_desc_t *)virt;
    uint8_t       *avail_raw = (uint8_t *)virt + qsz * sizeof(vring_desc_t);
    vring_avail_t *avail = (vring_avail_t *)avail_raw;
    uint8_t       *used_raw = (uint8_t *)virt + ((qsz * sizeof(vring_desc_t)
                              + sizeof(uint16_t) * (3 + qsz) + 4095) & ~4095);
    vring_used_t  *used  = (vring_used_t *)used_raw;

    *desc_out  = desc;
    *avail_out = avail;
    *used_out  = used;

    vw(p, VNET_QUEUE_NUM,   qsz);
    vw(p, VNET_QUEUE_ALIGN, 4096);
    vw(p, VNET_QUEUE_PFN,   (uint32_t)((uint64_t)phys / 4096));
}

/* ── TX ──────────────────────────────────────────────────── */

static int vnet_tx(netdev_t *dev, skb_t *skb)
{
    vnet_priv_t *p = (vnet_priv_t *)dev->priv;

    if (skb->len + VNET_HDR_SZ > VNET_BUF_SZ) { skb_free(skb); return -1; }

    /* build TX buffer: virtio_net_hdr + packet */
    uint8_t *buf = p->tx_buf;
    memset(buf, 0, VNET_HDR_SZ);
    memcpy(buf + VNET_HDR_SZ, skb->data, skb->len);
    uint32_t total = (uint32_t)(VNET_HDR_SZ + skb->len);
    skb_free(skb);

    uint16_t di = p->tx_last % VNET_Q;
    p->tx_desc[di].addr  = VIRT_TO_PHYS(buf);
    p->tx_desc[di].len   = total;
    p->tx_desc[di].flags = 0;

    p->tx_avail->ring[p->tx_avail->idx % VNET_Q] = di;
    __asm__ volatile("dsb sy" ::: "memory");
    p->tx_avail->idx++;
    __asm__ volatile("dsb sy" ::: "memory");

    vw(p, VNET_QUEUE_NTFY, 1);   /* notify TX queue (index 1) */

    /* poll for completion */
    for (int i = 0; i < 10000; i++) {
        __asm__ volatile("dsb sy" ::: "memory");
        if (p->tx_used->idx != p->tx_last) break;
    }
    p->tx_last = p->tx_used->idx;
    return 0;
}

/* ── RX poll ─────────────────────────────────────────────── */

static void vnet_poll(netdev_t *dev)
{
    vnet_priv_t *p = (vnet_priv_t *)dev->priv;
    if (!p) return;

    __asm__ volatile("dsb sy" ::: "memory");

    while (p->rx_last != p->rx_used->idx) {
        vring_used_elem_t *u = &p->rx_used->ring[p->rx_last % VNET_Q];
        uint32_t desc_id = u->id;
        uint32_t rcv_len = u->len;

        if (rcv_len > VNET_HDR_SZ) {
            uint8_t *pkt = p->rx_bufs[desc_id] + VNET_HDR_SZ;
            uint32_t pkt_len = rcv_len - (uint32_t)VNET_HDR_SZ;

            skb_t *skb = skb_alloc();
            if (skb) {
                if (pkt_len > SKB_DATA_MAX) pkt_len = SKB_DATA_MAX;
                memcpy(skb->data, pkt, pkt_len);
                skb->len = (uint16_t)pkt_len;
                netdev_rx_enqueue(dev, skb);
            }
        }

        /* re-arm descriptor */
        p->rx_avail->ring[p->rx_avail->idx % VNET_Q] = (uint16_t)desc_id;
        __asm__ volatile("dsb sy" ::: "memory");
        p->rx_avail->idx++;
        __asm__ volatile("dsb sy" ::: "memory");
        vw(p, VNET_QUEUE_NTFY, 0);   /* notify RX queue (index 0) */

        p->rx_last++;
    }
}

/* ── probe ───────────────────────────────────────────────── */

static void virtio_net_setup(volatile uint32_t *regs)
{
    vnet_priv_t *p = &g_priv;
    p->regs = regs;

    /* reset */
    vw(p, VNET_STATUS, 0);
    vw(p, VNET_STATUS, VIRTIO_STATUS_ACK);
    vw(p, VNET_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* negotiate features — accept whatever device offers */
    vw(p, VNET_HOST_SEL, 0);
    uint32_t feat = vr(p, VNET_HOST_FEAT);
    feat &= ~(1u << 6);   /* no VIRTIO_NET_F_MAC forced — we read MAC separately */
    vw(p, VNET_GUEST_SEL, 0);
    vw(p, VNET_GUEST_FEAT, feat);
    vw(p, VNET_GUEST_PG_SZ, 4096);

    /* setup queues */
    setup_queue(p, 0, &p->rx_desc, &p->rx_avail, &p->rx_used);
    setup_queue(p, 1, &p->tx_desc, &p->tx_avail, &p->tx_used);

    /* driver OK */
    vw(p, VNET_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_OK);

    /* read MAC from Config */
    volatile uint8_t *cfg = (volatile uint8_t *)(regs) + VNET_CONFIG;
    for (int i = 0; i < 6; i++) g_dev.mac[i] = cfg[i];

    /* pre-fill RX descriptors */
    for (int i = 0; i < VNET_Q; i++) {
        p->rx_desc[i].addr  = VIRT_TO_PHYS(p->rx_bufs[i]);
        p->rx_desc[i].len   = VNET_BUF_SZ;
        p->rx_desc[i].flags = VRING_DESC_F_WRITE;
        p->rx_avail->ring[i] = (uint16_t)i;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    p->rx_avail->idx = VNET_Q;
    __asm__ volatile("dsb sy" ::: "memory");
    vw(p, VNET_QUEUE_NTFY, 0);

    /* register netdev */
    memcpy(g_dev.name, "eth0", 5);
    /* QEMU slirp default: 10.0.2.15 */
    g_dev.ip   = (10u<<24)|(0u<<16)|(2u<<8)|15u;
    g_dev.gw   = (10u<<24)|(0u<<16)|(2u<<8)|2u;
    g_dev.mask = 0xffffff00u;
    g_dev.tx   = vnet_tx;
    g_dev.poll = vnet_poll;
    g_dev.priv = p;
    g_dev.rx_lock = (spinlock_t)SPINLOCK_INIT;
    net_register(&g_dev);
}

static void probe_mmio(uint64_t phys, uint64_t size, void *ud)
{
    (void)size; (void)ud;
    if (g_init_done) return;

    volatile uint32_t *r = (volatile uint32_t *)ioremap(
        phys, 0x200, VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);
    if (!r) return;

    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t magic = r[VNET_MAGIC / 4];
    uint32_t devid = r[VNET_DEVICE_ID / 4];

    if (magic != VNET_MAGIC_VAL) return;
    if (devid != VNET_DEV_NET) return;

    kprintf("virtio-net: found at 0x%lx\n", phys);
    virtio_net_setup(r);
    g_init_done = 1;
}

void virtio_net_init(void)
{
    fdt_foreach_node("virtio_mmio", probe_mmio, NULL);
    if (!g_init_done)
        kprintf("virtio-net: no device found\n");
}
