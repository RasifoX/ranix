/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/vfs.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/mmu.h>
#include <kernel/sync.h>
#include <kernel/proc.h>
#include <kernel/lib/string.h>
#include <kernel/config.h>
#include <kernel/hal.h>
#include <kernel/cgroup.h>
#include <kernel/errno.h>
#include <kernel/capabilities.h>


typedef struct {
    uint8_t      *buffer;
    uint32_t      size;        /* bytes the buffer holds: a power-of-two run of pages */
    int           order;       /* its buddy order */
    uint32_t      count;       /* bytes in it now; head and tail alone cannot tell full from empty */
    uint32_t      head;
    uint32_t      tail;
    uint32_t      readers;
    uint32_t      writers;
    spinlock_t    lock;
    void         *cg;          /* cgroup paying for the buffer */
    wait_queue_t  rd_wait;
    wait_queue_t  wr_wait;
    wait_queue_t  open_wait;   /* FIFO open rendezvous (wait for the other end) */
    vfs_file_t   *async_rd;    /* the read end that asked for O_ASYNC, told when data comes */
} pipe_t;

static void *pipe_buffer_alloc(pipe_t *pipe)
{
    if (cgroup_charge_file(&pipe->cg, 0, PIPE_BUF_SIZE) != 0) return NULL;
    void *p = buddy_alloc(0);
    if (!p) { cgroup_file_release(pipe->cg, PIPE_BUF_SIZE); return NULL; }
    pipe->size  = PIPE_BUF_SIZE;
    pipe->order = 0;
    return p;
}

static void pipe_buffer_undo(pipe_t *pipe)
{
    buddy_free(VIRT_TO_PHYS((uint64_t)pipe->buffer), pipe->order);
    cgroup_file_release(pipe->cg, pipe->size);
}

/* The ring is copied through a kernel buffer: user memory can fault, and a fault
 * may need the disk or a sleeping lock, neither of which is allowed under
 * pipe->lock. A read that faults after dequeuing loses those bytes. */
static ssize_t pipe_read(vfs_file_t *file, void *buf, size_t len)
{
    pipe_t *pipe = (pipe_t *)file->inode->priv;
    uint8_t *p = (uint8_t *)buf;
    size_t read = 0;
    ssize_t ret;

    if (len == 0) return 0;
    uint8_t *kbuf = kmalloc(len < PIPE_BUF_SIZE ? len : PIPE_BUF_SIZE);
    if (!kbuf) return -ENOMEM;

    while (read < len) {
        uint64_t f = spin_lock_irq(&pipe->lock);

        uint32_t avail = pipe->count;
        if (avail == 0) {
            if (read > 0 || pipe->writers == 0) {
                spin_unlock_irq(&pipe->lock, f);
                break; /* short read or EOF */
            }
            if (file->flags & O_NONBLOCK) {
                spin_unlock_irq(&pipe->lock, f);
                ret = -11; /* EAGAIN */
                goto out;
            }
            if (task_has_actionable_signal()) {
                spin_unlock_irq(&pipe->lock, f);
                ret = -4; /* EINTR */
                goto out;
            }
            sleep_on_locked(&pipe->rd_wait, &pipe->lock, f);
            continue;
        }

        int was_full = avail == pipe->size;
        uint32_t n = avail < (len - read) ? avail : (len - read);
        if (n > PIPE_BUF_SIZE) n = PIPE_BUF_SIZE;   /* kbuf's size */
        for (uint32_t i = 0; i < n; i++) {
            kbuf[i] = pipe->buffer[pipe->tail];
            pipe->tail = (pipe->tail + 1) % pipe->size;
        }
        pipe->count -= n;

        wake_up(&pipe->wr_wait);
        spin_unlock_irq(&pipe->lock, f);
        if (was_full) poll_notify_key(pipe);   /* an epoll/poll writer waits on poll_wq, not wr_wait */

        if (copy_fop_to_user(p + read, kbuf, n) != 0) {
            ret = read ? (ssize_t)read : -14; /* EFAULT */
            goto out;
        }
        read += n;
    }
    ret = (ssize_t)read;
out:
    kfree(kbuf);
    return ret;
}

static ssize_t pipe_write(vfs_file_t *file, const void *buf, size_t len)
{
    pipe_t *pipe = (pipe_t *)file->inode->priv;
    const uint8_t *p = (const uint8_t *)buf;
    size_t cap = len < PIPE_BUF_SIZE ? len : PIPE_BUF_SIZE;
    size_t written = 0;
    ssize_t ret;

    if (len == 0) return 0;
    uint8_t *kbuf = kmalloc(cap);
    if (!kbuf) return -ENOMEM;

    while (written < len) {
        size_t chunk = (len - written) < cap ? (len - written) : cap;
        if (copy_fop_from_user(kbuf, p + written, chunk) != 0) {
            ret = written ? (ssize_t)written : -14; /* EFAULT */
            goto out;
        }

        size_t off = 0;
        while (off < chunk) {
            uint64_t f = spin_lock_irq(&pipe->lock);

            if (pipe->readers == 0) {
                spin_unlock_irq(&pipe->lock, f);
                task_signal(task_current(), 13); /* SIGPIPE */
                ret = written ? (ssize_t)written : -32; /* EPIPE */
                goto out;
            }

            uint32_t free = pipe->size - pipe->count;
            /* POSIX: a write of up to PIPE_BUF goes in whole or waits, so writers
             * sharing a pipe never interleave inside one record */
            uint32_t need = len <= PIPE_BUF_SIZE ? (uint32_t)(chunk - off) : 1;
            if (free < need) {
                if (file->flags & O_NONBLOCK) {
                    spin_unlock_irq(&pipe->lock, f);
                    ret = written ? (ssize_t)written : -11; /* EAGAIN */
                    goto out;
                }
                if (task_has_actionable_signal()) {
                    spin_unlock_irq(&pipe->lock, f);
                    ret = written ? (ssize_t)written : -4; /* EINTR */
                    goto out;
                }
                sleep_on_locked(&pipe->wr_wait, &pipe->lock, f);
                continue;
            }

            uint32_t n = free < (chunk - off) ? free : (uint32_t)(chunk - off);
            for (uint32_t i = 0; i < n; i++) {
                pipe->buffer[pipe->head] = kbuf[off + i];
                pipe->head = (pipe->head + 1) % pipe->size;
            }
            pipe->count += n;

            wake_up(&pipe->rd_wait);
            vfs_file_t *async_rd = pipe->async_rd;
            spin_unlock_irq(&pipe->lock, f);
            poll_notify_key(pipe);
            vfs_file_sigio(async_rd);
            off += n;
            written += n;
        }
    }
    ret = (ssize_t)written;
out:
    kfree(kbuf);
    return ret;
}

static int pipe_close(vfs_file_t *file)
{
    pipe_t *pipe = (pipe_t *)file->inode->priv;
    uint64_t f = spin_lock_irq(&pipe->lock);

    if ((file->flags & 3) == O_RDONLY) pipe->readers--;
    else                               pipe->writers--;
    if (pipe->async_rd == file) pipe->async_rd = NULL;

    wake_up(&pipe->rd_wait);
    wake_up(&pipe->wr_wait);
    poll_notify();

    if (pipe->readers == 0 && pipe->writers == 0) {
        spin_unlock_irq(&pipe->lock, f);
        pipe_buffer_undo(pipe);
        vfs_inode_t *inode = file->inode;
        kfree(pipe);
        kfree(inode);
    } else {
        spin_unlock_irq(&pipe->lock, f);
    }

    return 0;
}

static int pipe_poll(vfs_file_t *file)
{
    pipe_t *pipe = (pipe_t *)file->inode->priv;
    uint64_t f = spin_lock_irq(&pipe->lock);
    uint32_t avail = pipe->count;
    uint32_t free  = pipe->size - avail;
    spin_unlock_irq(&pipe->lock, f);
    int acc = file->flags & 3;
    int r = 0;
    if (acc != O_WRONLY) {
        if (avail > 0) r |= 0x0001; /* POLLIN  */
        if (pipe->writers == 0) r |= 0x0011; /* POLLHUP | POLLIN: drained EOF is readable */
    }
    if (acc != O_RDONLY) {
        if (free > 0) r |= 0x0004; /* POLLOUT */
        if (pipe->readers == 0) r |= 0x0008; /* POLLERR: write end with no reader */
    }
    return r;
}

#define FIONREAD 0x541B

static int pipe_ioctl(vfs_file_t *file, uint32_t cmd, uint64_t arg)
{
    if (cmd != FIONREAD) return -ENOTTY;
    pipe_t *pipe = (pipe_t *)file->inode->priv;
    uint64_t f = spin_lock_irq(&pipe->lock);
    int avail = (int)pipe->count;
    spin_unlock_irq(&pipe->lock, f);
    return copy_to_user((void *)(uintptr_t)arg, &avail, sizeof(avail)) ? -14 : 0;
}

static vfs_fops_t pipe_fops = {
    .poll_keyed = 1,
    .read  = pipe_read,
    .write = pipe_write,
    .close = pipe_close,
    .poll  = pipe_poll,
    .ioctl = pipe_ioctl,
};

/* ── named pipes (FIFO) — mknod S_IFIFO + open rendezvous ──────────
 * The pipe_t lives on the inode for its lifetime (lazily created on first
 * open); reuses pipe_read/write/poll. open() blocks until both a reader and a
 * writer are present (Linux FIFO semantics), unless O_NONBLOCK. */
static spinlock_t fifo_glock = SPINLOCK_INIT;

static int fifo_open(vfs_inode_t *inode, vfs_file_t *file, int flags)
{
    uint64_t g = spin_lock_irq(&fifo_glock);
    pipe_t *pipe = (pipe_t *)inode->priv;
    if (!pipe) {
        pipe = kmalloc(sizeof(*pipe));
        if (!pipe) { spin_unlock_irq(&fifo_glock, g); return -ENOMEM; }
        memset(pipe, 0, sizeof(*pipe));
        uint64_t bp = (uint64_t)pipe_buffer_alloc(pipe);
        if (!bp) { kfree(pipe); spin_unlock_irq(&fifo_glock, g); return -ENOMEM; }
        pipe->buffer = (uint8_t *)PHYS_TO_VIRT(bp);
        pipe->lock = (spinlock_t)SPINLOCK_INIT;
        wait_queue_init(&pipe->rd_wait);
        wait_queue_init(&pipe->wr_wait);
        wait_queue_init(&pipe->open_wait);
        inode->priv = pipe;
    }
    spin_unlock_irq(&fifo_glock, g);

    int acc = flags & 3;   /* O_RDONLY=0 O_WRONLY=1 O_RDWR=2 */
    uint64_t f = spin_lock_irq(&pipe->lock);
    if (acc != O_WRONLY) pipe->readers++;
    if (acc != O_RDONLY) pipe->writers++;
    wake_up(&pipe->open_wait);
    spin_unlock_irq(&pipe->lock, f);

    if (acc == O_RDONLY) {
        for (;;) {
            f = spin_lock_irq(&pipe->lock);
            if (pipe->writers > 0 || (file->flags & O_NONBLOCK)) { spin_unlock_irq(&pipe->lock, f); break; }
            if (task_has_actionable_signal()) { spin_unlock_irq(&pipe->lock, f); return -EINTR; }
            sleep_on_locked(&pipe->open_wait, &pipe->lock, f);
        }
    } else if (acc == O_WRONLY) {
        for (;;) {
            f = spin_lock_irq(&pipe->lock);
            if (pipe->readers > 0) { spin_unlock_irq(&pipe->lock, f); break; }
            if (file->flags & O_NONBLOCK) { pipe->writers--; spin_unlock_irq(&pipe->lock, f); return -ENXIO; }
            if (task_has_actionable_signal()) { pipe->writers--; spin_unlock_irq(&pipe->lock, f); return -EINTR; }
            sleep_on_locked(&pipe->open_wait, &pipe->lock, f);
        }
    }
    return 0;
}

static int fifo_close(vfs_file_t *file)
{
    pipe_t *pipe = (pipe_t *)file->inode->priv;
    if (!pipe) return 0;
    uint64_t f = spin_lock_irq(&pipe->lock);
    int acc = file->flags & 3;
    if (acc != O_WRONLY && pipe->readers > 0) pipe->readers--;
    if (pipe->async_rd == file) pipe->async_rd = NULL;
    if (acc != O_RDONLY && pipe->writers > 0) pipe->writers--;
    wake_up(&pipe->rd_wait);
    wake_up(&pipe->wr_wait);
    wake_up(&pipe->open_wait);
    spin_unlock_irq(&pipe->lock, f);
    poll_notify();
    return 0;   /* pipe_t kept on the inode for its lifetime (freed with inode) */
}

static vfs_fops_t fifo_fops = {
    .poll_keyed = 1,
    .open  = fifo_open,
    .read  = pipe_read,
    .write = pipe_write,
    .close = fifo_close,
    .poll  = pipe_poll,
    .ioctl = pipe_ioctl,
};

void fifo_init_inode(vfs_inode_t *in)
{
    in->fops = &fifo_fops;
    in->priv = NULL;   /* pipe created on first open */
}

void fifo_free_inode(vfs_inode_t *in)
{
    pipe_t *pipe = in->priv;
    in->priv = NULL;
    if (!pipe) return;
    pipe_buffer_undo(pipe);
    kfree(pipe);
}

int vfs_pipe(int pipefd[2])
{
    pipe_t *pipe = kmalloc(sizeof(*pipe));
    if (!pipe) return -1;
    memset(pipe, 0, sizeof(*pipe));
    uint64_t buf_phys = (uint64_t)pipe_buffer_alloc(pipe);
    if (!buf_phys) { kfree(pipe); return -1; }
    pipe->buffer = (uint8_t *)PHYS_TO_VIRT(buf_phys);
    pipe->readers = 1;
    pipe->writers = 1;
    pipe->lock = (spinlock_t)SPINLOCK_INIT;
    wait_queue_init(&pipe->rd_wait);
    wait_queue_init(&pipe->wr_wait);

    vfs_inode_t *inode = kmalloc(sizeof(*inode));
    if (!inode) {
        pipe_buffer_undo(pipe);
        kfree(pipe);
        return -1;
    }
    memset(inode, 0, sizeof(*inode));
    inode->mode   = S_IFIFO | 0666;
    inode->fops   = &pipe_fops;
    inode->priv   = pipe;
    inode->refcount = 2;
    inode->cached = 1; /* pipe_close handles inode lifetime internally */

    vfs_file_t *rf = kmalloc(sizeof(*rf));
    vfs_file_t *wf = kmalloc(sizeof(*wf));
    if (!rf || !wf) {
        kfree(rf);
        kfree(wf);
        pipe_buffer_undo(pipe);
        kfree(pipe);
        kfree(inode);
        return -1;
    }

    memset(rf, 0, sizeof(*rf));
    rf->inode = inode;
    rf->flags = O_RDONLY;
    rf->refcount = 0;

    memset(wf, 0, sizeof(*wf));
    wf->inode = inode;
    wf->flags = O_WRONLY;
    wf->refcount = 0;

    process_t *p = task_current();
    pipefd[0] = fd_alloc(p->fds, rf);
    pipefd[1] = fd_alloc(p->fds, wf);

    return 0;
}

/* /proc/sys/fs/pipe-max-size: what F_SETPIPE_SZ grants without CAP_SYS_RESOURCE */
unsigned long pipe_max_size = 1048576;

/* F_GETPIPE_SZ and F_SETPIPE_SZ. A size becomes a power-of-two number of pages;
 * the data in the pipe moves over, and must fit (EBUSY). -EBADF: not a pipe. */
long pipe_fcntl_size(vfs_file_t *file, int set, unsigned long arg)
{
    if (!file->inode || (file->inode->fops != &pipe_fops && file->inode->fops != &fifo_fops) ||
        !file->inode->priv)
        return -EBADF;
    pipe_t *pipe = (pipe_t *)file->inode->priv;
    if (!set) return pipe->size;
    if (arg > (1UL << 31)) return -EINVAL;
    uint32_t size = PAGE_SIZE;
    int order = 0;
    while (size < arg) { size <<= 1; order++; }
    if (size == pipe->size) return size;
    if (size > pipe_max_size && size > pipe->size && !capable_task(task_current(), CAP_SYS_RESOURCE))
        return -EPERM;
    if (order >= BUDDY_MAX_ORDER) return -ENOMEM;
    void *cg = pipe->cg;
    if (cgroup_charge_file(&cg, 0, size) != 0) return -ENOMEM;
    void *phys = buddy_alloc(order);
    if (!phys) { cgroup_file_release(cg, size); return -ENOMEM; }
    uint8_t *nb = (uint8_t *)PHYS_TO_VIRT((uint64_t)phys);

    uint64_t f = spin_lock_irq(&pipe->lock);
    if (pipe->count > size) {
        spin_unlock_irq(&pipe->lock, f);
        buddy_free((uint64_t)phys, order);
        cgroup_file_release(cg, size);
        return -EBUSY;
    }
    for (uint32_t i = 0; i < pipe->count; i++) nb[i] = pipe->buffer[(pipe->tail + i) % pipe->size];
    uint8_t *old = pipe->buffer;
    int old_order = pipe->order;
    uint32_t old_size = pipe->size;
    pipe->buffer = nb;
    pipe->size   = size;
    pipe->order  = order;
    pipe->tail   = 0;
    pipe->head   = pipe->count % size;
    wake_up(&pipe->wr_wait);
    spin_unlock_irq(&pipe->lock, f);
    buddy_free(VIRT_TO_PHYS((uint64_t)old), old_order);
    cgroup_file_release(cg, old_size);
    poll_notify_key(pipe);
    return size;
}

/* F_SETFL changed O_ASYNC on a pipe end: its read end is told of new data */
void pipe_fcntl_async(vfs_file_t *file)
{
    if (!file->inode || (file->inode->fops != &pipe_fops && file->inode->fops != &fifo_fops) ||
        !file->inode->priv || (file->flags & 3) == O_WRONLY)
        return;
    pipe_t *pipe = (pipe_t *)file->inode->priv;
    uint64_t f = spin_lock_irq(&pipe->lock);
    if (file->flags & 020000 /* O_ASYNC */) pipe->async_rd = file;
    else if (pipe->async_rd == file)        pipe->async_rd = NULL;
    spin_unlock_irq(&pipe->lock, f);
}
