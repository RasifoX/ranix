/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* ── SysV IPC: shared memory + semaphores ─────────────────── */

typedef int32_t key_t;

#define IPC_PRIVATE  0
#define IPC_CREAT    01000
#define IPC_EXCL     02000
#define IPC_RMID     0
#define IPC_SET      1
#define IPC_STAT     2
#define IPC_INFO     3
#define IPC_64       0x100   /* the 64-bit layout; Linux takes and ignores it on 64-bit */

#include <kernel/sysvipc.h>
#include <kernel/time.h>

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
    uid_t    uid;     /* owner; these objects are reachable by any task in the */
    gid_t    gid;     /* namespace that guesses a small id, so they need these */
    int      mode;
    uid_t    cuid;    /* creator: IPC_SET can hand the owner on, not the creator */
    gid_t    cgid;
    size_t   segsz;   /* the size asked for; size is that in whole pages */
    int64_t  atime, dtime, ctime;
    int      cpid, lpid;
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

/* An existing object's get asks for the rw bits in flg's mode: each of them must
 * be granted (Linux ipcperms), else EACCES. */
static int ipc_get_may(uid_t uid, gid_t gid, int mode, int flg)
{
    return ipc_may(uid, gid, mode, (flg >> 6 | flg >> 3 | flg) & 6);
}

/* Only the owner, its creator, or root, may remove or reconfigure one. */
static int ipc_owner(uid_t uid, uid_t cuid)
{
    process_t *p = task_current();
    return p->euid == 0 || p->euid == uid || p->euid == cuid;
}

static int64_t ipc_now(void)
{
    int64_t sec, nsec;
    ranix_walltime(&sec, &nsec);
    return sec;
}

static int ipc_pid(void) { return (int)task_current()->tgid; }

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
                return -EEXIST;
            }
            shm_seg_t *seg = &shm_segs[id];
            int err = size > seg->segsz ? -EINVAL
                    : !ipc_get_may(seg->uid, seg->gid, seg->mode, shmflg) ? -EACCES : id;
            spin_unlock_irq(&shm_lock, f);
            return err;
        }
        if (!(shmflg & IPC_CREAT)) {
            spin_unlock_irq(&shm_lock, f);
            return -ENOENT;
        }
    }

    int slot = -1;
    for (int i = 0; i < SHM_MAX; i++) {
        if (!shm_segs[i].valid) { slot = i; break; }
    }
    if (slot < 0) { spin_unlock_irq(&shm_lock, f); return -ENOSPC; }
    if (current_ipc() != &init_ipc_ns) {
        int held = 0;
        for (int i = 0; i < SHM_MAX; i++)
            held += shm_segs[i].valid && shm_segs[i].ipc_ns == current_ipc();
        if (held >= SHM_NS_MAX) { spin_unlock_irq(&shm_lock, f); return -ENOSPC; }
    }
    if (size == 0) { spin_unlock_irq(&shm_lock, f); return -EINVAL; }   /* SHMMIN is 1 */
    size_t segsz = size;
    /* one contiguous buddy block; rounding and the order loop below must not wrap */
    if (size > ((size_t)PAGE_SIZE << (BUDDY_MAX_ORDER - 1))) {
        spin_unlock_irq(&shm_lock, f);
        return -EINVAL;
    }
    size = (size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);

    int order = 0;
    size_t pages = size / PAGE_SIZE;
    while ((size_t)(1 << order) < pages) order++;
    void *cg = NULL;
    long block = PAGE_SIZE << order;
    if (cgroup_charge_file(&cg, 0, block) != 0) { spin_unlock_irq(&shm_lock, f); return -ENOMEM; }
    void *phys = buddy_alloc(order);
    if (!phys) { cgroup_file_release(cg, block); spin_unlock_irq(&shm_lock, f); return -ENOMEM; }
    memset((void *)PHYS_TO_VIRT(phys), 0, (size_t)PAGE_SIZE << order);   /* the last owner's bytes */

    shm_segs[slot].uid    = shm_segs[slot].cuid = task_current()->euid;
    shm_segs[slot].gid    = shm_segs[slot].cgid = task_current()->egid;
    shm_segs[slot].mode   = shmflg & 0777;
    shm_segs[slot].segsz  = segsz;
    shm_segs[slot].atime  = shm_segs[slot].dtime = 0;
    shm_segs[slot].ctime  = ipc_now();
    shm_segs[slot].cpid   = ipc_pid();
    shm_segs[slot].lpid   = 0;
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
        return -EINVAL;
    }
    shm_seg_t *seg = &shm_segs[shmid];
    if (!ipc_may(seg->uid, seg->gid, seg->mode, rdonly ? 4 : 6)) {
        spin_unlock_irq(&shm_lock, f);
        return -EACCES;
    }
    size_t size = seg->size;
    void *kaddr = seg->kaddr;
    process_t *p = task_current();
    uint64_t va = (uint64_t)shmaddr;
    if ((va & (PAGE_SIZE - 1)) || (va && !access_ok((void *)va, size))) {
        spin_unlock_irq(&shm_lock, f);
        return -EINVAL;
    }
    seg->refs++;
    seg->atime = ipc_now();
    seg->lpid  = ipc_pid();
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
    if (!size) return -EINVAL;

    /* the segment is the one whose pages sit there; another of the same size must
     * not lose a count */
    uint64_t pa = arch_vm_get_paddr(p->vm->root, va);
    uint64_t fl = spin_lock_irq(&shm_lock);
    for (int i = 0; pa && i < SHM_MAX; i++) {
        if (shm_segs[i].valid && VIRT_TO_PHYS(shm_segs[i].kaddr) == pa) {
            shm_segs[i].refs--;
            shm_segs[i].dtime = ipc_now();
            shm_segs[i].lpid  = ipc_pid();
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

#define SHM_LOCK     11
#define SHM_UNLOCK   12
#define SHM_STAT     13
#define SHM_INFO     14
#define SHM_STAT_ANY 15
#define SHM_LOCKED   02000
#define SHMMAX_BYTES ((uint64_t)PAGE_SIZE << (BUDDY_MAX_ORDER - 1))   /* one buddy block */

typedef struct { uint64_t shmmax, shmmin, shmmni, shmseg, shmall, _u[4]; } shminfo_t;
typedef struct { int used_ids; uint64_t shm_tot, shm_rss, shm_swp, swap_attempts, swap_successes; } shm_info_t;

/* Attachments: each one holds the segment's pages, and so does fork's copy of
 * one, while the segment itself holds one reference. */
static uint64_t shm_nattch(const shm_seg_t *s)
{
    page_t *pg = paddr_to_page(VIRT_TO_PHYS(s->kaddr));
    return pg && pg->refcount > 1 ? pg->refcount - 1 : 0;
}

static void ipc_fill_perm(ipc_perm_t *pm, key_t key, uid_t uid, gid_t gid, uid_t cuid, gid_t cgid, int mode)
{
    pm->key = key;
    pm->uid = uid; pm->gid = gid; pm->cuid = cuid; pm->cgid = cgid;
    pm->mode = (uint32_t)mode;
}

static void shm_fill(shmid_ds_t *ds, const shm_seg_t *s)
{
    __builtin_memset(ds, 0, sizeof(*ds));
    ipc_fill_perm(&ds->shm_perm, s->key, s->uid, s->gid, s->cuid, s->cgid, s->mode);
    ds->shm_segsz  = s->segsz;
    ds->shm_atime  = s->atime;
    ds->shm_dtime  = s->dtime;
    ds->shm_ctime  = s->ctime;
    ds->shm_cpid   = s->cpid;
    ds->shm_lpid   = s->lpid;
    ds->shm_nattch = shm_nattch(s);
}

/* IPC_INFO and SHM_INFO: the limits, or what is in use; either way the answer
 * is the highest index in use */
static long shm_info(int cmd, void *buf)
{
    shminfo_t li;
    shm_info_t si;
    __builtin_memset(&li, 0, sizeof(li));
    __builtin_memset(&si, 0, sizeof(si));
    li.shmmax = SHMMAX_BYTES; li.shmmin = 1; li.shmmni = SHM_MAX; li.shmseg = SHM_MAX;
    li.shmall = SHMMAX_BYTES / PAGE_SIZE * SHM_MAX;
    int max = 0;
    uint64_t f = spin_lock_irq(&shm_lock);
    for (int i = 0; i < SHM_MAX; i++) {
        if (!shm_segs[i].valid || shm_segs[i].ipc_ns != current_ipc()) continue;
        max = i;
        si.used_ids++;
        si.shm_tot += shm_segs[i].size / PAGE_SIZE;
        si.shm_rss += shm_segs[i].size / PAGE_SIZE;
    }
    spin_unlock_irq(&shm_lock, f);
    int r = cmd == IPC_INFO ? copy_to_user(buf, &li, sizeof(li)) : copy_to_user(buf, &si, sizeof(si));
    return r ? -EFAULT : max;
}

static long sys_shmctl(int shmid, int cmd, void *buf)
{
    cmd &= ~IPC_64;
    if (cmd == IPC_INFO || cmd == SHM_INFO) return shm_info(cmd, buf);
    shmid_ds_t ds;
    if (cmd == IPC_SET && copy_from_user(&ds, buf, sizeof(ds)) != 0) return -EFAULT;
    if (shmid < 0 || shmid >= SHM_MAX) return -EINVAL;
    uint64_t f = spin_lock_irq(&shm_lock);
    if (!shm_segs[shmid].valid || shm_segs[shmid].ipc_ns != current_ipc()) {
        spin_unlock_irq(&shm_lock, f); return -EINVAL;
    }
    shm_seg_t *seg = &shm_segs[shmid];
    long ret = 0;
    switch (cmd) {
    case IPC_RMID: {
        if (!ipc_owner(seg->uid, seg->cuid)) { ret = -EPERM; break; }
        /* each attachment holds its pages, so they outlive the segment; waiting for
         * refs to reach 0 would keep a slot forever, as exit does not count down */
        size_t pages = seg->size / PAGE_SIZE;
        int order = 0;
        while ((size_t)(1 << order) < pages) order++;
        long freed = pmm_free_block(VIRT_TO_PHYS(seg->kaddr), order);
        cgroup_file_release(seg->cg, freed * PAGE_SIZE);
        seg->valid = 0;
        break;
    }
    case IPC_STAT: case SHM_STAT: case SHM_STAT_ANY:
        if (cmd != SHM_STAT_ANY && !ipc_may(seg->uid, seg->gid, seg->mode, 4)) { ret = -EACCES; break; }
        shm_fill(&ds, seg);
        spin_unlock_irq(&shm_lock, f);
        if (copy_to_user(buf, &ds, sizeof(ds)) != 0) return -EFAULT;
        return cmd == IPC_STAT ? 0 : shmid;
    case IPC_SET:
        if (!ipc_owner(seg->uid, seg->cuid)) { ret = -EPERM; break; }
        seg->uid   = ds.shm_perm.uid;
        seg->gid   = ds.shm_perm.gid;
        seg->mode  = (seg->mode & ~0777) | (int)(ds.shm_perm.mode & 0777);
        seg->ctime = ipc_now();
        break;
    case SHM_LOCK: case SHM_UNLOCK:
        /* nothing is ever swapped out: the bit is all there is to keep */
        if (!ipc_owner(seg->uid, seg->cuid)) { ret = -EPERM; break; }
        if (cmd == SHM_LOCK) seg->mode |= SHM_LOCKED; else seg->mode &= ~SHM_LOCKED;
        break;
    default:
        ret = -EINVAL;
    }
    spin_unlock_irq(&shm_lock, f);
    return ret;
}

/* ── SysV Semaphores ──────────────────────────────────────── */

typedef struct {
    int16_t  val;         /* 0..SEMVMX */
    uint16_t ncnt, zcnt;  /* sleepers waiting for it to grow, to reach 0 */
    int      pid;         /* the last to change it */
} sem_t;

typedef struct {
    key_t    key;
    int      nsems;
    int      valid;
    ipc_namespace_t *ipc_ns;  /* owning IPC namespace */
    uid_t    uid;
    gid_t    gid;
    int      mode;
    uid_t    cuid;
    gid_t    cgid;
    int64_t  otime, ctime;
    uint32_t seq;         /* bumped per semget: tells a reused slot from ours */
    int      wq_ready;
    wait_queue_t wq;      /* every sleeper on any semaphore of the array */
    sem_t    sems[SEM_NSEMS_MAX];
} sem_arr_t;

static sem_arr_t sem_arrs[SEM_MAX];
static spinlock_t sem_lock = SPINLOCK_INIT;

static long sys_semget(key_t key, int nsems, int semflg)
{
    if (nsems < 0 || nsems > SEM_NSEMS_MAX) return -EINVAL;
    uint64_t f = spin_lock_irq(&sem_lock);
    if (key != IPC_PRIVATE) {
        ipc_namespace_t *cur = current_ipc();
        for (int i = 0; i < SEM_MAX; i++) {
            if (sem_arrs[i].valid && sem_arrs[i].key == key && sem_arrs[i].ipc_ns == cur) {
                if ((semflg & IPC_CREAT) && (semflg & IPC_EXCL)) {
                    spin_unlock_irq(&sem_lock, f);
                    return -EEXIST;
                }
                sem_arr_t *a = &sem_arrs[i];
                int err = nsems > a->nsems ? -EINVAL
                        : !ipc_get_may(a->uid, a->gid, a->mode, semflg) ? -EACCES : i;
                spin_unlock_irq(&sem_lock, f);
                return err;
            }
        }
        if (!(semflg & IPC_CREAT)) { spin_unlock_irq(&sem_lock, f); return -ENOENT; }
    }
    int slot = -1;
    for (int i = 0; i < SEM_MAX; i++) {
        if (!sem_arrs[i].valid) { slot = i; break; }
    }
    if (slot < 0) { spin_unlock_irq(&sem_lock, f); return -ENOSPC; }
    if (nsems == 0) { spin_unlock_irq(&sem_lock, f); return -EINVAL; }
    if (current_ipc() != &init_ipc_ns) {
        int held = 0;
        for (int i = 0; i < SEM_MAX; i++)
            held += sem_arrs[i].valid && sem_arrs[i].ipc_ns == current_ipc();
        if (held >= SEM_NS_MAX) { spin_unlock_irq(&sem_lock, f); return -ENOSPC; }
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
    sem_arrs[slot].uid    = sem_arrs[slot].cuid = task_current()->euid;
    sem_arrs[slot].gid    = sem_arrs[slot].cgid = task_current()->egid;
    sem_arrs[slot].mode   = semflg & 0777;
    sem_arrs[slot].otime  = 0;
    sem_arrs[slot].ctime  = ipc_now();
    spin_unlock_irq(&sem_lock, f);
    return slot;
}

typedef struct { uint16_t sem_num; int16_t sem_op; int16_t sem_flg; } sembuf_t;

#define IPC_NOWAIT 04000

static long sys_semop(int semid, const sembuf_t *usops, size_t nsops)
{
    if (semid < 0 || semid >= SEM_MAX) return -EINVAL;
    if (nsops == 0 || nsops > SEM_NSEMS_MAX) return -EINVAL;

    sembuf_t sops[SEM_NSEMS_MAX];
    if (copy_from_user(sops, usops, nsops * sizeof(sembuf_t)) != 0) return -EFAULT;

    for (size_t i = 0; i < nsops; i++)
        if (sops[i].sem_num >= SEM_NSEMS_MAX) return -EFBIG;

    /* waiting for zero only reads; anything else alters */
    int alter = 0;
    for (size_t i = 0; i < nsops; i++) alter |= sops[i].sem_op != 0;
    uint32_t seq = 0;
    int first = 1;
    while (1) {
        uint64_t f = spin_lock_irq(&sem_lock);
        sem_arr_t *arr = &sem_arrs[semid];
        if (!arr->valid || arr->ipc_ns != current_ipc() || (!first && arr->seq != seq)) {
            spin_unlock_irq(&sem_lock, f);
            return first ? -EINVAL : -EIDRM;   /* removed while we slept */
        }
        if (first) seq = arr->seq;
        first = 0;
        if (!ipc_may(arr->uid, arr->gid, arr->mode, alter ? 2 : 4)) {
            spin_unlock_irq(&sem_lock, f); return -EACCES;
        }

        /* Apply the ops in order to a copy: two ops on one semaphore see each
         * other, and nothing changes unless all of them can go. */
        int vals[SEM_NSEMS_MAX];
        for (int i = 0; i < arr->nsems; i++) vals[i] = arr->sems[i].val;
        int blocked = -1;
        for (size_t i = 0; i < nsops; i++) {
            int n = sops[i].sem_num;
            if (n >= arr->nsems) { spin_unlock_irq(&sem_lock, f); return -EFBIG; } /* EFBIG */
            int v = vals[n] + sops[i].sem_op;
            if (sops[i].sem_op == 0 ? vals[n] != 0 : v < 0) { blocked = (int)i; break; }
            if (v > SEMVMX) { spin_unlock_irq(&sem_lock, f); return -ERANGE; }   /* ERANGE */
            vals[n] = v;
        }
        if (blocked < 0) {
            for (int i = 0; i < arr->nsems; i++) arr->sems[i].val = (int16_t)vals[i];
            for (size_t i = 0; i < nsops; i++) arr->sems[sops[i].sem_num].pid = ipc_pid();
            arr->otime = ipc_now();
            wake_up(&arr->wq);
            spin_unlock_irq(&sem_lock, f);
            return 0;
        }
        if (sops[blocked].sem_flg & IPC_NOWAIT) {
            spin_unlock_irq(&sem_lock, f);
            return -EAGAIN;
        }
        /* counted for GETNCNT/GETZCNT while asleep; queued before sem_lock
         * drops, so a change after this wakes us */
        sem_t *bs = &arr->sems[sops[blocked].sem_num];
        int forzero = sops[blocked].sem_op == 0;
        if (forzero) bs->zcnt++; else bs->ncnt++;
        sleep_on_locked(&arr->wq, &sem_lock, f);
        f = spin_lock_irq(&sem_lock);
        if (arr->valid && arr->seq == seq) { if (forzero) bs->zcnt--; else bs->ncnt--; }
        spin_unlock_irq(&sem_lock, f);
        if (task_has_actionable_signal()) return -EINTR;
    }
}

#define GETPID   11
#define GETVAL   12
#define GETALL   13
#define GETNCNT  14
#define GETZCNT  15
#define SETVAL   16
#define SETALL   17
#define SEM_STAT 18
#define SEM_INFO 19
#define SEM_STAT_ANY 20
#define SEMOPM   SEM_NSEMS_MAX   /* ops per semop */

typedef struct {
    ipc_perm_t sem_perm;
    int64_t    sem_otime;
    uint64_t   _u1;
    int64_t    sem_ctime;
    uint64_t   _u2;
    uint64_t   sem_nsems;
    uint64_t   _u3, _u4;
} semid_ds_t;

typedef struct { int semmap, semmni, semmns, semmnu, semmsl, semopm, semume, semusz, semvmx, semaem; } seminfo_t;

static long sem_info(int cmd, void *buf)
{
    seminfo_t si = { SEM_MAX * SEM_NSEMS_MAX, SEM_MAX, SEM_MAX * SEM_NSEMS_MAX, SEM_MAX * SEM_NSEMS_MAX,
                     SEM_NSEMS_MAX, SEMOPM, SEMOPM, 16, SEMVMX, SEMVMX };
    int max = 0, used = 0, sems = 0;
    uint64_t f = spin_lock_irq(&sem_lock);
    for (int i = 0; i < SEM_MAX; i++) {
        if (!sem_arrs[i].valid || sem_arrs[i].ipc_ns != current_ipc()) continue;
        max = i; used++; sems += sem_arrs[i].nsems;
    }
    spin_unlock_irq(&sem_lock, f);
    if (cmd == SEM_INFO) { si.semusz = used; si.semaem = sems; }   /* in use, as Linux reports */
    return copy_to_user(buf, &si, sizeof(si)) ? -EFAULT : max;
}

static long sys_semctl(int semid, int semnum, int cmd, uint64_t arg)
{
    cmd &= ~IPC_64;
    if (cmd == IPC_INFO || cmd == SEM_INFO) return sem_info(cmd, (void *)arg);
    semid_ds_t ds;
    if (cmd == IPC_SET && copy_from_user(&ds, (const void *)arg, sizeof(ds)) != 0) return -EFAULT;
    if (semid < 0 || semid >= SEM_MAX) return -EINVAL;
    uint64_t f = spin_lock_irq(&sem_lock);
    if (!sem_arrs[semid].valid || sem_arrs[semid].ipc_ns != current_ipc()) {
        spin_unlock_irq(&sem_lock, f); return -EINVAL;
    }
    sem_arr_t *arr = &sem_arrs[semid];
    long ret = 0;
    int need = (cmd == GETVAL || cmd == GETALL || cmd == GETPID || cmd == GETNCNT ||
                cmd == GETZCNT || cmd == IPC_STAT || cmd == SEM_STAT) ? 4
             : (cmd == SETVAL || cmd == SETALL) ? 2 : 0;
    if (need && !ipc_may(arr->uid, arr->gid, arr->mode, need)) {
        spin_unlock_irq(&sem_lock, f);
        return -EACCES;
    }
    int one = cmd == GETVAL || cmd == SETVAL || cmd == GETPID || cmd == GETNCNT || cmd == GETZCNT;
    if (one && (semnum < 0 || semnum >= arr->nsems)) { spin_unlock_irq(&sem_lock, f); return -EINVAL; }
    switch (cmd) {
    case SETVAL:
        if ((int)arg < 0 || (int)arg > SEMVMX) { ret = -34; break; }   /* ERANGE */
        arr->sems[semnum].val = (int16_t)arg;
        arr->sems[semnum].pid = ipc_pid();
        arr->ctime = ipc_now();
        wake_up(&arr->wq);   /* sleepers recheck their whole op list */
        break;
    case GETVAL:  ret = arr->sems[semnum].val;  break;
    case GETPID:  ret = arr->sems[semnum].pid;  break;
    case GETNCNT: ret = arr->sems[semnum].ncnt; break;
    case GETZCNT: ret = arr->sems[semnum].zcnt; break;
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
            return -EFAULT;
        for (int i = 0; i < nsems; i++)
            if (vals[i] > SEMVMX) return -ERANGE;
        f = spin_lock_irq(&sem_lock);
        /* removed, or the slot went to another array, while we copied */
        if (!arr->valid || arr->seq != seq) { ret = -43; break; }   /* EIDRM */
        for (int i = 0; i < nsems; i++) arr->sems[i].val = (int16_t)vals[i];
        arr->ctime = ipc_now();
        wake_up(&arr->wq);
        break;
    }
    case IPC_STAT: case SEM_STAT: case SEM_STAT_ANY:
        __builtin_memset(&ds, 0, sizeof(ds));
        ipc_fill_perm(&ds.sem_perm, arr->key, arr->uid, arr->gid, arr->cuid, arr->cgid, arr->mode);
        ds.sem_otime = arr->otime;
        ds.sem_ctime = arr->ctime;
        ds.sem_nsems = (uint64_t)arr->nsems;
        spin_unlock_irq(&sem_lock, f);
        if (copy_to_user((void *)arg, &ds, sizeof(ds)) != 0) return -EFAULT;
        return cmd == IPC_STAT ? 0 : semid;
    case IPC_SET:
        if (!ipc_owner(arr->uid, arr->cuid)) { ret = -EPERM; break; }
        arr->uid   = ds.sem_perm.uid;
        arr->gid   = ds.sem_perm.gid;
        arr->mode  = (arr->mode & ~0777) | (int)(ds.sem_perm.mode & 0777);
        arr->ctime = ipc_now();
        break;
    case IPC_RMID:
        if (!ipc_owner(arr->uid, arr->cuid)) { ret = -EPERM; break; }
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
#define MSGMNB       16384  /* default bytes a queue holds */
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
    uid_t      cuid;
    gid_t      cgid;
    int64_t    stime, rtime, ctime;
    int        lspid, lrpid;
    uint64_t   qbytes, cbytes;   /* the queue's byte limit, bytes in it */
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
                spin_unlock_irq(&msgq_lock, f); return -EEXIST;
            }
            msgq_t *q = &msg_queues[id];
            int err = ipc_get_may(q->uid, q->gid, q->mode, msgflg) ? id : -EACCES;
            spin_unlock_irq(&msgq_lock, f); return err;
        }
        if (!(msgflg & IPC_CREAT)) { spin_unlock_irq(&msgq_lock, f); return -ENOENT; }
    }
    int slot = -1;
    for (int i = 0; i < MSGQ_MAX; i++) if (!msg_queues[i].valid) { slot = i; break; }
    if (slot < 0) { spin_unlock_irq(&msgq_lock, f); return -ENOSPC; }
    msgq_t *q = &msg_queues[slot];
    q->key = key; q->valid = 1; q->count = 0; q->seq_next = 1;
    q->send_waiter = q->recv_waiter = NULL;
    q->ipc_ns = current_ipc();
    q->uid = q->cuid = task_current()->euid; q->gid = q->cgid = task_current()->egid;
    q->mode = msgflg & 0777;
    q->stime = q->rtime = 0; q->ctime = ipc_now();
    q->lspid = q->lrpid = 0;
    q->qbytes = MSGMNB; q->cbytes = 0;
    for (int i = 0; i < MSGQ_NMSG; i++) q->msgs[i].used = 0;
    spin_unlock_irq(&msgq_lock, f);
    return slot;
}

static long sys_msgsnd(int msqid, const void *msgp, size_t msgsz, int msgflg)
{
    if (msqid < 0 || msqid >= MSGQ_MAX || msgsz > MSGQ_MSGSZ) return -EINVAL;
    long mtype;
    if (copy_from_user(&mtype, msgp, sizeof(long)) != 0) return -EFAULT;
    if (mtype < 1) return -EINVAL;
    char tmp[MSGQ_MSGSZ];
    if (msgsz && copy_from_user(tmp, (const char *)msgp + sizeof(long), msgsz) != 0) return -EFAULT;

    while (1) {
        uint64_t f = spin_lock_irq(&msgq_lock);
        if (!msg_queues[msqid].valid || msg_queues[msqid].ipc_ns != current_ipc()) {
            spin_unlock_irq(&msgq_lock, f); return -EIDRM;
        }
        msgq_t *q = &msg_queues[msqid];
        if (!ipc_may(q->uid, q->gid, q->mode, 2)) { spin_unlock_irq(&msgq_lock, f); return -EACCES; }
        if (q->count < MSGQ_NMSG && q->cbytes + msgsz <= q->qbytes) {
            int slot = -1;
            for (int i = 0; i < MSGQ_NMSG; i++) if (!q->msgs[i].used) { slot = i; break; }
            sysv_msg_t *m = &q->msgs[slot];
            m->used = 1; m->seq = q->seq_next++; m->mtype = mtype; m->len = msgsz;
            if (msgsz) memcpy(m->data, tmp, msgsz);
            q->count++;
            q->cbytes += msgsz;
            q->stime = ipc_now();
            q->lspid = ipc_pid();
            if (q->recv_waiter) { task_wake_no_resched(q->recv_waiter); q->recv_waiter = NULL; }
            spin_unlock_irq(&msgq_lock, f);
            return 0;
        }
        if (msgflg & IPC_NOWAIT) { spin_unlock_irq(&msgq_lock, f); return -EAGAIN; }
        q->send_waiter = task_current();
        task_sleep_unlock(&msgq_lock, f);
        if (task_has_actionable_signal()) {
            f = spin_lock_irq(&msgq_lock);
            if (msg_queues[msqid].valid && msg_queues[msqid].send_waiter == task_current())
                msg_queues[msqid].send_waiter = NULL;
            spin_unlock_irq(&msgq_lock, f);
            return -EINTR;
        }
    }
}

static long sys_msgrcv(int msqid, void *msgp, size_t msgsz, long msgtyp, int msgflg)
{
    if (msqid < 0 || msqid >= MSGQ_MAX || msgsz > MSGQ_MSGSZ) return -EINVAL;

    while (1) {
        uint64_t f = spin_lock_irq(&msgq_lock);
        if (!msg_queues[msqid].valid || msg_queues[msqid].ipc_ns != current_ipc()) {
            spin_unlock_irq(&msgq_lock, f); return -EIDRM;
        }
        msgq_t *q = &msg_queues[msqid];
        if (!ipc_may(q->uid, q->gid, q->mode, 4)) { spin_unlock_irq(&msgq_lock, f); return -EACCES; }

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
                spin_unlock_irq(&msgq_lock, f); return -E2BIG;
            }
            size_t copy = m->len > msgsz ? msgsz : m->len;
            long mtype = m->mtype;
            char tmp[MSGQ_MSGSZ];
            if (copy) memcpy(tmp, m->data, copy);
            m->used = 0; q->count--;
            q->cbytes -= m->len;
            q->rtime = ipc_now();
            q->lrpid = ipc_pid();
            if (q->send_waiter) { task_wake_no_resched(q->send_waiter); q->send_waiter = NULL; }
            spin_unlock_irq(&msgq_lock, f);
            if (copy_to_user(msgp, &mtype, sizeof(long)) != 0) return -EFAULT;
            if (copy && copy_to_user((char *)msgp + sizeof(long), tmp, copy) != 0) return -EFAULT;
            return (long)copy;
        }
        if (msgflg & IPC_NOWAIT) { spin_unlock_irq(&msgq_lock, f); return -ENOMSG; }
        q->recv_waiter = task_current();
        task_sleep_unlock(&msgq_lock, f);
        if (task_has_actionable_signal()) {
            f = spin_lock_irq(&msgq_lock);
            if (msg_queues[msqid].valid && msg_queues[msqid].recv_waiter == task_current())
                msg_queues[msqid].recv_waiter = NULL;
            spin_unlock_irq(&msgq_lock, f);
            return -EINTR;
        }
    }
}

#define MSG_STAT     11
#define MSG_INFO     12
#define MSG_STAT_ANY 13

typedef struct {
    ipc_perm_t msg_perm;
    int64_t    msg_stime, msg_rtime, msg_ctime;
    uint64_t   msg_cbytes, msg_qnum, msg_qbytes;
    int        msg_lspid, msg_lrpid;
    uint64_t   _u4, _u5;
} msqid_ds_t;

typedef struct { int msgpool, msgmap, msgmax, msgmnb, msgmni, msgssz, msgtql; uint16_t msgseg; } msginfo_t;

static long msg_info(int cmd, void *buf)
{
    msginfo_t mi = { MSGQ_MAX * MSGQ_NMSG * MSGQ_MSGSZ / 1024, MSGQ_MAX * MSGQ_NMSG, MSGQ_MSGSZ, MSGMNB,
                     MSGQ_MAX, 16, MSGQ_MAX * MSGQ_NMSG, MSGQ_MAX * MSGQ_NMSG };
    int max = 0, used = 0, msgs = 0;
    uint64_t bytes = 0;
    uint64_t f = spin_lock_irq(&msgq_lock);
    for (int i = 0; i < MSGQ_MAX; i++) {
        if (!msg_queues[i].valid || msg_queues[i].ipc_ns != current_ipc()) continue;
        max = i; used++; msgs += msg_queues[i].count; bytes += msg_queues[i].cbytes;
    }
    spin_unlock_irq(&msgq_lock, f);
    /* MSG_INFO: queues, messages and bytes in use where the limits were */
    if (cmd == MSG_INFO) { mi.msgpool = used; mi.msgmap = msgs; mi.msgtql = (int)bytes; }
    return copy_to_user(buf, &mi, sizeof(mi)) ? -EFAULT : max;
}

static long sys_msgctl(int msqid, int cmd, void *buf)
{
    cmd &= ~IPC_64;
    if (cmd == IPC_INFO || cmd == MSG_INFO) return msg_info(cmd, buf);
    msqid_ds_t ds;
    if (cmd == IPC_SET && copy_from_user(&ds, buf, sizeof(ds)) != 0) return -EFAULT;
    if (msqid < 0 || msqid >= MSGQ_MAX) return -EINVAL;
    uint64_t f = spin_lock_irq(&msgq_lock);
    if (!msg_queues[msqid].valid || msg_queues[msqid].ipc_ns != current_ipc()) {
        spin_unlock_irq(&msgq_lock, f); return -EINVAL;
    }
    msgq_t *q = &msg_queues[msqid];
    long ret = 0;
    switch (cmd) {
    case IPC_RMID:
        if (!ipc_owner(q->uid, q->cuid)) { ret = -EPERM; break; }
        q->valid = 0;
        if (q->send_waiter) { task_wake_no_resched(q->send_waiter); q->send_waiter = NULL; }
        if (q->recv_waiter) { task_wake_no_resched(q->recv_waiter); q->recv_waiter = NULL; }
        break;
    case IPC_STAT: case MSG_STAT: case MSG_STAT_ANY:
        if (cmd != MSG_STAT_ANY && !ipc_may(q->uid, q->gid, q->mode, 4)) { ret = -EACCES; break; }
        __builtin_memset(&ds, 0, sizeof(ds));
        ipc_fill_perm(&ds.msg_perm, q->key, q->uid, q->gid, q->cuid, q->cgid, q->mode);
        ds.msg_stime  = q->stime;
        ds.msg_rtime  = q->rtime;
        ds.msg_ctime  = q->ctime;
        ds.msg_cbytes = q->cbytes;
        ds.msg_qnum   = (uint64_t)q->count;
        ds.msg_qbytes = q->qbytes;
        ds.msg_lspid  = q->lspid;
        ds.msg_lrpid  = q->lrpid;
        spin_unlock_irq(&msgq_lock, f);
        if (copy_to_user(buf, &ds, sizeof(ds)) != 0) return -EFAULT;
        return cmd == IPC_STAT ? 0 : msqid;
    case IPC_SET:
        if (!ipc_owner(q->uid, q->cuid)) { ret = -EPERM; break; }
        /* raising the byte limit past the default takes root */
        if (ds.msg_qbytes > MSGMNB && task_current()->euid != 0) { ret = -EPERM; break; }
        q->uid    = ds.msg_perm.uid;
        q->gid    = ds.msg_perm.gid;
        q->mode   = (q->mode & ~0777) | (int)(ds.msg_perm.mode & 0777);
        q->qbytes = ds.msg_qbytes;
        q->ctime  = ipc_now();
        break;
    default:
        ret = -EINVAL;
    }
    spin_unlock_irq(&msgq_lock, f);
    return ret;
}

/* ── procfs views ─────────────────────────────────────────── */

int sysvipc_row(int kind, int idx, sysvipc_row_t *r)
{
    int n = kind == SYSVIPC_SHM ? SHM_MAX : kind == SYSVIPC_SEM ? SEM_MAX : MSGQ_MAX;
    if (idx < 0 || idx >= n) return -1;
    __builtin_memset(r, 0, sizeof(*r));
    r->id = idx;
    uint64_t f;
    int got = 0;
    if (kind == SYSVIPC_SHM) {
        f = spin_lock_irq(&shm_lock);
        shm_seg_t *s = &shm_segs[idx];
        if ((got = s->valid && s->ipc_ns == current_ipc())) {
            r->key = s->key; r->mode = (uint32_t)s->mode; r->a = s->segsz; r->b = shm_nattch(s);
            r->pid1 = s->cpid; r->pid2 = s->lpid;
            r->uid = s->uid; r->gid = s->gid; r->cuid = s->cuid; r->cgid = s->cgid;
            r->t1 = s->atime; r->t2 = s->dtime; r->t3 = s->ctime;
        }
        spin_unlock_irq(&shm_lock, f);
    } else if (kind == SYSVIPC_SEM) {
        f = spin_lock_irq(&sem_lock);
        sem_arr_t *s = &sem_arrs[idx];
        if ((got = s->valid && s->ipc_ns == current_ipc())) {
            r->key = s->key; r->mode = (uint32_t)s->mode; r->a = (uint64_t)s->nsems;
            r->uid = s->uid; r->gid = s->gid; r->cuid = s->cuid; r->cgid = s->cgid;
            r->t1 = s->otime; r->t2 = s->ctime;
        }
        spin_unlock_irq(&sem_lock, f);
    } else {
        f = spin_lock_irq(&msgq_lock);
        msgq_t *q = &msg_queues[idx];
        if ((got = q->valid && q->ipc_ns == current_ipc())) {
            r->key = q->key; r->mode = (uint32_t)q->mode; r->a = q->cbytes; r->b = (uint64_t)q->count;
            r->pid1 = q->lspid; r->pid2 = q->lrpid;
            r->uid = q->uid; r->gid = q->gid; r->cuid = q->cuid; r->cgid = q->cgid;
            r->t1 = q->stime; r->t2 = q->rtime; r->t3 = q->ctime;
        }
        spin_unlock_irq(&msgq_lock, f);
    }
    return got;
}

uint64_t sysvipc_limit(const char *name)
{
    if (!strcmp(name, "shmmax")) return SHMMAX_BYTES;
    if (!strcmp(name, "shmmni")) return SHM_MAX;
    if (!strcmp(name, "shmall")) return SHMMAX_BYTES / PAGE_SIZE * SHM_MAX;
    if (!strcmp(name, "msgmax")) return MSGQ_MSGSZ;
    if (!strcmp(name, "msgmnb")) return MSGMNB;
    if (!strcmp(name, "msgmni")) return MSGQ_MAX;
    if (!strcmp(name, "semmsl")) return SEM_NSEMS_MAX;
    if (!strcmp(name, "semmns")) return SEM_MAX * SEM_NSEMS_MAX;
    if (!strcmp(name, "semopm")) return SEMOPM;
    if (!strcmp(name, "semmni")) return SEM_MAX;
    return 0;
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
            if (copy_fop_to_user(buf, tmp, copy) != 0) return -EFAULT;
            return (ssize_t)copy;
        }
        if (file->flags & O_NONBLOCK) { spin_unlock_irq(&mq_lock, f); return -EAGAIN; }
        mq->recv_waiter = task_current();
        task_sleep_unlock(&mq_lock, f);
        if (task_has_actionable_signal()) {
            f = spin_lock_irq(&mq_lock);
            if (mq->recv_waiter == task_current()) mq->recv_waiter = NULL;
            spin_unlock_irq(&mq_lock, f);
            return -EINTR;
        }
    }
}

static ssize_t mqfd_write(vfs_file_t *file, const void *buf, size_t len)
{
    if (len > MQ_MSGSIZE) return -EMSGSIZE;
    mq_t *mq = (mq_t *)file->inode->priv;
    char tmp[MQ_MSGSIZE];
    if (copy_fop_from_user(tmp, buf, len) != 0) return -EFAULT;
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
        if (file->flags & O_NONBLOCK) { spin_unlock_irq(&mq_lock, f); return -EAGAIN; }
        mq->send_waiter = task_current();
        task_sleep_unlock(&mq_lock, f);
        if (task_has_actionable_signal()) {
            f = spin_lock_irq(&mq_lock);
            if (mq->send_waiter == task_current()) mq->send_waiter = NULL;
            spin_unlock_irq(&mq_lock, f);
            return -EINTR;
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
    if (strncpy_from_user(kname, uname, sizeof(kname)) < 0) return -EFAULT;

    ipc_namespace_t *cur = current_ipc();
    uint64_t f = spin_lock_irq(&mq_lock);
    int slot = -1;
    for (int i = 0; i < MQ_MAX; i++) {
        if (mq_table[i].valid && mq_table[i].ipc_ns == cur &&
            strcmp(mq_table[i].name, kname) == 0) {
            if ((oflag & O_CREAT) && (oflag & O_EXCL)) {
                spin_unlock_irq(&mq_lock, f);
                return -EEXIST;
            }
            int acc = oflag & 3;
            if (!ipc_may(mq_table[i].uid, mq_table[i].gid, mq_table[i].mode,
                         acc == O_RDONLY ? 4 : (acc == O_WRONLY ? 2 : 6))) {
                spin_unlock_irq(&mq_lock, f);
                return -EACCES;
            }
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (!(oflag & O_CREAT)) { spin_unlock_irq(&mq_lock, f); return -ENOENT; }
        for (int i = 0; i < MQ_MAX; i++) {
            if (!mq_table[i].valid) { slot = i; break; }
        }
        if (slot < 0) { spin_unlock_irq(&mq_lock, f); return -ENOSPC; }
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
    if (!in) { mq_drop_ref(&mq_table[slot]); return -ENOMEM; }
    __builtin_memset(in, 0, sizeof(*in));
    in->fops     = &mq_fops;
    in->priv     = &mq_table[slot];
    in->refcount = 1;

    vfs_file_t *vf = kmalloc(sizeof(*vf));
    if (!vf) { kfree(in); mq_drop_ref(&mq_table[slot]); return -ENOMEM; }
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
    if (strncpy_from_user(kname, uname, sizeof(kname)) < 0) return -EFAULT;
    ipc_namespace_t *cur = current_ipc();
    uint64_t f = spin_lock_irq(&mq_lock);
    for (int i = 0; i < MQ_MAX; i++) {
        if (mq_table[i].valid && mq_table[i].ipc_ns == cur &&
            strcmp(mq_table[i].name, kname) == 0) {
            if (!ipc_owner(mq_table[i].uid, mq_table[i].uid)) { spin_unlock_irq(&mq_lock, f); return -EACCES; }
            if (mq_table[i].refs == 0) mq_table[i].valid = 0;
            spin_unlock_irq(&mq_lock, f);
            return 0;
        }
    }
    spin_unlock_irq(&mq_lock, f);
    return -ENOENT;
}

static long sys_mq_timedsend(int mqdes, const char *umsg, size_t len,
                             uint32_t prio, const void *ts)
{
    (void)prio; (void)ts;
    /* the write path copies through the kernel-or-user fop helper */
    if (!access_ok(umsg, len)) return -EFAULT;
    /* reuse write path */
    process_t *p = task_current();
    vfs_file_t *f = fget(p->fds, mqdes);   /* pin across the blocking send */
    if (!f || !f->inode || f->inode->fops != &mq_fops) { if (f) fput(f); return -EBADF; }
    long r = mqfd_write(f, umsg, len);
    fput(f);
    return r < 0 ? r : 0;   /* the byte count is write(2)'s answer, not this one's */
}

static long sys_mq_timedreceive(int mqdes, char *umsg, size_t len,
                                uint32_t *uprio, const void *ts)
{
    (void)uprio; (void)ts;
    /* the read path copies through the kernel-or-user fop helper */
    if (!access_ok(umsg, len)) return -EFAULT;
    process_t *p = task_current();
    vfs_file_t *f = fget(p->fds, mqdes);   /* pin across the blocking receive */
    if (!f || !f->inode || f->inode->fops != &mq_fops) { if (f) fput(f); return -EBADF; }
    long r = mqfd_read(f, umsg, len);
    fput(f);
    return r;
}

static long sys_mq_getsetattr(int mqdes, const mq_attr_t *newattr, mq_attr_t *oldattr)
{
    process_t *p = task_current();
    vfs_file_t *f = fd_get(p->fds, mqdes);
    if (!f || !f->inode || f->inode->fops != &mq_fops) return -EBADF;
    mq_t *mq = (mq_t *)f->inode->priv;
    if (oldattr) {
        mq_attr_t a = { .mq_flags = f->flags & O_NONBLOCK,
                        .mq_maxmsg = MQ_MSG_MAX,
                        .mq_msgsize = MQ_MSGSIZE,
                        .mq_curmsgs = mq->count };
        if (copy_to_user(oldattr, &a, sizeof(a)) != 0) return -EFAULT;
    }
    if (newattr) {
        mq_attr_t na;
        if (copy_from_user(&na, newattr, sizeof(na)) != 0) return -EFAULT;
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
