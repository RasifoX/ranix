/* ── SysV IPC: shared memory + semaphores ─────────────────── */

typedef int32_t key_t;

#define IPC_PRIVATE  0
#define IPC_CREAT    01000
#define IPC_EXCL     02000
#define IPC_RMID     0
#define IPC_SET      1
#define IPC_STAT     2

#define SHM_MAX      32
#define SHM_NS_MAX   8    /* slots any namespace but the first may hold */
#define SEM_MAX      32
#define SEM_NSEMS_MAX 32
#define SEM_NS_MAX   8    /* arrays any namespace but the first may hold */
#define SEMVMX       32767

/* ── SysV Shared Memory ───────────────────────────────────── */

typedef struct {
    key_t    key;
    size_t   size;
    void    *kaddr;   /* kernel virtual address of the shared buffer */
    int      refs;    /* number of active shmat attachments */
    int      valid;
    ipc_namespace_t *ipc_ns;  /* owning IPC namespace */
    void    *cg;      /* cgroup paying for it */
    uid_t    uid;     /* creator; these objects are reachable by any task in the */
    gid_t    gid;     /* namespace that guesses a small id, so they need these */
    int      mode;
} shm_seg_t;

static shm_seg_t shm_segs[SHM_MAX];
static spinlock_t shm_lock = SPINLOCK_INIT;
static int shm_next_id = 1;

/* The caller's right to a SysV object. want: 4 read, 2 write. */
static int ipc_may(uid_t uid, gid_t gid, int mode, int want)
{
    process_t *p = task_current();
    if (p->euid == 0) return 1;
    int shift = p->euid == uid ? 6 : (p->egid == gid ? 3 : 0);
    return ((mode >> shift) & want) == want;
}

/* Only the owner, or root, may remove or reconfigure one. */
static int ipc_owner(uid_t uid)
{
    process_t *p = task_current();
    return p->euid == 0 || p->euid == uid;
}

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
    if (current_ipc() != &init_ipc_ns) {
        int held = 0;
        for (int i = 0; i < SHM_MAX; i++)
            held += shm_segs[i].valid && shm_segs[i].ipc_ns == current_ipc();
        if (held >= SHM_NS_MAX) { spin_unlock_irq(&shm_lock, f); return -28; }
    }
    if (size == 0) size = PAGE_SIZE;
    /* one contiguous buddy block; rounding and the order loop below must not wrap */
    if (size > ((size_t)PAGE_SIZE << (BUDDY_MAX_ORDER - 1))) {
        spin_unlock_irq(&shm_lock, f);
        return -22;
    }
    size = (size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);

    int order = 0;
    size_t pages = size / PAGE_SIZE;
    while ((size_t)(1 << order) < pages) order++;
    void *cg = NULL;
    long block = PAGE_SIZE << order;
    if (cgroup_charge_file(&cg, 0, block) != 0) { spin_unlock_irq(&shm_lock, f); return -12; }
    void *phys = buddy_alloc(order);
    if (!phys) { cgroup_file_release(cg, block); spin_unlock_irq(&shm_lock, f); return -12; }
    memset((void *)PHYS_TO_VIRT(phys), 0, (size_t)PAGE_SIZE << order);   /* the last owner's bytes */

    shm_segs[slot].uid    = task_current()->euid;
    shm_segs[slot].gid    = task_current()->egid;
    shm_segs[slot].mode   = shmflg & 0777;
    shm_segs[slot].cg     = cg;
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
    int rdonly = (shmflg & 010000 /* SHM_RDONLY */) != 0;
    uint64_t f = spin_lock_irq(&shm_lock);
    if (shmid < 0 || shmid >= SHM_MAX || !shm_segs[shmid].valid ||
        shm_segs[shmid].ipc_ns != current_ipc()) {
        spin_unlock_irq(&shm_lock, f);
        return -22;
    }
    shm_seg_t *seg = &shm_segs[shmid];
    if (!ipc_may(seg->uid, seg->gid, seg->mode, rdonly ? 4 : 6)) {
        spin_unlock_irq(&shm_lock, f);
        return -13;
    }
    size_t size = seg->size;
    void *kaddr = seg->kaddr;
    process_t *p = task_current();
    uint64_t va = (uint64_t)shmaddr;
    if ((va & (PAGE_SIZE - 1)) || (va && !access_ok((void *)va, size))) {
        spin_unlock_irq(&shm_lock, f);
        return -22;
    }
    seg->refs++;
    spin_unlock_irq(&shm_lock, f);

    if (va) drop_old_pages(p, va, size);
    if (!va) {
        /* kernel picks address; the bump cursor is shared by all threads */
        uint64_t cf = spin_lock_irq(&p->vm->fault_lock);
        va = p->vm->mmap_next;
        if (!va) va = 0x50000000ULL;
        p->vm->mmap_next = va + size + PAGE_SIZE;
        spin_unlock_irq(&p->vm->fault_lock, cf);
    }

    /* map physical pages of kaddr into user VA; each mapping holds its pages,
     * which unmap and exit drop */
    uint64_t phys = VIRT_TO_PHYS(kaddr);
    uint32_t vmflags = VM_USER | VM_READ | (rdonly ? 0 : VM_WRITE);
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        pmm_hold((void *)(phys + off));
        vm_map(p->vm, va + off, phys + off, vmflags);
    }

    /* record VMA so munmap / process exit can clean up */
    vma_replace(p, va, size, VMA_USER | VMA_READ | VMA_SHARED | (rdonly ? 0 : VMA_WRITE));

    return (long)va;
}

static long sys_shmdt(const void *shmaddr)
{
    process_t *p  = task_current();
    uint64_t   va = (uint64_t)shmaddr;

    /* the attachment starts at va */
    size_t size = 0;
    uint64_t cf = spin_lock_irq(&p->vm->fault_lock);
    for (vma_t *v = p->vm->vmas; v; v = v->next)
        if (v->vaddr == va && (v->flags & VMA_SHARED)) { size = v->size; break; }
    spin_unlock_irq(&p->vm->fault_lock, cf);
    if (!size) return -22;

    /* the segment is the one whose pages sit there; another of the same size must
     * not lose a count */
    uint64_t pa = arch_vm_get_paddr(p->vm->root, va);
    uint64_t fl = spin_lock_irq(&shm_lock);
    for (int i = 0; pa && i < SHM_MAX; i++) {
        if (shm_segs[i].valid && VIRT_TO_PHYS(shm_segs[i].kaddr) == pa) {
            shm_segs[i].refs--;
            break;
        }
    }
    spin_unlock_irq(&shm_lock, fl);

    drop_old_pages(p, va, size);
    vma_clip(p, va, size);
    return 0;
}

/* struct ipc64_perm, 48 bytes */
typedef struct {
    key_t    key;
    uint32_t uid, gid, cuid, cgid, mode;
    uint16_t seq, _p0;
    uint32_t _p1;
    uint64_t _u1, _u2;
} ipc_perm_t;

typedef struct {
    ipc_perm_t shm_perm;
    size_t     shm_segsz;
    int64_t    shm_atime, shm_dtime, shm_ctime;
    int        shm_cpid, shm_lpid;
    uint64_t   shm_nattch;
    uint64_t   _u4, _u5;
} shmid_ds_t;

static long sys_shmctl(int shmid, int cmd, void *buf)
{
    if (shmid < 0 || shmid >= SHM_MAX) return -22;
    uint64_t f = spin_lock_irq(&shm_lock);
    if (!shm_segs[shmid].valid || shm_segs[shmid].ipc_ns != current_ipc()) {
        spin_unlock_irq(&shm_lock, f); return -22;
    }

    if (cmd == IPC_RMID) {
        if (!ipc_owner(shm_segs[shmid].uid)) { spin_unlock_irq(&shm_lock, f); return -1; }
        /* each attachment holds its pages, so they outlive the segment; waiting for
         * refs to reach 0 would keep a slot forever, as exit does not count down */
        size_t pages = shm_segs[shmid].size / PAGE_SIZE;
        int order = 0;
        while ((size_t)(1 << order) < pages) order++;
        long freed = pmm_free_block(VIRT_TO_PHYS(shm_segs[shmid].kaddr), order);
        cgroup_file_release(shm_segs[shmid].cg, freed * PAGE_SIZE);
        shm_segs[shmid].valid = 0;
        spin_unlock_irq(&shm_lock, f);
        return 0;
    }
    if (cmd == IPC_STAT && !ipc_may(shm_segs[shmid].uid, shm_segs[shmid].gid, shm_segs[shmid].mode, 4)) {
        spin_unlock_irq(&shm_lock, f);
        return -13;
    }
    if (cmd == IPC_STAT && buf) {
        shmid_ds_t ds;
        __builtin_memset(&ds, 0, sizeof(ds));
        ds.shm_perm.key  = shm_segs[shmid].key;
        ds.shm_perm.uid  = ds.shm_perm.cuid = shm_segs[shmid].uid;
        ds.shm_perm.gid  = ds.shm_perm.cgid = shm_segs[shmid].gid;
        ds.shm_perm.mode = (uint32_t)shm_segs[shmid].mode;
        ds.shm_segsz  = shm_segs[shmid].size;
        ds.shm_nattch = (uint64_t)shm_segs[shmid].refs;
        spin_unlock_irq(&shm_lock, f);
        return copy_to_user(buf, &ds, sizeof(ds)) ? -14 : 0;
    }
    spin_unlock_irq(&shm_lock, f);
    return -22;
}

/* ── SysV Semaphores ──────────────────────────────────────── */

typedef struct {
    int16_t val;          /* 0..SEMVMX */
} sem_t;

typedef struct {
    key_t    key;
    int      nsems;
    int      valid;
    ipc_namespace_t *ipc_ns;  /* owning IPC namespace */
    uid_t    uid;
    gid_t    gid;
    int      mode;
    uint32_t seq;         /* bumped per semget: tells a reused slot from ours */
    int      wq_ready;
    wait_queue_t wq;      /* every sleeper on any semaphore of the array */
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
    if (current_ipc() != &init_ipc_ns) {
        int held = 0;
        for (int i = 0; i < SEM_MAX; i++)
            held += sem_arrs[i].valid && sem_arrs[i].ipc_ns == current_ipc();
        if (held >= SEM_NS_MAX) { spin_unlock_irq(&sem_lock, f); return -28; }
    }
    /* the wait queue survives reuse: a sleeper woken by IPC_RMID may still be
     * unlinking itself from it */
    if (!sem_arrs[slot].wq_ready) { wait_queue_init(&sem_arrs[slot].wq); sem_arrs[slot].wq_ready = 1; }
    uint32_t seq = sem_arrs[slot].seq + 1;
    __builtin_memset(sem_arrs[slot].sems, 0, sizeof(sem_arrs[slot].sems));
    sem_arrs[slot].seq    = seq;
    sem_arrs[slot].key    = key;
    sem_arrs[slot].nsems  = nsems;
    sem_arrs[slot].valid  = 1;
    sem_arrs[slot].ipc_ns = current_ipc();
    sem_arrs[slot].uid    = task_current()->euid;
    sem_arrs[slot].gid    = task_current()->egid;
    sem_arrs[slot].mode   = semflg & 0777;
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

    for (size_t i = 0; i < nsops; i++)
        if (sops[i].sem_num >= SEM_NSEMS_MAX) return -27;   /* EFBIG */

    while (1) {
        uint64_t f = spin_lock_irq(&sem_lock);
        if (!sem_arrs[semid].valid || sem_arrs[semid].ipc_ns != current_ipc()) {
            spin_unlock_irq(&sem_lock, f); return -22;
        }
        sem_arr_t *arr = &sem_arrs[semid];
        if (!ipc_may(arr->uid, arr->gid, arr->mode, 6)) {
            spin_unlock_irq(&sem_lock, f); return -13;
        }

        /* Apply the ops in order to a copy: two ops on one semaphore see each
         * other, and nothing changes unless all of them can go. */
        int vals[SEM_NSEMS_MAX];
        for (int i = 0; i < arr->nsems; i++) vals[i] = arr->sems[i].val;
        int blocked = -1;
        for (size_t i = 0; i < nsops; i++) {
            int n = sops[i].sem_num;
            if (n >= arr->nsems) { spin_unlock_irq(&sem_lock, f); return -27; } /* EFBIG */
            int v = vals[n] + sops[i].sem_op;
            if (sops[i].sem_op == 0 ? vals[n] != 0 : v < 0) { blocked = (int)i; break; }
            if (v > SEMVMX) { spin_unlock_irq(&sem_lock, f); return -34; }   /* ERANGE */
            vals[n] = v;
        }
        if (blocked < 0) {
            for (int i = 0; i < arr->nsems; i++) arr->sems[i].val = (int16_t)vals[i];
            wake_up(&arr->wq);
            spin_unlock_irq(&sem_lock, f);
            return 0;
        }
        if (sops[blocked].sem_flg & IPC_NOWAIT) {
            spin_unlock_irq(&sem_lock, f);
            return -11; /* EAGAIN */
        }
        /* queued before sem_lock drops, so a change after this wakes us */
        sleep_on_locked(&arr->wq, &sem_lock, f);
        if (task_has_actionable_signal()) return -4; /* EINTR */
    }
}

#define SETVAL  16
#define GETVAL  12
#define GETALL  13
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
    int need = (cmd == GETVAL || cmd == GETALL) ? 4 : ((cmd == SETVAL || cmd == SETALL) ? 2 : 0);
    if (need && !ipc_may(arr->uid, arr->gid, arr->mode, need)) {
        spin_unlock_irq(&sem_lock, f);
        return -13;
    }
    switch (cmd) {
    case SETVAL:
        if (semnum < 0 || semnum >= arr->nsems) { ret = -22; break; }
        if ((int)arg < 0 || (int)arg > SEMVMX) { ret = -34; break; }   /* ERANGE */
        arr->sems[semnum].val = (int16_t)arg;
        wake_up(&arr->wq);   /* sleepers recheck their whole op list */
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
        uint16_t vals[SEM_NSEMS_MAX];
        int nsems = arr->nsems;
        uint32_t seq = arr->seq;
        spin_unlock_irq(&sem_lock, f);
        if (copy_from_user(vals, (const void *)arg, (size_t)nsems * sizeof(uint16_t)) != 0)
            return -14;
        for (int i = 0; i < nsems; i++)
            if (vals[i] > SEMVMX) return -34;               /* ERANGE */
        f = spin_lock_irq(&sem_lock);
        /* removed, or the slot went to another array, while we copied */
        if (!arr->valid || arr->seq != seq) { ret = -43; break; }   /* EIDRM */
        for (int i = 0; i < nsems; i++) arr->sems[i].val = (int16_t)vals[i];
        wake_up(&arr->wq);
        break;
    }
    case IPC_RMID:
        if (!ipc_owner(arr->uid)) { ret = -1; break; }
        arr->valid = 0;
        /* a sleeper finds the array gone; nobody else would ever wake it */
        wake_up(&arr->wq);
        break;
    default:
        ret = -22;
    }
    spin_unlock_irq(&sem_lock, f);
    return ret;
}

/* ── SysV Message Queues ──────────────────────────────────── */

#define MSGQ_MAX     16     /* queues per ipc namespace */
#define MSGQ_NMSG    64     /* messages per queue */
#define MSGQ_MSGSZ   1024   /* max mtext bytes per message */
#define MSG_NOERROR  010000
#define MSG_EXCEPT   020000

typedef struct {
    int      used;
    uint64_t seq;          /* FIFO order within the queue */
    long     mtype;
    size_t   len;          /* mtext length */
    char     data[MSGQ_MSGSZ];
} sysv_msg_t;

typedef struct {
    key_t      key;
    int        valid;
    int        count;
    uint64_t   seq_next;
    sysv_msg_t msgs[MSGQ_NMSG];
    process_t *send_waiter;
    process_t *recv_waiter;
    ipc_namespace_t *ipc_ns;
    uid_t      uid;
    gid_t      gid;
    int        mode;
} msgq_t;

static msgq_t     msg_queues[MSGQ_MAX];
static spinlock_t msgq_lock = SPINLOCK_INIT;

static int msgq_key_to_id(key_t key)
{
    ipc_namespace_t *cur = current_ipc();
    for (int i = 0; i < MSGQ_MAX; i++)
        if (msg_queues[i].valid && msg_queues[i].key == key && msg_queues[i].ipc_ns == cur)
            return i;
    return -1;
}

static long sys_msgget(key_t key, int msgflg)
{
    uint64_t f = spin_lock_irq(&msgq_lock);
    if (key != IPC_PRIVATE) {
        int id = msgq_key_to_id(key);
        if (id >= 0) {
            if ((msgflg & IPC_CREAT) && (msgflg & IPC_EXCL)) {
                spin_unlock_irq(&msgq_lock, f); return -17; /* EEXIST */
            }
            spin_unlock_irq(&msgq_lock, f); return id;
        }
        if (!(msgflg & IPC_CREAT)) { spin_unlock_irq(&msgq_lock, f); return -2; /* ENOENT */ }
    }
    int slot = -1;
    for (int i = 0; i < MSGQ_MAX; i++) if (!msg_queues[i].valid) { slot = i; break; }
    if (slot < 0) { spin_unlock_irq(&msgq_lock, f); return -28; /* ENOSPC */ }
    msgq_t *q = &msg_queues[slot];
    q->key = key; q->valid = 1; q->count = 0; q->seq_next = 1;
    q->send_waiter = q->recv_waiter = NULL;
    q->ipc_ns = current_ipc();
    q->uid = task_current()->euid; q->gid = task_current()->egid; q->mode = msgflg & 0777;
    for (int i = 0; i < MSGQ_NMSG; i++) q->msgs[i].used = 0;
    spin_unlock_irq(&msgq_lock, f);
    return slot;
}

static long sys_msgsnd(int msqid, const void *msgp, size_t msgsz, int msgflg)
{
    if (msqid < 0 || msqid >= MSGQ_MAX || msgsz > MSGQ_MSGSZ) return -22;
    long mtype;
    if (copy_from_user(&mtype, msgp, sizeof(long)) != 0) return -14;
    if (mtype < 1) return -22;
    char tmp[MSGQ_MSGSZ];
    if (msgsz && copy_from_user(tmp, (const char *)msgp + sizeof(long), msgsz) != 0) return -14;

    while (1) {
        uint64_t f = spin_lock_irq(&msgq_lock);
        if (!msg_queues[msqid].valid || msg_queues[msqid].ipc_ns != current_ipc()) {
            spin_unlock_irq(&msgq_lock, f); return -43; /* EIDRM */
        }
        msgq_t *q = &msg_queues[msqid];
        if (!ipc_may(q->uid, q->gid, q->mode, 2)) { spin_unlock_irq(&msgq_lock, f); return -13; }
        if (q->count < MSGQ_NMSG) {
            int slot = -1;
            for (int i = 0; i < MSGQ_NMSG; i++) if (!q->msgs[i].used) { slot = i; break; }
            sysv_msg_t *m = &q->msgs[slot];
            m->used = 1; m->seq = q->seq_next++; m->mtype = mtype; m->len = msgsz;
            if (msgsz) memcpy(m->data, tmp, msgsz);
            q->count++;
            if (q->recv_waiter) { task_wake_no_resched(q->recv_waiter); q->recv_waiter = NULL; }
            spin_unlock_irq(&msgq_lock, f);
            return 0;
        }
        if (msgflg & IPC_NOWAIT) { spin_unlock_irq(&msgq_lock, f); return -11; /* EAGAIN */ }
        q->send_waiter = task_current();
        task_sleep_unlock(&msgq_lock, f);
        if (task_has_actionable_signal()) {
            f = spin_lock_irq(&msgq_lock);
            if (msg_queues[msqid].valid && msg_queues[msqid].send_waiter == task_current())
                msg_queues[msqid].send_waiter = NULL;
            spin_unlock_irq(&msgq_lock, f);
            return -4; /* EINTR */
        }
    }
}

static long sys_msgrcv(int msqid, void *msgp, size_t msgsz, long msgtyp, int msgflg)
{
    if (msqid < 0 || msqid >= MSGQ_MAX || msgsz > MSGQ_MSGSZ) return -22;

    while (1) {
        uint64_t f = spin_lock_irq(&msgq_lock);
        if (!msg_queues[msqid].valid || msg_queues[msqid].ipc_ns != current_ipc()) {
            spin_unlock_irq(&msgq_lock, f); return -43; /* EIDRM */
        }
        msgq_t *q = &msg_queues[msqid];
        if (!ipc_may(q->uid, q->gid, q->mode, 4)) { spin_unlock_irq(&msgq_lock, f); return -13; }

        int best = -1;
        for (int i = 0; i < MSGQ_NMSG; i++) {
            if (!q->msgs[i].used) continue;
            long mt = q->msgs[i].mtype;
            int match;
            if (msgtyp == 0)      match = 1;
            else if (msgtyp > 0)  match = (msgflg & MSG_EXCEPT) ? (mt != msgtyp) : (mt == msgtyp);
            else                  match = (mt <= -msgtyp);
            if (!match) continue;
            if (best < 0)
                best = i;
            else if (msgtyp < 0 && q->msgs[i].mtype != q->msgs[best].mtype)
                best = (q->msgs[i].mtype < q->msgs[best].mtype) ? i : best; /* lowest type */
            else if (q->msgs[i].seq < q->msgs[best].seq)
                best = i;                                                   /* oldest */
        }
        if (best >= 0) {
            sysv_msg_t *m = &q->msgs[best];
            if (m->len > msgsz && !(msgflg & MSG_NOERROR)) {
                spin_unlock_irq(&msgq_lock, f); return -7; /* E2BIG */
            }
            size_t copy = m->len > msgsz ? msgsz : m->len;
            long mtype = m->mtype;
            char tmp[MSGQ_MSGSZ];
            if (copy) memcpy(tmp, m->data, copy);
            m->used = 0; q->count--;
            if (q->send_waiter) { task_wake_no_resched(q->send_waiter); q->send_waiter = NULL; }
            spin_unlock_irq(&msgq_lock, f);
            if (copy_to_user(msgp, &mtype, sizeof(long)) != 0) return -14;
            if (copy && copy_to_user((char *)msgp + sizeof(long), tmp, copy) != 0) return -14;
            return (long)copy;
        }
        if (msgflg & IPC_NOWAIT) { spin_unlock_irq(&msgq_lock, f); return -42; /* ENOMSG */ }
        q->recv_waiter = task_current();
        task_sleep_unlock(&msgq_lock, f);
        if (task_has_actionable_signal()) {
            f = spin_lock_irq(&msgq_lock);
            if (msg_queues[msqid].valid && msg_queues[msqid].recv_waiter == task_current())
                msg_queues[msqid].recv_waiter = NULL;
            spin_unlock_irq(&msgq_lock, f);
            return -4; /* EINTR */
        }
    }
}

static long sys_msgctl(int msqid, int cmd, void *buf)
{
    (void)buf;
    if (msqid < 0 || msqid >= MSGQ_MAX) return -22;
    uint64_t f = spin_lock_irq(&msgq_lock);
    if (!msg_queues[msqid].valid || msg_queues[msqid].ipc_ns != current_ipc()) {
        spin_unlock_irq(&msgq_lock, f); return -22;
    }
    msgq_t *q = &msg_queues[msqid];
    long ret = 0;
    if (cmd == IPC_RMID) {
        if (!ipc_owner(q->uid)) { spin_unlock_irq(&msgq_lock, f); return -1; }
        q->valid = 0;
        if (q->send_waiter) { task_wake_no_resched(q->send_waiter); q->send_waiter = NULL; }
        if (q->recv_waiter) { task_wake_no_resched(q->recv_waiter); q->recv_waiter = NULL; }
    } else {
        ret = -22; /* IPC_STAT/IPC_SET not modelled */
    }
    spin_unlock_irq(&msgq_lock, f);
    return ret;
}

/* ── POSIX Message Queues ─────────────────────────────────── */

#define MQ_MAX      CONFIG_MQ_MAX
#define MQ_MSG_MAX  CONFIG_MQ_MSG_MAX
#define MQ_MSGSIZE  CONFIG_MQ_MSGSIZE

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
    uid_t     uid;
    gid_t     gid;
    int       mode;
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
            /* highest priority, oldest-first within a priority */
            int best = mq->head, bestk = 0;
            for (int i = 0; i < mq->count; i++) {
                int slot = (mq->head + i) % MQ_MSG_MAX;
                if (mq->msgs[slot].prio > mq->msgs[best].prio) { best = slot; bestk = i; }
            }
            /* Copy out and remove the slot entirely under the lock, so two
             * concurrent readers can't pop the same message (count underflow)
             * and a re-used slot can't tear the copy. Shift the head..best run
             * forward by one and advance head to drop the chosen entry. */
            char tmp[MQ_MSGSIZE];
            size_t copy = mq->msgs[best].len < len ? mq->msgs[best].len : len;
            __builtin_memcpy(tmp, mq->msgs[best].buf, copy);
            for (int j = bestk; j >= 1; j--)
                mq->msgs[(mq->head + j) % MQ_MSG_MAX] = mq->msgs[(mq->head + j - 1) % MQ_MSG_MAX];
            mq->head = (mq->head + 1) % MQ_MSG_MAX;
            mq->count--;
            if (mq->send_waiter) {
                task_wake_no_resched(mq->send_waiter);
                mq->send_waiter = NULL;
            }
            spin_unlock_irq(&mq_lock, f);
            if (copy_fop_to_user(buf, tmp, copy) != 0) return -14;
            return (ssize_t)copy;
        }
        if (file->flags & O_NONBLOCK) { spin_unlock_irq(&mq_lock, f); return -11; }
        mq->recv_waiter = task_current();
        task_sleep_unlock(&mq_lock, f);
        if (task_has_actionable_signal()) {
            f = spin_lock_irq(&mq_lock);
            if (mq->recv_waiter == task_current()) mq->recv_waiter = NULL;
            spin_unlock_irq(&mq_lock, f);
            return -4; /* EINTR */
        }
    }
}

static ssize_t mqfd_write(vfs_file_t *file, const void *buf, size_t len)
{
    if (len > MQ_MSGSIZE) return -90; /* EMSGSIZE */
    mq_t *mq = (mq_t *)file->inode->priv;
    char tmp[MQ_MSGSIZE];
    if (copy_fop_from_user(tmp, buf, len) != 0) return -14;
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
        if (task_has_actionable_signal()) {
            f = spin_lock_irq(&mq_lock);
            if (mq->send_waiter == task_current()) mq->send_waiter = NULL;
            spin_unlock_irq(&mq_lock, f);
            return -4; /* EINTR */
        }
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

static void mq_drop_ref(mq_t *mq)
{
    uint64_t f = spin_lock_irq(&mq_lock);
    mq->refs--;
    spin_unlock_irq(&mq_lock, f);
}

static long sys_mq_open(const char *uname, int oflag, mode_t mode, mq_attr_t *attr)
{
    (void)attr;
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
            int acc = oflag & 3;
            if (!ipc_may(mq_table[i].uid, mq_table[i].gid, mq_table[i].mode,
                         acc == O_RDONLY ? 4 : (acc == O_WRONLY ? 2 : 6))) {
                spin_unlock_irq(&mq_lock, f);
                return -13;
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
        mq_table[slot].uid    = task_current()->euid;
        mq_table[slot].gid    = task_current()->egid;
        mq_table[slot].mode   = mode & 0777;
    }
    mq_table[slot].refs++;
    spin_unlock_irq(&mq_lock, f);

    vfs_inode_t *in = kmalloc(sizeof(*in));
    if (!in) { mq_drop_ref(&mq_table[slot]); return -12; }
    __builtin_memset(in, 0, sizeof(*in));
    in->fops     = &mq_fops;
    in->priv     = &mq_table[slot];
    in->refcount = 1;

    vfs_file_t *vf = kmalloc(sizeof(*vf));
    if (!vf) { kfree(in); mq_drop_ref(&mq_table[slot]); return -12; }
    __builtin_memset(vf, 0, sizeof(*vf));
    vf->inode    = in;
    vf->refcount = 0;   /* fd_alloc takes the +1; close() frees */
    vf->flags    = oflag & O_NONBLOCK;

    process_t *p = task_current();
    int fd = fd_alloc(p->fds, vf);
    if (fd < 0) { kfree(vf); kfree(in); mq_drop_ref(&mq_table[slot]); return fd == -1 ? -12 : fd; }
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
            if (!ipc_owner(mq_table[i].uid)) { spin_unlock_irq(&mq_lock, f); return -13; }
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
    /* the write path copies through the kernel-or-user fop helper */
    if (!access_ok(umsg, len)) return -14;
    /* reuse write path */
    process_t *p = task_current();
    vfs_file_t *f = fget(p->fds, mqdes);   /* pin across the blocking send */
    if (!f || !f->inode || f->inode->fops != &mq_fops) { if (f) fput(f); return -9; }
    long r = mqfd_write(f, umsg, len);
    fput(f);
    return r < 0 ? r : 0;   /* the byte count is write(2)'s answer, not this one's */
}

static long sys_mq_timedreceive(int mqdes, char *umsg, size_t len,
                                uint32_t *uprio, const void *ts)
{
    (void)uprio; (void)ts;
    /* the read path copies through the kernel-or-user fop helper */
    if (!access_ok(umsg, len)) return -14;
    process_t *p = task_current();
    vfs_file_t *f = fget(p->fds, mqdes);   /* pin across the blocking receive */
    if (!f || !f->inode || f->inode->fops != &mq_fops) { if (f) fput(f); return -9; }
    long r = mqfd_read(f, umsg, len);
    fput(f);
    return r;
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
            long freed = pmm_free_block(VIRT_TO_PHYS(shm_segs[i].kaddr), order);
            cgroup_file_release(shm_segs[i].cg, freed * PAGE_SIZE);
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

    f = spin_lock_irq(&msgq_lock);
    for (int i = 0; i < MSGQ_MAX; i++)
        if (msg_queues[i].valid && msg_queues[i].ipc_ns == ns) msg_queues[i].valid = 0;
    spin_unlock_irq(&msgq_lock, f);

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
