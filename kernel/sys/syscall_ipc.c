/* ── SysV IPC: shared memory + semaphores ─────────────────── */

typedef int32_t key_t;

#define IPC_PRIVATE  0
#define IPC_CREAT    01000
#define IPC_EXCL     02000
#define IPC_RMID     0
#define IPC_SET      1
#define IPC_STAT     2

#define SHM_MAX      32
#define SEM_MAX      32
#define SEM_NSEMS_MAX 32

/* ── SysV Shared Memory ───────────────────────────────────── */

typedef struct {
    key_t    key;
    size_t   size;
    void    *kaddr;   /* kernel virtual address of the shared buffer */
    int      refs;    /* number of active shmat attachments */
    int      valid;
    ipc_namespace_t *ipc_ns;  /* owning IPC namespace */
} shm_seg_t;

static shm_seg_t shm_segs[SHM_MAX];
static spinlock_t shm_lock = SPINLOCK_INIT;
static int shm_next_id = 1;

static int shm_key_to_id(key_t key)
{
    ipc_namespace_t *cur = current_ipc();
    for (int i = 0; i < SHM_MAX; i++)
        if (shm_segs[i].valid && shm_segs[i].key == key && shm_segs[i].ipc_ns == cur)
            return i;
    return -1;
}

static long sys_shmget(key_t key, size_t size, int shmflg)
{
    uint64_t f = spin_lock_irq(&shm_lock);

    if (key != IPC_PRIVATE) {
        int id = shm_key_to_id(key);
        if (id >= 0) {
            if ((shmflg & IPC_CREAT) && (shmflg & IPC_EXCL)) {
                spin_unlock_irq(&shm_lock, f);
                return -17; /* EEXIST */
            }
            spin_unlock_irq(&shm_lock, f);
            return id;
        }
        if (!(shmflg & IPC_CREAT)) {
            spin_unlock_irq(&shm_lock, f);
            return -2; /* ENOENT */
        }
    }

    int slot = -1;
    for (int i = 0; i < SHM_MAX; i++) {
        if (!shm_segs[i].valid) { slot = i; break; }
    }
    if (slot < 0) { spin_unlock_irq(&shm_lock, f); return -28; /* ENOSPC */ }
    if (size == 0) size = PAGE_SIZE;
    size = (size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);

    int order = 0;
    size_t pages = size / PAGE_SIZE;
    while ((size_t)(1 << order) < pages) order++;
    void *phys = buddy_alloc(order);
    if (!phys) { spin_unlock_irq(&shm_lock, f); return -12; }

    shm_segs[slot].key    = key;
    shm_segs[slot].size   = size;
    shm_segs[slot].kaddr  = (void *)PHYS_TO_VIRT(phys);
    shm_segs[slot].refs   = 0;
    shm_segs[slot].valid  = 1;
    shm_segs[slot].ipc_ns = current_ipc();
    int id = slot;
    (void)shm_next_id;
    spin_unlock_irq(&shm_lock, f);
    return id;
}

static long sys_shmat(int shmid, const void *shmaddr, int shmflg)
{
    (void)shmflg;
    uint64_t f = spin_lock_irq(&shm_lock);
    if (shmid < 0 || shmid >= SHM_MAX || !shm_segs[shmid].valid ||
        shm_segs[shmid].ipc_ns != current_ipc()) {
        spin_unlock_irq(&shm_lock, f);
        return -22;
    }
    shm_seg_t *seg = &shm_segs[shmid];
    size_t size = seg->size;
    void *kaddr = seg->kaddr;
    seg->refs++;
    spin_unlock_irq(&shm_lock, f);

    process_t *p = task_current();
    uint64_t va = (uint64_t)shmaddr;
    if (!va) {
        /* kernel picks address */
        va = p->mmap_next;
        if (!va) va = 0x50000000ULL;
        p->mmap_next = va + size + PAGE_SIZE;
    }

    /* map physical pages of kaddr into user VA */
    uint64_t phys = VIRT_TO_PHYS(kaddr);
    uint32_t vmflags = VM_USER | VM_READ | VM_WRITE;
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        vm_map(p->vm, va + off, phys + off, vmflags);
    }

    /* record VMA so munmap / process exit can clean up */
    vma_t *vma = kmalloc(sizeof(*vma));
    if (vma) {
        vma->vaddr  = va;
        vma->size  = size;
        vma->flags = vmflags;
        vma->next  = p->vmas;
        p->vmas    = vma;
    }

    return (long)va;
}

static long sys_shmdt(const void *shmaddr)
{
    process_t *p  = task_current();
    uint64_t   va = (uint64_t)shmaddr;

    /* find and decrement ref for segment at this VA */
    ipc_namespace_t *cur = current_ipc();
    uint64_t fl = spin_lock_irq(&shm_lock);
    for (int i = 0; i < SHM_MAX; i++) {
        if (!shm_segs[i].valid || shm_segs[i].ipc_ns != cur) continue;
        /* check if any VMA at va matches this segment's size */
        vma_t *v = p->vmas;
        while (v) {
            if (v->vaddr == va && v->size == shm_segs[i].size) {
                shm_segs[i].refs--;
                break;
            }
            v = v->next;
        }
    }
    spin_unlock_irq(&shm_lock, fl);

    /* unmap pages from user VA space */
    vma_t *prev = NULL;
    vma_t *vma  = p->vmas;
    while (vma) {
        if (vma->vaddr == va) {
            for (size_t off = 0; off < vma->size; off += PAGE_SIZE)
                vm_unmap(p->vm, va + off);
            if (prev) prev->next = vma->next;
            else      p->vmas   = vma->next;
            kfree(vma);
            return 0;
        }
        prev = vma;
        vma  = vma->next;
    }
    return -22;
}

typedef struct {
    int      uid, gid, cuid, cgid;
    uint16_t mode;
    uint16_t _seq;
    key_t    key;
} ipc_perm_t;

typedef struct {
    ipc_perm_t shm_perm;
    size_t     shm_segsz;
    int64_t    shm_atime, shm_dtime, shm_ctime;
    int        shm_cpid, shm_lpid;
    uint64_t   shm_nattch;
} shmid_ds_t;

static long sys_shmctl(int shmid, int cmd, void *buf)
{
    if (shmid < 0 || shmid >= SHM_MAX) return -22;
    uint64_t f = spin_lock_irq(&shm_lock);
    if (!shm_segs[shmid].valid || shm_segs[shmid].ipc_ns != current_ipc()) {
        spin_unlock_irq(&shm_lock, f); return -22;
    }

    if (cmd == IPC_RMID) {
        if (shm_segs[shmid].refs == 0) {
            size_t pages = shm_segs[shmid].size / PAGE_SIZE;
            int order = 0;
            while ((size_t)(1 << order) < pages) order++;
            buddy_free(VIRT_TO_PHYS(shm_segs[shmid].kaddr), order);
            shm_segs[shmid].valid = 0;
        }
        spin_unlock_irq(&shm_lock, f);
        return 0;
    }
    if (cmd == IPC_STAT && buf) {
        shmid_ds_t ds;
        __builtin_memset(&ds, 0, sizeof(ds));
        ds.shm_segsz  = shm_segs[shmid].size;
        ds.shm_nattch = (uint64_t)shm_segs[shmid].refs;
        spin_unlock_irq(&shm_lock, f);
        return copy_to_user(buf, &ds, sizeof(ds));
    }
    spin_unlock_irq(&shm_lock, f);
    return 0;
}

/* ── SysV Semaphores ──────────────────────────────────────── */

typedef struct {
    int16_t val;
    process_t *waiter;
} sem_t;

typedef struct {
    key_t    key;
    int      nsems;
    int      valid;
    ipc_namespace_t *ipc_ns;  /* owning IPC namespace */
    sem_t    sems[SEM_NSEMS_MAX];
} sem_arr_t;

static sem_arr_t sem_arrs[SEM_MAX];
static spinlock_t sem_lock = SPINLOCK_INIT;

static long sys_semget(key_t key, int nsems, int semflg)
{
    uint64_t f = spin_lock_irq(&sem_lock);
    if (key != IPC_PRIVATE) {
        ipc_namespace_t *cur = current_ipc();
        for (int i = 0; i < SEM_MAX; i++) {
            if (sem_arrs[i].valid && sem_arrs[i].key == key && sem_arrs[i].ipc_ns == cur) {
                if ((semflg & IPC_CREAT) && (semflg & IPC_EXCL)) {
                    spin_unlock_irq(&sem_lock, f);
                    return -17;
                }
                spin_unlock_irq(&sem_lock, f);
                return i;
            }
        }
        if (!(semflg & IPC_CREAT)) { spin_unlock_irq(&sem_lock, f); return -2; }
    }
    int slot = -1;
    for (int i = 0; i < SEM_MAX; i++) {
        if (!sem_arrs[i].valid) { slot = i; break; }
    }
    if (slot < 0) { spin_unlock_irq(&sem_lock, f); return -28; }
    if (nsems <= 0 || nsems > SEM_NSEMS_MAX) { spin_unlock_irq(&sem_lock, f); return -22; }
    __builtin_memset(&sem_arrs[slot], 0, sizeof(sem_arrs[slot]));
    sem_arrs[slot].key    = key;
    sem_arrs[slot].nsems  = nsems;
    sem_arrs[slot].valid  = 1;
    sem_arrs[slot].ipc_ns = current_ipc();
    spin_unlock_irq(&sem_lock, f);
    return slot;
}

typedef struct { uint16_t sem_num; int16_t sem_op; int16_t sem_flg; } sembuf_t;

#define IPC_NOWAIT 04000

static long sys_semop(int semid, const sembuf_t *usops, size_t nsops)
{
    if (semid < 0 || semid >= SEM_MAX) return -22;
    if (nsops == 0 || nsops > SEM_NSEMS_MAX) return -22;

    sembuf_t sops[SEM_NSEMS_MAX];
    if (copy_from_user(sops, usops, nsops * sizeof(sembuf_t)) != 0) return -14;

    while (1) {
        uint64_t f = spin_lock_irq(&sem_lock);
        if (!sem_arrs[semid].valid || sem_arrs[semid].ipc_ns != current_ipc()) {
            spin_unlock_irq(&sem_lock, f); return -22;
        }
        sem_arr_t *arr = &sem_arrs[semid];

        /* check if all ops can proceed */
        int can_proceed = 1;
        for (size_t i = 0; i < nsops; i++) {
            int n = sops[i].sem_num;
            if (n < 0 || n >= arr->nsems) { spin_unlock_irq(&sem_lock, f); return -22; }
            int16_t cur = arr->sems[n].val;
            if (sops[i].sem_op < 0 && cur + sops[i].sem_op < 0) {
                can_proceed = 0;
                break;
            }
        }
        if (can_proceed) {
            for (size_t i = 0; i < nsops; i++)
                arr->sems[sops[i].sem_num].val += sops[i].sem_op;
            /* wake any waiters */
            for (int i = 0; i < arr->nsems; i++) {
                if (arr->sems[i].waiter) {
                    task_wake_no_resched(arr->sems[i].waiter);
                    arr->sems[i].waiter = NULL;
                }
            }
            spin_unlock_irq(&sem_lock, f);
            return 0;
        }
        /* check IPC_NOWAIT on any blocking op */
        for (size_t i = 0; i < nsops; i++) {
            if (sops[i].sem_op < 0 && (sops[i].sem_flg & IPC_NOWAIT)) {
                spin_unlock_irq(&sem_lock, f);
                return -11; /* EAGAIN */
            }
        }
        /* sleep on first blocking semaphore */
        for (size_t i = 0; i < nsops; i++) {
            int n = sops[i].sem_num;
            if (sops[i].sem_op < 0 && arr->sems[n].val + sops[i].sem_op < 0) {
                arr->sems[n].waiter = task_current();
                task_sleep_unlock(&sem_lock, f);
                break;
            }
        }
    }
}

#define SETVAL  16
#define GETVAL   2
#define GETALL   6
#define SETALL  17
#define IPC_STAT 2

static long sys_semctl(int semid, int semnum, int cmd, uint64_t arg)
{
    if (semid < 0 || semid >= SEM_MAX) return -22;
    uint64_t f = spin_lock_irq(&sem_lock);
    if (!sem_arrs[semid].valid || sem_arrs[semid].ipc_ns != current_ipc()) {
        spin_unlock_irq(&sem_lock, f); return -22;
    }
    sem_arr_t *arr = &sem_arrs[semid];
    long ret = 0;
    switch (cmd) {
    case SETVAL:
        if (semnum < 0 || semnum >= arr->nsems) { ret = -22; break; }
        arr->sems[semnum].val = (int16_t)arg;
        /* wake any waiters that might now proceed */
        if (arr->sems[semnum].waiter) {
            task_wake_no_resched(arr->sems[semnum].waiter);
            arr->sems[semnum].waiter = NULL;
        }
        break;
    case GETVAL:
        if (semnum < 0 || semnum >= arr->nsems) { ret = -22; break; }
        ret = arr->sems[semnum].val;
        break;
    case GETALL: {
        int16_t vals[SEM_NSEMS_MAX];
        for (int i = 0; i < arr->nsems; i++) vals[i] = arr->sems[i].val;
        spin_unlock_irq(&sem_lock, f);
        return copy_to_user((void *)arg, vals, (size_t)arr->nsems * sizeof(int16_t)) == 0
               ? 0 : -14;
    }
    case SETALL: {
        int16_t vals[SEM_NSEMS_MAX];
        spin_unlock_irq(&sem_lock, f);
        if (copy_from_user(vals, (const void *)arg, (size_t)arr->nsems * sizeof(int16_t)) != 0)
            return -14;
        f = spin_lock_irq(&sem_lock);
        for (int i = 0; i < arr->nsems; i++) arr->sems[i].val = vals[i];
        break;
    }
    case IPC_RMID:
        arr->valid = 0;
        break;
    default:
        ret = -22;
    }
    spin_unlock_irq(&sem_lock, f);
    return ret;
}

/* ── POSIX Message Queues ─────────────────────────────────── */

#define MQ_MAX      16
#define MQ_MSG_MAX  32
#define MQ_MSGSIZE  512

typedef struct {
    char    buf[MQ_MSGSIZE];
    size_t  len;
    uint32_t prio;
} mq_msg_t;

typedef struct {
    char      name[64];
    int       valid;
    int       flags;
    mq_msg_t  msgs[MQ_MSG_MAX];
    int       head, tail, count;
    process_t *send_waiter;
    process_t *recv_waiter;
    int       refs;
    ipc_namespace_t *ipc_ns;  /* owning IPC namespace */
} mq_t;

static mq_t mq_table[MQ_MAX];
static spinlock_t mq_lock = SPINLOCK_INIT;

typedef struct { long mq_flags, mq_maxmsg, mq_msgsize, mq_curmsgs; } mq_attr_t;
#define O_EXCL   0x80
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

static ssize_t mqfd_read(vfs_file_t *file, void *buf, size_t len)
{
    /* mqd_t is an fd backed by this; receive = pop highest-priority msg */
    mq_t *mq = (mq_t *)file->inode->priv;
    while (1) {
        uint64_t f = spin_lock_irq(&mq_lock);
        if (mq->count > 0) {
            /* find highest priority */
            int best = mq->head;
            int idx  = mq->head;
            for (int i = 0; i < mq->count; i++) {
                int slot = (mq->head + i) % MQ_MSG_MAX;
                if (mq->msgs[slot].prio > mq->msgs[best].prio)
                    best = slot;
                idx = slot;
            }
            (void)idx;
            mq_msg_t *m = &mq->msgs[best];
            size_t copy = m->len < len ? m->len : len;
            spin_unlock_irq(&mq_lock, f);
            if (copy_to_user(buf, m->buf, copy) != 0) return -14;
            f = spin_lock_irq(&mq_lock);
            /* remove best from ring by shifting — simple */
            mq->count--;
            if (mq->send_waiter) {
                task_wake_no_resched(mq->send_waiter);
                mq->send_waiter = NULL;
            }
            spin_unlock_irq(&mq_lock, f);
            return (ssize_t)copy;
        }
        if (file->flags & O_NONBLOCK) { spin_unlock_irq(&mq_lock, f); return -11; }
        mq->recv_waiter = task_current();
        task_sleep_unlock(&mq_lock, f);
    }
}

static ssize_t mqfd_write(vfs_file_t *file, const void *buf, size_t len)
{
    if (len > MQ_MSGSIZE) return -90; /* EMSGSIZE */
    mq_t *mq = (mq_t *)file->inode->priv;
    char tmp[MQ_MSGSIZE];
    if (copy_from_user(tmp, buf, len) != 0) return -14;
    while (1) {
        uint64_t f = spin_lock_irq(&mq_lock);
        if (mq->count < MQ_MSG_MAX) {
            int slot = (mq->head + mq->count) % MQ_MSG_MAX;
            __builtin_memcpy(mq->msgs[slot].buf, tmp, len);
            mq->msgs[slot].len  = len;
            mq->msgs[slot].prio = 0;
            mq->count++;
            if (mq->recv_waiter) {
                task_wake_no_resched(mq->recv_waiter);
                mq->recv_waiter = NULL;
            }
            spin_unlock_irq(&mq_lock, f);
            return (ssize_t)len;
        }
        if (file->flags & O_NONBLOCK) { spin_unlock_irq(&mq_lock, f); return -11; }
        mq->send_waiter = task_current();
        task_sleep_unlock(&mq_lock, f);
    }
}

static int mqfd_poll(vfs_file_t *file)
{
    mq_t *mq = (mq_t *)file->inode->priv;
    int r = 0;
    uint64_t f = spin_lock_irq(&mq_lock);
    if (mq->count > 0)           r |= POLLIN;
    if (mq->count < MQ_MSG_MAX)  r |= POLLOUT;
    spin_unlock_irq(&mq_lock, f);
    return r;
}

static int mqfd_close(vfs_file_t *file)
{
    mq_t *mq = (mq_t *)file->inode->priv;
    uint64_t f = spin_lock_irq(&mq_lock);
    mq->refs--;
    spin_unlock_irq(&mq_lock, f);
    file->inode->priv = NULL;   /* mq is refcounted/pool-managed; vfs_close frees inode + file */
    return 0;
}

static vfs_fops_t mq_fops = {
    .read  = mqfd_read,
    .write = mqfd_write,
    .poll  = mqfd_poll,
    .close = mqfd_close,
};

static long sys_mq_open(const char *uname, int oflag, mode_t mode, mq_attr_t *attr)
{
    (void)mode; (void)attr;
    char kname[64];
    if (strncpy_from_user(kname, uname, sizeof(kname)) < 0) return -14;

    ipc_namespace_t *cur = current_ipc();
    uint64_t f = spin_lock_irq(&mq_lock);
    int slot = -1;
    for (int i = 0; i < MQ_MAX; i++) {
        if (mq_table[i].valid && mq_table[i].ipc_ns == cur &&
            strcmp(mq_table[i].name, kname) == 0) {
            if ((oflag & O_CREAT) && (oflag & O_EXCL)) {
                spin_unlock_irq(&mq_lock, f);
                return -17;
            }
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (!(oflag & O_CREAT)) { spin_unlock_irq(&mq_lock, f); return -2; }
        for (int i = 0; i < MQ_MAX; i++) {
            if (!mq_table[i].valid) { slot = i; break; }
        }
        if (slot < 0) { spin_unlock_irq(&mq_lock, f); return -28; }
        __builtin_memset(&mq_table[slot], 0, sizeof(mq_table[slot]));
        strncpy(mq_table[slot].name, kname, 63);
        mq_table[slot].valid  = 1;
        mq_table[slot].ipc_ns = cur;
    }
    mq_table[slot].refs++;
    spin_unlock_irq(&mq_lock, f);

    vfs_inode_t *in = kmalloc(sizeof(*in));
    if (!in) return -12;
    __builtin_memset(in, 0, sizeof(*in));
    in->fops     = &mq_fops;
    in->priv     = &mq_table[slot];
    in->refcount = 1;

    vfs_file_t *vf = kmalloc(sizeof(*vf));
    if (!vf) { kfree(in); return -12; }
    __builtin_memset(vf, 0, sizeof(*vf));
    vf->inode    = in;
    vf->refcount = 0;   /* fd_alloc takes the +1; close() frees */
    vf->flags    = oflag & O_NONBLOCK;

    process_t *p = task_current();
    int fd = fd_alloc(p->fds, vf);
    if (fd < 0) { kfree(vf); kfree(in); return -12; }
    return fd;
}

static long sys_mq_unlink(const char *uname)
{
    char kname[64];
    if (strncpy_from_user(kname, uname, sizeof(kname)) < 0) return -14;
    ipc_namespace_t *cur = current_ipc();
    uint64_t f = spin_lock_irq(&mq_lock);
    for (int i = 0; i < MQ_MAX; i++) {
        if (mq_table[i].valid && mq_table[i].ipc_ns == cur &&
            strcmp(mq_table[i].name, kname) == 0) {
            if (mq_table[i].refs == 0) mq_table[i].valid = 0;
            spin_unlock_irq(&mq_lock, f);
            return 0;
        }
    }
    spin_unlock_irq(&mq_lock, f);
    return -2;
}

static long sys_mq_timedsend(int mqdes, const char *umsg, size_t len,
                             uint32_t prio, const void *ts)
{
    (void)prio; (void)ts;
    /* reuse write path */
    process_t *p = task_current();
    vfs_file_t *f = fd_get(p->fds, mqdes);
    if (!f || !f->inode || f->inode->fops != &mq_fops) return -9;
    return mqfd_write(f, umsg, len);
}

static long sys_mq_timedreceive(int mqdes, char *umsg, size_t len,
                                uint32_t *uprio, const void *ts)
{
    (void)uprio; (void)ts;
    process_t *p = task_current();
    vfs_file_t *f = fd_get(p->fds, mqdes);
    if (!f || !f->inode || f->inode->fops != &mq_fops) return -9;
    return mqfd_read(f, umsg, len);
}

static long sys_mq_getsetattr(int mqdes, const mq_attr_t *newattr, mq_attr_t *oldattr)
{
    process_t *p = task_current();
    vfs_file_t *f = fd_get(p->fds, mqdes);
    if (!f || !f->inode || f->inode->fops != &mq_fops) return -9;
    mq_t *mq = (mq_t *)f->inode->priv;
    if (oldattr) {
        mq_attr_t a = { .mq_flags = f->flags & O_NONBLOCK,
                        .mq_maxmsg = MQ_MSG_MAX,
                        .mq_msgsize = MQ_MSGSIZE,
                        .mq_curmsgs = mq->count };
        if (copy_to_user(oldattr, &a, sizeof(a)) != 0) return -14;
    }
    if (newattr) {
        mq_attr_t na;
        if (copy_from_user(&na, newattr, sizeof(na)) != 0) return -14;
        if (na.mq_flags & O_NONBLOCK) f->flags |= O_NONBLOCK;
        else                          f->flags &= ~O_NONBLOCK;
    }
    return 0;
}

/* ── IPC namespace lifecycle ──────────────────────────────────
 * A new IPC namespace starts empty; its objects live in the shared tables
 * tagged by owner, and are reclaimed when the namespace's last reference
 * drops. */

ipc_namespace_t init_ipc_ns = { .refcount = 1, .inum = 0xF0000001UL };
static spinlock_t ipc_ns_lock = SPINLOCK_INIT;

ipc_namespace_t *current_ipc(void)
{
    process_t *p = task_current();
    return (p && p->ipc_ns) ? p->ipc_ns : &init_ipc_ns;
}

ipc_namespace_t *ipc_ns_get(ipc_namespace_t *ns)
{
    if (!ns) ns = &init_ipc_ns;
    if (ns != &init_ipc_ns) {
        uint64_t f = spin_lock_irq(&ipc_ns_lock);
        ns->refcount++;
        spin_unlock_irq(&ipc_ns_lock, f);
    }
    return ns;
}

ipc_namespace_t *ipc_ns_clone(ipc_namespace_t *old)
{
    (void)old;
    ipc_namespace_t *ns = kmalloc(sizeof(*ns));
    if (!ns) return NULL;
    ns->refcount = 1;
    ns->inum     = ns_inum_alloc();
    return ns;
}

static void ipc_ns_destroy(ipc_namespace_t *ns)
{
    uint64_t f = spin_lock_irq(&shm_lock);
    for (int i = 0; i < SHM_MAX; i++) {
        if (shm_segs[i].valid && shm_segs[i].ipc_ns == ns) {
            size_t pages = shm_segs[i].size / PAGE_SIZE;
            int order = 0; while ((size_t)(1 << order) < pages) order++;
            buddy_free(VIRT_TO_PHYS(shm_segs[i].kaddr), order);
            shm_segs[i].valid = 0;
        }
    }
    spin_unlock_irq(&shm_lock, f);

    f = spin_lock_irq(&sem_lock);
    for (int i = 0; i < SEM_MAX; i++)
        if (sem_arrs[i].valid && sem_arrs[i].ipc_ns == ns) sem_arrs[i].valid = 0;
    spin_unlock_irq(&sem_lock, f);

    f = spin_lock_irq(&mq_lock);
    for (int i = 0; i < MQ_MAX; i++)
        if (mq_table[i].valid && mq_table[i].ipc_ns == ns) mq_table[i].valid = 0;
    spin_unlock_irq(&mq_lock, f);

    kfree(ns);
}

void ipc_ns_put(ipc_namespace_t *ns)
{
    if (!ns || ns == &init_ipc_ns) return;
    uint64_t f = spin_lock_irq(&ipc_ns_lock);
    int last = (--ns->refcount == 0);
    spin_unlock_irq(&ipc_ns_lock, f);
    if (last) ipc_ns_destroy(ns);
}
