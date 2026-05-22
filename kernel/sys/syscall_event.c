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
    if (len < 8) return -22;
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
            copy_to_user(buf, &val, 8);
            return 8;
        }
        if (file->flags & O_NONBLOCK) {
            spin_unlock_irq(&efd->lock, f);
            return -11;
        }
        sleep_on_locked(&efd->rd_wait, &efd->lock, f);
    }
}

static ssize_t eventfd_write_vfs(vfs_file_t *file, const void *buf, size_t len)
{
    if (len < 8) return -22;
    eventfd_t *efd = (eventfd_t *)file->inode->priv;
    uint64_t val = 0;
    if (copy_from_user(&val, buf, 8) != 0) return -14;
    if (val == (uint64_t)-1) return -22; /* EINVAL */
    while (1) {
        uint64_t f = spin_lock_irq(&efd->lock);
        if ((uint64_t)-2 - efd->count >= val) {
            efd->count += val;
            wake_up(&efd->rd_wait);
            spin_unlock_irq(&efd->lock, f);
            return 8;
        }
        if (file->flags & O_NONBLOCK) {
            spin_unlock_irq(&efd->lock, f);
            return -11;
        }
        sleep_on_locked(&efd->wr_wait, &efd->lock, f);
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
    vfs_inode_t *in  = file->inode;
    kfree(efd);
    kfree(in);
    return 0;
}

static vfs_fops_t eventfd_fops = {
    .read  = eventfd_read_vfs,
    .write = eventfd_write_vfs,
    .poll  = eventfd_poll_vfs,
    .close = eventfd_close_vfs,
};

static long sys_eventfd2(unsigned int initval, int flags)
{
    eventfd_t *efd = kmalloc(sizeof(*efd));
    if (!efd) return -12;
    efd->count = initval;
    efd->flags = flags;
    efd->lock  = (spinlock_t)SPINLOCK_INIT;
    wait_queue_init(&efd->rd_wait);
    wait_queue_init(&efd->wr_wait);

    vfs_inode_t *in = kmalloc(sizeof(*in));
    if (!in) { kfree(efd); return -12; }
    memset(in, 0, sizeof(*in));
    in->fops     = &eventfd_fops;
    in->priv     = efd;
    in->refcount = 1;

    vfs_file_t *f = kmalloc(sizeof(*f));
    if (!f) { kfree(in); kfree(efd); return -12; }
    memset(f, 0, sizeof(*f));
    f->inode    = in;
    f->flags    = (flags & EFD_NONBLOCK) ? O_NONBLOCK : 0;
    f->refcount = 1;

    process_t *p = task_current();
    int fd = fd_alloc(p->fds, f);
    if (fd < 0) { kfree(f); kfree(in); kfree(efd); return -12; }
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
    extern volatile uint64_t kernel_ticks;
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
    if (len < 8) return -22;
    timerfd_t *t = (timerfd_t *)file->inode->priv;
    while (1) {
        uint64_t f = spin_lock_irq(&t->lock);
        if (t->expirations > 0) {
            uint64_t val = t->expirations;
            t->expirations = 0;
            spin_unlock_irq(&t->lock, f);
            copy_to_user(buf, &val, 8);
            return 8;
        }
        if (file->flags & O_NONBLOCK) {
            spin_unlock_irq(&t->lock, f);
            return -11;
        }
        sleep_on_locked(&t->rd_wait, &t->lock, f);
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
    vfs_inode_t *in = file->inode;
    timerfd_unregister(t);
    kfree(t);
    kfree(in);
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
    timerfd_t *t = kmalloc(sizeof(*t));
    if (!t) return -12;
    memset(t, 0, sizeof(*t));
    t->clockid = clockid;
    t->flags   = flags;
    t->lock    = (spinlock_t)SPINLOCK_INIT;
    wait_queue_init(&t->rd_wait);

    vfs_inode_t *in = kmalloc(sizeof(*in));
    if (!in) { kfree(t); return -12; }
    memset(in, 0, sizeof(*in));
    in->fops     = &timerfd_fops;
    in->priv     = t;
    in->refcount = 1;

    vfs_file_t *f = kmalloc(sizeof(*f));
    if (!f) { kfree(in); kfree(t); return -12; }
    memset(f, 0, sizeof(*f));
    f->inode    = in;
    f->flags    = (flags & O_NONBLOCK) ? O_NONBLOCK : 0;
    f->refcount = 1;

    process_t *p = task_current();
    int fd = fd_alloc(p->fds, f);
    if (fd < 0) { kfree(f); kfree(in); kfree(t); return -12; }

    timerfd_register(t);
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
    if (!t) return -9;
    linux_itimerspec_t kn;
    if (copy_from_user(&kn, unew, sizeof(kn)) != 0) return -14;

    extern volatile uint64_t kernel_ticks;

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
        copy_to_user(uold, &ko, sizeof(ko));
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
    if (!t) return -9;
    extern volatile uint64_t kernel_ticks;

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
    copy_to_user(ucur, &ko, sizeof(ko));
    return 0;
}

/* ── memfd_create ────────────────────────────────────────── */

static long sys_memfd_create(const char *uname, unsigned int flags)
{
    (void)uname; (void)flags;
    /* Create an anonymous tmpfs file backed by ramfs */
    /* Use ramfs to allocate an inode; return an fd */
    extern vfs_inode_t *ramfs_create_anon(void);
    vfs_inode_t *in = ramfs_create_anon();
    if (!in) return -12;

    vfs_file_t *f = kmalloc(sizeof(*f));
    if (!f) { /* can't easily free ramfs inode here; leak minimal */ return -12; }
    memset(f, 0, sizeof(*f));
    f->inode    = in;
    f->flags    = O_RDWR;
    f->refcount = 1;

    process_t *p = task_current();
    int fd = fd_alloc(p->fds, f);
    if (fd < 0) { kfree(f); return -12; }
    return fd;
}

/* ── signalfd (stub: returns fd that is never readable) ─── */

static long sys_signalfd4(int fd, const uint64_t *mask, size_t sigsetsize, int flags)
{
    (void)mask; (void)sigsetsize; (void)flags;
    /* If fd == -1, create a new signalfd (stub, never delivers events).
     * Many programs use signalfd only as a fallback; returning a valid fd
     * that never becomes readable is enough to not crash them. */
    if (fd != -1) return 0; /* modifying existing fd: no-op */
    /* Reuse eventfd infrastructure with count=0, non-writable from outside */
    return sys_eventfd2(0, flags & O_NONBLOCK);
}

