#include <kernel/net/arp.h>
#include <kernel/net/eth.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>

#define ARP_HW_ETH   1
#define ARP_OP_REQ   1
#define ARP_OP_REPLY 2

typedef struct {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t oper;
    uint8_t  sha[6]; uint32_t spa;
    uint8_t  tha[6]; uint32_t tpa;
} __attribute__((packed)) arp_pkt_t;

/* ── ARP table ───────────────────────────────────────────── */

#define ARP_TABLE_SIZE 64

typedef struct { uint32_t ip; uint8_t mac[6]; int valid; uint32_t age; } arp_entry_t;
static arp_entry_t arp_table[ARP_TABLE_SIZE];
static uint32_t    arp_clock;

static void arp_table_update(uint32_t ip, const uint8_t *mac)
{
    arp_clock++;
    /* update existing */
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            memcpy(arp_table[i].mac, mac, 6);
            arp_table[i].age = arp_clock;
            return;
        }
    }
    /* insert into free slot */
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].valid) {
            arp_table[i].ip = ip; arp_table[i].age = arp_clock;
            memcpy(arp_table[i].mac, mac, 6);
            arp_table[i].valid = 1;
            return;
        }
    }
    /* evict oldest entry (LRU) */
    int oldest = 0;
    for (int i = 1; i < ARP_TABLE_SIZE; i++)
        if (arp_table[i].age < arp_table[oldest].age) oldest = i;
    arp_table[oldest].ip = ip; arp_table[oldest].age = arp_clock;
    memcpy(arp_table[oldest].mac, mac, 6);
    arp_table[oldest].valid = 1;
}

int arp_lookup(netdev_t *dev, uint32_t ip, uint8_t *mac_out)
{
    /* loopback / self */
    if (ip == dev->ip) { memcpy(mac_out, dev->mac, 6); return 0; }

    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            memcpy(mac_out, arp_table[i].mac, 6);
            return 0;
        }
    }

    /* send request */
    skb_t *skb = skb_alloc();
    if (!skb) return -1;
    arp_pkt_t *a = (arp_pkt_t *)skb->data;
    skb->len = sizeof(arp_pkt_t);
    a->htype = htons(ARP_HW_ETH);
    a->ptype = htons(ETH_P_IP);
    a->hlen  = 6; a->plen = 4;
    a->oper  = htons(ARP_OP_REQ);
    memcpy(a->sha, dev->mac, 6);
    a->spa = htonl(dev->ip);
    memset(a->tha, 0, 6);
    a->tpa = htonl(ip);

    /* prepend ethernet header */
    eth_hdr_t *eh = (eth_hdr_t *)skb_push(skb, sizeof(eth_hdr_t));
    memcpy(eh->dst, eth_bcast, 6);
    memcpy(eh->src, dev->mac, 6);
    eh->type = htons(ETH_P_ARP);

    dev->tx(dev, skb);
    return -1;
}

void arp_rx(netdev_t *dev, skb_t *skb)
{
    if (skb->len < (uint16_t)sizeof(arp_pkt_t)) { skb_free(skb); return; }
    arp_pkt_t *a = (arp_pkt_t *)skb->data;

    if (ntohs(a->htype) != ARP_HW_ETH || ntohs(a->ptype) != ETH_P_IP) {
        skb_free(skb); return;
    }

    uint32_t spa = ntohl(a->spa);
    uint32_t tpa = ntohl(a->tpa);

    arp_table_update(spa, a->sha);

    if (ntohs(a->oper) == ARP_OP_REQ && tpa == dev->ip) {
        /* send reply */
        arp_pkt_t *r = (arp_pkt_t *)skb->data;
        r->oper = htons(ARP_OP_REPLY);
        memcpy(r->tha, a->sha, 6);
        r->tpa = a->spa;
        memcpy(r->sha, dev->mac, 6);
        r->spa = htonl(dev->ip);

        eth_hdr_t *eh = (eth_hdr_t *)skb_push(skb, sizeof(eth_hdr_t));
        memcpy(eh->dst, a->sha, 6);
        memcpy(eh->src, dev->mac, 6);
        eh->type = htons(ETH_P_ARP);
        dev->tx(dev, skb);
    } else {
        skb_free(skb);
    }
}
