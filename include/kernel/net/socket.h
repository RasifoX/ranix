#ifndef _KERNEL_NET_SOCKET_H
#define _KERNEL_NET_SOCKET_H

#include <uapi/types.h>
#include <kernel/net/net.h>
#include <kernel/sync.h>

struct process;
typedef struct process process_t;

/* AF / type / proto */
#define AF_UNIX     1
#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3

/* TCP states */
#define TCP_CLOSED      0
#define TCP_LISTEN      1
#define TCP_SYN_SENT    2
#define TCP_SYN_RECV    3
#define TCP_ESTABLISHED 4
#define TCP_FIN_WAIT1   5
#define TCP_FIN_WAIT2   6
#define TCP_CLOSE_WAIT  7
#define TCP_CLOSING     8
#define TCP_LAST_ACK    9
#define TCP_TIME_WAIT   10

/* sockaddr_in */
typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;    /* big-endian */
    uint32_t sin_addr;    /* big-endian */
    uint8_t  sin_zero[8];
} __attribute__((packed)) sockaddr_in_t;

/* sockaddr_un */
typedef struct {
    uint16_t sun_family;
    char     sun_path[108];
} __attribute__((packed)) sockaddr_un_t;

typedef struct socket {
    int       type;         /* SOCK_STREAM / SOCK_DGRAM */
    int       domain;       /* AF_INET */
    int       proto;

    uint32_t  local_ip;
    uint16_t  local_port;
    uint32_t  remote_ip;
    uint16_t  remote_port;

    netdev_t *dev;

    /* recv queue */
    spinlock_t  recv_lock;
    skb_t      *recv_head;
    skb_t      *recv_tail;
    process_t  *recv_waiter;    /* process sleeping in recvfrom */

    /* TCP */
    int         tcp_state;
    uint32_t    snd_nxt;
    uint32_t    rcv_nxt;
    uint32_t    snd_una;
    uint16_t    rcv_wnd;        /* advertised receive window */
    uint16_t    snd_wnd;        /* remote receive window (flow control) */
    process_t  *connect_waiter; /* blocking connect() / send() */

    /* TCP listen: incoming connection queue */
    int         backlog;
    struct socket *accept_head; /* completed connections (ESTABLISHED) */
    struct socket *accept_tail;
    process_t  *accept_waiter;  /* blocking accept() */

    /* TCP retransmit: single unacked segment */
    uint8_t    *retx_data;      /* kmalloc'd copy, NULL if nothing pending */
    uint16_t    retx_len;
    uint32_t    retx_seq;
    uint8_t     retx_flags;
    uint64_t    retx_deadline;  /* kernel_ticks deadline */
    int         retx_count;     /* retransmit attempts since last ack */

    /* FIN teardown: process sleeping in close() waiting for CLOSED/TIME_WAIT */
    process_t  *close_waiter;

    /* AF_UNIX: peer socket for socketpair() and path-based connections */
    struct socket *unix_peer;
    char           unix_path[108];  /* bound path; [0]=='\0' → abstract namespace */
    int            unix_path_len;   /* 0 = not bound, >0 = length of unix_path */
} socket_t;

/* Per-process socket table — max 16 sockets per process for now */
#define SOCK_MAX_PER_PROC 16

/* Kernel socket API */
socket_t *sock_alloc(int domain, int type, int proto);
void      sock_free(socket_t *s);

/* UDP delivery hook */
void sock_udp_deliver(uint32_t src_ip, uint16_t src_port,
                      uint16_t dst_port, const void *data, uint16_t len);

/* ICMP echo-reply delivery to RAW sockets */
void sock_icmp_deliver(uint32_t src_ip, const void *data, uint16_t len);

/* Global socket table */
void      sock_register(socket_t *s);
void      sock_unregister(socket_t *s);
int       gsock_count(void);
extern socket_t **gsock;
extern int        gsock_cap;

/* TCP data delivery */
void sock_tcp_deliver(socket_t *s, const void *data, uint16_t len);

/* AF_UNIX send */
int unix_send(socket_t *s, const void *data, uint16_t len);

#endif
