/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* ── eventfd ─────────────────────────────────────────────── */

#define EFD_SEMAPHORE 1
#define EFD_NONBLOCK  O_NONBLOCK
#define EFD_CLOEXEC   O_CLOEXEC

typedef struct {
    uint64_t   count;
    int        flags;
    spinlock_t lock;
    wait_queue_t rd_wait;
    wait_queue_t wr_wait;
} eventfd_t;

static ssize_t eventfd_read_vfs(vfs_file_t *file, void *buf, size_t len)
{
    if (len < 8) return -EINVAL;
    eventfd_t *efd = (eventfd_t *)file->inode->priv;
    while (1) {
        uint64_t f = spin_lock_irq(&efd->lock);
        if (efd->count > 0) {
            uint64_t val;
            if (efd->flags & EFD_SEMAPHORE) {
                val = 1;
                efd->count--;
            } else {
                val = efd->count;
                efd->count = 0;
            }
            wake_up(&efd->wr_wait);
            spin_unlock_irq(&efd->lock, f);
            return copy_fop_to_user(buf, &val, 8) ? -14 : 8;
        }
        if (file->flags & O_NONBLOCK) {
            spin_unlock_irq(&efd->lock, f);
            return -EAGAIN;
        }
        sleep_on_locked(&efd->rd_wait, &efd->lock, f);
        if (task_has_actionable_signal()) return -EINTR;
    }
}

static ssize_t eventfd_write_vfs(vfs_file_t *file, const void *buf, size_t len)
{
    if (len < 8) return -EINVAL;
    eventfd_t *efd = (eventfd_t *)file->inode->priv;
    uint64_t val = 0;
    if (copy_fop_from_user(&val, buf, 8) != 0) return -EFAULT;
    if (val == (uint64_t)-1) return -EINVAL;
    while (1) {
        uint64_t f = spin_lock_irq(&efd->lock);
        if ((uint64_t)-2 - efd->count >= val) {
            efd->count += val;
            wake_up(&efd->rd_wait);
            spin_unlock_irq(&efd->lock, f);
            poll_notify_key(efd);
            return 8;
        }
        if (file->flags & O_NONBLOCK) {
            spin_unlock_irq(&efd->lock, f);
            return -EAGAIN;
        }
        sleep_on_locked(&efd->wr_wait, &efd->lock, f);
        if (task_has_actionable_signal()) return -EINTR;
    }
}

static int eventfd_poll_vfs(vfs_file_t *file)
{
    eventfd_t *efd = (eventfd_t *)file->inode->priv;
    uint64_t f = spin_lock_irq(&efd->lock);
    uint64_t cnt = efd->count;
    spin_unlock_irq(&efd->lock, f);
    int r = 0;
    if (cnt > 0)              r |= 0x0001; /* POLLIN  */
    if (cnt < (uint64_t)-2)   r |= 0x0004; /* POLLOUT */
    return r;
}

static int eventfd_close_vfs(vfs_file_t *file)
{
    eventfd_t   *efd = (eventfd_t *)file->inode->priv;
    kfree(efd);
    file->inode->priv = NULL;   /* vfs_close frees the inode (avoid double free) */
    return 0;
}

static vfs_fops_t eventfd_fops = {
    .poll_keyed = 1,
    .read  = eventfd_read_vfs,
    .write = eventfd_write_vfs,
    .poll  = eventfd_poll_vfs,
    .close = eventfd_close_vfs,
};

static long sys_eventfd2(unsigned int initval, int flags)
{
    if (flags & ~(EFD_SEMAPHORE | EFD_NONBLOCK | EFD_CLOEXEC)) return -EINVAL;
    eventfd_t *efd = kmalloc(sizeof(*efd));
    if (!efd) return -ENOMEM;
    efd->count = initval;
    efd->flags = flags;
    efd->lock  = (spinlock_t)SPINLOCK_INIT;
    wait_queue_init(&efd->rd_wait);
    wait_queue_init(&efd->wr_wait);

    vfs_inode_t *in = kmalloc(sizeof(*in));
    if (!in) { kfree(efd); return -ENOMEM; }
    memset(in, 0, sizeof(*in));
    in->fops     = &eventfd_fops;
    in->priv     = efd;
    in->refcount = 1;

    vfs_file_t *f = kmalloc(sizeof(*f));
    if (!f) { kfree(in); kfree(efd); return -ENOMEM; }
    memset(f, 0, sizeof(*f));
    f->inode    = in;
    f->flags    = (flags & EFD_NONBLOCK) ? O_NONBLOCK : 0;
    f->refcount = 0;   /* fd_alloc takes the +1; a single close() then frees */

    process_t *p = task_current();
    int fd = fd_alloc(p->fds, f);
    if (fd < 0) { kfree(f); kfree(in); kfree(efd); return fd == -1 ? -12 : fd; }
    if (flags & EFD_CLOEXEC) fd_set_cloexec(p->fds, fd, 1);
    return fd;
}

/* ── timerfd ─────────────────────────────────────────────── */

typedef struct {
    int        clockid;
    int        flags;
    uint64_t   interval_ns;  /* 0 = one-shot */
    uint64_t   expire_tick;  /* absolute kernel_ticks, 0 = disarmed */
    uint64_t   expirations;  /* accumulated un-read expirations */
    spinlock_t lock;
    wait_queue_t rd_wait;
} timerfd_t;

/* Called from sched_tick — check all timerfds */
void timerfd_tick(void);

static timerfd_t **timerfd_table;
static int         timerfd_cap;
static int         timerfd_cnt;
static spinlock_t  timerfd_lock = SPINLOCK_INIT;

static void timerfd_register(timerfd_t *t)
{
    uint64_t f = spin_lock_irq(&timerfd_lock);
    if (timerfd_cnt >= timerfd_cap) {
        int nc = timerfd_cap ? timerfd_cap * 2 : 16;
        timerfd_t **nb = kmalloc((size_t)nc * sizeof(timerfd_t *));
        if (nb) {
            memset(nb, 0, (size_t)nc * sizeof(timerfd_t *));
            if (timerfd_table) {
                memcpy(nb, timerfd_table,
                       (size_t)timerfd_cap * sizeof(timerfd_t *));
                kfree(timerfd_table);
            }
            timerfd_table = nb;
            timerfd_cap   = nc;
        }
    }
    for (int i = 0; i < timerfd_cap; i++) {
        if (!timerfd_table[i]) { timerfd_table[i] = t; timerfd_cnt++; break; }
    }
    spin_unlock_irq(&timerfd_lock, f);
}

static void timerfd_unregister(timerfd_t *t)
{
    uint64_t f = spin_lock_irq(&timerfd_lock);
    for (int i = 0; i < timerfd_cap; i++) {
        if (timerfd_table[i] == t) { timerfd_table[i] = NULL; timerfd_cnt--; break; }
    }
    spin_unlock_irq(&timerfd_lock, f);
}

void timerfd_tick(void)
{
    uint64_t f = spin_lock_irq(&timerfd_lock);
    for (int i = 0; i < timerfd_cap; i++) {
        timerfd_t *t = timerfd_table[i];
        if (!t || !t->expire_tick) continue;
        if (kernel_ticks < t->expire_tick) continue;

        uint64_t tf2 = spin_lock_irq(&t->lock);
        t->expirations++;
        if (t->interval_ns) {
            uint64_t ticks = (t->interval_ns * TIMER_HZ + 999999999UL) / 1000000000UL;
            if (!ticks) ticks = 1;
            t->expire_tick += ticks;
        } else {
            t->expire_tick = 0;
        }
        wake_up(&t->rd_wait);
        spin_unlock_irq(&t->lock, tf2);
    }
    spin_unlock_irq(&timerfd_lock, f);
}

static ssize_t timerfd_read_vfs(vfs_file_t *file, void *buf, size_t len)
{
    if (len < 8) return -EINVAL;
    timerfd_t *t = (timerfd_t *)file->inode->priv;
    while (1) {
        uint64_t f = spin_lock_irq(&t->lock);
        if (t->expirations > 0) {
            uint64_t val = t->expirations;
            t->expirations = 0;
            spin_unlock_irq(&t->lock, f);
            return copy_fop_to_user(buf, &val, 8) ? -14 : 8;
        }
        if (file->flags & O_NONBLOCK) {
            spin_unlock_irq(&t->lock, f);
            return -EAGAIN;
        }
        sleep_on_locked(&t->rd_wait, &t->lock, f);
        if (task_has_actionable_signal()) return -EINTR;
    }
}

static int timerfd_poll_vfs(vfs_file_t *file)
{
    timerfd_t *t = (timerfd_t *)file->inode->priv;
    uint64_t f = spin_lock_irq(&t->lock);
    uint64_t exp = t->expirations;
    spin_unlock_irq(&t->lock, f);
    return exp ? 0x0001 : 0;
}

static int timerfd_close_vfs(vfs_file_t *file)
{
    timerfd_t   *t  = (timerfd_t *)file->inode->priv;
    timerfd_unregister(t);
    kfree(t);
    file->inode->priv = NULL;   /* vfs_close frees the inode (avoid double free) */
    return 0;
}

static vfs_fops_t timerfd_fops = {
    .read  = timerfd_read_vfs,
    .poll  = timerfd_poll_vfs,
    .close = timerfd_close_vfs,
};

typedef struct { int32_t tv_sec; int32_t tv_nsec; } linux_itimerspec_old_t;
typedef struct { linux_timespec_t it_interval; linux_timespec_t it_value; } linux_itimerspec_t;

static long sys_timerfd_create(int clockid, int flags)
{
    if (clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC && clockid != CLOCK_BOOTTIME)
        return -EINVAL;
    if (flags & ~(O_NONBLOCK | O_CLOEXEC)) return -EINVAL;
    timerfd_t *t = kmalloc(sizeof(*t));
    if (!t) return -ENOMEM;
    memset(t, 0, sizeof(*t));
    t->clockid = clockid;
    t->flags   = flags;
    t->lock    = (spinlock_t)SPINLOCK_INIT;
    wait_queue_init(&t->rd_wait);

    vfs_inode_t *in = kmalloc(sizeof(*in));
    if (!in) { kfree(t); return -ENOMEM; }
    memset(in, 0, sizeof(*in));
    in->fops     = &timerfd_fops;
    in->priv     = t;
    in->refcount = 1;

    vfs_file_t *f = kmalloc(sizeof(*f));
    if (!f) { kfree(in); kfree(t); return -ENOMEM; }
    memset(f, 0, sizeof(*f));
    f->inode    = in;
    f->flags    = (flags & O_NONBLOCK) ? O_NONBLOCK : 0;
    f->refcount = 0;   /* fd_alloc takes the +1; a single close() then frees */

    /* Before the fd exists: a sibling closing it first would free t, and the
     * registration would leave the tick walking a dangling pointer. */
    timerfd_register(t);

    process_t *p = task_current();
    int fd = fd_alloc(p->fds, f);
    if (fd < 0) {
        timerfd_unregister(t);
        kfree(f); kfree(in); kfree(t);
        return fd == -1 ? -12 : fd;
    }
    if (flags & O_CLOEXEC) fd_set_cloexec(p->fds, fd, 1);
    return fd;
}

static timerfd_t *fd_to_timerfd(int fd)
{
    process_t  *p = task_current();
    vfs_file_t *f = fd_get(p->fds, fd);
    if (!f || !f->inode || f->inode->fops != &timerfd_fops) return NULL;
    return (timerfd_t *)f->inode->priv;
}

static long sys_timerfd_settime(int fd, int flags,
                                const linux_itimerspec_t *unew,
                                linux_itimerspec_t *uold)
{
    timerfd_t *t = fd_to_timerfd(fd);
    if (!t) return -EBADF;
    linux_itimerspec_t kn;
    if (copy_from_user(&kn, unew, sizeof(kn)) != 0) return -EFAULT;
    if (flags & ~3) return -EINVAL;
    if (kn.it_value.tv_sec < 0 || kn.it_interval.tv_sec < 0 ||
        (uint64_t)kn.it_value.tv_nsec >= 1000000000UL ||
        (uint64_t)kn.it_interval.tv_nsec >= 1000000000UL) return -EINVAL;


    uint64_t new_interval_ns = (uint64_t)kn.it_interval.tv_sec * 1000000000UL
                               + (uint64_t)kn.it_interval.tv_nsec;
    uint64_t new_value_ns    = (uint64_t)kn.it_value.tv_sec    * 1000000000UL
                               + (uint64_t)kn.it_value.tv_nsec;

    if (uold) {
        linux_itimerspec_t ko = {{0,0},{0,0}};
        /* return old value (approximate — we store absolute tick) */
        if (t->expire_tick > kernel_ticks) {
            uint64_t rem_ns = ((t->expire_tick - kernel_ticks) * 1000000000UL) / TIMER_HZ;
            ko.it_value.tv_sec  = (int64_t)(rem_ns / 1000000000UL);
            ko.it_value.tv_nsec = (int64_t)(rem_ns % 1000000000UL);
        }
        ko.it_interval.tv_sec  = (int64_t)(t->interval_ns / 1000000000UL);
        ko.it_interval.tv_nsec = (int64_t)(t->interval_ns % 1000000000UL);
        if (copy_to_user(uold, &ko, sizeof(ko)) != 0) return -EFAULT;
    }

    uint64_t lf = spin_lock_irq(&t->lock);
    t->interval_ns = new_interval_ns;
    t->expirations = 0;
    if (new_value_ns == 0) {
        t->expire_tick = 0;  /* disarm */
    } else {
        uint64_t ticks = (new_value_ns * TIMER_HZ + 999999999UL) / 1000000000UL;
        if (!ticks) ticks = 1;
        if (flags & 1 /* TFD_TIMER_ABSTIME */) {
            /* absolute: convert from ns-since-boot to ticks */
            t->expire_tick = (new_value_ns * TIMER_HZ + 999999999UL) / 1000000000UL;
        } else {
            t->expire_tick = kernel_ticks + ticks;
        }
    }
    spin_unlock_irq(&t->lock, lf);
    return 0;
}

static long sys_timerfd_gettime(int fd, linux_itimerspec_t *ucur)
{
    timerfd_t *t = fd_to_timerfd(fd);
    if (!t) return -EBADF;

    linux_itimerspec_t ko = {{0,0},{0,0}};
    uint64_t lf = spin_lock_irq(&t->lock);
    if (t->expire_tick > kernel_ticks) {
        uint64_t rem_ns = ((t->expire_tick - kernel_ticks) * 1000000000UL) / TIMER_HZ;
        ko.it_value.tv_sec  = (int64_t)(rem_ns / 1000000000UL);
        ko.it_value.tv_nsec = (int64_t)(rem_ns % 1000000000UL);
    }
    ko.it_interval.tv_sec  = (int64_t)(t->interval_ns / 1000000000UL);
    ko.it_interval.tv_nsec = (int64_t)(t->interval_ns % 1000000000UL);
    spin_unlock_irq(&t->lock, lf);
    return copy_to_user(ucur, &ko, sizeof(ko)) ? -14 : 0;
}

/* ── memfd_create ────────────────────────────────────────── */

static long sys_memfd_create(const char *uname, unsigned int flags)
{
    char ign[2];
    if (flags & ~7u) return -EINVAL;   /* CLOEXEC, ALLOW_SEALING, HUGETLB */
    if (strncpy_from_user(ign, uname, sizeof(ign)) < 0) return -EFAULT;
    /* Create an anonymous tmpfs file backed by ramfs */
    /* Use ramfs to allocate an inode; return an fd */
    vfs_inode_t *in = ramfs_create_anon();
    if (!in) return -ENOMEM;

    vfs_file_t *f = kmalloc(sizeof(*f));
    if (!f) { in->iops->evict(in); return -ENOMEM; }
    memset(f, 0, sizeof(*f));
    f->inode    = in;
    f->flags    = O_RDWR;
    f->refcount = 0;   /* fd_alloc takes the +1; close() frees */

    process_t *p = task_current();
    /* before the fd is visible: a sibling's close drops this count */
    atomic_fetch_add32((volatile uint32_t *)&in->open_refs, 1);
    int fd = fd_alloc(p->fds, f);
    if (fd < 0) {
        atomic_sub_fetch32((volatile uint32_t *)&in->open_refs, 1);
        in->iops->evict(in);
        kfree(f);
        return fd == -1 ? -12 : fd;
    }
    if (flags & 1) fd_set_cloexec(p->fds, fd, 1);   /* MFD_CLOEXEC */
    return fd;
}

/* ── inotify ─────────────────────────────────────────────── */

#define IN_ACCESS        0x00000001u
#define IN_MODIFY        0x00000002u
#define IN_CREATE        0x00000100u
#define IN_DELETE        0x00000200u
#define IN_MOVED_FROM    0x00000040u
#define IN_MOVED_TO      0x00000080u
#define IN_DELETE_SELF   0x00000400u
#define IN_MOVE_SELF     0x00000800u
#define IN_IGNORED       0x00008000u
#define IN_ALL_EVENTS    0x00000ffffu

#define INOT_MAX         8    /* max inotify instances */
#define INOT_WATCH_MAX   32
#define INOT_EVT_MAX     64

typedef struct {
    int      wd;
    char     path[256];
    uint32_t mask;
} inot_watch_t;

typedef struct {
    int        wd;
    uint32_t   mask;
    uint32_t   cookie;     /* the rename a MOVED_FROM/MOVED_TO belongs to */
    char       name[256];
} inot_evt_t;

typedef struct {
    int          active;
    int          wd_next;
    inot_watch_t watches[INOT_WATCH_MAX];
    spinlock_t   lock;
    wait_queue_t rd_wait;
    inot_evt_t   evts[INOT_EVT_MAX];
    int          evt_head;
    int          evt_tail;
} inotify_t;

static inotify_t inot_pool[INOT_MAX];
static spinlock_t inot_pool_lock = SPINLOCK_INIT;

/* caller holds inst->lock; a full queue drops the event */
static void inot_queue(inotify_t *inst, int wd, uint32_t mask, uint32_t cookie, const char *name)
{
    int next = (inst->evt_tail + 1) % INOT_EVT_MAX;
    if (next == inst->evt_head) return;
    inot_evt_t *e = &inst->evts[inst->evt_tail];
    e->wd = wd; e->mask = mask; e->cookie = cookie;
    if (name) { strncpy(e->name, name, 255); e->name[255] = '\0'; }
    else      e->name[0] = '\0';
    inst->evt_tail = next;
    wake_up(&inst->rd_wait);
}

/* An event on path, for watches on path itself (name NULL) or on the directory
 * it is in (name set). Two are about the watch: DELETE_SELF ends it (IN_IGNORED
 * follows), and MOVE_SELF, whose name is the new path, moves it and every
 * watch below it there. */
static void inotify_deliver_canon(const char *path, uint32_t mask, const char *name);

/* watches hold canonical paths ("dir/./x" is "dir/x"), so events are made so too */
static void inotify_deliver_event(const char *path, uint32_t mask, const char *name)
{
    size_t n = strlen(path);
    char *p = n < PATH_MAX ? kmalloc(PATH_MAX) : NULL;
    if (!p) return;
    memcpy(p, path, n + 1);
    path_normalize(p);
    char *np = NULL;
    if ((mask & IN_ALL_EVENTS) == IN_MOVE_SELF && name && (np = kmalloc(PATH_MAX))) {
        strncpy(np, name, PATH_MAX - 1); np[PATH_MAX - 1] = '\0';
        path_normalize(np);
        name = np;
    }
    inotify_deliver_canon(p, mask, name);
    kfree(np);
    kfree(p);
}

static void inotify_deliver_canon(const char *path, uint32_t mask, const char *name)
{
    uint32_t ev = mask & IN_ALL_EVENTS;
    uint32_t cookie = (ev & (IN_MOVED_FROM | IN_MOVED_TO)) ? vfs_inotify_cookie : 0;
    size_t pl_len = strlen(path);
    uint64_t pl = spin_lock_irq(&inot_pool_lock);
    for (int i = 0; i < INOT_MAX; i++) {
        inotify_t *inst = &inot_pool[i];
        if (!inst->active) continue;
        uint64_t f = spin_lock_irq(&inst->lock);
        for (int w = 0; w < INOT_WATCH_MAX; w++) {
            inot_watch_t *wt = &inst->watches[w];
            if (!wt->wd) continue;
            if (ev == IN_MOVE_SELF) {
                int self = strcmp(wt->path, path) == 0;
                int below = !self && strncmp(wt->path, path, pl_len) == 0 && wt->path[pl_len] == '/';
                if (self && (wt->mask & IN_MOVE_SELF)) inot_queue(inst, wt->wd, IN_MOVE_SELF, 0, NULL);
                if ((self || below) && name) {
                    char np[256];
                    size_t nl = strlen(name), rest = strlen(wt->path + pl_len);
                    if (nl + rest < sizeof(np)) {
                        memcpy(np, name, nl);
                        memcpy(np + nl, wt->path + pl_len, rest + 1);
                        memcpy(wt->path, np, nl + rest + 1);
                    }
                }
                continue;
            }
            if (strncmp(wt->path, path, 255) != 0) continue;
            if (ev == IN_DELETE_SELF) {
                if (wt->mask & IN_DELETE_SELF) inot_queue(inst, wt->wd, IN_DELETE_SELF, 0, NULL);
                inot_queue(inst, wt->wd, IN_IGNORED, 0, NULL);
                wt->wd = 0;
                continue;
            }
            if (!(wt->mask & ev)) continue;
            inot_queue(inst, wt->wd, mask, cookie, name);
        }
        spin_unlock_irq(&inst->lock, f);
    }
    spin_unlock_irq(&inot_pool_lock, pl);
}

/* As many whole events as fit in len, as Linux returns them; EINVAL if not
 * even the first does. */
static ssize_t inotify_read_vfs(vfs_file_t *file, void *buf, size_t len)
{
    inotify_t *inst = (inotify_t *)file->inode->priv;
    if (!inst) return -EBADF;
    size_t cap = len < INOT_EVT_MAX * (16 + 256) ? len : INOT_EVT_MAX * (16 + 256);
    uint8_t *kb = kmalloc(cap ? cap : 1);
    if (!kb) return -ENOMEM;
    while (1) {
        uint64_t f = spin_lock_irq(&inst->lock);
        size_t out = 0;
        while (inst->evt_head != inst->evt_tail) {
            inot_evt_t *e = &inst->evts[inst->evt_head];
            /* struct inotify_event { int wd; u32 mask; u32 cookie; u32 len; char name[]; },
             * the name NUL-padded to a multiple of 4 */
            uint32_t namelen = e->name[0] ? (uint32_t)(strlen(e->name) + 1) : 0;
            uint32_t padded = (namelen + 3) & ~3u;
            size_t evsize = 16 + padded;
            if (out + evsize > cap) break;   /* staged: the copy out can fault, this lock is IRQ-off */
            uint8_t *ev = kb + out;
            memset(ev, 0, evsize);
            memcpy(ev,      &e->wd,     4);
            memcpy(ev +  4, &e->mask,   4);
            memcpy(ev +  8, &e->cookie, 4);
            memcpy(ev + 12, &padded,    4);
            if (namelen) memcpy(ev + 16, e->name, namelen);
            inst->evt_head = (inst->evt_head + 1) % INOT_EVT_MAX;
            out += evsize;
        }
        int pending = inst->evt_head != inst->evt_tail;
        if (out) {
            spin_unlock_irq(&inst->lock, f);
            ssize_t r = copy_fop_to_user(buf, kb, out) ? -14 : (ssize_t)out;
            kfree(kb);
            return r;
        }
        if (pending) { spin_unlock_irq(&inst->lock, f); kfree(kb); return -EINVAL; }   /* first one too big */
        if (file->flags & O_NONBLOCK) { spin_unlock_irq(&inst->lock, f); kfree(kb); return -EAGAIN; }
        sleep_on_locked(&inst->rd_wait, &inst->lock, f);
        if (task_has_actionable_signal()) { kfree(kb); return -EINTR; }
    }
}

static int inotify_poll_vfs(vfs_file_t *file)
{
    inotify_t *inst = (inotify_t *)file->inode->priv;
    if (!inst) return 0;
    uint64_t f = spin_lock_irq(&inst->lock);
    int r = (inst->evt_head != inst->evt_tail) ? 1 : 0;
    spin_unlock_irq(&inst->lock, f);
    return r;
}

static int inotify_close_vfs(vfs_file_t *file)
{
    inotify_t *inst = (inotify_t *)file->inode->priv;
    if (inst) {
        uint64_t f = spin_lock_irq(&inot_pool_lock);
        if (inst->active) __atomic_sub_fetch(&vfs_inotify_users, 1, __ATOMIC_RELAXED);
        inst->active = 0;
        spin_unlock_irq(&inot_pool_lock, f);
    }
    file->inode->priv = NULL;   /* inst is pool-managed; vfs_close frees the inode */
    return 0;
}

static vfs_fops_t inotify_fops = {
    .read  = inotify_read_vfs,
    .poll  = inotify_poll_vfs,
    .close = inotify_close_vfs,
};

static long sys_inotify_init1(int flags)
{
    /* register the VFS hook on first call */
    if (!vfs_inotify_hook) vfs_inotify_hook = inotify_deliver_event;

    uint64_t pl = spin_lock_irq(&inot_pool_lock);
    inotify_t *inst = NULL;
    for (int i = 0; i < INOT_MAX; i++) {
        if (!inot_pool[i].active) { inst = &inot_pool[i]; break; }
    }
    if (inst) {
        memset(inst, 0, sizeof(*inst));
        inst->active  = 1;
        __atomic_add_fetch(&vfs_inotify_users, 1, __ATOMIC_RELAXED);
        inst->wd_next = 1;
        inst->lock    = (spinlock_t)SPINLOCK_INIT;
    }
    spin_unlock_irq(&inot_pool_lock, pl);
    if (!inst) return -EMFILE;

    vfs_inode_t *in = kmalloc(sizeof(*in));
    if (!in) { inst->active = 0; __atomic_sub_fetch(&vfs_inotify_users, 1, __ATOMIC_RELAXED); return -ENOMEM; }
    memset(in, 0, sizeof(*in));
    in->fops     = &inotify_fops;
    in->priv     = inst;
    in->refcount = 1;

    vfs_file_t *f = kmalloc(sizeof(*f));
    if (!f) { kfree(in); inst->active = 0; __atomic_sub_fetch(&vfs_inotify_users, 1, __ATOMIC_RELAXED); return -ENOMEM; }
    memset(f, 0, sizeof(*f));
    f->inode    = in;
    f->refcount = 0;   /* fd_alloc takes the +1; close() frees */
    f->flags    = flags & O_NONBLOCK;

    process_t *p = task_current();
    int fd = fd_alloc(p->fds, f);
    if (fd < 0) { kfree(f); kfree(in); inst->active = 0; __atomic_sub_fetch(&vfs_inotify_users, 1, __ATOMIC_RELAXED); return fd == -1 ? -12 : fd; }
    if (flags & O_CLOEXEC) fd_set_cloexec(p->fds, fd, 1);
    return fd;
}

static long sys_inotify_add_watch(int ifd, const char *upath, uint32_t mask)
{
    process_t  *p = task_current();
    vfs_file_t *ff = fd_get(p->fds, ifd);
    if (!ff || !ff->inode || ff->inode->fops != &inotify_fops) return -EBADF;
    inotify_t *inst = (inotify_t *)ff->inode->priv;
    if (!inst) return -EBADF;

    if (!(mask & 0x00000fffu)) return -EINVAL;   /* no event requested */
    /* events carry the absolute path, so a relative one would never match */
    char kpath[PATH_MAX];
    int rp = resolve_path(kpath, AT_FDCWD, upath);
    if (rp) return rp;
    path_normalize(kpath);   /* "." is the directory: events carry canonical paths */
    if (strlen(kpath) >= 256) return -ENAMETOOLONG;    /* inot_watch_t.path */
    long ar = access_check(kpath, 4, AT_EACCESS);     /* a watch reads the directory's names */
    if (ar) return ar;

    uint64_t f = spin_lock_irq(&inst->lock);
    /* check if path already watched — update mask */
    for (int i = 0; i < INOT_WATCH_MAX; i++) {
        if (inst->watches[i].wd && strcmp(inst->watches[i].path, kpath) == 0) {
            inst->watches[i].mask = mask;
            int wd = inst->watches[i].wd;
            spin_unlock_irq(&inst->lock, f);
            return wd;
        }
    }
    /* find free slot */
    int slot = -1;
    for (int i = 0; i < INOT_WATCH_MAX; i++) {
        if (!inst->watches[i].wd) { slot = i; break; }
    }
    if (slot < 0) { spin_unlock_irq(&inst->lock, f); return -ENOSPC; }
    int wd = inst->wd_next++;
    inst->watches[slot].wd   = wd;
    inst->watches[slot].mask = mask;
    strncpy(inst->watches[slot].path, kpath, 255);
    inst->watches[slot].path[255] = '\0';
    spin_unlock_irq(&inst->lock, f);
    return wd;
}

static long sys_inotify_rm_watch(int ifd, int wd)
{
    process_t  *p = task_current();
    vfs_file_t *ff = fd_get(p->fds, ifd);
    if (!ff || !ff->inode || ff->inode->fops != &inotify_fops) return -EBADF;
    inotify_t *inst = (inotify_t *)ff->inode->priv;
    if (!inst) return -EBADF;

    uint64_t f = spin_lock_irq(&inst->lock);
    for (int i = 0; i < INOT_WATCH_MAX; i++) {
        if (inst->watches[i].wd == wd) {
            inst->watches[i].wd = 0;
            spin_unlock_irq(&inst->lock, f);
            return 0;
        }
    }
    spin_unlock_irq(&inst->lock, f);
    return -EINVAL;
}

/* ── signalfd ─────────────────────────────────────────────── */

typedef struct {
    uint64_t   mask;    /* signals monitored */
    int        flags;
} signalfd_t;

/* signalfd_siginfo: 128-byte struct passed to read() */
typedef struct {
    uint32_t ssi_signo;
    int32_t  ssi_errno;
    int32_t  ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    int32_t  ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t  ssi_status;
    int32_t  ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint16_t ssi_addr_lsb;
    uint16_t _pad2;
    int32_t  ssi_syscall;
    uint64_t ssi_call_addr;
    uint32_t ssi_arch;
    uint8_t  _pad[28];
} __attribute__((packed)) signalfd_siginfo_t;

static ssize_t signalfd_read_vfs(vfs_file_t *file, void *buf, size_t len)
{
    if (len < sizeof(signalfd_siginfo_t)) return -EINVAL;
    signalfd_t *sfd = (signalfd_t *)file->inode->priv;
    /* signalfd dequeues signals pending for the *reading* task,
     * not for whoever created the fd — the creator may have exited and been reaped
     * while a fork/SCM_RIGHTS child still holds the inherited fd. */
    process_t  *p   = task_current();

    while (1) {
        /* find+clear under sched_lock so the clear can't race (and lose) a bit a
         * concurrent poster / nested timer tick OR's into sig_pending. */
        uint64_t sf = spin_lock_irq(&sched_lock);
        uint64_t pending = p->sig_pending & sfd->mask;
        int sig = 0;
        if (pending) {
            sig = 1;
            while (sig <= 64 && !((pending >> (sig-1)) & 1)) sig++;
            p->sig_pending &= ~(1ULL << (sig-1));
            sig_from_forget(p, 1ULL << (sig-1));
        }
        spin_unlock_irq(&sched_lock, sf);
        if (sig) {
            signalfd_siginfo_t info;
            __builtin_memset(&info, 0, sizeof(info));
            info.ssi_signo = (uint32_t)sig;
            info.ssi_pid   = (uint32_t)p->pid;
            return copy_fop_to_user(buf, &info, sizeof(info)) == 0
                   ? (ssize_t)sizeof(info) : -14;
        }
        if (sfd->flags & O_NONBLOCK) return -EAGAIN;
        if (task_has_actionable_signal()) return -EINTR;
        /* event-driven: task_signal wakes the reader when a signal is posted; the
         * poll_block backstop re-checks otherwise. Off the per-tick blanket wake. */
        poll_arm();
        poll_block((uint64_t)-1);
    }
}

static int signalfd_poll_vfs(vfs_file_t *file)
{
    signalfd_t *sfd = (signalfd_t *)file->inode->priv;
    return (task_current()->sig_pending & sfd->mask) ? POLLIN : 0;
}

static int signalfd_close_vfs(vfs_file_t *file)
{
    signalfd_t  *sfd = (signalfd_t *)file->inode->priv;
    kfree(sfd);
    file->inode->priv = NULL;   /* vfs_close frees the inode and the file (avoid double free) */
    return 0;
}

static vfs_fops_t signalfd_fops = {
    .read  = signalfd_read_vfs,
    .poll  = signalfd_poll_vfs,
    .close = signalfd_close_vfs,
};

static long sys_signalfd4(int fd, const uint64_t *umask, size_t sigsetsize, int flags)
{
    if (sigsetsize != 8 || (flags & ~(O_NONBLOCK | O_CLOEXEC))) return -EINVAL;
    uint64_t kmask = 0;
    if (umask && copy_from_user(&kmask, umask, sizeof(kmask)) != 0) return -EFAULT;
    /* a read clears what it reports: SIGKILL must not be consumed that way */
    kmask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));

    process_t *p = task_current();

    if (fd != -1) {
        /* update mask on existing signalfd */
        vfs_file_t *f = fd_get(p->fds, fd);
        if (!f || !f->inode || f->inode->fops != &signalfd_fops) return -EBADF;
        ((signalfd_t *)f->inode->priv)->mask  = kmask;
        ((signalfd_t *)f->inode->priv)->flags = flags;
        return 0;
    }

    signalfd_t *sfd = kmalloc(sizeof(*sfd));
    if (!sfd) return -ENOMEM;
    sfd->mask  = kmask;
    sfd->flags = flags;

    vfs_inode_t *in = kmalloc(sizeof(*in));
    if (!in) { kfree(sfd); return -ENOMEM; }
    __builtin_memset(in, 0, sizeof(*in));
    in->fops     = &signalfd_fops;
    in->priv     = sfd;
    in->refcount = 1;

    vfs_file_t *f = kmalloc(sizeof(*f));
    if (!f) { kfree(sfd); kfree(in); return -ENOMEM; }
    __builtin_memset(f, 0, sizeof(*f));
    f->inode    = in;
    f->refcount = 0;   /* fd_alloc takes the +1; a single close() then frees */
    if (flags & O_NONBLOCK) f->flags = O_NONBLOCK;

    int newfd = fd_alloc(p->fds, f);
    if (newfd < 0) { kfree(f); kfree(sfd); kfree(in); return newfd == -1 ? -12 : newfd; }
    if (flags & O_CLOEXEC) fd_set_cloexec(p->fds, newfd, 1);
    return newfd;
}

