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

/* ── inotify ─────────────────────────────────────────────── */

#define IN_ACCESS        0x00000001u
#define IN_MODIFY        0x00000002u
#define IN_CREATE        0x00000100u
#define IN_DELETE        0x00000200u
#define IN_MOVED_FROM    0x00000040u
#define IN_MOVED_TO      0x00000080u
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

static void inotify_deliver_event(const char *path, uint32_t mask, const char *name)
{
    uint64_t pl = spin_lock_irq(&inot_pool_lock);
    for (int i = 0; i < INOT_MAX; i++) {
        inotify_t *inst = &inot_pool[i];
        if (!inst->active) continue;
        uint64_t f = spin_lock_irq(&inst->lock);
        for (int w = 0; w < INOT_WATCH_MAX; w++) {
            if (!inst->watches[w].wd) continue;
            if (!(inst->watches[w].mask & mask)) continue;
            if (strncmp(inst->watches[w].path, path, 255) != 0) continue;
            int next = (inst->evt_tail + 1) % INOT_EVT_MAX;
            if (next != inst->evt_head) { /* queue not full */
                inst->evts[inst->evt_tail].wd   = inst->watches[w].wd;
                inst->evts[inst->evt_tail].mask  = mask;
                if (name) strncpy(inst->evts[inst->evt_tail].name, name, 255);
                else      inst->evts[inst->evt_tail].name[0] = '\0';
                inst->evt_tail = next;
                wake_up(&inst->rd_wait);
            }
        }
        spin_unlock_irq(&inst->lock, f);
    }
    spin_unlock_irq(&inot_pool_lock, pl);
}

static ssize_t inotify_read_vfs(vfs_file_t *file, void *buf, size_t len)
{
    inotify_t *inst = (inotify_t *)file->inode->priv;
    if (!inst) return -9;

    while (1) {
        uint64_t f = spin_lock_irq(&inst->lock);
        if (inst->evt_head != inst->evt_tail) {
            inot_evt_t *e = &inst->evts[inst->evt_head];
            /* format: struct inotify_event { int wd; u32 mask; u32 cookie; u32 len; char name[]; } */
            uint32_t namelen = e->name[0] ? (uint32_t)(strlen(e->name) + 1) : 0;
            /* align namelen to 4 */
            uint32_t namelen_aligned = (namelen + 3) & ~3u;
            size_t evsize = 16 + namelen_aligned; /* 4+4+4+4 + name */
            if (len < evsize) { spin_unlock_irq(&inst->lock, f); return -22; }
            uint8_t *p = (uint8_t *)buf;
            copy_to_user(p,      &e->wd,   4);
            copy_to_user(p +  4, &e->mask, 4);
            uint32_t zero = 0;
            copy_to_user(p +  8, &zero, 4); /* cookie */
            copy_to_user(p + 12, &namelen_aligned, 4);
            if (namelen_aligned > 0) {
                char nbuf[256]; memset(nbuf, 0, namelen_aligned < 256 ? namelen_aligned : 256);
                memcpy(nbuf, e->name, namelen < 256 ? namelen : 255);
                copy_to_user(p + 16, nbuf, namelen_aligned < 256 ? namelen_aligned : 255);
            }
            inst->evt_head = (inst->evt_head + 1) % INOT_EVT_MAX;
            spin_unlock_irq(&inst->lock, f);
            return (ssize_t)evsize;
        }
        if (file->flags & O_NONBLOCK) { spin_unlock_irq(&inst->lock, f); return -11; }
        sleep_on_locked(&inst->rd_wait, &inst->lock, f);
        if (task_current()->sig_pending & ~task_current()->sig_mask) return -4;
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
        inst->active = 0;
        spin_unlock_irq(&inot_pool_lock, f);
    }
    kfree(file->inode);
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
    extern vfs_inotify_fn_t vfs_inotify_hook;
    if (!vfs_inotify_hook) vfs_inotify_hook = inotify_deliver_event;

    uint64_t pl = spin_lock_irq(&inot_pool_lock);
    inotify_t *inst = NULL;
    for (int i = 0; i < INOT_MAX; i++) {
        if (!inot_pool[i].active) { inst = &inot_pool[i]; break; }
    }
    if (inst) {
        memset(inst, 0, sizeof(*inst));
        inst->active  = 1;
        inst->wd_next = 1;
        inst->lock    = (spinlock_t)SPINLOCK_INIT;
    }
    spin_unlock_irq(&inot_pool_lock, pl);
    if (!inst) return -24; /* EMFILE */

    vfs_inode_t *in = kmalloc(sizeof(*in));
    if (!in) { inst->active = 0; return -12; }
    memset(in, 0, sizeof(*in));
    in->fops     = &inotify_fops;
    in->priv     = inst;
    in->refcount = 1;

    vfs_file_t *f = kmalloc(sizeof(*f));
    if (!f) { kfree(in); inst->active = 0; return -12; }
    memset(f, 0, sizeof(*f));
    f->inode    = in;
    f->refcount = 1;
    f->flags    = flags & O_NONBLOCK;

    process_t *p = task_current();
    int fd = fd_alloc(p->fds, f);
    if (fd < 0) { kfree(f); kfree(in); inst->active = 0; return -12; }
    if (flags & O_CLOEXEC) fd_set_cloexec(p->fds, fd, 1);
    return fd;
}

static long sys_inotify_add_watch(int ifd, const char *upath, uint32_t mask)
{
    process_t  *p = task_current();
    vfs_file_t *ff = fd_get(p->fds, ifd);
    if (!ff || !ff->inode || ff->inode->fops != &inotify_fops) return -9;
    inotify_t *inst = (inotify_t *)ff->inode->priv;
    if (!inst) return -9;

    char kpath[256];
    if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0) return -14;

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
    if (slot < 0) { spin_unlock_irq(&inst->lock, f); return -28; }
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
    if (!ff || !ff->inode || ff->inode->fops != &inotify_fops) return -9;
    inotify_t *inst = (inotify_t *)ff->inode->priv;
    if (!inst) return -9;

    uint64_t f = spin_lock_irq(&inst->lock);
    for (int i = 0; i < INOT_WATCH_MAX; i++) {
        if (inst->watches[i].wd == wd) {
            inst->watches[i].wd = 0;
            spin_unlock_irq(&inst->lock, f);
            return 0;
        }
    }
    spin_unlock_irq(&inst->lock, f);
    return -22;
}

/* ── signalfd ─────────────────────────────────────────────── */

typedef struct {
    uint64_t   mask;    /* signals monitored */
    int        flags;
    process_t *owner;
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
    if (len < sizeof(signalfd_siginfo_t)) return -22;
    signalfd_t *sfd = (signalfd_t *)file->inode->priv;
    process_t  *p   = sfd->owner;

    while (1) {
        uint64_t pending = p->sig_pending & sfd->mask;
        if (pending) {
            int sig = 1;
            while (sig <= 64 && !((pending >> (sig-1)) & 1)) sig++;
            p->sig_pending &= ~(1ULL << (sig-1));
            signalfd_siginfo_t info;
            __builtin_memset(&info, 0, sizeof(info));
            info.ssi_signo = (uint32_t)sig;
            info.ssi_pid   = (uint32_t)p->pid;
            return copy_to_user(buf, &info, sizeof(info)) == 0
                   ? (ssize_t)sizeof(info) : -14;
        }
        if (sfd->flags & O_NONBLOCK) return -11; /* EAGAIN */
        if (p->sig_pending & ~p->sig_mask) return -4; /* EINTR */
        task_sleep();
    }
}

static int signalfd_poll_vfs(vfs_file_t *file)
{
    signalfd_t *sfd = (signalfd_t *)file->inode->priv;
    return (sfd->owner->sig_pending & sfd->mask) ? POLLIN : 0;
}

static int signalfd_close_vfs(vfs_file_t *file)
{
    signalfd_t  *sfd = (signalfd_t *)file->inode->priv;
    vfs_inode_t *in  = file->inode;
    kfree(sfd);
    kfree(in);
    kfree(file);
    return 0;
}

static vfs_fops_t signalfd_fops = {
    .read  = signalfd_read_vfs,
    .poll  = signalfd_poll_vfs,
    .close = signalfd_close_vfs,
};

static long sys_signalfd4(int fd, const uint64_t *umask, size_t sigsetsize, int flags)
{
    (void)sigsetsize;
    uint64_t kmask = 0;
    if (umask && copy_from_user(&kmask, umask, sizeof(kmask)) != 0) return -14;

    process_t *p = task_current();

    if (fd != -1) {
        /* update mask on existing signalfd */
        vfs_file_t *f = fd_get(p->fds, fd);
        if (!f || !f->inode || f->inode->fops != &signalfd_fops) return -9;
        ((signalfd_t *)f->inode->priv)->mask  = kmask;
        ((signalfd_t *)f->inode->priv)->flags = flags;
        return 0;
    }

    signalfd_t *sfd = kmalloc(sizeof(*sfd));
    if (!sfd) return -12;
    sfd->mask  = kmask;
    sfd->flags = flags;
    sfd->owner = p;

    vfs_inode_t *in = kmalloc(sizeof(*in));
    if (!in) { kfree(sfd); return -12; }
    __builtin_memset(in, 0, sizeof(*in));
    in->fops     = &signalfd_fops;
    in->priv     = sfd;
    in->refcount = 1;

    vfs_file_t *f = kmalloc(sizeof(*f));
    if (!f) { kfree(sfd); kfree(in); return -12; }
    __builtin_memset(f, 0, sizeof(*f));
    f->inode    = in;
    f->refcount = 1;
    if (flags & O_NONBLOCK) f->flags = O_NONBLOCK;

    int newfd = fd_alloc(p->fds, f);
    if (newfd < 0) { kfree(f); kfree(sfd); kfree(in); return -12; }
    return newfd;
}

