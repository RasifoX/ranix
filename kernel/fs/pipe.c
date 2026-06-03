#include <kernel/vfs.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/mmu.h>
#include <kernel/sync.h>
#include <kernel/proc.h>
#include <kernel/lib/string.h>
#include <kernel/config.h>

typedef struct {
    uint8_t      *buffer;
    uint32_t      head;
    uint32_t      tail;
    uint32_t      readers;
    uint32_t      writers;
    spinlock_t    lock;
    wait_queue_t  rd_wait;
    wait_queue_t  wr_wait;
    wait_queue_t  open_wait;   /* FIFO open rendezvous (wait for the other end) */
} pipe_t;

static ssize_t pipe_read(vfs_file_t *file, void *buf, size_t len)
{
    pipe_t *pipe = (pipe_t *)file->inode->priv;
    uint8_t *p = (uint8_t *)buf;
    size_t read = 0;

    while (read < len) {
        uint64_t f = spin_lock_irq(&pipe->lock);

        uint32_t avail = (pipe->head - pipe->tail) % PIPE_BUF_SIZE;
        if (avail == 0) {
            if (pipe->writers == 0) {
                spin_unlock_irq(&pipe->lock, f);
                break; /* EOF */
            }
            if (file->flags & O_NONBLOCK) {
                spin_unlock_irq(&pipe->lock, f);
                return read ? (ssize_t)read : -11; /* EAGAIN */
            }
            sleep_on_locked(&pipe->rd_wait, &pipe->lock, f);
            continue;
        }

        uint32_t n = avail < (len - read) ? avail : (len - read);
        for (uint32_t i = 0; i < n; i++) {
            p[read++] = pipe->buffer[pipe->tail];
            pipe->tail = (pipe->tail + 1) % PIPE_BUF_SIZE;
        }

        wake_up(&pipe->wr_wait);
        spin_unlock_irq(&pipe->lock, f);
    }

    return (ssize_t)read;
}

static ssize_t pipe_write(vfs_file_t *file, const void *buf, size_t len)
{
    pipe_t *pipe = (pipe_t *)file->inode->priv;
    const uint8_t *p = (const uint8_t *)buf;
    size_t written = 0;

    while (written < len) {
        uint64_t f = spin_lock_irq(&pipe->lock);

        if (pipe->readers == 0) {
            spin_unlock_irq(&pipe->lock, f);
            task_signal(task_current(), 13); /* SIGPIPE */
            return -32; /* EPIPE */
        }

        uint32_t free = PIPE_BUF_SIZE - 1 - ((pipe->head - pipe->tail) % PIPE_BUF_SIZE);
        if (free == 0) {
            if (file->flags & O_NONBLOCK) {
                spin_unlock_irq(&pipe->lock, f);
                return written ? (ssize_t)written : -11; /* EAGAIN */
            }
            sleep_on_locked(&pipe->wr_wait, &pipe->lock, f);
            continue;
        }

        uint32_t n = free < (len - written) ? free : (len - written);
        for (uint32_t i = 0; i < n; i++) {
            pipe->buffer[pipe->head] = p[written++];
            pipe->head = (pipe->head + 1) % PIPE_BUF_SIZE;
        }

        wake_up(&pipe->rd_wait);
        spin_unlock_irq(&pipe->lock, f);
        poll_notify();
    }

    return (ssize_t)written;
}

static int pipe_close(vfs_file_t *file)
{
    pipe_t *pipe = (pipe_t *)file->inode->priv;
    uint64_t f = spin_lock_irq(&pipe->lock);

    if ((file->flags & 3) == O_RDONLY) pipe->readers--;
    else                               pipe->writers--;

    wake_up(&pipe->rd_wait);
    wake_up(&pipe->wr_wait);
    poll_notify();

    if (pipe->readers == 0 && pipe->writers == 0) {
        spin_unlock_irq(&pipe->lock, f);
        buddy_free(VIRT_TO_PHYS((uint64_t)pipe->buffer), 0);
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
    uint32_t avail = (pipe->head - pipe->tail) % PIPE_BUF_SIZE;
    uint32_t free  = PIPE_BUF_SIZE - 1 - avail;
    spin_unlock_irq(&pipe->lock, f);
    int r = 0;
    if (avail > 0) r |= 0x0001; /* POLLIN  */
    if (free  > 0) r |= 0x0004; /* POLLOUT */
    return r;
}

static vfs_fops_t pipe_fops = {
    .read  = pipe_read,
    .write = pipe_write,
    .close = pipe_close,
    .poll  = pipe_poll,
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
        if (!pipe) { spin_unlock_irq(&fifo_glock, g); return -12; }
        memset(pipe, 0, sizeof(*pipe));
        uint64_t bp = (uint64_t)buddy_alloc(0);
        if (!bp) { kfree(pipe); spin_unlock_irq(&fifo_glock, g); return -12; }
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
            sleep_on_locked(&pipe->open_wait, &pipe->lock, f);
        }
    } else if (acc == O_WRONLY) {
        for (;;) {
            f = spin_lock_irq(&pipe->lock);
            if (pipe->readers > 0) { spin_unlock_irq(&pipe->lock, f); break; }
            if (file->flags & O_NONBLOCK) { pipe->writers--; spin_unlock_irq(&pipe->lock, f); return -6; /* ENXIO */ }
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
    if (acc != O_RDONLY && pipe->writers > 0) pipe->writers--;
    wake_up(&pipe->rd_wait);
    wake_up(&pipe->wr_wait);
    wake_up(&pipe->open_wait);
    spin_unlock_irq(&pipe->lock, f);
    poll_notify();
    return 0;   /* pipe_t kept on the inode for its lifetime (freed with inode) */
}

static vfs_fops_t fifo_fops = {
    .open  = fifo_open,
    .read  = pipe_read,
    .write = pipe_write,
    .close = fifo_close,
    .poll  = pipe_poll,
};

void fifo_init_inode(vfs_inode_t *in)
{
    in->fops = &fifo_fops;
    in->priv = NULL;   /* pipe created on first open */
}

int vfs_pipe(int pipefd[2])
{
    pipe_t *pipe = kmalloc(sizeof(*pipe));
    if (!pipe) return -1;
    memset(pipe, 0, sizeof(*pipe));
    uint64_t buf_phys = (uint64_t)buddy_alloc(0);
    if (!buf_phys) { kfree(pipe); return -1; }
    pipe->buffer = (uint8_t *)PHYS_TO_VIRT(buf_phys);
    pipe->readers = 1;
    pipe->writers = 1;
    pipe->lock = (spinlock_t)SPINLOCK_INIT;
    wait_queue_init(&pipe->rd_wait);
    wait_queue_init(&pipe->wr_wait);

    vfs_inode_t *inode = kmalloc(sizeof(*inode));
    if (!inode) {
        buddy_free(VIRT_TO_PHYS((uint64_t)pipe->buffer), 0);
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
        buddy_free(VIRT_TO_PHYS((uint64_t)pipe->buffer), 0);
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
