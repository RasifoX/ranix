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
        if (file->flags & O_NONBLOCK) {
            spin_unlock_irq(&s->recv_lock, f);
            return -11;
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
            if (file->flags & O_NONBLOCK) { kfree(kbuf); return -11; }
            s->connect_waiter = task_current();
            poll_block((uint64_t)-1);
            if (task_has_actionable_signal()) { s->connect_waiter = NULL; kfree(kbuf); return -4; }
        }
        kfree(kbuf);
        return rc;
    }
    return -38;
}

#define FIONREAD 0x541B
#define FIONBIO  0x5421

#define SIOCGIFNAME    0x8910
#define SIOCGIFCONF    0x8912
#define SIOCGIFFLAGS   0x8913
#define SIOCGIFADDR    0x8915
#define SIOCGIFNETMASK 0x891b
#define SIOCGIFMTU     0x8921
#define SIOCGIFHWADDR  0x8927
#define SIOCGIFINDEX   0x8933

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
    if (copy_from_user(&r, (void *)(uintptr_t)arg, sizeof(r))) return -14;
    netdev_t *d = ifr_dev(&r);
    if (!d) return -19;   /* ENODEV */
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
            if (!e) return -19;
            if (e == d) { r.u.ifru_ifindex = i + 1; break; }
        }
        break;
    }
    case SIOCGIFHWADDR:
        memset(&r.u, 0, sizeof(r.u));
        r.u.ifru_hwaddr.sa_family =
            (d->ip == 0x7f000001) ? ARPHRD_LOOPBACK : ARPHRD_ETHER;
        memcpy(r.u.ifru_hwaddr.sa_data, d->mac, 6);
        break;
    default:
        return -25;
    }
    if (copy_to_user((void *)(uintptr_t)arg, &r, sizeof(r))) return -14;
    return 0;
}

/* SIOCGIFCONF: fill the caller's buffer with one struct ifreq (name + IPv4
 * addr) per interface, and report the byte count actually used. */
static int sock_ioctl_ifconf(uint64_t arg)
{
    struct ifconf c;
    if (copy_from_user(&c, (void *)(uintptr_t)arg, sizeof(c))) return -14;
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
            if (copy_to_user(&ureq[used], &r, sizeof(r))) return -14;
        }
        used++;
    }
    c.ifc_len = used * (int)sizeof(struct ifreq);
    if (copy_to_user((void *)(uintptr_t)arg, &c, sizeof(c))) return -14;
    return 0;
}

static int sock_ioctl_vfs(vfs_file_t *file, uint32_t cmd, uint64_t arg)
{
    socket_t *s = (socket_t *)file->inode->priv;
    if (!s) return -9;
    switch (cmd) {
    case SIOCGIFADDR: case SIOCGIFNETMASK: case SIOCGIFFLAGS:
    case SIOCGIFMTU:  case SIOCGIFINDEX:   case SIOCGIFHWADDR:
        return sock_ioctl_ifreq(cmd, arg);
    case SIOCGIFCONF:
        return sock_ioctl_ifconf(arg);
    case FIONBIO: {                         /* set/clear non-blocking mode */
        int on;
        if (copy_from_user(&on, (void *)(uintptr_t)arg, sizeof(on)) != 0) return -14;
        if (on) file->flags |=  O_NONBLOCK;
        else    file->flags &= ~O_NONBLOCK;
        return 0;
    }
    case FIONREAD: {                        /* bytes queued for reading */
        int navail = 0;
        uint64_t f = spin_lock_irq(&s->recv_lock);
        for (skb_t *skb = s->recv_head; skb; skb = skb->next) navail += skb->len;
        spin_unlock_irq(&s->recv_lock, f);
        return copy_to_user((void *)(uintptr_t)arg, &navail, sizeof(navail));
    }
    default:
        return -25; /* ENOTTY */
    }
}

static vfs_fops_t sock_fops = {
    .read  = sock_read_vfs,
    .write = sock_write_vfs,
    .close = sock_close_vfs,
    .poll  = sock_poll_vfs,
    .ioctl = sock_ioctl_vfs,
};

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
    if (sflags & SOCK_NONBLOCK) f->flags |= O_NONBLOCK;

    process_t *p = task_current();
    int fd = fd_alloc(p->fds, f);
    if (fd < 0) { kfree(f); kfree(in); return -12; }
    if (sflags & SOCK_CLOEXEC) fd_set_cloexec(p->fds, fd, 1);
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
    int sflags = type & (SOCK_CLOEXEC | SOCK_NONBLOCK);
    type &= SOCK_TYPE_MASK;
    if (domain != AF_INET && domain != AF_INET6 && domain != AF_UNIX) return -97;
    if (type != SOCK_DGRAM && type != SOCK_STREAM && type != SOCK_RAW) return -22;
    socket_t *s = sock_alloc(domain, type, proto);
    if (!s) return -12;
    s->dev = net_default_dev();
    if (type == SOCK_RAW) sock_register(s);
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
        uint16_t port = ntohs(sa6.sin6_port);
        if (ip_port_taken(s, AF_INET6, s->type, port)) return -98; /* EADDRINUSE */
        s->local_ip6  = sa6.sin6_addr;
        s->local_port = port;
        sock_register(s);
        return 0;
    }

    sockaddr_in_t sa;
    if (copy_from_user(&sa, uaddr, sizeof(sa)) != 0) return -14;
    uint16_t port = ntohs(sa.sin_port);
    if (ip_port_taken(s, AF_INET, s->type, port)) return -98; /* EADDRINUSE */
    s->local_ip   = ntohl(sa.sin_addr);
    s->local_port = port;
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
    if (!s) return -9;
    /* AF_UNIX send()/sendto() delivers through the peer's queue, not the IP
     * stack. Without this an AF_UNIX datagram socket (s->dev is set like any
     * socket) falls into the udp_tx path below and is shipped as a bogus UDP
     * frame — exactly what a libc syslog write to /dev/log does. */
    if (s->domain == AF_UNIX) {
        if (len > SKB_DATA_MAX) len = SKB_DATA_MAX;
        uint8_t *kb = kmalloc(len);
        if (!kb) return -12;
        if (copy_from_user(kb, buf, len) != 0) { kfree(kb); return -14; }
        int rc = unix_send(s, kb, (uint16_t)len);
        kfree(kb);
        return rc < 0 ? rc : (long)len;
    }
    if (!s->dev) return -9;

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
        /* UDP datagrams up to the IPv6 max (65487) are fragmented by ipv6_tx;
         * the ICMPv6 path below copies into one skb and clamps separately. */
        if (len > 65487) len = 65487;
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
            extern uint64_t mono_ns(void);
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

    /* UDP datagrams up to the IPv4 max (65507) are fragmented by ip_tx; other
     * paths (raw ICMP) still copy into one skb and clamp below. */
    if (len > 65507) len = 65507;
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
        /* the first send to an unresolved next-hop triggers ARP and is dropped;
         * wait (time-based, not a microsecond-fast count) for the ARP reply so
         * the datagram actually goes out — a real round-trip takes real ms */
        extern uint64_t mono_ns(void);
        uint64_t deadline = mono_ns() + 1000000000ULL;   /* ~1s, IRQ-independent */
        int rc = -1;
        do {
            rc = udp_tx(txdev, dst_ip, s->local_port, dst_port, kbuf, (uint16_t)len);
            if (rc != -1) break;
            net_poll();
        } while (mono_ns() < deadline);
        r = rc < 0 ? rc : (long)len;
    } else if ((s->type == SOCK_RAW || s->type == SOCK_DGRAM) && s->proto == 1 /* IPPROTO_ICMP */) {
        if (len > SKB_DATA_MAX) len = SKB_DATA_MAX;   /* raw ICMP: single skb */
        if (len >= sizeof(icmp_hdr_t)) {
            icmp_hdr_t *ih = (icmp_hdr_t *)kbuf;
            ih->checksum = 0;
            ih->checksum = inet_cksum(kbuf, len);
            /* time-based ARP resolve (first attempt sends the ARP request; a real
             * reply takes real ms — a 64-spin count finishes in microseconds and
             * gives a spurious ENOSYS before the next-hop is ever resolved) */
            extern uint64_t mono_ns(void);
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

    extern volatile uint64_t kernel_ticks;
    uint64_t rcv_deadline = s->rcv_timeo ? kernel_ticks + s->rcv_timeo : 0;

    /* SOCK_STREAM has no message boundaries: one recv drains as many queued
     * bytes as fit in buf, coalescing across writes. Applies
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
            if (rcv_deadline && kernel_ticks >= rcv_deadline) {
                spin_unlock_irq(&s->recv_lock, f); return -11; /* EAGAIN: SO_RCVTIMEO */
            }
            s->recv_waiter = task_current();
            if (rcv_deadline) task_current()->wake_deadline = rcv_deadline;
            task_sleep_unlock(&s->recv_lock, f);
            task_current()->wake_deadline = 0;
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
        if (rcv_deadline && kernel_ticks >= rcv_deadline) {
            spin_unlock_irq(&s->recv_lock, f); return -11; /* EAGAIN: SO_RCVTIMEO */
        }
        s->recv_waiter = task_current();
        if (rcv_deadline) task_current()->wake_deadline = rcv_deadline;
        task_sleep_unlock(&s->recv_lock, f);
        task_current()->wake_deadline = 0;
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
                    vfs_file_t *vf = fget(p->fds, cfds[i]);   /* atomic in-flight ref under the table lock */
                    if (!vf) { for (int j = 0; j < nf; j++) vfs_file_put(files[j]); return -9; }
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
	if (!s) return -9; /* EBADF */

	if (lv == SOL_RANIX_FILTER && opt == NF_RULE) {
		char line[160];
		if (!val || len == 0 || len >= sizeof(line)) return -22;
		if (copy_from_user(line, val, len) != 0) return -14;
		return nf_ctl_set(line, len);
	}

	int iv = 0;
	if (val && len >= sizeof(int)) {
		if (copy_from_user(&iv, val, sizeof(int)) != 0) return -14;
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
				return -14;
			uint32_t t = tv_to_ticks(&tv);
			if (opt == SO_RCVTIMEO) s->rcv_timeo = t; else s->snd_timeo = t;
			return 0;
		}
		case SO_ERROR: case SO_TYPE: case SO_ACCEPTCONN:
			return -92;   /* ENOPROTOOPT: read-only */
		case SO_DONTROUTE: case SO_BROADCAST: case SO_LINGER:
		case SO_PASSCRED:  case SO_PRIORITY:  case SO_BSDCOMPAT:
		case SO_OOBINLINE:
			return 0;     /* known but no behavioral effect here */
		default:
			return -92;   /* ENOPROTOOPT */
		}
	} else if (lv == SO_IPPROTO_TCP) {
		if (opt == SO_TCP_NODELAY) { s->tcp_nodelay = iv ? 1 : 0; return 0; }
		return -92;
	}
	/* IPPROTO_IP / IPPROTO_IPV6 and the like: accept known multicast/ttl knobs
	 * as no-ops, reject the genuinely unknown. */
	return 0;
}

static long sys_getsockopt(int fd, int lv, int opt, void *val, uint32_t *len)
{
	socket_t *s = fd_to_sock(fd);
	if (!s) return -9;            /* EBADF */
	if (!val || !len) return -14; /* EFAULT */

	uint32_t ulen;
	if (copy_from_user(&ulen, len, sizeof(ulen)) != 0) return -14;

	if (lv == SO_IPPROTO_TCP && opt == SO_TCP_INFO) {
		if (s->type != SOCK_STREAM) return -92;
		uint8_t info[96];
		uint32_t n = tcp_get_info(s, info, ulen < sizeof(info) ? ulen : sizeof(info));
		if (n && copy_to_user(val, info, n) != 0) return -14;
		if (copy_to_user(len, &n, sizeof(n)) != 0) return -14;
		return 0;
	}

	if (lv == SOL_RANIX_FILTER && opt == NF_RULE) {
		char tmp[512];
		int n = nf_ctl_dump(tmp, sizeof(tmp));
		uint32_t cp = ulen < (uint32_t)n ? ulen : (uint32_t)n;
		if (cp && copy_to_user(val, tmp, cp) != 0) return -14;
		uint32_t rn = (uint32_t)n;
		if (copy_to_user(len, &rn, sizeof(rn)) != 0) return -14;
		return 0;
	}

	if (lv == SO_SOL_SOCKET && (opt == SO_RCVTIMEO || opt == SO_SNDTIMEO)) {
		uint32_t t = (opt == SO_RCVTIMEO) ? s->rcv_timeo : s->snd_timeo;
		struct k_timeval tv;
		tv.tv_sec  = t / TIMER_HZ;
		tv.tv_usec = (int64_t)(t % TIMER_HZ) * 1000000 / TIMER_HZ;
		uint32_t cp = ulen < sizeof(tv) ? ulen : sizeof(tv);
		if (cp && copy_to_user(val, &tv, cp) != 0) return -14;
		uint32_t rl = sizeof(tv);
		if (copy_to_user(len, &rl, sizeof(rl)) != 0) return -14;
		return 0;
	}

	int out;
	if (lv == SO_SOL_SOCKET) {
		switch (opt) {
		case SO_TYPE:       out = s->type; break;
		case SO_ERROR:      out = s->so_error; s->so_error = 0; break;  /* read-and-clear */
		case SO_ACCEPTCONN: out = (s->tcp_state == TCP_LISTEN); break;
		case SO_RCVBUF:     out = (int)s->so_rcvbuf; break;
		case SO_SNDBUF:     out = (int)s->so_sndbuf; break;
		case SO_REUSEADDR:  out = s->so_reuseaddr; break;
		case SO_REUSEPORT:  out = s->so_reuseport; break;
		case SO_KEEPALIVE:  out = s->so_keepalive; break;
		default:            return -92;  /* ENOPROTOOPT */
		}
	} else if (lv == SO_IPPROTO_TCP) {
		if (opt == SO_TCP_NODELAY) out = s->tcp_nodelay;
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
    process_t *lp = task_current();
    vfs_file_t *lf = fd_get(lp->fds, fd);
    if (!lf || !lf->inode || lf->inode->fops != &sock_fops) return -9;
    socket_t *ls = (socket_t *)lf->inode->priv;
    if (ls->tcp_state != TCP_LISTEN) return -22;
    int nonblock = (lf->flags & O_NONBLOCK) ? 1 : 0;

    /* wait for a completed connection — tcp_input enqueues it then wakes
     * accept_waiter under recv_lock, so this register + re-check is atomic. */
    uint64_t af = spin_lock_irq(&ls->recv_lock);
    while (!ls->accept_head) {
        if (nonblock) { spin_unlock_irq(&ls->recv_lock, af); return -11; }
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

    int nfd = sock_to_fd(ns, fl & (SOCK_CLOEXEC | SOCK_NONBLOCK));
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
    char *kp = kmalloc(PATH_MAX);
    if (!kp) return -12;
    char *parent_path = kmalloc(PATH_MAX);
    char *kbuf = kmalloc(PATH_MAX);
    if (!parent_path || !kbuf) { kfree(kp); kfree(parent_path); kfree(kbuf); return -12; }

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
    if (!parent || !parent->inode) { ret = -2; goto out; }
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
    if (!in->iops || !in->iops->readlink) {
        if (!in->cached) kfree(in);
        ret = -22; goto out;
    }

    int n = in->iops->readlink(in, kbuf, PATH_MAX);
    if (!in->cached) kfree(in);
    if (n < 0) { ret = n; goto out; }
    size_t slen = strlen(kbuf);
    if (slen > bufsiz) slen = bufsiz;
    copy_to_user(ubuf, kbuf, slen);
    ret = (long)slen;
out:
    kfree(kp); kfree(parent_path); kfree(kbuf);
    return ret;
}

