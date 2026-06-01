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
        kfree(in);
        return 0;
    }

    if (s->type == SOCK_STREAM &&
        s->tcp_state != TCP_CLOSED && s->tcp_state != TCP_LISTEN) {
        tcp_close(s);
        while (s->tcp_state != TCP_CLOSED &&
               s->tcp_state != TCP_TIME_WAIT) {
            s->close_waiter = task_current();
            task_sleep();
        }
    }
    sock_free(s);
    kfree(in);
    return 0;
}

static int sock_poll_vfs(vfs_file_t *file)
{
    socket_t *s = (socket_t *)file->inode->priv;
    if (!s) return 0;
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
        uint64_t f = spin_lock_irq(&s->recv_lock);
        skb_t *skb = s->recv_head;
        if (skb) {
            s->recv_head = skb->next;
            if (!s->recv_head) s->recv_tail = NULL;
            spin_unlock_irq(&s->recv_lock, f);
            size_t n = skb->len < len ? skb->len : len;
            if (n) copy_to_user(buf, skb->data, n);
            skb_free(skb);
            return (ssize_t)(n ? n : 0);  /* 0 = EOF (FIN) */
        }
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
        } else {
            while ((rc = tcp_send(s, kbuf, (uint16_t)len)) == -11) {
                s->connect_waiter = task_current();
                task_sleep();
                process_t *p = task_current();
                if (task_has_actionable_signal()) { s->connect_waiter = NULL; kfree(kbuf); return -4; }
            }
        }
        kfree(kbuf);
        return rc < 0 ? rc : (ssize_t)len;
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
    f->refcount = 1;

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
    if (domain != AF_INET && domain != AF_UNIX) return -97;
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

    sockaddr_in_t sa;
    if (copy_from_user(&sa, uaddr, sizeof(sa)) != 0) return -14;
    s->remote_ip   = ntohl(sa.sin_addr);
    s->remote_port = ntohs(sa.sin_port);
    if (s->type == SOCK_DGRAM) return 0;
    if (s->type != SOCK_STREAM) return -38;
    if (!s->dev) return -101; /* ENETUNREACH — no network device to reach the peer */

    int rc = tcp_connect(s);
    if (rc < 0) return rc;

    /* block until ESTABLISHED or RST */
    while (s->tcp_state == TCP_SYN_SENT) {
        s->connect_waiter = task_current();
        task_sleep();
        process_t *p = task_current();
        if (task_has_actionable_signal()) { s->connect_waiter = NULL; return -4; }
    }
    return (s->tcp_state == TCP_ESTABLISHED) ? 0 : -111; /* ECONNREFUSED */
}

static long sys_sendto(int fd, const void *buf, size_t len, int flags,
                       const void *uaddr, uint32_t addrlen)
{
    (void)flags; (void)addrlen;
    socket_t *s = fd_to_sock(fd);
    if (!s || !s->dev) return -9;

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
            task_sleep();
            process_t *p = task_current();
            if (task_has_actionable_signal()) { s->connect_waiter = NULL; kfree(kbuf); return -4; }
        }
        r = rc < 0 ? rc : (long)len;
    } else if (s->type == SOCK_DGRAM) {
        int rc = udp_tx(s->dev, dst_ip, s->local_port, dst_port, kbuf, (uint16_t)len);
        r = rc < 0 ? rc : (long)len;
    } else if (s->type == SOCK_RAW && s->proto == 1 /* IPPROTO_ICMP */) {
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

    while (1) {
        uint64_t f = spin_lock_irq(&s->recv_lock);
        skb_t *skb = s->recv_head;
        if (skb) {
            s->recv_head = skb->next;
            if (!s->recv_head) s->recv_tail = NULL;
            spin_unlock_irq(&s->recv_lock, f);
            size_t n = skb->len < len ? skb->len : len;
            copy_to_user(buf, skb->data, n);
            if (uaddr) {
                sockaddr_in_t sa;
                memset(&sa, 0, sizeof(sa));
                sa.sin_family = AF_INET;
                sa.sin_port   = htons(s->remote_port);
                if (s->type == SOCK_RAW && skb->len >= 20) {
                    /* extract src IP from the prepended IP header (network byte order) */
                    uint32_t src_net;
                    memcpy(&src_net, skb->data + 12, 4);
                    sa.sin_addr = src_net;
                } else {
                    sa.sin_addr = htonl(s->remote_ip);
                }
                copy_to_user(uaddr, &sa, sizeof(sa));
            }
            skb_free(skb);
            return (long)n;
        }
        if (nonblock) { spin_unlock_irq(&s->recv_lock, f); return -11; /* EAGAIN */ }
        s->recv_waiter = task_current();
        task_sleep_unlock(&s->recv_lock, f);
        process_t *p = task_current();
        if (task_has_actionable_signal()) { s->recv_waiter = NULL; return -4; }
    }
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

    /* wait for a completed connection */
    while (!ls->accept_head) {
        ls->accept_waiter = task_current();
        task_sleep();
        process_t *p = task_current();
        if (task_has_actionable_signal()) { ls->accept_waiter = NULL; return -4; }
    }

    /* dequeue from accept list (accept_head chained via accept_head field of child) */
    socket_t *ns = ls->accept_head;
    ls->accept_head = ns->accept_head;
    if (!ls->accept_head) ls->accept_tail = NULL;
    ns->accept_head = NULL;

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

