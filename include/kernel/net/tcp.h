#ifndef _KERNEL_NET_TCP_H
#define _KERNEL_NET_TCP_H

#include <kernel/net/net.h>
#include <kernel/net/ip.h>
#include <kernel/net/ipv6.h>
#include <kernel/net/socket.h>

/* TCP flags */
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10
#define TCP_URG  0x20

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;   /* high 4 bits = header len in 32-bit words */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed)) tcp_hdr_t;

/* pseudo header for checksum */
typedef struct {
    uint32_t src;
    uint32_t dst;
    uint8_t  zero;
    uint8_t  proto;
    uint16_t tcp_len;
} __attribute__((packed)) tcp_pseudo_t;

void tcp_rx(netdev_t *dev, skb_t *skb, ip_hdr_t *iph);
void tcp6_rx(netdev_t *dev, skb_t *skb, ipv6_hdr_t *ip6);

/* Called by connect() — send SYN, move to SYN_SENT */
int tcp_connect(socket_t *s);

/* Called by send() — send data segment */
int tcp_send(socket_t *s, const void *data, uint16_t len);

/* Called by close() — send FIN */
int tcp_close(socket_t *s);

/* Fill a struct tcp_info prefix (cwnd/ssthresh/rtt) for getsockopt(TCP_INFO);
 * returns the number of bytes written into buf (≤ buflen). */
uint32_t tcp_get_info(socket_t *s, void *buf, uint32_t buflen);

/* Called by close() on a LISTEN socket — RST and free every queued connection
 * that was never accept()ed (otherwise they leak in gsock[] and tcp_tick keeps
 * driving them). */
void tcp_listener_abort(socket_t *s);

/* Called by listen() — mark LISTEN, set backlog */
int tcp_listen(socket_t *s, int backlog);

/* Called from sched tick — retransmit expired segments */
void tcp_tick(void);

/* Deliver received data into socket's recv queue and wake waiter */
void sock_tcp_deliver(socket_t *s, const void *data, uint16_t len);

/* Find a SOCK_STREAM socket by 4-tuple */
socket_t *tcp_sock_find(struct net_namespace *ns, uint32_t local_ip, uint16_t local_port,
                        uint32_t remote_ip, uint16_t remote_port);
socket_t *tcp_sock_find6(struct net_namespace *ns, uint16_t local_port,
                         const in6_addr_t *remote_ip6, uint16_t remote_port);

/* Find a LISTEN socket by local port + family */
socket_t *tcp_listen_find(struct net_namespace *ns, uint16_t local_port, int domain);

#endif
