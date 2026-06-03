/* ── socket syscalls ─────────────────────────────────────── */

static void unix_reg_remove(socket_t *s); /* forward */

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
        uint64_t cf = spin_lock_irq(&s->recv_lock);
        while (s->tcp_state != TCP_CLOSED &&
               s->tcp_state != TCP_TIME_WAIT) {
            s->close_waiter = task_current();
            task_sleep_unlock(&s->recv_lock, cf);
            cf = spin_lock_irq(&s->recv_lock);
        }
        spin_unlock_irq(&s->recv_lock, cf);
    }
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
    uint64_t f = spin_lock_irq(&s->recv_lock);
    int has_data = (s->recv_head != NULL);
    spin_unlock_irq(&s->recv_lock, f);
    int eof;
    if (s->domain == AF_UNIX)
        eof = (s->unix_peer == NULL);
    else
        eof = (s->type == SOCK_STREAM &&
               (s->tcp_state == TCP_CLOSE_WAIT || s->tcp_state == TCP_CLOSED ||
                s->tcp_state == TCP_LAST_ACK   || s->tcp_state == TCP_TIME_WAIT));
    return ((has_data || eof) ? 0x0001 : 0) | 0x0004;
}

static ssize_t sock_read_vfs(vfs_file_t *file, void *buf, size_t len)
{
    socket_t *s = (socket_t *)file->inode->priv;
    if (!s) return -9;
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
                if (n) copy_to_user((char *)buf + total, skb->data, n);
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
                extern void tcp_rx_consumed(socket_t *, uint32_t);
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
            if (n) copy_to_user(buf, skb->data, n);
            skb_free(skb);
            return (ssize_t)(n ? n : 0);
        }
        spin_unlock_irq(&s->recv_lock, f);
    wait_or_eof:;
        f = spin_lock_irq(&s->recv_lock);
        if (s->recv_head) { spin_unlock_irq(&s->recv_lock, f); continue; }
        /* EOF: TCP half-closed or AF_UNIX peer disconnected */
        int eof = (s->domain == AF_UNIX)
            ? (s->unix_peer == NULL)
            : (s->type == SOCK_STREAM &&
               (s->tcp_state == TCP_CLOSE_WAIT || s->tcp_state == TCP_CLOSED ||
                s->tcp_state == TCP_TIME_WAIT  || s->tcp_state == TCP_LAST_ACK));
        if (eof) {
            spin_unlock_irq(&s->recv_lock, f);
            return 0;
        }
        s->recv_waiter = task_current();
        task_sleep_unlock(&s->recv_lock, f);
        process_t *p = task_current();
        if (task_has_actionable_signal()) { s->recv_waiter = NULL; return -4; }
    }
}

static ssize_t sock_write_vfs(vfs_file_t *file, const void *buf, size_t len)
{
    socket_t *s = (socket_t *)file->inode->priv;
    if (!s) return -9;
    if (s->type == SOCK_STREAM || (s->domain == AF_UNIX && s->type == SOCK_DGRAM)) {
        uint8_t *kbuf = kmalloc(len);
        if (!kbuf) return -12;
        if (copy_from_user(kbuf, buf, len) != 0) { kfree(kbuf); return -14; }
        int rc;
        if (s->domain == AF_UNIX) {
            rc = unix_send(s, kbuf, (uint16_t)len);
            kfree(kbuf);
            return rc < 0 ? rc : (ssize_t)len;
        }
        /* TCP: tcp_send queues at most one MSS; cap the request so the uint16
         * length can't truncate, and report what it actually took so the caller
         * loops for the rest. */
        uint16_t chunk = len > 65535 ? 65535 : (uint16_t)len;
        while ((rc = tcp_send(s, kbuf, chunk)) == -11) {
            s->connect_waiter = task_current();
            poll_block((uint64_t)-1);
            if (task_has_actionable_signal()) { s->connect_waiter = NULL; kfree(kbuf); return -4; }
        }
        kfree(kbuf);
        return rc;
    }
    return -38;
}

static vfs_fops_t sock_fops = {
    .read  = sock_read_vfs,
    .write = sock_write_vfs,
    .close = sock_close_vfs,
    .poll  = sock_poll_vfs,
};

static socket_t *fd_to_sock(int fd)
{
    process_t  *p = task_current();
    vfs_file_t *f = fd_get(p->fds, fd);
    if (!f || !f->inode || f->inode->fops != &sock_fops) return NULL;
    return (socket_t *)f->inode->priv;
}

static int sock_to_fd(socket_t *s)
{
    vfs_inode_t *in = kmalloc(sizeof(*in));
    if (!in) return -12;
    memset(in, 0, sizeof(*in));
    in->fops     = &sock_fops;
    in->priv     = s;
    in->refcount = 1;

    vfs_file_t *f = kmalloc(sizeof(*f));
    if (!f) { kfree(in); return -12; }
    memset(f, 0, sizeof(*f));
    f->inode    = in;
    f->refcount = 0;   /* fd_alloc takes the +1, like vfs_open; a single close() then frees */

    process_t *p = task_current();
    int fd = fd_alloc(p->fds, f);
    if (fd < 0) { kfree(f); kfree(in); return -12; }
    return fd;
}

/* ── AF_UNIX path registry ───────────────────────────────── */
/*
 * Handles both filesystem paths (sun_path[0] != '\0') and abstract
 * namespace sockets (sun_path[0] == '\0', name in sun_path[1..len-1]).
 * pathlen is the number of bytes in path[] that identify the socket.
 */

#define UNIX_REG_MAX 64
static struct { char path[108]; int pathlen; socket_t *sock; } unix_reg[UNIX_REG_MAX];
static spinlock_t unix_reg_lock = SPINLOCK_INIT;

static socket_t *unix_reg_find(const char *path, int pathlen)
{
    uint64_t f = spin_lock_irq(&unix_reg_lock);
    socket_t *found = NULL;
    for (int i = 0; i < UNIX_REG_MAX; i++) {
        if (unix_reg[i].sock && unix_reg[i].pathlen == pathlen &&
            memcmp(unix_reg[i].path, path, (size_t)pathlen) == 0) {
            found = unix_reg[i].sock;
            break;
        }
    }
    spin_unlock_irq(&unix_reg_lock, f);
    return found;
}

static int unix_reg_add(const char *path, int pathlen, socket_t *s)
{
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
    if (domain != AF_INET && domain != AF_INET6 && domain != AF_UNIX) return -97;
    if (type != SOCK_DGRAM && type != SOCK_STREAM && type != SOCK_RAW) return -22;
    socket_t *s = sock_alloc(domain, type, proto);
    if (!s) return -12;
    s->dev = net_default_dev();
    if (type == SOCK_RAW) sock_register(s);
    int fd = sock_to_fd(s);
    if (fd < 0) { sock_free(s); return fd; }
    return fd;
}

static long sys_bind(int fd, const void *uaddr, uint32_t addrlen)
{
    (void)addrlen;
    socket_t *s = fd_to_sock(fd);
    if (!s) return -9;

    uint16_t family = 0;
    if (copy_from_user(&family, uaddr, sizeof(family)) != 0) return -14;

    if (family == AF_UNIX) {
        sockaddr_un_t su;
        uint32_t copylen = addrlen < sizeof(su) ? addrlen : sizeof(su);
        if (copy_from_user(&su, uaddr, copylen) != 0) return -14;
        int pathlen;
        if (su.sun_path[0] == '\0') {
            /* abstract namespace: name is sun_path[0..addrlen-3], includes \0 */
            pathlen = (int)addrlen - 2;
            if (pathlen <= 0 || pathlen > 107) return -22;
        } else {
            /* filesystem path: use strlen+1 (null terminator included) */
            int slen = 0;
            while (slen < 107 && su.sun_path[slen]) slen++;
            pathlen = slen + 1;
        }
        if (unix_reg_find(su.sun_path, pathlen)) return -98; /* EADDRINUSE */
        memcpy(s->unix_path, su.sun_path, (size_t)pathlen);
        s->unix_path_len = pathlen;
        return unix_reg_add(s->unix_path, pathlen, s);
    }

    if (family == AF_INET6) {
        sockaddr_in6_t sa6;
        if (copy_from_user(&sa6, uaddr, sizeof(sa6)) != 0) return -14;
        s->local_ip6  = sa6.sin6_addr;
        s->local_port = ntohs(sa6.sin6_port);
        sock_register(s);
        return 0;
    }

    sockaddr_in_t sa;
    if (copy_from_user(&sa, uaddr, sizeof(sa)) != 0) return -14;
    s->local_ip   = ntohl(sa.sin_addr);
    s->local_port = ntohs(sa.sin_port);
    sock_register(s);
    return 0;
}

static long sys_connect(int fd, const void *uaddr, uint32_t addrlen)
{
    (void)addrlen;
    socket_t *s = fd_to_sock(fd);
    if (!s) return -9;

    uint16_t family = 0;
    if (copy_from_user(&family, uaddr, sizeof(family)) != 0) return -14;

    if (family == AF_UNIX) {
        sockaddr_un_t su;
        uint32_t copylen = addrlen < sizeof(su) ? addrlen : sizeof(su);
        if (copy_from_user(&su, uaddr, copylen) != 0) return -14;
        int pathlen;
        if (su.sun_path[0] == '\0') {
            pathlen = (int)addrlen - 2;
            if (pathlen <= 0 || pathlen > 107) return -22;
        } else {
            int slen = 0;
            while (slen < 107 && su.sun_path[slen]) slen++;
            pathlen = slen + 1;
        }
        socket_t *ls = unix_reg_find(su.sun_path, pathlen);
        if (!ls) return -111; /* ECONNREFUSED */
        if (ls->tcp_state != TCP_LISTEN) return -111;

        /* allocate server-side peer socket */
        socket_t *peer = sock_alloc(AF_UNIX, s->type, 0);
        if (!peer) return -12;
        peer->unix_peer = s;
        s->unix_peer    = peer;
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
        if (w) task_wake(w);
        return 0;
    }

    if (family == AF_INET6) {
        sockaddr_in6_t sa6;
        if (copy_from_user(&sa6, uaddr, sizeof(sa6)) != 0) return -14;
        s->remote_ip6  = sa6.sin6_addr;
        s->remote_port = ntohs(sa6.sin6_port);
    } else {
        sockaddr_in_t sa;
        if (copy_from_user(&sa, uaddr, sizeof(sa)) != 0) return -14;
        s->remote_ip   = ntohl(sa.sin_addr);
        s->remote_port = ntohs(sa.sin_port);
    }
    if (s->type == SOCK_DGRAM) return 0;
    if (s->type != SOCK_STREAM) return -38;
    if (!s->dev) return -101; /* ENETUNREACH — no network device to reach the peer */
    /* fail fast on an unreachable IPv6 peer (no on-link prefix, no router) so an
     * AF_UNSPEC client falls back to IPv4 instead of blocking on a dropped SYN */
    if (s->domain == AF_INET6) {
        netdev_t *d6 = net_route_dev6(&s->remote_ip6);
        if (!ipv6_reachable(d6, &s->remote_ip6)) return -113; /* EHOSTUNREACH */
    }

    int rc = tcp_connect(s);
    if (rc < 0) return rc;

    /* block until ESTABLISHED or RST — event-driven: tcp_input updates tcp_state
     * then wakes connect_waiter under recv_lock, so registering + re-checking
     * under the same lock is atomic against the waker (no lost wakeup, no poll). */
    uint64_t cf = spin_lock_irq(&s->recv_lock);
    while (s->tcp_state == TCP_SYN_SENT) {
        s->connect_waiter = task_current();
        task_sleep_unlock(&s->recv_lock, cf);
        if (task_has_actionable_signal()) {
            cf = spin_lock_irq(&s->recv_lock);
            s->connect_waiter = NULL;
            spin_unlock_irq(&s->recv_lock, cf);
            return -4;
        }
        cf = spin_lock_irq(&s->recv_lock);
    }
    spin_unlock_irq(&s->recv_lock, cf);
    return (s->tcp_state == TCP_ESTABLISHED) ? 0 : -111; /* ECONNREFUSED */
}

static long sys_sendto(int fd, const void *buf, size_t len, int flags,
                       const void *uaddr, uint32_t addrlen)
{
    (void)flags; (void)addrlen;
    socket_t *s = fd_to_sock(fd);
    if (!s || !s->dev) return -9;

    /* IPv6 raw (ping6): kernel fills in the ICMPv6 checksum, then ipv6_tx */
    if (s->domain == AF_INET6) {
        in6_addr_t dst6;
        uint16_t   dst_port6;
        if (uaddr) {
            sockaddr_in6_t sa6;
            if (copy_from_user(&sa6, uaddr, sizeof(sa6)) != 0) return -14;
            dst6      = sa6.sin6_addr;
            dst_port6 = ntohs(sa6.sin6_port);
        } else {
            dst6      = s->remote_ip6;
            dst_port6 = s->remote_port;
        }
        if (len > SKB_DATA_MAX) len = SKB_DATA_MAX;
        uint8_t *kb = kmalloc(len);
        if (!kb) return -12;
        if (copy_from_user(kb, buf, len) != 0) { kfree(kb); return -14; }
        /* fail fast if the destination has no route, rather than spinning the
         * resolve loop and returning a misleading ENOSYS */
        if (!ipv6_reachable(net_route_dev6(&dst6), &dst6)) { kfree(kb); return -113; }

        long rr = -38;
        /* ICMPv6 over SOCK_RAW *or* SOCK_DGRAM (Linux "ping socket": unprivileged
         * ping uses socket(AF_INET6,SOCK_DGRAM,IPPROTO_ICMPV6)). Both carry a
         * user-built ICMPv6 message; the kernel just checksums and ships it. */
        if (s->proto == IPPROTO_ICMPV6 &&
            (s->type == SOCK_RAW || s->type == SOCK_DGRAM) &&
            len >= sizeof(icmp6_hdr_t)) {
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
            extern volatile uint64_t kernel_ticks;
            uint64_t deadline = kernel_ticks + 100;   /* ~1s at 100 Hz */
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
            } while (kernel_ticks < deadline);
        } else if (s->type == SOCK_DGRAM) {
            if (!s->local_port) { extern uint16_t alloc_port(void); s->local_port = alloc_port(); }
            sock_register(s);
            netdev_t *txdev = net_route_dev6(&dst6);
            for (int attempt = 0; attempt < 64; attempt++) {
                int rc = udp6_tx(txdev, &dst6, s->local_port, dst_port6,
                                 kb, (uint16_t)len);
                if (rc >= 0) { rr = (long)len; break; }
                if (rc != -1) { rr = rc; break; }
                net_poll();   /* NDP resolution (later phase) */
            }
        }
        kfree(kb);
        return rr;
    }

    uint32_t dst_ip; uint16_t dst_port;
    if (uaddr) {
        sockaddr_in_t sa;
        if (copy_from_user(&sa, uaddr, sizeof(sa)) != 0) return -14;
        dst_ip   = ntohl(sa.sin_addr);
        dst_port = ntohs(sa.sin_port);
    } else {
        dst_ip = s->remote_ip; dst_port = s->remote_port;
    }

    if (len > SKB_DATA_MAX) len = SKB_DATA_MAX;
    uint8_t *kbuf = kmalloc(len);
    if (!kbuf) return -12;
    if (copy_from_user(kbuf, buf, len) != 0) { kfree(kbuf); return -14; }

    long r = -38;
    if (s->type == SOCK_STREAM) {
        /* TCP: ignore dst from uaddr, use connected remote */
        int rc;
        while ((rc = tcp_send(s, kbuf, (uint16_t)len)) == -11) {
            s->connect_waiter = task_current();
            poll_block((uint64_t)-1);
            if (task_has_actionable_signal()) { s->connect_waiter = NULL; kfree(kbuf); return -4; }
        }
        r = rc;   /* bytes tcp_send actually queued (clamped to MSS); caller loops for the rest */
    } else if (s->type == SOCK_DGRAM && s->proto != 1 /* not an ICMP ping socket */) {
        /* an unbound socket needs a source port and a delivery registration, or
         * the peer's reply lands on port 0 and matches nothing */
        if (!s->local_port) { extern uint16_t alloc_port(void); s->local_port = alloc_port(); }
        sock_register(s);
        /* first send to an unresolved peer triggers ARP and is dropped; poll RX
         * for the reply and retry so the datagram actually goes out */
        netdev_t *txdev = net_route_dev(dst_ip);
        int rc = -1;
        for (int attempt = 0; attempt < 64 && rc == -1; attempt++) {
            rc = udp_tx(txdev, dst_ip, s->local_port, dst_port, kbuf, (uint16_t)len);
            if (rc == -1) net_poll();
        }
        r = rc < 0 ? rc : (long)len;
    } else if ((s->type == SOCK_RAW || s->type == SOCK_DGRAM) && s->proto == 1 /* IPPROTO_ICMP */) {
        if (len >= sizeof(icmp_hdr_t)) {
            icmp_hdr_t *ih = (icmp_hdr_t *)kbuf;
            ih->checksum = 0;
            ih->checksum = inet_cksum(kbuf, len);
            /* retry loop: first attempt sends ARP request; subsequent ones hit cached MAC */
            for (int attempt = 0; attempt < 64; attempt++) {
                skb_t *skb = skb_alloc();
                if (!skb) { r = -12; break; }
                memcpy(skb->data, kbuf, len);
                skb->len = (uint16_t)len;
                int rc = ip_tx(s->dev, skb, dst_ip, 1 /* IPPROTO_ICMP */);
                if (rc >= 0) { r = (long)len; break; }
                if (rc != -1) { r = rc; break; }
                net_poll();  /* drain RX — ARP reply may arrive */
            }
        }
    }
    kfree(kbuf);
    return r;
}

static long sys_recvfrom(int fd, void *buf, size_t len, int flags,
                         void *uaddr, uint32_t *addrlen)
{
    (void)addrlen;
#define MSG_DONTWAIT 0x40
    int nonblock = (flags & MSG_DONTWAIT);
    socket_t *s = fd_to_sock(fd);
    if (!s) return -9;

    /* SOCK_STREAM has no message boundaries: one recv drains as many queued
     * bytes as fit in buf, coalescing across writes (Linux semantics). Applies
     * to TCP and unix stream alike. DGRAM/RAW preserve per-datagram boundaries
     * (handled below). */
    if (s->type == SOCK_STREAM) {
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
                copy_to_user((char *)buf + total, skb->data, n);
                if (uaddr && !got_addr) {
                    if (s->domain == AF_INET6) {
                        sockaddr_in6_t sa6; memset(&sa6, 0, sizeof(sa6));
                        sa6.sin6_family = AF_INET6; sa6.sin6_addr = skb->src_ip6;
                        sa6.sin6_port = htons(s->remote_port);
                        copy_to_user(uaddr, &sa6, sizeof(sa6));
                    } else {
                        sockaddr_in_t sa; memset(&sa, 0, sizeof(sa));
                        sa.sin_family = AF_INET; sa.sin_addr = htonl(skb->src_ip);
                        sa.sin_port = htons(skb->src_port);
                        copy_to_user(uaddr, &sa, sizeof(sa));
                    }
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
                extern void tcp_rx_consumed(socket_t *, uint32_t);
                tcp_rx_consumed(s, (uint32_t)total);
                return (long)total;
            }
            if (nonblock) return -11; /* EAGAIN */
            uint64_t f = spin_lock_irq(&s->recv_lock);
            if (s->recv_head) { spin_unlock_irq(&s->recv_lock, f); continue; }
            s->recv_waiter = task_current();
            task_sleep_unlock(&s->recv_lock, f);
            if (task_has_actionable_signal()) { s->recv_waiter = NULL; return -4; }
        }
    }

    while (1) {
        uint64_t f = spin_lock_irq(&s->recv_lock);
        skb_t *skb = s->recv_head;
        if (skb) {
            s->recv_head = skb->next;
            if (!s->recv_head) s->recv_tail = NULL;
            spin_unlock_irq(&s->recv_lock, f);
            size_t n = skb->len < len ? skb->len : len;
            copy_to_user(buf, skb->data, n);
            if (uaddr && s->domain == AF_INET6) {
                sockaddr_in6_t sa6;
                memset(&sa6, 0, sizeof(sa6));
                sa6.sin6_family = AF_INET6;
                sa6.sin6_addr   = skb->src_ip6;
                sa6.sin6_port   = htons(s->remote_port);
                copy_to_user(uaddr, &sa6, sizeof(sa6));
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
                copy_to_user(uaddr, &sa, sizeof(sa));
            }
            /* stream: a short read consumes only n bytes; keep the rest queued */
            if (s->type == SOCK_STREAM && n < skb->len) {
                skb->data += n; skb->len -= (uint16_t)n;
                uint64_t f2 = spin_lock_irq(&s->recv_lock);
                skb->next = s->recv_head;
                s->recv_head = skb;
                if (!s->recv_tail) s->recv_tail = skb;
                spin_unlock_irq(&s->recv_lock, f2);
            } else {
                skb_free(skb);
            }
            if (s->type == SOCK_STREAM && n) { extern void tcp_rx_consumed(socket_t *, uint32_t); tcp_rx_consumed(s, (uint32_t)n); }
            return (long)n;
        }
        if (nonblock) { spin_unlock_irq(&s->recv_lock, f); return -11; /* EAGAIN */ }
        s->recv_waiter = task_current();
        task_sleep_unlock(&s->recv_lock, f);
        process_t *p = task_current();
        if (task_has_actionable_signal()) { s->recv_waiter = NULL; return -4; }
    }
}

/* sendmsg/recvmsg — vectored send/receive + SCM_RIGHTS fd-passing over AF_UNIX.
 * Plain data delegates to the tested sendto/recvfrom/read/write paths. */
typedef struct { void *base; uint64_t len; } k_iovec;
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
    if (copy_from_user(&m, umsg, sizeof(m)) != 0) return -14;
    if (m.msg_iovlen == 0) return 0;
    if (m.msg_iovlen > 64) return -22;
    k_iovec iov[64];
    if (copy_from_user(iov, m.msg_iov, (size_t)m.msg_iovlen * sizeof(k_iovec)) != 0) return -14;

    /* SCM_RIGHTS fd-passing — AF_UNIX only: collect the sender's files, hold an
     * in-flight reference on each, and deliver them with the data in one skb. */
    if (m.msg_control && m.msg_controllen >= sizeof(k_cmsghdr)) {
        socket_t *s = fd_to_sock(fd);
        if (s && s->domain == AF_UNIX) {
            uint8_t cb[sizeof(k_cmsghdr) + SKB_SCM_MAX * sizeof(int)];
            size_t clen = m.msg_controllen < sizeof(cb) ? m.msg_controllen : sizeof(cb);
            if (copy_from_user(cb, m.msg_control, clen) != 0) return -14;
            k_cmsghdr *ch = (k_cmsghdr *)cb;
            if (ch->cmsg_level == SOL_SOCKET_ && ch->cmsg_type == SCM_RIGHTS_ &&
                ch->cmsg_len >= sizeof(k_cmsghdr) && ch->cmsg_len <= clen) {
                int nfd = (int)((ch->cmsg_len - sizeof(k_cmsghdr)) / sizeof(int));
                if (nfd > SKB_SCM_MAX) nfd = SKB_SCM_MAX;
                int *cfds = (int *)(cb + sizeof(k_cmsghdr));
                process_t *p = task_current();
                void *files[SKB_SCM_MAX]; int nf = 0;
                for (int i = 0; i < nfd; i++) {
                    vfs_file_t *vf = fd_get(p->fds, cfds[i]);
                    if (!vf) { for (int j = 0; j < nf; j++) vfs_file_put(files[j]); return -9; }
                    vf->refcount++;             /* in-flight reference */
                    files[nf++] = vf;
                }
                uint8_t dbuf[SKB_DATA_MAX]; size_t dl = 0;
                for (uint64_t i = 0; i < m.msg_iovlen && dl < sizeof(dbuf); i++) {
                    size_t c = iov[i].len; if (c > sizeof(dbuf) - dl) c = sizeof(dbuf) - dl;
                    if (c && copy_from_user(dbuf + dl, iov[i].base, c) != 0) {
                        for (int j = 0; j < nf; j++) vfs_file_put(files[j]); return -14;
                    }
                    dl += c;
                }
                int r = unix_send_scm(s, dbuf, (uint16_t)dl, files, nf);
                if (r < 0) { for (int j = 0; j < nf; j++) vfs_file_put(files[j]); return r; }
                return r;
            }
        }
    }

    long total = 0;
    for (uint64_t i = 0; i < m.msg_iovlen; i++) {
        if (iov[i].len == 0) continue;
        long r = m.msg_name
               ? sys_sendto(fd, iov[i].base, (size_t)iov[i].len, flags, m.msg_name, m.msg_namelen)
               : vfs_write(fd, iov[i].base, (size_t)iov[i].len);
        if (r < 0) return total ? total : r;
        total += r;
        if ((uint64_t)r < iov[i].len) break;   /* short write: stop */
    }
    return total;
}

static long sys_recvmsg(int fd, void *umsg, int flags)
{
    k_msghdr m;
    if (copy_from_user(&m, umsg, sizeof(m)) != 0) return -14;
    uint64_t zero64 = 0; int zero32 = 0;
    copy_to_user((char *)umsg + 40, &zero64, sizeof(zero64)); /* msg_controllen */
    copy_to_user((char *)umsg + 48, &zero32, sizeof(zero32)); /* msg_flags */
    if (m.msg_iovlen == 0) return 0;
    if (m.msg_iovlen > 64) return -22;
    k_iovec iov[64];
    if (copy_from_user(iov, m.msg_iov, (size_t)m.msg_iovlen * sizeof(k_iovec)) != 0) return -14;

    /* AF_UNIX: dequeue ourselves so we can coalesce stream data AND pick up any
     * passed files (SCM_RIGHTS) the consumed skbs carry. */
    socket_t *s = fd_to_sock(fd);
    if (s && s->domain == AF_UNIX) {
        int nonblock = flags & 0x40 /*MSG_DONTWAIT*/;
        size_t cap = 0;
        for (uint64_t i = 0; i < m.msg_iovlen; i++) cap += (size_t)iov[i].len;
        size_t total = 0; uint64_t iidx = 0; size_t ioff = 0;
        void *cfiles[SKB_SCM_MAX]; int ncf = 0, truncated = 0;

        while (total < cap) {
            uint64_t f = spin_lock_irq(&s->recv_lock);
            skb_t *skb = s->recv_head;
            if (!skb) {
                spin_unlock_irq(&s->recv_lock, f);
                if (total > 0) break;                       /* return what we have */
                if (s->unix_peer == NULL) return 0;         /* EOF */
                if (nonblock) return -11;
                uint64_t f2 = spin_lock_irq(&s->recv_lock);
                if (s->recv_head) { spin_unlock_irq(&s->recv_lock, f2); continue; }
                if (s->unix_peer == NULL) { spin_unlock_irq(&s->recv_lock, f2); return 0; }
                s->recv_waiter = task_current();
                task_sleep_unlock(&s->recv_lock, f2);
                if (task_has_actionable_signal()) { s->recv_waiter = NULL; return -4; }
                continue;
            }
            s->recv_head = skb->next;
            if (!s->recv_head) s->recv_tail = NULL;
            spin_unlock_irq(&s->recv_lock, f);

            size_t off = 0;                                  /* scatter into iovecs */
            while (off < skb->len && total < cap) {
                while (iidx < m.msg_iovlen && iov[iidx].len == 0) iidx++;
                if (iidx >= m.msg_iovlen) break;
                size_t room = (size_t)iov[iidx].len - ioff;
                size_t c = (skb->len - off < room) ? (size_t)(skb->len - off) : room;
                copy_to_user((char *)iov[iidx].base + ioff, skb->data + off, c);
                off += c; total += c; ioff += c;
                if (ioff == iov[iidx].len) { iidx++; ioff = 0; }
            }
            for (int j = 0; j < skb->scm_nfiles; j++) {      /* collect fds once */
                if (ncf < SKB_SCM_MAX) cfiles[ncf++] = skb->scm_files[j];
                else { vfs_file_put((vfs_file_t *)skb->scm_files[j]); truncated = 1; }
            }
            skb->scm_nfiles = 0;
            if (off < skb->len) {                            /* iov full mid-skb: requeue */
                skb->data += off; skb->len -= (uint16_t)off;
                uint64_t f2 = spin_lock_irq(&s->recv_lock);
                skb->next = s->recv_head; s->recv_head = skb;
                if (!s->recv_tail) s->recv_tail = skb;
                spin_unlock_irq(&s->recv_lock, f2);
                break;
            }
            skb_free(skb);
        }

        if (ncf > 0) {                                       /* install fds + build cmsg */
            process_t *p = task_current();
            int maxfds = (m.msg_control && m.msg_controllen >= sizeof(k_cmsghdr))
                       ? (int)((m.msg_controllen - sizeof(k_cmsghdr)) / sizeof(int)) : 0;
            int newfds[SKB_SCM_MAX]; int nf = 0;
            for (int i = 0; i < ncf; i++) {
                vfs_file_t *vf = (vfs_file_t *)cfiles[i];
                if (nf < maxfds) { int nfd = fd_alloc(p->fds, vf); if (nfd >= 0) newfds[nf++] = nfd; vfs_file_put(vf); }
                else { vfs_file_put(vf); truncated = 1; }
            }
            if (nf > 0) {
                uint8_t cb[sizeof(k_cmsghdr) + SKB_SCM_MAX * sizeof(int)];
                k_cmsghdr *ch = (k_cmsghdr *)cb;
                ch->cmsg_len = sizeof(k_cmsghdr) + (uint64_t)nf * sizeof(int);
                ch->cmsg_level = SOL_SOCKET_; ch->cmsg_type = SCM_RIGHTS_;
                memcpy(cb + sizeof(k_cmsghdr), newfds, (size_t)nf * sizeof(int));
                copy_to_user(m.msg_control, cb, ch->cmsg_len);
                copy_to_user((char *)umsg + 40, &ch->cmsg_len, sizeof(ch->cmsg_len));
            }
        }
        if (truncated) { int fl = MSG_CTRUNC_; copy_to_user((char *)umsg + 48, &fl, sizeof(fl)); }
        return (long)total;
    }

    /* non-unix: plain data path */
    for (uint64_t i = 0; i < m.msg_iovlen; i++) {
        if (iov[i].len == 0) continue;
        uint32_t nl = m.msg_namelen;
        long r = sys_recvfrom(fd, iov[i].base, (size_t)iov[i].len, flags, m.msg_name, m.msg_name ? &nl : (uint32_t *)0);
        if (r >= 0 && m.msg_name) copy_to_user((char *)umsg + 8, &nl, sizeof(nl));
        return r;
    }
    return 0;
}

static long sys_getsockname(int fd, void *uaddr, uint32_t *uaddrlen)
{
    socket_t *s = fd_to_sock(fd);
    if (!s) return -9;
    sockaddr_in_t sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(s->local_port);
    sa.sin_addr   = htonl(s->local_ip ? s->local_ip : (s->dev ? s->dev->ip : 0));
    copy_to_user(uaddr, &sa, sizeof(sa));
    if (uaddrlen) { uint32_t l = sizeof(sa); copy_to_user(uaddrlen, &l, 4); }
    return 0;
}

static long sys_getpeername(int fd, void *uaddr, uint32_t *addrlen)
{
    socket_t *s = fd_to_sock(fd);
    if (!s) return -9;
    sockaddr_in_t sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(s->remote_port);
    sa.sin_addr   = htonl(s->remote_ip);
    copy_to_user(uaddr, &sa, sizeof(sa));
    if (addrlen) { uint32_t l = sizeof(sa); copy_to_user(addrlen, &l, 4); }
    return 0;
}

#define SO_SOL_SOCKET   1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_ACCEPTCONN  30
#define SO_IPPROTO_TCP  6
#define SO_TCP_NODELAY  1

static long sys_setsockopt(int fd, int lv, int opt, const void *val, uint32_t len)
{
	(void)lv; (void)opt; (void)val; (void)len;
	/* Accept and ignore: no tunable per-socket buffers/flags yet, but success
	 * is what well-behaved programs expect. Reject only a bad fd. */
	if (!fd_to_sock(fd)) return -9; /* EBADF */
	return 0;
}

static long sys_getsockopt(int fd, int lv, int opt, void *val, uint32_t *len)
{
	socket_t *s = fd_to_sock(fd);
	if (!s) return -9;            /* EBADF */
	if (!val || !len) return -14; /* EFAULT */

	uint32_t ulen;
	if (copy_from_user(&ulen, len, sizeof(ulen)) != 0) return -14;

	int out;
	if (lv == SO_SOL_SOCKET) {
		switch (opt) {
		case SO_TYPE:       out = s->type; break;
		case SO_ERROR:      out = 0; break;                     /* no pending error */
		case SO_ACCEPTCONN: out = (s->tcp_state == TCP_LISTEN); break;
		case SO_RCVBUF:     out = 65536; break;
		case SO_SNDBUF:     out = 65536; break;
		case SO_REUSEADDR:
		case SO_KEEPALIVE:  out = 0; break;
		default:            return -92;  /* ENOPROTOOPT */
		}
	} else if (lv == SO_IPPROTO_TCP) {
		if (opt == SO_TCP_NODELAY) out = 0;
		else return -92;
	} else {
		return -92;
	}

	uint32_t cp = ulen < sizeof(int) ? ulen : sizeof(int);
	if (cp && copy_to_user(val, &out, cp) != 0) return -14;
	uint32_t rl = sizeof(int);
	if (copy_to_user(len, &rl, sizeof(rl)) != 0) return -14;
	return 0;
}

static long sys_listen(int fd, int bl)
{
    socket_t *s = fd_to_sock(fd);
    if (!s) return -9;
    if (s->type != SOCK_STREAM) return -95; /* EOPNOTSUPP */
    if (s->domain == AF_UNIX) {
        s->backlog   = bl;
        s->tcp_state = TCP_LISTEN;
        return 0;
    }
    return tcp_listen(s, bl);
}

static long sys_accept4(int fd, void *ua, uint32_t *al, int fl)
{
    (void)fl;
    socket_t *ls = fd_to_sock(fd);
    if (!ls) return -9;
    if (ls->tcp_state != TCP_LISTEN) return -22;

    /* wait for a completed connection — tcp_input enqueues it then wakes
     * accept_waiter under recv_lock, so this register + re-check is atomic. */
    uint64_t af = spin_lock_irq(&ls->recv_lock);
    while (!ls->accept_head) {
        ls->accept_waiter = task_current();
        task_sleep_unlock(&ls->recv_lock, af);
        if (task_has_actionable_signal()) {
            af = spin_lock_irq(&ls->recv_lock);
            ls->accept_waiter = NULL;
            spin_unlock_irq(&ls->recv_lock, af);
            return -4;
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
    if (ua) {
        if (ls->domain == AF_UNIX) {
            sockaddr_un_t su;
            memset(&su, 0, sizeof(su));
            su.sun_family = AF_UNIX;
            copy_to_user(ua, &su, sizeof(su));
            if (al) { uint32_t l = sizeof(su); copy_to_user(al, &l, 4); }
        } else {
            sockaddr_in_t sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port   = htons(ns->remote_port);
            sa.sin_addr   = htonl(ns->remote_ip);
            copy_to_user(ua, &sa, sizeof(sa));
            if (al) { uint32_t l = sizeof(sa); copy_to_user(al, &l, 4); }
        }
    }

    int nfd = sock_to_fd(ns);
    if (nfd < 0) { sock_free(ns); return nfd; }
    return nfd;
}

static long sys_shutdown(int fd, int how)
{
    (void)how;
    socket_t *s = fd_to_sock(fd);
    if (s && s->type == SOCK_STREAM) tcp_close(s);
    return 0;
}

/* ── readlinkat via VFS ──────────────────────────────────── */

static long sys_readlinkat_vfs(int dirfd, const char *upath, char *ubuf, size_t bufsiz)
{
    char kp[256];
    int r = resolve_path(kp, dirfd, upath);
    if (r) return r;

    /* Split into parent dir and filename — look up last component without following */
    char parent_path[256];
    const char *slash = kp + strlen(kp) - 1;
    while (slash > kp && *slash != '/') slash--;
    int plen = (int)(slash - kp);
    if (plen == 0) { parent_path[0] = '/'; parent_path[1] = '\0'; }
    else           { memcpy(parent_path, kp, plen); parent_path[plen] = '\0'; }
    const char *fname = slash + 1;

    vfs_dentry_t *parent = vfs_dentry_resolve(parent_path);
    if (!parent || !parent->inode) return -2;
    /* a final component of "", ".", or ".." names a directory (the parent or an
     * ancestor, already shown to exist) — never a symlink → EINVAL, not ENOENT. */
    if (fname[0] == '\0' ||
        (fname[0] == '.' && (fname[1] == '\0' ||
                             (fname[1] == '.' && fname[2] == '\0'))))
        return -22;
    if (!parent->inode->iops || !parent->inode->iops->lookup) return -2;

    vfs_inode_t *in = parent->inode->iops->lookup(parent->inode, fname);
    if (!in) return -2;
    if (!in->iops || !in->iops->readlink) {
        if (!in->cached) kfree(in);
        return -22;
    }

    char kbuf[256];
    int n = in->iops->readlink(in, kbuf, sizeof(kbuf));
    if (!in->cached) kfree(in);
    if (n < 0) return n;
    size_t slen = strlen(kbuf);
    if (slen > bufsiz) slen = bufsiz;
    copy_to_user(ubuf, kbuf, slen);
    return (long)slen;
}

