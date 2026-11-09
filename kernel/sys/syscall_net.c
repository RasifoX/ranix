/* ── socket syscalls ─────────────────────────────────────── */

static void unix_reg_remove(socket_t *s); /* forward */

#define MSG_DONTWAIT 0x40

/* A nonblocking fd must not sleep in recv* any more than in read(). */
static int sock_nonblocking(int fd, int flags)
{
    vfs_file_t *f = fd_get(task_current()->fds, fd);
    return (flags & MSG_DONTWAIT) || (f && (f->flags & O_NONBLOCK));
}

/* ::ffff:a.b.c.d → a.b.c.d (host order) */
static int in6_v4mapped(const in6_addr_t *a, uint32_t *v4)
{
    const uint8_t *b = a->s6;
    for (int i = 0; i < 10; i++) if (b[i]) return 0;
    if (b[10] != 0xff || b[11] != 0xff) return 0;
    *v4 = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) | ((uint32_t)b[14] << 8) | b[15];
    return 1;
}

static void in6_set_v4mapped(in6_addr_t *a, uint32_t v4)
{
    memset(a->s6, 0, 10);
    a->s6[10] = a->s6[11] = 0xff;
    a->s6[12] = (uint8_t)(v4 >> 24); a->s6[13] = (uint8_t)(v4 >> 16);
    a->s6[14] = (uint8_t)(v4 >> 8);  a->s6[15] = (uint8_t)v4;
}

static int sock_at_eof(const socket_t *s)
{
    if (s->domain == AF_UNIX) return s->unix_peer == NULL || s->unix_rd_eof;
    return s->type == SOCK_STREAM &&
           (s->tcp_state == TCP_CLOSE_WAIT || s->tcp_state == TCP_CLOSED ||
            s->tcp_state == TCP_LAST_ACK   || s->tcp_state == TCP_TIME_WAIT ||
            s->tcp_state == TCP_CLOSING);
}

/* What a read at the end of the stream returns: an aborted connection says so,
 * once; a plain EOF is 0. */
static long sock_eof_rc(socket_t *s)
{
    if (s->domain == AF_UNIX || !s->so_error) return 0;
    long e = s->so_error;
    s->so_error = 0;
    return -e;
}

/* One netlink datagram: the whole iovec is one request, never one per iovec. */
static long nl_sendv(socket_t *s, const k_iovec *iov, uint64_t niov,
                     const void *uaddr, uint32_t addrlen)
{
    size_t total = 0;
    for (uint64_t i = 0; i < niov; i++) {
        if (iov[i].len > NETLINK_SEND_MAX || total + iov[i].len > NETLINK_SEND_MAX)
            return -EMSGSIZE;
        total += (size_t)iov[i].len;
    }
    if (uaddr && addrlen) {
        sockaddr_nl_t a;
        if (addrlen < sizeof(a)) return -EINVAL;
        if (copy_from_user(&a, uaddr, sizeof(a)) != 0) return -EFAULT;
        if (a.nl_family != AF_NETLINK) return -EINVAL;
        if (a.nl_pid) return -ECONNREFUSED;   /* only the kernel listens here */
        if (a.nl_groups) return -EOPNOTSUPP;   /* no multicast */
    }
    if (total == 0) return 0;

    uint8_t *kb = kmalloc(total);
    if (!kb) return -ENOMEM;
    size_t off = 0;
    for (uint64_t i = 0; i < niov; i++) {
        if (iov[i].len && copy_fop_from_user(kb + off, iov[i].base, (size_t)iov[i].len) != 0) {
            kfree(kb);
            return -EFAULT;
        }
        off += (size_t)iov[i].len;
    }
    int r = netlink_request(s, kb, total);
    kfree(kb);
    return r < 0 ? r : (long)total;
}

static long nl_name_to_user(const socket_t *s, void *uaddr, uint32_t *uaddrlen, int peer)
{
    sockaddr_nl_t a = { .nl_family = AF_NETLINK };
    if (!peer) { a.nl_pid = s->nl_pid; a.nl_groups = s->nl_groups; }
    uint32_t have;
    if (copy_from_user(&have, uaddrlen, sizeof(have)) != 0) return -EFAULT;
    if (have > sizeof(a)) have = sizeof(a);
    if (copy_to_user(uaddr, &a, have) != 0) return -EFAULT;
    uint32_t full = sizeof(a);
    return copy_to_user(uaddrlen, &full, sizeof(full)) ? -14 : 0;
}

static int sock_close_vfs(vfs_file_t *file)
{
    socket_t    *s  = (socket_t *)file->inode->priv;
    vfs_inode_t *in = file->inode;

    if (s->domain == AF_UNIX) {
        if (s->unix_path_len > 0) unix_reg_remove(s);
        socket_t *peer = s->unix_peer;
        s->unix_peer = NULL;
        if (peer && peer->unix_peer == s) {
            peer->unix_peer = NULL;
            /* wake peer's blocked recv — empty queue + no peer = EOF */
            uint64_t f = spin_lock_irq(&peer->recv_lock);
            process_t *w = peer->recv_waiter;
            peer->recv_waiter = NULL;
            spin_unlock_irq(&peer->recv_lock, f);
            if (w) task_wake(w);
            /* wake peer's blocked send */
            process_t *sw = peer->connect_waiter;
            peer->connect_waiter = NULL;
            if (sw) task_wake(sw);
        }
        sock_free(s);
        in->priv = NULL;   /* socket freed; vfs_close frees the inode (avoid double free) */
        return 0;
    }

    if (s->type == SOCK_STREAM &&
        s->tcp_state != TCP_CLOSED && s->tcp_state != TCP_LISTEN) {
        tcp_close(s);
        if (!tcp_orphan(s)) {
            in->priv = NULL;
            return 0;
        }
    }
    if (s->type == SOCK_STREAM && s->tcp_state == TCP_LISTEN)
        tcp_listener_abort(s);
    sock_free(s);
    in->priv = NULL;   /* socket freed; vfs_close frees the inode (avoid double free) */
    return 0;
}

static int sock_poll_vfs(vfs_file_t *file)
{
    socket_t *s = (socket_t *)file->inode->priv;
    if (!s) return 0;
    /* a listening socket is readable when a completed connection is queued for
     * accept() — select()/poll()-driven servers (dropbear) wait on exactly this,
     * never on recv_head, so without it the listen fd never signals POLLIN. */
    if (s->tcp_state == TCP_LISTEN) {
        uint64_t lf = spin_lock_irq(&s->recv_lock);
        int ready = (s->accept_head != NULL);
        spin_unlock_irq(&s->recv_lock, lf);
        return ready ? 0x0001 : 0;
    }
    /* a nonblocking connect() still in handshake is neither readable nor
     * writable; once it settles (ESTABLISHED or error) the socket becomes
     * writable so poll(POLLOUT) returns and getsockopt(SO_ERROR) reports it. */
    if (s->connect_inprog && s->type == SOCK_STREAM) {
        if (s->tcp_state == TCP_SYN_SENT || s->tcp_state == TCP_SYN_RECV)
            return 0;
        /* connect failed: POLLOUT so SO_ERROR is read, and POLLERR|POLLHUP as Linux
         * gives, which is all some clients look at (libcups: a refused connection
         * read as connected, then retried thousands of times a second) */
        if (s->tcp_state == TCP_CLOSED)
            return 0x0004 | 0x0008 | 0x0010;
        s->connect_inprog = 0;   /* connected — resume normal readability checks */
    }
    uint64_t f = spin_lock_irq(&s->recv_lock);
    int has_data = (s->recv_head != NULL);
    spin_unlock_irq(&s->recv_lock, f);
    int out = 0x0004;
    if (s->domain != AF_UNIX && s->type == SOCK_STREAM &&
        (s->tcp_state == TCP_ESTABLISHED || s->tcp_state == TCP_CLOSE_WAIT) && !tcp_can_send(s)) {
        s->poll_out_blocked = 1;
        out = tcp_can_send(s) ? 0x0004 : 0;   /* an ACK may have just gone by */
    }
    return ((has_data || sock_at_eof(s)) ? 0x0001 : 0) | out;
}

/* All of it on a blocking socket, as much as fits otherwise. A buffer the size of
 * the request would be copied again for every short return of one segment. */
#define TCP_COPY_MAX 65536UL
static long tcp_stream_tx(socket_t *s, const void *buf, size_t len, int nonblock, int fop)
{
    if (!len) return 0;
    uint8_t *kbuf = kmalloc(len < TCP_COPY_MAX ? len : TCP_COPY_MAX);
    if (!kbuf) return -ENOMEM;
    size_t done = 0;
    long err = 0;
    while (done < len && !err) {
        size_t n = len - done < TCP_COPY_MAX ? len - done : TCP_COPY_MAX;
        const char *from = (const char *)buf + done;
        if ((fop ? copy_fop_from_user(kbuf, from, n) : copy_from_user(kbuf, from, n)) != 0) {
            err = -14;
            break;
        }
        for (size_t off = 0; off < n; ) {
            int rc = tcp_send(s, kbuf + off, n - off > 65535 ? 65535 : (uint16_t)(n - off));
            if (rc == -11) {
                if (nonblock) { err = -11; break; }
                err = tcp_send_wait(s);
                if (err) break;
                continue;
            }
            if (rc < 0) { err = rc; break; }
            off += (size_t)rc;
            done += (size_t)rc;
        }
    }
    kfree(kbuf);
    return done ? (long)done : err;
}

static ssize_t sock_read_vfs(vfs_file_t *file, void *buf, size_t len)
{
    socket_t *s = (socket_t *)file->inode->priv;
    if (!s) return -EBADF;
    if (s->domain == AF_NETLINK) {
        k_iovec v = { buf, len };
        int mflags;
        return netlink_recv(s, &v, 1, 0, file->flags & O_NONBLOCK, &mflags);
    }
    /* blocking read: wait for data in recv queue */
    while (1) {
        /* SOCK_STREAM: coalesce queued bytes up to len (no message boundaries). */
        if (s->type == SOCK_STREAM) {
            size_t total = 0;
            while (total < len) {
                uint64_t f = spin_lock_irq(&s->recv_lock);
                skb_t *skb = s->recv_head;
                if (!skb) { spin_unlock_irq(&s->recv_lock, f); break; }
                s->recv_head = skb->next;
                if (!s->recv_head) s->recv_tail = NULL;
                spin_unlock_irq(&s->recv_lock, f);
                size_t n = skb->len < (len - total) ? skb->len : (len - total);
                if (n && copy_fop_to_user((char *)buf + total, skb->data, n) != 0) {
                    skb_free(skb);
                    return total ? (ssize_t)total : -14;
                }
                total += n;
                if (n < skb->len) {
                    skb->data += n; skb->len -= (uint16_t)n;
                    uint64_t f2 = spin_lock_irq(&s->recv_lock);
                    skb->next = s->recv_head; s->recv_head = skb;
                    if (!s->recv_tail) s->recv_tail = skb;
                    spin_unlock_irq(&s->recv_lock, f2);
                    break;
                }
                skb_free(skb);
            }
            if (total > 0) {
                tcp_rx_consumed(s, (uint32_t)total);
                return (ssize_t)total;
            }
            goto wait_or_eof;   /* nothing queued */
        }
        uint64_t f = spin_lock_irq(&s->recv_lock);
        skb_t *skb = s->recv_head;
        if (skb) {
            s->recv_head = skb->next;
            if (!s->recv_head) s->recv_tail = NULL;
            spin_unlock_irq(&s->recv_lock, f);
            size_t n = skb->len < len ? skb->len : len;
            int bad = n && copy_fop_to_user(buf, skb->data, n) != 0;
            skb_free(skb);
            return bad ? -14 : (ssize_t)n;
        }
        spin_unlock_irq(&s->recv_lock, f);
    wait_or_eof:;
        f = spin_lock_irq(&s->recv_lock);
        if (s->recv_head) { spin_unlock_irq(&s->recv_lock, f); continue; }
        /* EOF: TCP half-closed or AF_UNIX peer disconnected */
        if (sock_at_eof(s)) {
            spin_unlock_irq(&s->recv_lock, f);
            return sock_eof_rc(s);
        }
        if (file->flags & O_NONBLOCK) {
            spin_unlock_irq(&s->recv_lock, f);
            return -EAGAIN;
        }
        s->recv_waiter = task_current();
        task_sleep_unlock(&s->recv_lock, f);
        process_t *p = task_current();
        if (task_has_actionable_signal()) { s->recv_waiter = NULL; return -EINTR; }
    }
}

#define UNIX_COPY_MAX 65536UL

#define UNIX_DGRAM_MAX 65535UL   /* skb->len is 16 bits */

/* One packet per skb. A stream goes on until all of it is queued; at the budget a
 * blocking sender waits for its reader to free some, like a full send buffer. */
static long unix_send_wait(socket_t *s, const uint8_t *kb, size_t len, int nonblock)
{
    size_t done = 0;
    /* a datagram goes whole or not at all: Chromium's zygote sends 2 KB requests
     * over SOCK_SEQPACKET, and a cut one was a launch that silently failed */
    if (s->type != SOCK_STREAM && len > UNIX_DGRAM_MAX) return -EMSGSIZE;
    while (done < len) {
        size_t n = s->type != SOCK_STREAM ? len - done
                 : (len - done < SKB_DATA_MAX ? len - done : SKB_DATA_MAX);
        int r = unix_send(s, kb + done, (uint16_t)n);
        if (r == -11) {
            if (nonblock) return done ? (long)done : -11;
            if (task_has_actionable_signal()) return done ? (long)done : -4;
            poll_arm();
            poll_block((uint64_t)-1);
            continue;
        }
        if (r < 0) return done ? (long)done : r;
        done += (size_t)r;
        if (s->type != SOCK_STREAM) break;
    }
    return (long)done;
}

static long sock_sendto(socket_t *s, int nonblock, const void *buf, size_t len,
                        const void *uaddr, uint32_t addrlen);

static ssize_t sock_write_vfs(vfs_file_t *file, const void *buf, size_t len)
{
    socket_t *s = (socket_t *)file->inode->priv;
    if (!s) return -EBADF;
    if (s->domain == AF_NETLINK) {
        k_iovec v = { (void *)buf, len };
        return nl_sendv(s, &v, 1, NULL, 0);
    }
    if (s->type == SOCK_STREAM || (s->domain == AF_UNIX && s->type == SOCK_DGRAM)) {
        if (s->domain == AF_UNIX) {
            size_t done = 0;
            size_t cap = len < UNIX_COPY_MAX ? len : UNIX_COPY_MAX;
            uint8_t *kb = kmalloc(cap);
            if (!kb) return -ENOMEM;
            while (done < len) {
                size_t n = len - done < cap ? len - done : cap;
                if (copy_fop_from_user(kb, (const uint8_t *)buf + done, n) != 0) {
                    kfree(kb);
                    return done ? (ssize_t)done : -14;
                }
                long r = unix_send_wait(s, kb, n, file->flags & O_NONBLOCK);
                if (r < 0) { kfree(kb); return done ? (ssize_t)done : r; }
                done += (size_t)r;
                if ((size_t)r < n || s->type != SOCK_STREAM) break;
            }
            kfree(kb);
            return (ssize_t)done;
        }
        return tcp_stream_tx(s, buf, len, file->flags & O_NONBLOCK, 1);
    }
    if (s->type == SOCK_DGRAM && s->remote_port)   /* a connected UDP socket */
        return sock_sendto(s, file->flags & O_NONBLOCK, buf, len, NULL, 0);
    return -ENOSYS;
}

#define FIONREAD 0x541B

#define SIOCGIFNAME    0x8910
#define SIOCGIFCONF    0x8912
#define SIOCGIFFLAGS   0x8913
#define SIOCGIFADDR    0x8915
#define SIOCGIFNETMASK 0x891b
#define SIOCGIFMTU     0x8921
#define SIOCGIFHWADDR  0x8927
#define SIOCGIFINDEX   0x8933
#define SIOCGIFTXQLEN  0x8942

/* no device configuration through ioctl: same refusal as a netlink mutation */
#define SIOCSIFFLAGS   0x8914
#define SIOCSIFADDR    0x8916
#define SIOCSIFNETMASK 0x891c
#define SIOCSIFMTU     0x8922
#define SIOCSIFNAME    0x8923
#define SIOCSIFHWADDR  0x8924
#define SIOCSIFTXQLEN  0x8943

#define IFF_UP        0x1
#define IFF_BROADCAST 0x2
#define IFF_LOOPBACK  0x8
#define IFF_RUNNING   0x40
#define IFF_MULTICAST 0x1000

#define IFNAMSIZ 16
#define ARPHRD_ETHER    1
#define ARPHRD_LOOPBACK 772

struct ifreq {
    char ifr_name[IFNAMSIZ];
    union {
        sockaddr_in_t  ifru_addr;
        short          ifru_flags;
        int            ifru_ifindex;
        int            ifru_qlen;
        int            ifru_mtu;
        struct { unsigned short sa_family; char sa_data[14]; } ifru_hwaddr;
        char           ifru_pad[24];
    } u;
};

struct ifconf {
    int   ifc_len;
    union { uint64_t pad; struct ifreq *ifcu_req; } u;
};

static netdev_t *ifr_dev(const struct ifreq *r)
{
    char name[IFNAMSIZ];
    memcpy(name, r->ifr_name, IFNAMSIZ);
    name[IFNAMSIZ - 1] = '\0';
    for (int i = 0; ; i++) {
        netdev_t *d = net_dev(i);
        if (!d) return NULL;
        if (strcmp(d->name, name) == 0) return d;
    }
}

static short ifr_flags(const netdev_t *d)
{
    short f = IFF_UP | IFF_RUNNING;
    if (d->ip == 0x7f000001) f |= IFF_LOOPBACK;
    else                     f |= IFF_BROADCAST | IFF_MULTICAST;
    return f;
}

static int sock_ioctl_ifreq(uint32_t cmd, uint64_t arg)
{
    struct ifreq r;
    if (copy_from_user(&r, (void *)(uintptr_t)arg, sizeof(r))) return -EFAULT;
    netdev_t *d = ifr_dev(&r);
    if (!d) return -ENODEV;
    switch (cmd) {
    case SIOCGIFADDR: {
        memset(&r.u, 0, sizeof(r.u));
        r.u.ifru_addr.sin_family = AF_INET;
        r.u.ifru_addr.sin_addr   = htonl(d->ip);
        break;
    }
    case SIOCGIFNETMASK: {
        memset(&r.u, 0, sizeof(r.u));
        r.u.ifru_addr.sin_family = AF_INET;
        r.u.ifru_addr.sin_addr   = htonl(d->mask);
        break;
    }
    case SIOCGIFFLAGS:
        memset(&r.u, 0, sizeof(r.u));
        r.u.ifru_flags = ifr_flags(d);
        break;
    case SIOCGIFMTU:
        memset(&r.u, 0, sizeof(r.u));
        r.u.ifru_mtu = 1500;
        break;
    case SIOCGIFINDEX: {
        memset(&r.u, 0, sizeof(r.u));
        for (int i = 0; ; i++) {
            netdev_t *e = net_dev(i);
            if (!e) return -ENODEV;
            if (e == d) { r.u.ifru_ifindex = i + 1; break; }
        }
        break;
    }
    case SIOCGIFTXQLEN:
        memset(&r.u, 0, sizeof(r.u));   /* no queueing discipline: qlen stays 0 */
        break;
    case SIOCGIFHWADDR:
        memset(&r.u, 0, sizeof(r.u));
        r.u.ifru_hwaddr.sa_family =
            (d->ip == 0x7f000001) ? ARPHRD_LOOPBACK : ARPHRD_ETHER;
        memcpy(r.u.ifru_hwaddr.sa_data, d->mac, 6);
        break;
    default:
        return -ENOTTY;
    }
    if (copy_to_user((void *)(uintptr_t)arg, &r, sizeof(r))) return -EFAULT;
    return 0;
}

/* SIOCGIFCONF: fill the caller's buffer with one struct ifreq (name + IPv4
 * addr) per interface, and report the byte count actually used. */
static int sock_ioctl_ifconf(uint64_t arg)
{
    struct ifconf c;
    if (copy_from_user(&c, (void *)(uintptr_t)arg, sizeof(c))) return -EFAULT;
    struct ifreq *ureq = c.u.ifcu_req;
    int cap   = (int)(c.ifc_len / (int)sizeof(struct ifreq));
    int used  = 0;
    for (int i = 0; ; i++) {
        netdev_t *d = net_dev(i);
        if (!d) break;
        if (ureq) {                       /* NULL buffer → just size the request */
            if (used >= cap) break;
            struct ifreq r;
            memset(&r, 0, sizeof(r));
            int n = (int)strlen(d->name);
            if (n > IFNAMSIZ - 1) n = IFNAMSIZ - 1;
            memcpy(r.ifr_name, d->name, (size_t)n);
            r.u.ifru_addr.sin_family = AF_INET;
            r.u.ifru_addr.sin_addr   = htonl(d->ip);
            if (copy_to_user(&ureq[used], &r, sizeof(r))) return -EFAULT;
        }
        used++;
    }
    c.ifc_len = used * (int)sizeof(struct ifreq);
    if (copy_to_user((void *)(uintptr_t)arg, &c, sizeof(c))) return -EFAULT;
    return 0;
}

static int sock_ioctl_vfs(vfs_file_t *file, uint32_t cmd, uint64_t arg)
{
    socket_t *s = (socket_t *)file->inode->priv;
    if (!s) return -EBADF;
    switch (cmd) {
    case SIOCGIFADDR: case SIOCGIFNETMASK: case SIOCGIFFLAGS:
    case SIOCGIFMTU:  case SIOCGIFINDEX:   case SIOCGIFHWADDR:
    case SIOCGIFTXQLEN:
        return sock_ioctl_ifreq(cmd, arg);
    case SIOCSIFFLAGS: case SIOCSIFADDR: case SIOCSIFNETMASK: case SIOCSIFMTU:
    case SIOCSIFNAME:  case SIOCSIFHWADDR: case SIOCSIFTXQLEN:
        return capable_task(task_current(), CAP_NET_ADMIN) ? -95 : -1;
    case SIOCGIFCONF:
        return sock_ioctl_ifconf(arg);
    case FIONREAD: {                        /* bytes queued for reading */
        int navail = 0;
        uint64_t f = spin_lock_irq(&s->recv_lock);
        for (skb_t *skb = s->recv_head; skb; skb = skb->next) navail += skb->len;
        spin_unlock_irq(&s->recv_lock, f);
        return copy_to_user((void *)(uintptr_t)arg, &navail, sizeof(navail)) ? -14 : 0;
    }
    default:
        return -ENOTTY;
    }
}

static vfs_fops_t sock_fops = {
    .poll_keyed = 1,
    .read  = sock_read_vfs,
    .write = sock_write_vfs,
    .close = sock_close_vfs,
    .poll  = sock_poll_vfs,
    .ioctl = sock_ioctl_vfs,
};

int vfs_file_is_socket(vfs_file_t *f)
{
    return f->inode && f->inode->fops == &sock_fops;
}

/* The errno for a fd that fd_to_sock refused */
static long sock_fd_err(int fd)
{
    return fd_get(task_current()->fds, fd) ? -88 /* ENOTSOCK */ : -9;
}

/* Fill a user sockaddr: at most the caller's *klen bytes, and *klen becomes the
 * full size, so a short buffer is not overrun and the caller sees truncation. */
/* The caller's address, exactly addrlen bytes of it, into a zeroed kernel
 * buffer (Linux move_addr_to_kernel). Nothing past addrlen is read: the user
 * may end the address at the last byte of a page. */
static long sock_addr_in(uint8_t ka[SOCK_ADDR_MAX], const void *uaddr, uint32_t addrlen)
{
    if (addrlen > SOCK_ADDR_MAX) return -EINVAL;
    memset(ka, 0, SOCK_ADDR_MAX);
    if (addrlen && copy_from_user(ka, uaddr, addrlen) != 0) return -EFAULT;
    return 0;
}

static long sock_name_put(void *uaddr, const void *sa, uint32_t sz, uint32_t *klen)
{
    uint32_t n = *klen < sz ? *klen : sz;
    *klen = sz;
    return n && copy_to_user(uaddr, sa, n) != 0 ? -14 : 0;
}

/* The same for a user length pointer. */
static long sock_name_put_u(void *uaddr, const void *sa, uint32_t sz, uint32_t *ulen)
{
    uint32_t l;
    if (!ulen) return -EFAULT;
    if (copy_from_user(&l, ulen, sizeof(l)) != 0) return -EFAULT;
    if ((int)l < 0) return -EINVAL;
    if (sock_name_put(uaddr, sa, sz, &l) != 0) return -EFAULT;
    return copy_to_user(ulen, &l, sizeof(l)) ? -14 : 0;
}

static socket_t *fd_to_sock(int fd)
{
    process_t  *p = task_current();
    vfs_file_t *f = fd_get(p->fds, fd);
    if (!f || !f->inode || f->inode->fops != &sock_fops) return NULL;
    return (socket_t *)f->inode->priv;
}

static int sock_to_fd(socket_t *s, int sflags)
{
    vfs_inode_t *in = kmalloc(sizeof(*in));
    if (!in) return -ENOMEM;
    memset(in, 0, sizeof(*in));
    in->fops     = &sock_fops;
    in->priv     = s;
    in->refcount = 1;
    /* fstat() of a socket says so (S_IFSOCK), with an inode number of its own */
    static volatile uint32_t sock_ino = 0;
    in->mode     = 0140000 | 0777;
    in->nlink    = 1;
    in->ino      = 0x50000000ULL + atomic_fetch_add32(&sock_ino, 1);

    vfs_file_t *f = kmalloc(sizeof(*f));
    if (!f) { kfree(in); return -ENOMEM; }
    memset(f, 0, sizeof(*f));
    f->inode    = in;
    f->refcount = 0;   /* fd_alloc takes the +1, like vfs_open; a single close() then frees */
    if (sflags & SOCK_NONBLOCK) f->flags |= O_NONBLOCK;

    process_t *p = task_current();
    int fd = fd_alloc(p->fds, f);
    if (fd < 0) { kfree(f); kfree(in); return fd == -1 ? -12 : fd; }
    if (sflags & SOCK_CLOEXEC) fd_set_cloexec(p->fds, fd, 1);
    return fd;
}

/* ── AF_UNIX path registry ───────────────────────────────── */
/*
 * Handles both filesystem paths (sun_path[0] != '\0') and abstract
 * namespace sockets (sun_path[0] == '\0', name in sun_path[1..len-1]).
 * pathlen is the number of bytes in path[] that identify the socket.
 */

static spinlock_t unix_reg_lock = SPINLOCK_INIT;

/* The registry holds names, not inodes: nothing on the way to a socket would
 * otherwise be asked. An abstract name has no way to be asked about. */
/* connect: like open for writing, the socket file's mode decides who may
 * talk to it (root always may). No file there: the registry answers. */
static int unix_may_connect(const char *kp)
{
    vfs_stat_t vs = {0};
    if (vfs_stat(kp, &vs) != 0 || (vs.mode & 0170000) != 0140000) return 0;
    process_t *p = task_current();
    if (!p || p->euid == 0) return 0;
    int shift = 0;
    if (vs.uid == p->euid) shift = 6;
    else {
        int in_group = (vs.gid == p->egid);
        for (int i = 0; !in_group && i < p->ngroups; i++)
            if (p->groups[i] == vs.gid) in_group = 1;
        if (in_group) shift = 3;
    }
    return ((vs.mode >> shift) & 2) ? 0 : -13;   /* EACCES */
}

static int unix_path_reachable(const sockaddr_un_t *su, int connecting)
{
    if (su->sun_path[0] == '\0') return 0;
    size_t slen = 0;
    while (slen < 107 && su->sun_path[slen]) slen++;
    size_t n = 0;
    fs_path_t *cwd = fs_cwd_get(task_current()->fs);
    if (su->sun_path[0] != '/') n = strlen(cwd->s) + 1;
    /* cwd may be up to PATH_MAX-1 long: the joined name must still fit kp */
    char *kp = n + slen >= PATH_MAX ? NULL : kmalloc(PATH_MAX);
    if (!kp) { fs_path_put(cwd); return n + slen >= PATH_MAX ? -36 : -12; }   /* ENAMETOOLONG */
    if (n) {
        memcpy(kp, cwd->s, n - 1);
        kp[n - 1] = '/';
    }
    fs_path_put(cwd);
    memcpy(kp + n, su->sun_path, slen);
    kp[n + slen] = '\0';
    int r = vfs_path_searchable(kp);
    if (!r && connecting) r = unix_may_connect(kp);
    kfree(kp);
    return r;
}

/* A relative path names the same socket from any directory that resolves to
 * it, so the registry keys it by the absolute path. Too long for the key: the
 * name as given, as before. Fills sun_path, returns the key's length. */
static int unix_path_key(sockaddr_un_t *su)
{
    int slen = 0;
    while (slen < 107 && su->sun_path[slen]) slen++;
    if (su->sun_path[0] != '/') {
        fs_path_t *held = fs_cwd_get(task_current()->fs);
        const char *cwd = held->s;
        int n = (int)strlen(cwd);
        int sep = n && cwd[n - 1] != '/';
        if (n + sep + slen <= 107) {
            memmove(su->sun_path + n + sep, su->sun_path, (size_t)slen);
            memcpy(su->sun_path, cwd, (size_t)n);
            if (sep) su->sun_path[n] = '/';
            slen += n + sep;
            su->sun_path[slen] = '\0';
        }
        fs_path_put(held);
    }
    return slen + 1;
}

static socket_t *unix_reg_find_locked(net_namespace_t *ns, const char *path, int pathlen)
{
    unix_bind_t *unix_reg = ns->unix_reg;
    for (int i = 0; i < UNIX_REG_MAX; i++) {
        if (unix_reg[i].sock && unix_reg[i].pathlen == pathlen &&
            memcmp(unix_reg[i].path, path, (size_t)pathlen) == 0)
            return unix_reg[i].sock;
    }
    return NULL;
}

static socket_t *unix_reg_find(net_namespace_t *ns, const char *path, int pathlen)
{
    uint64_t f = spin_lock_irq(&unix_reg_lock);
    socket_t *found = unix_reg_find_locked(ns, path, pathlen);
    spin_unlock_irq(&unix_reg_lock, f);
    return found;
}

static int unix_reg_add(const char *path, int pathlen, socket_t *s)
{
    unix_bind_t *unix_reg = s->net_ns->unix_reg;
    uint64_t f = spin_lock_irq(&unix_reg_lock);
    int slot = -1;
    for (int i = 0; i < UNIX_REG_MAX; i++) {
        if (!unix_reg[i].sock) { slot = i; break; }
    }
    if (slot >= 0) {
        memcpy(unix_reg[slot].path, path, (size_t)pathlen);
        unix_reg[slot].pathlen = pathlen;
        unix_reg[slot].sock    = s;
    }
    spin_unlock_irq(&unix_reg_lock, f);
    return slot >= 0 ? 0 : -28; /* ENOSPC */
}

static void unix_reg_remove(socket_t *s)
{
    unix_bind_t *unix_reg = s->net_ns->unix_reg;
    uint64_t f = spin_lock_irq(&unix_reg_lock);
    for (int i = 0; i < UNIX_REG_MAX; i++) {
        if (unix_reg[i].sock == s) {
            unix_reg[i].sock    = NULL;
            unix_reg[i].pathlen = 0;
            break;
        }
    }
    spin_unlock_irq(&unix_reg_lock, f);
}

static long sys_socket(int domain, int type, int proto)
{
    int sflags = type & (SOCK_CLOEXEC | SOCK_NONBLOCK);
    type &= SOCK_TYPE_MASK;
    if (domain != AF_INET && domain != AF_INET6 && domain != AF_UNIX && domain != AF_NETLINK)
        return -EAFNOSUPPORT;
    if (type != SOCK_DGRAM && type != SOCK_STREAM && type != SOCK_RAW) return -EINVAL;
    if (domain == AF_NETLINK) {
        if (type == SOCK_STREAM) return -ESOCKTNOSUPPORT;
        if (proto != NETLINK_ROUTE) return -EPROTONOSUPPORT;
    }
    if (domain == AF_UNIX && type == SOCK_RAW) return -ESOCKTNOSUPPORT;
    /* a raw socket sees and forges traffic that is not the caller's */
    if ((domain == AF_INET || domain == AF_INET6) && type == SOCK_RAW &&
        !capable_task(task_current(), CAP_NET_RAW)) return -1;
    socket_t *s = sock_alloc(domain, type, proto);
    if (!s) return -ENOMEM;
    if (sock_own(s) != 0) { sock_free(s); return -ENOMEM; }
    s->dev = net_default_dev();
    if (type == SOCK_RAW || domain == AF_NETLINK) sock_register(s);
    int fd = sock_to_fd(s, sflags);
    if (fd < 0) { sock_free(s); return fd; }
    return fd;
}

/* Is local_port already taken by another socket of the same family+type?
 * SO_REUSEADDR (or SO_REUSEPORT on both ends) permits sharing it. */
static int ip_port_taken(socket_t *self, int domain, int type, uint16_t port)
{
    if (port == 0) return 0;
    int taken = 0;
    uint64_t f = spin_lock_irq(&gsock_lock);
    for (int i = 0; i < gsock_cap; i++) {
        socket_t *o = gsock[i];
        if (!o || o == self) continue;
        if (o->net_ns != self->net_ns) continue;
        if (o->domain != domain || o->type != type) continue;
        if (o->local_port != port) continue;
        if (o->orphan) continue;   /* the last of a closed connection holds nothing */
        if ((self->so_reuseaddr && o->so_reuseaddr) ||
            (self->so_reuseport && o->so_reuseport))
            continue;   /* both opted in — sharing allowed */
        taken = 1;
        break;
    }
    spin_unlock_irq(&gsock_lock, f);
    return taken;
}


static long sys_bind(int fd, const void *uaddr, uint32_t addrlen)
{
    (void)addrlen;
    socket_t *s = fd_to_sock(fd);
    if (!s) return sock_fd_err(fd);

    if (s->domain == AF_NETLINK) {
        sockaddr_nl_t a;
        if (addrlen < sizeof(a)) return -EINVAL;
        if (copy_from_user(&a, uaddr, sizeof(a)) != 0) return -EFAULT;
        if (a.nl_family != AF_NETLINK) return -EINVAL;
        return netlink_bind(s, a.nl_pid, a.nl_groups);
    }

    uint16_t family = 0;
    if (addrlen < sizeof(family)) return -EINVAL;
    if (copy_from_user(&family, uaddr, sizeof(family)) != 0) return -EFAULT;
    /* the two branches below store into different halves of socket_t */
    if ((family == AF_UNIX) != (s->domain == AF_UNIX)) return -EAFNOSUPPORT;
    if (family == AF_INET6 && s->domain != AF_INET6) return -EAFNOSUPPORT;

    if (family == AF_UNIX) {
        sockaddr_un_t su;
        memset(&su, 0, sizeof(su));   /* a short addrlen leaves the rest as stack bytes */
        uint32_t copylen = addrlen < sizeof(su) ? addrlen : sizeof(su);
        if (copy_from_user(&su, uaddr, copylen) != 0) return -EFAULT;
        /* a second bind would leave the first registry slot behind at close,
         * pointing at the freed socket */
        if (s->unix_path_len > 0) return -EINVAL;
        int pathlen;
        if (su.sun_path[0] == '\0') {
            /* abstract namespace: name is sun_path[0..addrlen-3], includes \0 */
            pathlen = (int)addrlen - 2;
            if (pathlen <= 0 || pathlen > 107) return -EINVAL;
        } else {
            /* filesystem path: use strlen+1 (null terminator included) */
            pathlen = unix_path_key(&su);
        }
        int pr = unix_path_reachable(&su, 0);
        if (pr) return pr;
        if (unix_reg_find(s->net_ns, su.sun_path, pathlen)) return -EADDRINUSE;
        memcpy(s->unix_path, su.sun_path, (size_t)pathlen);
        s->unix_path_len = pathlen;
        /* The name is visible in the file system, as programs check with
         * stat() before they connect. A node left by a closed socket is reused. */
        if (su.sun_path[0] == '/')
            vfs_mknod(su.sun_path, 0140000 /* S_IFSOCK */ | (0777 & ~fs_umask_of((struct process *)task_current())), 0);
        return unix_reg_add(s->unix_path, pathlen, s);
    }

    if (s->local_port) return -EINVAL;   /* already bound */
    if (family == AF_INET6) {
        sockaddr_in6_t sa6;
        if (addrlen < sizeof(sa6)) return -EINVAL;
        if (copy_from_user(&sa6, uaddr, sizeof(sa6)) != 0) return -EFAULT;
        uint16_t port = ntohs(sa6.sin6_port);
        uint32_t v4;
        if (in6_v4mapped(&sa6.sin6_addr, &v4)) {        /* an IPv4 address after all */
            if (s->v6only) return -EADDRNOTAVAIL;
            if (port && port < 1024 && !capable_task(task_current(), CAP_NET_BIND_SERVICE)) return -EACCES;
            if (!port) port = alloc_port();
            if (ip_port_taken(s, AF_INET, s->type, port)) return -EADDRINUSE;
            s->domain     = AF_INET;
            s->v4mapped   = 1;
            s->local_ip   = v4;
            s->local_port = port;
            sock_register(s);
            return 0;
        }
        if (port && port < 1024 && !capable_task(task_current(), CAP_NET_BIND_SERVICE)) return -EACCES;
        if (!port) port = alloc_port();   /* then listen() and getsockname() have one */
        if (ip_port_taken(s, AF_INET6, s->type, port)) return -EADDRINUSE;
        s->local_ip6  = sa6.sin6_addr;
        s->local_port = port;
        sock_register(s);
        return 0;
    }

    sockaddr_in_t sa;
    if (addrlen < sizeof(sa)) return -EINVAL;
    if (copy_from_user(&sa, uaddr, sizeof(sa)) != 0) return -EFAULT;
    uint16_t port = ntohs(sa.sin_port);
    if (port && port < 1024 && !capable_task(task_current(), CAP_NET_BIND_SERVICE)) return -EACCES;
    if (!port) port = alloc_port();
    if (ip_port_taken(s, AF_INET, s->type, port)) return -EADDRINUSE;
    s->local_ip   = ntohl(sa.sin_addr);
    s->local_port = port;
    sock_register(s);
    return 0;
}

/* connect() on a netlink socket: only the kernel (pid 0) can be the peer. */
static long connect_netlink(socket_t *s, const void *uaddr, uint32_t addrlen)
{
    sockaddr_nl_t a;
    if (addrlen < sizeof(a)) return -EINVAL;
    if (copy_from_user(&a, uaddr, sizeof(a)) != 0) return -EFAULT;
    if (a.nl_family != AF_NETLINK) return -EINVAL;
    if (a.nl_pid) return -ECONNREFUSED;
    if (a.nl_groups) return -EOPNOTSUPP;
    return netlink_bind(s, 0, s->nl_groups);
}

/* connect() on a unix socket: make the server-side peer and queue it on the listener. */
static long connect_unix(socket_t *s, const void *uaddr, uint32_t addrlen)
{
    sockaddr_un_t su;
    memset(&su, 0, sizeof(su));   /* a short addrlen leaves the rest as stack bytes */
    uint32_t copylen = addrlen < sizeof(su) ? addrlen : sizeof(su);
    if (copy_from_user(&su, uaddr, copylen) != 0) return -EFAULT;
    /* a replaced peer would keep pointing here after this socket is freed */
    if (s->unix_peer) return -EISCONN;
    if (s->tcp_state == TCP_LISTEN) return -EINVAL;
    int pathlen;
    if (su.sun_path[0] == '\0') {
        pathlen = (int)addrlen - 2;
        if (pathlen <= 0 || pathlen > 107) return -EINVAL;
    } else {
        pathlen = unix_path_key(&su);
    }
    int pr = unix_path_reachable(&su, 1);
    if (pr) return pr;

    /* allocate server-side peer socket */
    socket_t *peer = sock_alloc(AF_UNIX, s->type, 0);
    if (!peer) return -ENOMEM;
    if (sock_own(peer) != 0) { sock_free(peer); return -ENOMEM; }

    /* The listener stays valid only while the registry lock is held: its
     * close unregisters under this lock before freeing it. */
    uint64_t rf = spin_lock_irq(&unix_reg_lock);
    socket_t *ls = unix_reg_find_locked(s->net_ns, su.sun_path, pathlen);
    if (!ls || ls->tcp_state != TCP_LISTEN) {
        spin_unlock_irq(&unix_reg_lock, rf);
        sock_free(peer);
        return -ECONNREFUSED;
    }
    peer->unix_peer = s;
    s->unix_peer    = peer;
    {   /* SO_PEERCRED both ways: the server side sees us, we see the listener */
        process_t *me = task_current();
        peer->pc_set = 1; peer->pc_pid = (int32_t)me->tgid; peer->pc_uid = me->euid; peer->pc_gid = me->egid;
        s->pc_set = 1;    s->pc_pid = ls->own_pid;          s->pc_uid = ls->own_uid; s->pc_gid = ls->own_gid;
    }
    peer->tcp_state = TCP_ESTABLISHED;
    s->tcp_state    = TCP_ESTABLISHED;

    /* enqueue peer onto listener's accept queue */
    uint64_t f = spin_lock_irq(&ls->recv_lock);
    if (!ls->accept_head) ls->accept_head = peer;
    else                  ls->accept_tail->accept_head = peer;
    ls->accept_tail = peer;
    peer->accept_head = NULL;
    process_t *w = ls->accept_waiter;
    ls->accept_waiter = NULL;
    spin_unlock_irq(&ls->recv_lock, f);
    spin_unlock_irq(&unix_reg_lock, rf);
    if (w) task_wake(w);
    return 0;
}

/* Store the peer address (IPv4, IPv6 or v4-mapped IPv6) in the socket.
 * ka is the kernel copy (sock_addr_in), zero past addrlen. */
static long connect_set_peer(socket_t *s, const uint8_t *ka, uint32_t addrlen, uint16_t family)
{
    uint32_t mapped4 = 0;
    int is_mapped = 0;
    if (addrlen < (family == AF_INET6 ? SIN6_LEN_RFC2133 : sizeof(sockaddr_in_t))) return -EINVAL;
    if (family == AF_INET6 && (s->type == SOCK_STREAM || s->type == SOCK_DGRAM) && s->proto != IPPROTO_ICMPV6) {
        sockaddr_in6_t m6;
        memcpy(&m6, ka, sizeof(m6));
        is_mapped = in6_v4mapped(&m6.sin6_addr, &mapped4);
        if (is_mapped && s->v6only) return -ENETUNREACH;
        if (is_mapped) {                                /* go on as IPv4 */
            uint32_t bound4 = 0;
            if (s->domain == AF_INET6 && s->local_port && !in6_v4mapped(&s->local_ip6, &bound4))
                bound4 = 0;                             /* bound to "::": any address */
            if (s->domain == AF_INET6) s->local_ip = bound4;
            s->domain      = AF_INET;
            s->v4mapped    = 1;
            s->remote_ip   = mapped4;
            s->remote_port = ntohs(m6.sin6_port);
        }
    }
    if (is_mapped) {
        /* addresses set above */
    } else if (family == AF_INET6) {
        sockaddr_in6_t sa6;
        memcpy(&sa6, ka, sizeof(sa6));
        s->remote_ip6  = sa6.sin6_addr;
        s->remote_port = ntohs(sa6.sin6_port);
        if (s->type == SOCK_DGRAM) {
            /* getaddrinfo probes a source address this way: no route means no such
             * source, not a socket that claims to be somewhere */
            netdev_t *d6 = net_route_dev6(&s->remote_ip6);
            if (!ipv6_reachable(d6, &s->remote_ip6)) return -ENETUNREACH;
            s->local_ip6 = ipv6_src_for(d6, &s->remote_ip6);
        }
    } else {
        sockaddr_in_t sa;
        memcpy(&sa, ka, sizeof(sa));
        s->remote_ip   = ntohl(sa.sin_addr);
        s->remote_port = ntohs(sa.sin_port);
        if (s->domain == AF_INET6 && s->type == SOCK_DGRAM) {
            /* getaddrinfo keeps one socket for both families, then asserts that the
             * source of an IPv4 peer reads back as ::ffff:a.b.c.d */
            netdev_t *d = net_route_dev(s->remote_ip);
            uint32_t ip = htonl(d ? d->ip : 0);
            uint8_t *b = (uint8_t *)&s->local_ip6;
            memset(b, 0, 10);
            b[10] = b[11] = 0xff;
            memcpy(b + 12, &ip, 4);
        }
    }
    return 0;
}

    /* block until ESTABLISHED or RST — event-driven: tcp_input updates tcp_state
     * then wakes connect_waiter under recv_lock, so registering + re-checking
     * under the same lock is atomic against the waker (no lost wakeup, no poll). */
static long connect_tcp_wait(socket_t *s)
{
    uint64_t cf = spin_lock_irq(&s->recv_lock);
    while (s->tcp_state == TCP_SYN_SENT) {
        s->connect_waiter = task_current();
        task_sleep_unlock(&s->recv_lock, cf);
        if (task_has_actionable_signal()) {
            cf = spin_lock_irq(&s->recv_lock);
            s->connect_waiter = NULL;
            spin_unlock_irq(&s->recv_lock, cf);
            return -EINTR;
        }
        cf = spin_lock_irq(&s->recv_lock);
    }
    spin_unlock_irq(&s->recv_lock, cf);
    if (s->tcp_state != TCP_CLOSED) return 0;   /* a quick peer may have closed already */
    long e = s->so_error ? s->so_error : 111;   /* ECONNREFUSED */
    s->so_error = 0;
    return -e;
}

static long sys_connect(int fd, const void *uaddr, uint32_t addrlen)
{
    process_t  *cp = task_current();
    vfs_file_t *cfile = fd_get(cp->fds, fd);
    if (!cfile) return -EBADF;
    if (!cfile->inode || cfile->inode->fops != &sock_fops) return -ENOTSOCK;
    socket_t *s = (socket_t *)cfile->inode->priv;
    int nonblock = (cfile->flags & O_NONBLOCK) ? 1 : 0;

    uint8_t ka[SOCK_ADDR_MAX];
    uint16_t family = 0;
    long ar = sock_addr_in(ka, uaddr, addrlen);
    if (ar) return ar;
    if (addrlen < sizeof(family)) return -EINVAL;
    memcpy(&family, ka, sizeof(family));

    if (s->domain == AF_NETLINK)
        return connect_netlink(s, uaddr, addrlen);
    /* the two branches below store into different halves of socket_t */
    if ((family == AF_UNIX) != (s->domain == AF_UNIX)) return -EAFNOSUPPORT;
    if (family == AF_INET6 && s->domain != AF_INET6) return -EAFNOSUPPORT;

    if (family == AF_UNIX)
        return connect_unix(s, uaddr, addrlen);

    long pr = connect_set_peer(s, ka, addrlen, family);
    if (pr) return pr;
    if (s->type == SOCK_DGRAM) return 0;
    if (s->type != SOCK_STREAM) return -ENOSYS;
    if (!s->dev) return -ENETUNREACH;   /* no network device to reach the peer */
    /* a namespace with only loopback has no way to this peer; the SYN would loop
     * back to nobody and connect would sleep forever */
    if (s->domain == AF_INET && (s->remote_ip >> 24) != 127 &&
        net_route_dev(s->remote_ip) == &current_net_ns()->lo)
        return -ENETUNREACH;
    /* fail fast on an unreachable IPv6 peer (no on-link prefix, no router) so an
     * AF_UNSPEC client falls back to IPv4 instead of blocking on a dropped SYN */
    if (s->domain == AF_INET6) {
        netdev_t *d6 = net_route_dev6(&s->remote_ip6);
        if (!ipv6_reachable(d6, &s->remote_ip6)) {
            /* nonblocking: report the failure through SO_ERROR/poll like Linux,
             * so a client iterating addrinfo (libfetch) moves on to the next
             * address instead of mistaking a stale POLLOUT for success. */
            if (nonblock) { s->so_error = 113; s->connect_inprog = 1; return -EINPROGRESS; }
            return -EHOSTUNREACH;
        }
    }

    int rc = tcp_connect(s);
    if (rc < 0) return rc;

    /* nonblocking connect: the SYN is out — return EINPROGRESS at once. The
     * result surfaces later via poll(POLLOUT) + getsockopt(SO_ERROR), exactly
     * as on Linux; tcp_input records it in so_error when the handshake settles. */
    if (nonblock) { s->connect_inprog = 1; s->so_error = 0; return -EINPROGRESS; }

    return connect_tcp_wait(s);
}

/* send on a unix socket: copy the data in and queue it on the peer. */
static long sendto_unix(socket_t *s, int nonblock, const void *buf, size_t len)
{
    if (len > SKB_DATA_MAX) len = SKB_DATA_MAX;
    uint8_t *kb = kmalloc(len);
    if (!kb) return -ENOMEM;
    if (copy_from_user(kb, buf, len) != 0) { kfree(kb); return -EFAULT; }
    long rc = unix_send_wait(s, kb, len, nonblock);
    kfree(kb);
    return rc;
}

/* a datagram to ::ffff:a.b.c.d from an IPv6 socket on "::" is IPv4 */
static long sendto_v4mapped(socket_t *s, const uint8_t *ka)
{
    sockaddr_in6_t m6;
    uint32_t v4;
    static const in6_addr_t any6;
    memcpy(&m6, ka, sizeof(m6));
    if (m6.sin6_family == AF_INET6 && in6_v4mapped(&m6.sin6_addr, &v4)) {
        if (s->v6only || memcmp(&s->local_ip6, &any6, sizeof(any6)) != 0) return -ENETUNREACH;
        s->domain = AF_INET;
        s->v4mapped = 1;
        s->local_ip = 0;
    }
    return 0;
}

/* ICMPv6 echo and friends: checksum the user's message and send it, waiting
 * up to ~1 s for the neighbour to resolve. */
static long sendto_icmp6(uint8_t *kb, size_t len, in6_addr_t dst6)
{
    long rr;
    if (len > SKB_DATA_MAX) len = SKB_DATA_MAX;   /* raw ICMPv6: single skb */
    netdev_t  *txdev = net_route_dev6(&dst6);
    in6_addr_t src6  = ipv6_src_for(txdev, &dst6);
    icmp6_hdr_t *ih = (icmp6_hdr_t *)kb;
    ih->checksum = 0;
    ih->checksum = ipv6_pseudo_cksum(&src6, &dst6, IP6_NH_ICMPV6,
                                     kb, (uint16_t)len);
    /* time-based resolve: a fresh neighbour's NS→NA round-trip takes real
     * milliseconds (on the wire, not just loopback), so spin net_poll
     * against a wall-clock deadline rather than a fixed iteration count —
     * a tight 64-spin finishes in microseconds, long before the NA. */
    uint64_t deadline = mono_ns() + 1000000000ULL;   /* ~1s, IRQ-independent */
    rr = -113;   /* EHOSTUNREACH if the neighbour never answers */
    do {
        skb_t *skb = skb_alloc();
        if (!skb) { rr = -12; break; }
        memcpy(skb->data, kb, len);
        skb->len = (uint16_t)len;
        int rc = ipv6_tx(txdev, skb, &dst6, IP6_NH_ICMPV6);
        if (rc >= 0) { rr = (long)len; break; }
        if (rc != -1) { rr = rc; break; }
        net_poll();   /* drain RX — the NDP NA may arrive */
    } while (mono_ns() < deadline);
    return rr;
}

/* UDP over IPv6, retried while neighbour discovery runs. */
static long sendto_udp6(socket_t *s, uint8_t *kb, size_t len, in6_addr_t dst6, uint16_t dst_port6)
{
    long rr = -38;
    if (!s->local_port) s->local_port = alloc_port();
    sock_register(s);
    netdev_t *txdev = net_route_dev6(&dst6);
    for (int attempt = 0; attempt < 64; attempt++) {
        int rc = udp6_tx(txdev, &dst6, s->local_port, dst_port6,
                         kb, (uint16_t)len);
        if (rc >= 0) { rr = (long)len; break; }
        if (rc != -1) { rr = rc; break; }
        net_poll();   /* NDP resolution (later phase) */
    }
    return rr;
}

/* IPv6 raw (ping6): kernel fills in the ICMPv6 checksum, then ipv6_tx.
 * ka: kernel copy of the destination (at least SIN6_LEN_RFC2133), or NULL. */
static long sendto_ipv6(socket_t *s, const void *buf, size_t len, const uint8_t *ka)
{
    in6_addr_t dst6;
    uint16_t   dst_port6;
    if (ka) {
        sockaddr_in6_t sa6;
        memcpy(&sa6, ka, sizeof(sa6));
        dst6      = sa6.sin6_addr;
        dst_port6 = ntohs(sa6.sin6_port);
    } else {
        dst6      = s->remote_ip6;
        dst_port6 = s->remote_port;
    }
    /* UDP datagrams up to the IPv6 max (65487) are fragmented by ipv6_tx;
     * the ICMPv6 path below copies into one skb and clamps separately. */
    if (len > 65487) len = 65487;
    uint8_t *kb = kmalloc(len);
    if (!kb) return -ENOMEM;
    if (copy_from_user(kb, buf, len) != 0) { kfree(kb); return -EFAULT; }
    /* fail fast if the destination has no route, rather than spinning the
     * resolve loop and returning a misleading ENOSYS */
    if (!ipv6_reachable(net_route_dev6(&dst6), &dst6)) { kfree(kb); return -EHOSTUNREACH; }

    long rr = -38;
    /* ICMPv6 over SOCK_RAW *or* SOCK_DGRAM (Linux "ping socket": unprivileged
     * ping uses socket(AF_INET6,SOCK_DGRAM,IPPROTO_ICMPV6)). Both carry a
     * user-built ICMPv6 message; the kernel just checksums and ships it. */
    if (s->proto == IPPROTO_ICMPV6 &&
        (s->type == SOCK_RAW || s->type == SOCK_DGRAM) &&
        len >= sizeof(icmp6_hdr_t)) {
        rr = sendto_icmp6(kb, len, dst6);
    } else if (s->type == SOCK_DGRAM) {
        rr = sendto_udp6(s, kb, len, dst6, dst_port6);
    }
    kfree(kb);
    return rr;
}

/* UDP over IPv4, retried for up to ~1 s while ARP resolves the next hop. */
static long sendto_udp4(socket_t *s, uint8_t *kbuf, size_t len, uint32_t dst_ip, uint16_t dst_port)
{
    long r;
    /* an unbound socket needs a source port and a delivery registration, or
     * the peer's reply lands on port 0 and matches nothing */
    if (!s->local_port) s->local_port = alloc_port();
    sock_register(s);
    /* first send to an unresolved peer triggers ARP and is dropped; poll RX
     * for the reply and retry so the datagram actually goes out */
    netdev_t *txdev = net_route_dev(dst_ip);
    /* the first send to an unresolved next-hop triggers ARP and is dropped;
     * wait (time-based, not a microsecond-fast count) for the ARP reply so
     * the datagram actually goes out — a real round-trip takes real ms */
    uint64_t deadline = mono_ns() + 1000000000ULL;   /* ~1s, IRQ-independent */
    int rc = -1;
    do {
        rc = udp_tx(txdev, dst_ip, s->local_port, dst_port, kbuf, (uint16_t)len);
        if (rc != -1) break;
        net_poll();
    } while (mono_ns() < deadline);
    r = rc < 0 ? rc : (long)len;
    return r;
}

/* Raw or ping-socket ICMP over IPv4: checksum and send, waiting for ARP. */
static long sendto_icmp4(socket_t *s, uint8_t *kbuf, size_t len, uint32_t dst_ip)
{
    long r = -38;
    if (len > SKB_DATA_MAX) len = SKB_DATA_MAX;   /* raw ICMP: single skb */
    if (len >= sizeof(icmp_hdr_t)) {
        icmp_hdr_t *ih = (icmp_hdr_t *)kbuf;
        ih->checksum = 0;
        ih->checksum = inet_cksum(kbuf, len);
        /* time-based ARP resolve (first attempt sends the ARP request; a real
         * reply takes real ms — a 64-spin count finishes in microseconds and
         * gives a spurious ENOSYS before the next-hop is ever resolved) */
        uint64_t deadline = mono_ns() + 1000000000ULL;   /* ~1s, IRQ-independent */
        r = -113;   /* EHOSTUNREACH if the next hop never resolves */
        do {
            skb_t *skb = skb_alloc();
            if (!skb) { r = -12; break; }
            memcpy(skb->data, kbuf, len);
            skb->len = (uint16_t)len;
            int rc = ip_tx(s->dev, skb, dst_ip, 1 /* IPPROTO_ICMP */);
            if (rc >= 0) { r = (long)len; break; }
            if (rc != -1) { r = rc; break; }
            net_poll();  /* drain RX — ARP reply may arrive */
        } while (mono_ns() < deadline);
    }
    return r;
}

/* IPv4 datagram or raw send (also an IPv6 socket that went v4-mapped).
 * ka: kernel copy of the destination, addrlen bytes of it, or NULL. */
static long sendto_ipv4(socket_t *s, const void *buf, size_t len, const uint8_t *ka, uint32_t addrlen)
{
    uint32_t dst_ip; uint16_t dst_port;
    uint16_t dfam = 0;
    if (ka && s->v4mapped) memcpy(&dfam, ka, sizeof(dfam));
    if (ka && addrlen < (dfam == AF_INET6 ? SIN6_LEN_RFC2133 : sizeof(sockaddr_in_t))) return -EINVAL;
    if (ka && dfam == AF_INET6) {                       /* ::ffff:a.b.c.d from a mapped socket */
        sockaddr_in6_t sa6;
        memcpy(&sa6, ka, sizeof(sa6));
        if (!in6_v4mapped(&sa6.sin6_addr, &dst_ip)) return -ENETUNREACH;
        dst_port = ntohs(sa6.sin6_port);
    } else if (ka) {
        sockaddr_in_t sa;
        memcpy(&sa, ka, sizeof(sa));
        dst_ip   = ntohl(sa.sin_addr);
        dst_port = ntohs(sa.sin_port);
    } else {
        dst_ip = s->remote_ip; dst_port = s->remote_port;
    }

    /* UDP datagrams up to the IPv4 max (65507) are fragmented by ip_tx; other
     * paths (raw ICMP) still copy into one skb and clamp below. */
    if (len > 65507) len = 65507;
    uint8_t *kbuf = kmalloc(len);
    if (!kbuf) return -ENOMEM;
    if (copy_from_user(kbuf, buf, len) != 0) { kfree(kbuf); return -EFAULT; }

    long r = -38;
    if (s->type == SOCK_DGRAM && s->proto != 1 /* not an ICMP ping socket */) {
        r = sendto_udp4(s, kbuf, len, dst_ip, dst_port);
    } else if ((s->type == SOCK_RAW || s->type == SOCK_DGRAM) && s->proto == 1 /* IPPROTO_ICMP */) {
        r = sendto_icmp4(s, kbuf, len, dst_ip);
    }
    kfree(kbuf);
    return r;
}

static long sock_sendto(socket_t *s, int nonblock, const void *buf, size_t len,
                        const void *uaddr, uint32_t addrlen)
{
    uint8_t ka[SOCK_ADDR_MAX];
    const uint8_t *kaddr = NULL;
    if (uaddr) {
        long e = sock_addr_in(ka, uaddr, addrlen);
        if (e) return e;
        kaddr = ka;
    }
    if (s->domain == AF_NETLINK) {
        k_iovec v = { (void *)buf, len };
        return nl_sendv(s, &v, 1, uaddr, addrlen);
    }
    /* AF_UNIX send()/sendto() delivers through the peer's queue, not the IP
     * stack. Without this an AF_UNIX datagram socket (s->dev is set like any
     * socket) falls into the udp_tx path below and is shipped as a bogus UDP
     * frame — exactly what a libc syslog write to /dev/log does. */
    if (s->domain == AF_UNIX) {
        return sendto_unix(s, nonblock, buf, len);
    }
    if (s->type == SOCK_STREAM)   /* the connected remote; a dst in uaddr means nothing */
        return tcp_stream_tx(s, buf, len, nonblock, 0);
    if (!s->dev) return -EBADF;

    if (s->domain == AF_INET6 && kaddr && addrlen < SIN6_LEN_RFC2133) return -EINVAL;
    if (s->domain == AF_INET6 && s->type == SOCK_DGRAM && s->proto != IPPROTO_ICMPV6 && kaddr) {
        long e = sendto_v4mapped(s, kaddr);
        if (e) return e;
    }
    if (s->domain == AF_INET6) {
        return sendto_ipv6(s, buf, len, kaddr);
    }

    return sendto_ipv4(s, buf, len, kaddr, addrlen);
}

static long sys_sendto(int fd, const void *buf, size_t len, int flags,
                       const void *uaddr, uint32_t addrlen)
{
    socket_t *s = fd_to_sock(fd);
    if (!s) return sock_fd_err(fd);
    /* the netlink path copies through the kernel-or-user fop helpers */
    if (!access_ok(buf, len)) return -EFAULT;
    return sock_sendto(s, sock_nonblocking(fd, flags), buf, len, uaddr, addrlen);
}

/* klen: kernel copy of the caller's name length, NULL when no name is wanted */
/* recv on a netlink socket; the sender is always the kernel. */
static long recvfrom_netlink(socket_t *s, void *buf, size_t len, int flags, int nonblock,
                             void *uaddr, uint32_t *klen)
{
    k_iovec v = { buf, len };
    int mflags;
    long r = netlink_recv(s, &v, 1, flags, nonblock, &mflags);
    if (r >= 0 && uaddr && klen) {
        sockaddr_nl_t kernel = { .nl_family = AF_NETLINK };
        if (sock_name_put(uaddr, &kernel, sizeof(kernel), klen) != 0) return -EFAULT;
    }
    return r;
}

/* Stream recv: the sender's address, written once per call. */
static void recv_stream_name(socket_t *s, skb_t *skb, void *uaddr, uint32_t *klen)
{
    if (s->v4mapped) {
        sockaddr_in6_t sa6; memset(&sa6, 0, sizeof(sa6));
        sa6.sin6_family = AF_INET6; in6_set_v4mapped(&sa6.sin6_addr, skb->src_ip);
        sa6.sin6_port = htons(skb->src_port);
        sock_name_put(uaddr, &sa6, sizeof(sa6), klen);
    } else if (s->domain == AF_INET6) {
        sockaddr_in6_t sa6; memset(&sa6, 0, sizeof(sa6));
        sa6.sin6_family = AF_INET6; sa6.sin6_addr = skb->src_ip6;
        sa6.sin6_port = htons(s->remote_port);
        sock_name_put(uaddr, &sa6, sizeof(sa6), klen);
    } else {
        sockaddr_in_t sa; memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET; sa.sin_addr = htonl(skb->src_ip);
        sa.sin_port = htons(skb->src_port);
        sock_name_put(uaddr, &sa, sizeof(sa), klen);
    }
}

/* SOCK_STREAM has no message boundaries: one recv drains as many queued
 * bytes as fit in buf, coalescing across writes. Applies
 * to TCP and unix stream alike. DGRAM/RAW preserve per-datagram boundaries
 * (handled below). */
static long recvfrom_stream(socket_t *s, void *buf, size_t len, int nonblock,
                            void *uaddr, uint32_t *klen, uint64_t rcv_deadline)
{
    while (1) {
        size_t total = 0;
        int got_addr = 0;
        while (total < len) {
            uint64_t f = spin_lock_irq(&s->recv_lock);
            skb_t *skb = s->recv_head;
            if (!skb) { spin_unlock_irq(&s->recv_lock, f); break; }
            s->recv_head = skb->next;
            if (!s->recv_head) s->recv_tail = NULL;
            spin_unlock_irq(&s->recv_lock, f);

            size_t n = skb->len < (len - total) ? skb->len : (len - total);
            if (n && copy_to_user((char *)buf + total, skb->data, n) != 0) {
                skb_free(skb);
                return total ? (long)total : -14;
            }
            if (uaddr && klen && !got_addr) {
                recv_stream_name(s, skb, uaddr, klen);
                got_addr = 1;
            }
            total += n;
            if (n < skb->len) {                     /* buf full: requeue remainder */
                skb->data += n; skb->len -= (uint16_t)n;
                uint64_t f2 = spin_lock_irq(&s->recv_lock);
                skb->next = s->recv_head; s->recv_head = skb;
                if (!s->recv_tail) s->recv_tail = skb;
                spin_unlock_irq(&s->recv_lock, f2);
                break;
            }
            skb_free(skb);
        }
        if (total > 0) {
            tcp_rx_consumed(s, (uint32_t)total);
            return (long)total;
        }
        uint64_t f = spin_lock_irq(&s->recv_lock);
        if (s->recv_head) { spin_unlock_irq(&s->recv_lock, f); continue; }
        if (sock_at_eof(s)) { spin_unlock_irq(&s->recv_lock, f); return sock_eof_rc(s); }
        if (nonblock) { spin_unlock_irq(&s->recv_lock, f); return -EAGAIN; }
        if (rcv_deadline && kernel_ticks >= rcv_deadline) {
            spin_unlock_irq(&s->recv_lock, f); return -EAGAIN;   /* SO_RCVTIMEO */
        }
        s->recv_waiter = task_current();
        if (rcv_deadline) { task_current()->wake_deadline = rcv_deadline; sched_note_wake_deadline(rcv_deadline); }
        task_sleep_unlock(&s->recv_lock, f);
        task_current()->wake_deadline = 0;
        if (task_has_actionable_signal()) { s->recv_waiter = NULL; return -EINTR; }
    }
}

/* Datagram recv: the sender's address, from the packet itself. */
static void recv_dgram_name(socket_t *s, skb_t *skb, void *uaddr, uint32_t *klen)
{
    if (!klen) {
        /* no name wanted */
    } else if (uaddr && s->v4mapped) {
        sockaddr_in6_t sa6;
        memset(&sa6, 0, sizeof(sa6));
        sa6.sin6_family = AF_INET6;
        in6_set_v4mapped(&sa6.sin6_addr, skb->src_ip);
        sa6.sin6_port   = htons(skb->src_port);
        sock_name_put(uaddr, &sa6, sizeof(sa6), klen);
    } else if (uaddr && s->domain == AF_INET6) {
        sockaddr_in6_t sa6;
        memset(&sa6, 0, sizeof(sa6));
        sa6.sin6_family = AF_INET6;
        sa6.sin6_addr   = skb->src_ip6;
        sa6.sin6_port   = htons(s->remote_port);
        sock_name_put(uaddr, &sa6, sizeof(sa6), klen);
    } else if (uaddr) {
        sockaddr_in_t sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        if (s->type == SOCK_RAW && skb->len >= 20) {
            /* extract src IP from the prepended IP header (network byte order) */
            uint32_t src_net;
            memcpy(&src_net, skb->data + 12, 4);
            sa.sin_addr = src_net;
            sa.sin_port = htons(s->remote_port);
        } else {
            /* the datagram's real origin, not the socket's remote — an
             * unconnected socket has none, and senders verify the source */
            sa.sin_addr = htonl(skb->src_ip);
            sa.sin_port = htons(skb->src_port);
        }
        sock_name_put(uaddr, &sa, sizeof(sa), klen);
    }
}

/* Datagram and raw recv: one queued packet per call, its boundary kept.
 * Never a SOCK_STREAM socket: do_recvfrom sends those to recvfrom_stream. */
static long recvfrom_dgram(socket_t *s, void *buf, size_t len, int nonblock,
                           void *uaddr, uint32_t *klen, uint64_t rcv_deadline)
{
    while (1) {
        uint64_t f = spin_lock_irq(&s->recv_lock);
        skb_t *skb = s->recv_head;
        if (skb) {
            s->recv_head = skb->next;
            if (!s->recv_head) s->recv_tail = NULL;
            spin_unlock_irq(&s->recv_lock, f);
            size_t n = skb->len < len ? skb->len : len;
            if (n && copy_to_user(buf, skb->data, n) != 0) {
                skb_free(skb);
                return -EFAULT;
            }
            recv_dgram_name(s, skb, uaddr, klen);
            skb_free(skb);   /* a datagram: what did not fit is dropped */
            return (long)n;
        }
        /* a seqpacket pair whose peer is gone reads end of file, as on Linux.
         * Rust's Command::spawn waits on such a pair for exec to close it; without
         * this VS Code's proxy resolver (gsettings monitor) hung the main process. */
        if (s->domain == AF_UNIX && s->unix_seqpacket && sock_at_eof(s)) {
            spin_unlock_irq(&s->recv_lock, f); return 0;
        }
        if (nonblock) { spin_unlock_irq(&s->recv_lock, f); return -EAGAIN; }
        if (rcv_deadline && kernel_ticks >= rcv_deadline) {
            spin_unlock_irq(&s->recv_lock, f); return -EAGAIN;   /* SO_RCVTIMEO */
        }
        s->recv_waiter = task_current();
        if (rcv_deadline) { task_current()->wake_deadline = rcv_deadline; sched_note_wake_deadline(rcv_deadline); }
        task_sleep_unlock(&s->recv_lock, f);
        task_current()->wake_deadline = 0;
        if (task_has_actionable_signal()) { s->recv_waiter = NULL; return -EINTR; }
    }
}

static long do_recvfrom(int fd, void *buf, size_t len, int flags, void *uaddr, uint32_t *klen)
{
    int nonblock = sock_nonblocking(fd, flags);
    socket_t *s = fd_to_sock(fd);
    if (!s) return sock_fd_err(fd);
    if (!access_ok(buf, len)) return -EFAULT;

    if (s->domain == AF_NETLINK) {
        return recvfrom_netlink(s, buf, len, flags, nonblock, uaddr, klen);
    }

    uint64_t rcv_deadline = s->rcv_timeo ? kernel_ticks + s->rcv_timeo : 0;

    if (s->type == SOCK_STREAM) {
        return recvfrom_stream(s, buf, len, nonblock, uaddr, klen, rcv_deadline);
    }

    return recvfrom_dgram(s, buf, len, nonblock, uaddr, klen, rcv_deadline);
}

static long sys_recvfrom(int fd, void *buf, size_t len, int flags,
                         void *uaddr, uint32_t *addrlen)
{
    if (!uaddr || !addrlen) return do_recvfrom(fd, buf, len, flags, NULL, NULL);
    uint32_t l;
    if (copy_from_user(&l, addrlen, sizeof(l)) != 0) return -EFAULT;
    if ((int)l < 0) return -EINVAL;
    long r = do_recvfrom(fd, buf, len, flags, uaddr, &l);
    if (r >= 0 && copy_to_user(addrlen, &l, sizeof(l)) != 0) return -EFAULT;
    return r;
}

/* sendmsg/recvmsg — vectored send/receive + SCM_RIGHTS fd-passing over AF_UNIX.
 * Plain data delegates to the tested sendto/recvfrom/read/write paths. */
typedef struct {
    void     *msg_name;
    uint32_t  msg_namelen;
    uint32_t  _pad;
    k_iovec  *msg_iov;
    uint64_t  msg_iovlen;
    void     *msg_control;
    uint64_t  msg_controllen;
    int       msg_flags;
} k_msghdr;

typedef struct { uint64_t cmsg_len; int cmsg_level; int cmsg_type; } k_cmsghdr;
#define SOL_SOCKET_  1
#define SCM_RIGHTS_  1
#define MSG_CTRUNC_  8

static long sys_sendmsg(int fd, const void *umsg, int flags)
{
    k_msghdr m;
    if (copy_from_user(&m, umsg, sizeof(m)) != 0) return -EFAULT;
    if (m.msg_iovlen == 0) return 0;
    /* Linux: a name of length 0 is no name, a longer one is cut to the maximum */
    if (m.msg_namelen == 0) m.msg_name = NULL;
    if (m.msg_namelen > SOCK_ADDR_MAX) m.msg_namelen = SOCK_ADDR_MAX;
    if (m.msg_iovlen > 64) {
        /* A stream may take part of it: the caller sends the rest. Firefox's IPC
         * gathers long messages from hundreds of pieces. */
        socket_t *t = fd_to_sock(fd);
        if (m.msg_iovlen > 1024 || !t || t->type != SOCK_STREAM) return -EINVAL;
        m.msg_iovlen = 64;
    }
    k_iovec iov[64];
    if (copy_from_user(iov, m.msg_iov, (size_t)m.msg_iovlen * sizeof(k_iovec)) != 0) return -EFAULT;
    /* before any path below: the netlink one copies through the kernel-or-user
     * fop helpers, which would read a kernel address as kernel memory */
    for (uint64_t i = 0; i < m.msg_iovlen; i++)
        if (!access_ok(iov[i].base, (size_t)iov[i].len)) return -EFAULT;

    socket_t *ns = fd_to_sock(fd);
    if (ns && ns->domain == AF_NETLINK)
        return nl_sendv(ns, iov, m.msg_iovlen, m.msg_name, m.msg_namelen);

    /* SCM_RIGHTS fd-passing — AF_UNIX only: collect the sender's files, hold an
     * in-flight reference on each, and deliver them with the data in one skb. */
    if (m.msg_control && m.msg_controllen >= sizeof(k_cmsghdr)) {
        socket_t *s = fd_to_sock(fd);
        if (s && s->domain == AF_UNIX) {
            uint8_t cb[sizeof(k_cmsghdr) + SKB_SCM_MAX * sizeof(int)];
            size_t clen = m.msg_controllen < sizeof(cb) ? m.msg_controllen : sizeof(cb);
            if (copy_from_user(cb, m.msg_control, clen) != 0) return -EFAULT;
            k_cmsghdr *ch = (k_cmsghdr *)cb;
            if (m.msg_controllen > sizeof(cb)) {
                kprintf(KERN_WARN "sendmsg: control %lu bytes too long\n", (unsigned long)m.msg_controllen);
                return -EINVAL;
            }
            /* the rights may follow other messages (SCM_CREDENTIALS): find them */
            for (size_t co = 0; co + sizeof(k_cmsghdr) <= clen; ) {
                k_cmsghdr *c = (k_cmsghdr *)(cb + co);
                if (c->cmsg_len < sizeof(k_cmsghdr) || c->cmsg_len > clen - co) break;
                if (c->cmsg_level == SOL_SOCKET_ && c->cmsg_type == SCM_RIGHTS_) { ch = c; break; }
                co += (c->cmsg_len + 7) & ~(size_t)7;
            }
            if (ch->cmsg_level == SOL_SOCKET_ && ch->cmsg_type == SCM_RIGHTS_ &&
                ch->cmsg_len >= sizeof(k_cmsghdr) && (uint8_t *)ch + ch->cmsg_len <= cb + clen) {
                int nfd = (int)((ch->cmsg_len - sizeof(k_cmsghdr)) / sizeof(int));
                if (nfd > SKB_SCM_MAX) {                 /* never lose them quietly */
                    kprintf(KERN_WARN "sendmsg: %d descriptors, limit %d\n", nfd, SKB_SCM_MAX);
                    return -EINVAL;
                }
                int *cfds = (int *)((uint8_t *)ch + sizeof(k_cmsghdr));
                process_t *p = task_current();
                void *files[SKB_SCM_MAX]; int nf = 0;
                for (int i = 0; i < nfd; i++) {
                    vfs_file_t *vf = fget(p->fds, cfds[i]);   /* atomic in-flight ref under the table lock */
                    if (!vf) { for (int j = 0; j < nf; j++) vfs_file_put(files[j]); return -EBADF; }
                    files[nf++] = vf;
                }
                /* a stream takes the first skb's worth now, the caller sends the
                 * rest; a datagram goes whole, with its files */
                size_t want = 0;
                for (uint64_t i = 0; i < m.msg_iovlen; i++) want += (size_t)iov[i].len;
                size_t dcap = s->type == SOCK_STREAM ? SKB_DATA_MAX : UNIX_DGRAM_MAX;
                if (s->type != SOCK_STREAM && want > dcap) {
                    for (int j = 0; j < nf; j++) vfs_file_put(files[j]); return -EMSGSIZE;
                }
                if (want > dcap) want = dcap;
                uint8_t *dbuf = kmalloc(want ? want : 1); size_t dl = 0;
                if (!dbuf) { for (int j = 0; j < nf; j++) vfs_file_put(files[j]); return -ENOMEM; }
                for (uint64_t i = 0; i < m.msg_iovlen && dl < want; i++) {
                    size_t c = iov[i].len; if (c > want - dl) c = want - dl;
                    if (c && copy_from_user(dbuf + dl, iov[i].base, c) != 0) {
                        kfree(dbuf);
                        for (int j = 0; j < nf; j++) vfs_file_put(files[j]); return -EFAULT;
                    }
                    dl += c;
                }
                int r = unix_send_scm(s, dbuf, (uint16_t)dl, files, nf);
                kfree(dbuf);
                if (r < 0) { for (int j = 0; j < nf; j++) vfs_file_put(files[j]); return r; }
                return r;
            }
        }
    }

    /* an AF_UNIX datagram/seqpacket is one message however many pieces it
     * comes in: gather them, rather than one message per piece */
    if (ns && ns->domain == AF_UNIX && ns->type != SOCK_STREAM && m.msg_iovlen > 1 && !m.msg_name) {
        size_t want = 0;
        for (uint64_t i = 0; i < m.msg_iovlen; i++) want += (size_t)iov[i].len;
        if (want > UNIX_DGRAM_MAX) return -EMSGSIZE;
        uint8_t *kb = kmalloc(want ? want : 1);
        if (!kb) return -ENOMEM;
        size_t dl = 0;
        for (uint64_t i = 0; i < m.msg_iovlen; i++) {
            if (iov[i].len && copy_from_user(kb + dl, iov[i].base, (size_t)iov[i].len) != 0) { kfree(kb); return -EFAULT; }
            dl += (size_t)iov[i].len;
        }
        vfs_file_t *sf = fd_get(task_current()->fds, fd);
        long r = unix_send_wait(ns, kb, dl, sock_nonblocking(fd, flags) || (sf && (sf->flags & O_NONBLOCK)));
        kfree(kb);
        return r;
    }

    long total = 0;
    for (uint64_t i = 0; i < m.msg_iovlen; i++) {
        if (iov[i].len == 0) continue;
        long r = ns
               ? sys_sendto(fd, iov[i].base, (size_t)iov[i].len, flags, m.msg_name, m.msg_namelen)
               : vfs_write(fd, iov[i].base, (size_t)iov[i].len);
        if (r < 0) return total ? total : r;
        total += r;
        if ((uint64_t)r < iov[i].len) break;   /* short write: stop */
    }
    return total;
}

/* an empty iovec is how MSG_PEEK|MSG_TRUNC callers size the next datagram */
static long recvmsg_netlink(socket_t *s, int fd, int flags, void *umsg, k_msghdr m, k_iovec *iov)
{
    int mflags = 0;
    long r = netlink_recv(s, iov, m.msg_iovlen, flags, sock_nonblocking(fd, flags), &mflags);
    if (r < 0) return r;
    if (m.msg_name) {
        sockaddr_nl_t kernel = { .nl_family = AF_NETLINK };
        uint32_t have = m.msg_namelen < sizeof(kernel) ? m.msg_namelen : sizeof(kernel);
        uint32_t full = sizeof(kernel);
        copy_to_user(m.msg_name, &kernel, have);
        copy_to_user((char *)umsg + 8, &full, sizeof(full)); /* msg_namelen */
    }
    copy_to_user((char *)umsg + 48, &mflags, sizeof(mflags));
    return r;
}

/* Unix recvmsg control data: SCM_CREDENTIALS, then the passed files as new
 * fds (SCM_RIGHTS), then the control length and truncation flags. */
static void recvmsg_unix_cmsg(socket_t *s, void *umsg, k_msghdr m,
                              int have_cred, int32_t cr_pid, uint32_t cr_uid, uint32_t cr_gid,
                              void **cfiles, int ncf, int truncated, int msg_trunc)
{
    /* SCM_CREDENTIALS first, as Linux orders them: the sender's pid, uid, gid,
     * when this socket asked with SO_PASSCRED */
    uint64_t cused = 0;
    if (s->so_passcred && have_cred && m.msg_control) {
        struct { k_cmsghdr h; int32_t pid; uint32_t uid, gid; uint32_t pad; } cc;
        memset(&cc, 0, sizeof(cc));
        cc.h.cmsg_len = sizeof(k_cmsghdr) + 12;
        cc.h.cmsg_level = SOL_SOCKET_; cc.h.cmsg_type = 2 /* SCM_CREDENTIALS */;
        cc.pid = cr_pid; cc.uid = cr_uid; cc.gid = cr_gid;
        if (m.msg_controllen >= sizeof(cc)) {
            if (copy_to_user(m.msg_control, &cc, sizeof(cc)) == 0) cused = sizeof(cc);
        } else {
            truncated = 1;
        }
    }
    if (ncf > 0) {                                       /* install fds + build cmsg */
        process_t *p = task_current();
        int maxfds = (m.msg_control && m.msg_controllen >= cused + sizeof(k_cmsghdr))
                   ? (int)((m.msg_controllen - cused - sizeof(k_cmsghdr)) / sizeof(int)) : 0;
        int newfds[SKB_SCM_MAX]; int nf = 0;
        for (int i = 0; i < ncf; i++) {
            vfs_file_t *vf = (vfs_file_t *)cfiles[i];
            if (p->asess && !asess_file_allowed(vf)) { vfs_file_put(vf); truncated = 1; }
            else if (nf < maxfds) { int nfd = fd_alloc(p->fds, vf); if (nfd >= 0) newfds[nf++] = nfd; vfs_file_put(vf); }
            else { vfs_file_put(vf); truncated = 1; }
        }
        if (nf > 0) {
            uint8_t cb[sizeof(k_cmsghdr) + SKB_SCM_MAX * sizeof(int)];
            k_cmsghdr *ch = (k_cmsghdr *)cb;
            ch->cmsg_len = sizeof(k_cmsghdr) + (uint64_t)nf * sizeof(int);
            ch->cmsg_level = SOL_SOCKET_; ch->cmsg_type = SCM_RIGHTS_;
            memcpy(cb + sizeof(k_cmsghdr), newfds, (size_t)nf * sizeof(int));
            copy_to_user((char *)m.msg_control + cused, cb, ch->cmsg_len);
            cused += ch->cmsg_len;
        }
    }
    if (cused) copy_to_user((char *)umsg + 40, &cused, sizeof(cused));
    if (truncated || msg_trunc) {
        int fl = (truncated ? MSG_CTRUNC_ : 0) | (msg_trunc ? 0x20 /* MSG_TRUNC */ : 0);
        copy_to_user((char *)umsg + 48, &fl, sizeof(fl));
    }
}

/* AF_UNIX: dequeue ourselves so we can coalesce stream data AND pick up any
 * passed files (SCM_RIGHTS) the consumed skbs carry. */
static long recvmsg_unix(socket_t *s, int fd, int flags, void *umsg, k_msghdr m, k_iovec *iov)
{
    int nonblock = sock_nonblocking(fd, flags);
    size_t cap = 0;
    for (uint64_t i = 0; i < m.msg_iovlen; i++) cap += (size_t)iov[i].len;
    size_t total = 0; uint64_t iidx = 0; size_t ioff = 0;
    void *cfiles[SKB_SCM_MAX]; int ncf = 0, truncated = 0, fault = 0;
    int have_cred = 0; int32_t cr_pid = 0; uint32_t cr_uid = 0, cr_gid = 0;
    int msg_trunc = 0;

    while (total < cap) {
        uint64_t f = spin_lock_irq(&s->recv_lock);
        skb_t *skb = s->recv_head;
        if (!skb) {
            spin_unlock_irq(&s->recv_lock, f);
            if (total > 0) break;                       /* return what we have */
            if (sock_at_eof(s)) return 0;               /* EOF */
            if (nonblock) return -EAGAIN;
            uint64_t f2 = spin_lock_irq(&s->recv_lock);
            if (s->recv_head) { spin_unlock_irq(&s->recv_lock, f2); continue; }
            if (sock_at_eof(s)) { spin_unlock_irq(&s->recv_lock, f2); return 0; }
            s->recv_waiter = task_current();
            task_sleep_unlock(&s->recv_lock, f2);
            if (task_has_actionable_signal()) { s->recv_waiter = NULL; return -EINTR; }
            continue;
        }
        /* Passed files stay with their own bytes: one read never spans them,
         * or a caller sized for one message would lose the next one's. */
        if (skb->scm_nfiles && total > 0) {
            spin_unlock_irq(&s->recv_lock, f);
            break;
        }
        s->recv_head = skb->next;
        if (!s->recv_head) s->recv_tail = NULL;
        spin_unlock_irq(&s->recv_lock, f);
        if (!have_cred) {
            have_cred = 1;
            cr_pid = skb->cred_pid; cr_uid = skb->cred_uid; cr_gid = skb->cred_gid;
        }

        size_t off = 0;                                  /* scatter into iovecs */
        while (off < skb->len && total < cap) {
            while (iidx < m.msg_iovlen && iov[iidx].len == 0) iidx++;
            if (iidx >= m.msg_iovlen) break;
            size_t room = (size_t)iov[iidx].len - ioff;
            size_t c = (skb->len - off < room) ? (size_t)(skb->len - off) : room;
            if (copy_to_user((char *)iov[iidx].base + ioff, skb->data + off, c) != 0) {
                fault = 1;
                break;
            }
            off += c; total += c; ioff += c;
            if (ioff == iov[iidx].len) { iidx++; ioff = 0; }
        }
        for (int j = 0; j < skb->scm_nfiles; j++) {      /* collect fds once */
            if (ncf < SKB_SCM_MAX) cfiles[ncf++] = skb->scm_files[j];
            else { vfs_file_put((vfs_file_t *)skb->scm_files[j]); truncated = 1; }
        }
        skb->scm_nfiles = 0;
        if (off < skb->len && s->type != SOCK_STREAM) {  /* a datagram: the rest is lost */
            msg_trunc = 1;
            skb_free(skb);
            break;
        }
        if (off < skb->len) {                            /* iov full mid-skb: requeue */
            skb->data += off; skb->len -= (uint16_t)off;
            uint64_t f2 = spin_lock_irq(&s->recv_lock);
            skb->next = s->recv_head; s->recv_head = skb;
            if (!s->recv_tail) s->recv_tail = skb;
            spin_unlock_irq(&s->recv_lock, f2);
            break;
        }
        skb_free(skb);
        if (ncf > 0 || s->type != SOCK_STREAM)            /* one message per call */
            break;
    }

    recvmsg_unix_cmsg(s, umsg, m, have_cred, cr_pid, cr_uid, cr_gid,
                      cfiles, ncf, truncated, msg_trunc);
    return (fault && !total) ? -14 : (long)total;
}

static long sys_recvmsg(int fd, void *umsg, int flags)
{
    k_msghdr m;
    if (copy_from_user(&m, umsg, sizeof(m)) != 0) return -EFAULT;
    uint64_t zero64 = 0; int zero32 = 0;
    if (copy_to_user((char *)umsg + 40, &zero64, sizeof(zero64)) != 0) return -EFAULT; /* msg_controllen */
    if (copy_to_user((char *)umsg + 48, &zero32, sizeof(zero32)) != 0) return -EFAULT; /* msg_flags */
    if (m.msg_iovlen > 64) {
        socket_t *t = fd_to_sock(fd);
        if (m.msg_iovlen > 1024 || !t || t->type != SOCK_STREAM) return -EINVAL;
        m.msg_iovlen = 64;
    }
    k_iovec iov[64];
    if (m.msg_iovlen &&
        copy_from_user(iov, m.msg_iov, (size_t)m.msg_iovlen * sizeof(k_iovec)) != 0) return -EFAULT;
    /* netlink_recv writes through the kernel-or-user fop helper: a kernel address
     * here would be a write into the kernel */
    for (uint64_t i = 0; i < m.msg_iovlen; i++)
        if (!access_ok(iov[i].base, (size_t)iov[i].len)) return -EFAULT;

    socket_t *s = fd_to_sock(fd);

    if (s && s->domain == AF_NETLINK) {
        return recvmsg_netlink(s, fd, flags, umsg, m, iov);
    }
    if (m.msg_iovlen == 0) return 0;

    if (s && s->domain == AF_UNIX) {
        return recvmsg_unix(s, fd, flags, umsg, m, iov);
    }

    /* non-unix: plain data path */
    for (uint64_t i = 0; i < m.msg_iovlen; i++) {
        if (iov[i].len == 0) continue;
        uint32_t nl = m.msg_namelen;
        long r = do_recvfrom(fd, iov[i].base, (size_t)iov[i].len, flags, m.msg_name, m.msg_name ? &nl : (uint32_t *)0);
        if (r >= 0 && m.msg_name) copy_to_user((char *)umsg + 8, &nl, sizeof(nl));
        return r;
    }
    return 0;
}

static long sock_name6(const in6_addr_t *ip, uint16_t port, void *uaddr, uint32_t *ulen)
{
    sockaddr_in6_t sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin6_family = AF_INET6;
    sa.sin6_port   = htons(port);
    sa.sin6_addr   = *ip;
    return sock_name_put_u(uaddr, &sa, sizeof(sa), ulen);
}

/* AF_UNIX: the family, then as much of the name as was bound */
static long sock_name_unix(const socket_t *s, void *uaddr, uint32_t *ulen)
{
    sockaddr_un_t su;
    memset(&su, 0, sizeof(su));
    su.sun_family = AF_UNIX;
    uint32_t pl = s ? (uint32_t)s->unix_path_len : 0;
    if (pl > sizeof(su.sun_path)) pl = sizeof(su.sun_path);
    if (pl) memcpy(su.sun_path, s->unix_path, pl);
    return sock_name_put_u(uaddr, &su, (uint32_t)sizeof(su.sun_family) + pl, ulen);
}

static long sys_getsockname(int fd, void *uaddr, uint32_t *uaddrlen)
{
    socket_t *s = fd_to_sock(fd);
    if (!s) return sock_fd_err(fd);
    if (s->domain == AF_NETLINK) return nl_name_to_user(s, uaddr, uaddrlen, 0);
    if (s->domain == AF_UNIX) return sock_name_unix(s, uaddr, uaddrlen);
    if (s->domain == AF_INET6) return sock_name6(&s->local_ip6, s->local_port, uaddr, uaddrlen);
    if (s->v4mapped) {
        in6_addr_t a;
        uint32_t ip = s->local_ip ? s->local_ip : (s->remote_ip && s->dev ? s->dev->ip : 0);
        if ((s->remote_ip >> 24) == 127 && !s->local_ip) ip = 0x7f000001;
        if (ip) in6_set_v4mapped(&a, ip); else memset(&a, 0, sizeof(a));
        return sock_name6(&a, s->local_port, uaddr, uaddrlen);
    }
    sockaddr_in_t sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(s->local_port);
    sa.sin_addr   = htonl(s->local_ip ? s->local_ip : (s->dev ? s->dev->ip : 0));
    return sock_name_put_u(uaddr, &sa, sizeof(sa), uaddrlen);
}

static long sys_getpeername(int fd, void *uaddr, uint32_t *addrlen)
{
    socket_t *s = fd_to_sock(fd);
    if (!s) return sock_fd_err(fd);
    if (s->domain == AF_NETLINK) return nl_name_to_user(s, uaddr, addrlen, 1);
    if (s->domain == AF_UNIX) {
        if (!s->unix_peer) return -ENOTCONN;
        return sock_name_unix(s->unix_peer, uaddr, addrlen);
    }
    if (s->domain == AF_INET6) return sock_name6(&s->remote_ip6, s->remote_port, uaddr, addrlen);
    if (s->v4mapped) {
        if (!s->remote_port) return -ENOTCONN;
        in6_addr_t a;
        in6_set_v4mapped(&a, s->remote_ip);
        return sock_name6(&a, s->remote_port, uaddr, addrlen);
    }
    sockaddr_in_t sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(s->remote_port);
    sa.sin_addr   = htonl(s->remote_ip);
    return sock_name_put_u(uaddr, &sa, sizeof(sa), addrlen);
}

#define SO_SOL_SOCKET   1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_REUSEPORT   15
#define SO_RCVTIMEO    20
#define SO_SNDTIMEO    21
#define SO_ACCEPTCONN  30
#define SO_IPPROTO_TCP  6
#define SO_TCP_NODELAY  1
#define SO_TCP_INFO    11
/* options accepted as no-ops: settable but with no behavioral effect here.
 * Returning success (not ENOPROTOOPT) matches Linux for these known opts. */
#define SO_DONTROUTE    5
#define SO_BROADCAST    6
#define SO_LINGER      13
#define SO_PASSCRED    16
#define SO_PRIORITY    12
#define SO_BSDCOMPAT   14
#define SO_OOBINLINE   10

/* Ranix-native firewall control: a setsockopt level whose option value is one
 * filter command line ("drop tcp dport 9999", "flush"); getsockopt renders the
 * current ruleset. Any socket fd works — the ruleset is global, not per-socket. */
#define SOL_RANIX_FILTER 0x52414e58   /* 'RANX' */
#define NF_RULE          1

struct k_timeval { int64_t tv_sec; int64_t tv_usec; };

static uint32_t tv_to_ticks(const struct k_timeval *tv)
{
	return (uint32_t)((uint64_t)tv->tv_sec * TIMER_HZ +
	                  (uint64_t)tv->tv_usec * TIMER_HZ / 1000000);
}

static long sys_setsockopt(int fd, int lv, int opt, const void *val, uint32_t len)
{
	socket_t *s = fd_to_sock(fd);
	if (!s) return sock_fd_err(fd); /* EBADF */

	if (lv == SOL_RANIX_FILTER && opt == NF_RULE) {
		if (!capable_task(task_current(), CAP_NET_ADMIN)) return -EPERM;
		char line[160];
		if (!val || len == 0 || len >= sizeof(line)) return -EINVAL;
		if (copy_from_user(line, val, len) != 0) return -EFAULT;
		return nf_ctl_set(line, len);
	}

	int iv = 0;
	if (val && len >= sizeof(int)) {
		if (copy_from_user(&iv, val, sizeof(int)) != 0) return -EFAULT;
	}

	if (lv == SO_SOL_SOCKET) {
		switch (opt) {
		case SO_REUSEADDR:  s->so_reuseaddr = iv ? 1 : 0; return 0;
		case SO_REUSEPORT:  s->so_reuseport = iv ? 1 : 0; return 0;
		case SO_KEEPALIVE:  s->so_keepalive = iv ? 1 : 0; return 0;
		case SO_RCVBUF:     s->so_rcvbuf = (uint32_t)(iv < 0 ? 0 : iv); return 0;
		case SO_SNDBUF:     s->so_sndbuf = (uint32_t)(iv < 0 ? 0 : iv); return 0;
		case SO_RCVTIMEO:
		case SO_SNDTIMEO: {
			struct k_timeval tv = {0, 0};
			if (val && len >= sizeof(tv) && copy_from_user(&tv, val, sizeof(tv)) != 0)
				return -EFAULT;
			uint32_t t = tv_to_ticks(&tv);
			if (opt == SO_RCVTIMEO) s->rcv_timeo = t; else s->snd_timeo = t;
			return 0;
		}
		case SO_ERROR: case SO_TYPE: case SO_ACCEPTCONN:
			return -ENOPROTOOPT;   /* read-only */
		case SO_PASSCRED:   s->so_passcred = iv ? 1 : 0; return 0;
		case SO_DONTROUTE: case SO_BROADCAST: case SO_LINGER:
		case SO_PRIORITY:  case SO_BSDCOMPAT:
		case SO_OOBINLINE:
			return 0;     /* known but no behavioral effect here */
		default:
			return -ENOPROTOOPT;
		}
	} else if (lv == SO_IPPROTO_TCP) {
		if (opt == SO_TCP_NODELAY) { s->tcp_nodelay = iv ? 1 : 0; return 0; }
		/* TCP_KEEPIDLE/KEEPINTVL/KEEPCNT: keepalive timing, accepted (Chromium
		 * sets them on every connection and logs ENOPROTOOPT) */
		if (opt >= 4 && opt <= 6) return iv > 0 ? 0 : -22;
		return -ENOPROTOOPT;
	}
	if (lv == 41 /* IPPROTO_IPV6 */ && opt == 26 /* IPV6_V6ONLY */) {
		if (s->domain != AF_INET6) return -EINVAL;
		s->v6only = iv ? 1 : 0;
		return 0;
	}
	/* IPPROTO_IP / IPPROTO_IPV6 and the like: accept known multicast/ttl knobs
	 * as no-ops, reject the genuinely unknown. */
	return 0;
}

static long sys_getsockopt(int fd, int lv, int opt, void *val, uint32_t *len)
{
	socket_t *s = fd_to_sock(fd);
	if (!s) return sock_fd_err(fd);            /* EBADF */
	if (!val || !len) return -EFAULT;

	uint32_t ulen;
	if (copy_from_user(&ulen, len, sizeof(ulen)) != 0) return -EFAULT;

	if (lv == SO_IPPROTO_TCP && opt == SO_TCP_INFO) {
		if (s->type != SOCK_STREAM) return -ENOPROTOOPT;
		uint8_t info[96];
		uint32_t n = tcp_get_info(s, info, ulen < sizeof(info) ? ulen : sizeof(info));
		if (n && copy_to_user(val, info, n) != 0) return -EFAULT;
		if (copy_to_user(len, &n, sizeof(n)) != 0) return -EFAULT;
		return 0;
	}

	if (lv == SOL_RANIX_FILTER && opt == NF_RULE) {
		if (!capable_task(task_current(), CAP_NET_ADMIN)) return -EPERM;
		char tmp[512];
		int n = nf_ctl_dump(tmp, sizeof(tmp));
		uint32_t cp = ulen < (uint32_t)n ? ulen : (uint32_t)n;
		if (cp && copy_to_user(val, tmp, cp) != 0) return -EFAULT;
		uint32_t rn = (uint32_t)n;
		if (copy_to_user(len, &rn, sizeof(rn)) != 0) return -EFAULT;
		return 0;
	}

	if (lv == SO_SOL_SOCKET && opt == 17 /* SO_PEERCRED */) {
		if (s->domain != AF_UNIX) return -ENOPROTOOPT;
		/* unconnected: Linux answers pid 0, uid and gid -1, not an error */
		struct { int32_t pid; uint32_t uid, gid; } uc = { 0, 0xffffffffu, 0xffffffffu };
		if (s->pc_set) {
			uc.pid = (int32_t)pid_to_user((pid_t)s->pc_pid);
			uc.uid = s->pc_uid; uc.gid = s->pc_gid;
		}
		uint32_t cp = ulen < sizeof(uc) ? ulen : sizeof(uc);
		if (cp && copy_to_user(val, &uc, cp) != 0) return -EFAULT;
		uint32_t rl = sizeof(uc);
		if (copy_to_user(len, &rl, sizeof(rl)) != 0) return -EFAULT;
		return 0;
	}

	if (lv == SO_SOL_SOCKET && (opt == SO_RCVTIMEO || opt == SO_SNDTIMEO)) {
		uint32_t t = (opt == SO_RCVTIMEO) ? s->rcv_timeo : s->snd_timeo;
		struct k_timeval tv;
		tv.tv_sec  = t / TIMER_HZ;
		tv.tv_usec = (int64_t)(t % TIMER_HZ) * 1000000 / TIMER_HZ;
		uint32_t cp = ulen < sizeof(tv) ? ulen : sizeof(tv);
		if (cp && copy_to_user(val, &tv, cp) != 0) return -EFAULT;
		uint32_t rl = sizeof(tv);
		if (copy_to_user(len, &rl, sizeof(rl)) != 0) return -EFAULT;
		return 0;
	}

	int out;
	if (lv == SO_SOL_SOCKET) {
		switch (opt) {
		case SO_TYPE:       out = s->unix_seqpacket ? 5 /* SOCK_SEQPACKET */ : s->type; break;
		case SO_ERROR:      out = s->so_error; s->so_error = 0;
		                    s->connect_inprog = 0; break;  /* read-and-clear */
		case SO_ACCEPTCONN: out = (s->tcp_state == TCP_LISTEN); break;
		case SO_RCVBUF:     out = (int)s->so_rcvbuf; break;
		case SO_SNDBUF:     out = (int)s->so_sndbuf; break;
		case SO_REUSEADDR:  out = s->so_reuseaddr; break;
		case SO_REUSEPORT:  out = s->so_reuseport; break;
		case SO_KEEPALIVE:  out = s->so_keepalive; break;
		case SO_PASSCRED:   out = s->so_passcred; break;
		default:            return -ENOPROTOOPT;
		}
	} else if (lv == SO_IPPROTO_TCP) {
		if (opt == SO_TCP_NODELAY) out = s->tcp_nodelay;
		else return -ENOPROTOOPT;
	} else if (lv == 41 && opt == 26 && (s->domain == AF_INET6 || s->v4mapped)) {
		out = s->v6only;
	} else {
		return -ENOPROTOOPT;
	}

	uint32_t cp = ulen < sizeof(int) ? ulen : sizeof(int);
	if (cp && copy_to_user(val, &out, cp) != 0) return -EFAULT;
	uint32_t rl = sizeof(int);
	if (copy_to_user(len, &rl, sizeof(rl)) != 0) return -EFAULT;
	return 0;
}

static long sys_listen(int fd, int bl)
{
    socket_t *s = fd_to_sock(fd);
    if (!s) return sock_fd_err(fd);
    if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
    if (s->domain == AF_UNIX && s->unix_peer) return -EINVAL;   /* a connected socket cannot also accept */
    /* a listener is a door: remember whose (SO_PEERCRED, /proc/net/tcp uid) */
    process_t *me = task_current();
    s->own_pid = (int32_t)me->tgid; s->own_uid = me->euid; s->own_gid = me->egid;
    if (s->domain == AF_UNIX) {
        s->backlog   = bl;
        s->tcp_state = TCP_LISTEN;
        return 0;
    }
    return tcp_listen(s, bl);
}

static long sys_accept4(int fd, void *ua, uint32_t *al, int fl)
{
    process_t *lp = task_current();
    vfs_file_t *lf = fd_get(lp->fds, fd);
    if (fl & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)) return -EINVAL;
    if (!lf) return -EBADF;
    if (!lf->inode || lf->inode->fops != &sock_fops) return -ENOTSOCK;
    socket_t *ls = (socket_t *)lf->inode->priv;
    if (ls->tcp_state != TCP_LISTEN) return -EINVAL;
    int nonblock = (lf->flags & O_NONBLOCK) ? 1 : 0;

    /* wait for a completed connection — tcp_input enqueues it then wakes
     * accept_waiter under recv_lock, so this register + re-check is atomic. */
    uint64_t af = spin_lock_irq(&ls->recv_lock);
    while (!ls->accept_head) {
        if (nonblock) { spin_unlock_irq(&ls->recv_lock, af); return -EAGAIN; }
        ls->accept_waiter = task_current();
        task_sleep_unlock(&ls->recv_lock, af);
        if (task_has_actionable_signal()) {
            af = spin_lock_irq(&ls->recv_lock);
            ls->accept_waiter = NULL;
            spin_unlock_irq(&ls->recv_lock, af);
            return -EINTR;
        }
        af = spin_lock_irq(&ls->recv_lock);
    }
    /* dequeue under the lock — the net RX thread enqueues under it concurrently */
    socket_t *ns = ls->accept_head;
    ls->accept_head = ns->accept_head;
    if (!ls->accept_head) ls->accept_tail = NULL;
    ns->accept_head = NULL;
    spin_unlock_irq(&ls->recv_lock, af);

    /* fill peer addr if requested */
    long nr = 0;
    if (ua) {
        if (ls->domain == AF_UNIX) {
            nr = sock_name_unix(NULL, ua, al);
        } else if (ns->v4mapped) {
            in6_addr_t a;
            in6_set_v4mapped(&a, ns->remote_ip);
            nr = sock_name6(&a, ns->remote_port, ua, al);
        } else if (ls->domain == AF_INET6) {
            nr = sock_name6(&ns->remote_ip6, ns->remote_port, ua, al);
        } else {
            sockaddr_in_t sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port   = htons(ns->remote_port);
            sa.sin_addr   = htonl(ns->remote_ip);
            nr = sock_name_put_u(ua, &sa, sizeof(sa), al);
        }
    }

    int nfd = sock_to_fd(ns, fl & (SOCK_CLOEXEC | SOCK_NONBLOCK));
    if (nfd < 0) { sock_free(ns); return nfd; }
    if (nr) { vfs_close(nfd); return nr; }
    return nfd;
}

static void unix_wake_reader(socket_t *s)
{
    uint64_t f = spin_lock_irq(&s->recv_lock);
    process_t *w = s->recv_waiter;
    s->recv_waiter = NULL;
    spin_unlock_irq(&s->recv_lock, f);
    if (w) task_wake(w);
}

/* struct mmsghdr: a msghdr and how many bytes went through it. glibc's resolver
 * sends its A and AAAA queries with sendmmsg. */
#define MMSG_SIZE    64
#define MMSG_LEN_OFF 56
#define UIO_MAXIOV   1024

static long sys_sendmmsg(int fd, void *uvec, uint32_t vlen, int flags)
{
    if (vlen > UIO_MAXIOV) vlen = UIO_MAXIOV;
    for (uint32_t i = 0; i < vlen; i++) {
        char *h = (char *)uvec + (size_t)i * MMSG_SIZE;
        long r = sys_sendmsg(fd, h, flags);
        if (r < 0) return i ? (long)i : r;
        uint32_t n = (uint32_t)r;
        if (copy_to_user(h + MMSG_LEN_OFF, &n, sizeof(n)) != 0) return i ? (long)i : -14;
    }
    return (long)vlen;
}

/* the timeout argument is not honoured: only the first receive may block */
static long sys_recvmmsg(int fd, void *uvec, uint32_t vlen, int flags)
{
    if (vlen > UIO_MAXIOV) vlen = UIO_MAXIOV;
    for (uint32_t i = 0; i < vlen; i++) {
        char *h = (char *)uvec + (size_t)i * MMSG_SIZE;
        long r = sys_recvmsg(fd, h, i ? flags | MSG_DONTWAIT : flags);
        if (r < 0) return i ? (long)i : r;
        uint32_t n = (uint32_t)r;
        if (copy_to_user(h + MMSG_LEN_OFF, &n, sizeof(n)) != 0) return i ? (long)i : -14;
    }
    return (long)vlen;
}

static long sys_shutdown(int fd, int how)
{
    socket_t *s = fd_to_sock(fd);
    if (!s) return sock_fd_err(fd);
    if (how < 0 || how > 2) return -EINVAL;
    if (s && s->domain == AF_UNIX) {
        socket_t *peer = s->unix_peer;
        if (how != 1 /* SHUT_WR */) {
            s->unix_rd_eof = 1;
            unix_wake_reader(s);
        }
        if (how != 0 /* SHUT_RD */) {
            s->unix_wr_shut = 1;
            if (peer) { peer->unix_rd_eof = 1; unix_wake_reader(peer); }
        }
        poll_notify();
    } else if (s && s->type == SOCK_STREAM) {
        if (how != 0 /* SHUT_RD */) tcp_close(s);   /* a FIN says we stopped sending */
    }
    return 0;
}

/* ── readlinkat via VFS ──────────────────────────────────── */

static long sys_readlinkat_vfs(int dirfd, const char *upath, char *ubuf, size_t bufsiz)
{
    if (!bufsiz) return -EINVAL;
    char *kp = kmalloc(PATH_MAX);
    if (!kp) return -ENOMEM;
    char *parent_path = kmalloc(PATH_MAX);
    char *kbuf = kmalloc(PATH_MAX);
    if (!parent_path || !kbuf) { kfree(kp); kfree(parent_path); kfree(kbuf); return -ENOMEM; }

    long ret = resolve_path(kp, dirfd, upath);
    if (ret) goto out;

    /* Split into parent dir and filename — look up last component without following */
    const char *slash = kp + strlen(kp) - 1;
    while (slash > kp && *slash != '/') slash--;
    int plen = (int)(slash - kp);
    if (plen == 0) { parent_path[0] = '/'; parent_path[1] = '\0'; }
    else           { memcpy(parent_path, kp, plen); parent_path[plen] = '\0'; }
    const char *fname = slash + 1;

    vfs_dentry_t *parent = vfs_dentry_resolve(parent_path);
    if (!parent || !parent->inode || !vfs_may_search(parent->inode)) { ret = -2; goto out; }
    /* a final component of "", ".", or ".." names a directory (the parent or an
     * ancestor, already shown to exist) — never a symlink → EINVAL, not ENOENT. */
    if (fname[0] == '\0' ||
        (fname[0] == '.' && (fname[1] == '\0' ||
                             (fname[1] == '.' && fname[2] == '\0')))) {
        ret = -22; goto out;
    }
    if (!parent->inode->iops || !parent->inode->iops->lookup) { ret = -2; goto out; }

    vfs_inode_t *in = parent->inode->iops->lookup(parent->inode, fname);
    if (!in) { ret = -2; goto out; }
    /* /dev/stdin and friends are devices that also read as links */
    if ((!S_ISLNK(in->mode) && !S_ISCHR(in->mode)) || !in->iops || !in->iops->readlink) {
        vfs_inode_free(in);
        ret = -22; goto out;
    }

    int n = in->iops->readlink(in, kbuf, PATH_MAX);
    vfs_inode_free(in);
    if (n < 0) { ret = n; goto out; }
    size_t slen = strlen(kbuf);
    if (slen > bufsiz) slen = bufsiz;
    ret = copy_to_user(ubuf, kbuf, slen) ? -14 : (long)slen;
out:
    kfree(kp); kfree(parent_path); kfree(kbuf);
    return ret;
}

