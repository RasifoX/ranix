#include <kernel/vfs.h>
#include <kernel/mm/buddy.h>
#include <kernel/sync.h>
#include <kernel/sched.h>
#include <kernel/lib/string.h>

#define PIPE_BUF_SIZE 4096

typedef struct {
    uint8_t      *buffer;
    uint32_t      head;
    uint32_t      tail;
    uint32_t      readers;
    uint32_t      writers;
    spinlock_t    lock;
    wait_queue_t  rd_wait;
    wait_queue_t  wr_wait;
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
            spin_unlock_irq(&pipe->lock, f);
            sleep_on(&pipe->rd_wait);
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
            return -1; /* Broken pipe */
        }

        uint32_t free = PIPE_BUF_SIZE - 1 - ((pipe->head - pipe->tail) % PIPE_BUF_SIZE);
        if (free == 0) {
            spin_unlock_irq(&pipe->lock, f);
            sleep_on(&pipe->wr_wait);
            continue;
        }

        uint32_t n = free < (len - written) ? free : (len - written);
        for (uint32_t i = 0; i < n; i++) {
            pipe->buffer[pipe->head] = p[written++];
            pipe->head = (pipe->head + 1) % PIPE_BUF_SIZE;
        }

        wake_up(&pipe->rd_wait);
        spin_unlock_irq(&pipe->lock, f);
    }

    return (ssize_t)written;
}

static int pipe_close(vfs_file_t *file)
{
    pipe_t *pipe = (pipe_t *)file->inode->priv;
    uint64_t f = spin_lock_irq(&pipe->lock);

    if (file->flags == O_RDONLY) pipe->readers--;
    else pipe->writers--;

    wake_up(&pipe->rd_wait);
    wake_up(&pipe->wr_wait);

    if (pipe->readers == 0 && pipe->writers == 0) {
        buddy_free((uint64_t)pipe->buffer, 0);
        buddy_free((uint64_t)pipe, 0);
        buddy_free((uint64_t)file->inode, 0);
    } else {
        spin_unlock_irq(&pipe->lock, f);
    }

    return 0;
}

static vfs_fops_t pipe_fops = {
    .read  = pipe_read,
    .write = pipe_write,
    .close = pipe_close,
};

int vfs_pipe(int pipefd[2])
{
    pipe_t *pipe = buddy_alloc(0);
    if (!pipe) return -1;
    memset(pipe, 0, sizeof(*pipe));
    pipe->buffer = buddy_alloc(0);
    if (!pipe->buffer) { buddy_free((uint64_t)pipe, 0); return -1; }
    pipe->readers = 1;
    pipe->writers = 1;
    pipe->lock = (spinlock_t)SPINLOCK_INIT;
    wait_queue_init(&pipe->rd_wait);
    wait_queue_init(&pipe->wr_wait);

    vfs_inode_t *inode = buddy_alloc(0);
    if (!inode) {
        buddy_free((uint64_t)pipe->buffer, 0);
        buddy_free((uint64_t)pipe, 0);
        return -1;
    }
    memset(inode, 0, sizeof(*inode));
    inode->mode = S_IFIFO | 0666;
    inode->fops = &pipe_fops;
    inode->priv = pipe;
    inode->refcount = 2;

    vfs_file_t *rf = buddy_alloc(0);
    vfs_file_t *wf = buddy_alloc(0);
    if (!rf || !wf) {
        /* cleanup ... */
        return -1;
    }

    memset(rf, 0, sizeof(*rf));
    rf->inode = inode;
    rf->flags = O_RDONLY;
    rf->refcount = 1;

    memset(wf, 0, sizeof(*wf));
    wf->inode = inode;
    wf->flags = O_WRONLY;
    wf->refcount = 1;

    process_t *p = task_current();
    pipefd[0] = fd_alloc(p->fds, rf);
    pipefd[1] = fd_alloc(p->fds, wf);

    return 0;
}
