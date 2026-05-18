#ifndef VFS_H
#define VFS_H

#include <uapi/types.h>

/* ── mode bits ───────────────────────────────────────────── */
#define S_IFREG  0x8000
#define S_IFDIR  0x4000
#define S_IFCHR  0x2000
#define S_IFBLK  0x6000
#define S_IFIFO  0x1000
#define S_IFLNK  0xA000
#define S_ISREG(m)  (((m) & 0xF000) == S_IFREG)
#define S_ISDIR(m)  (((m) & 0xF000) == S_IFDIR)
#define S_ISCHR(m)  (((m) & 0xF000) == S_IFCHR)
#define S_ISBLK(m)  (((m) & 0xF000) == S_IFBLK)
#define S_ISFIFO(m) (((m) & 0xF000) == S_IFIFO)
#define S_ISLNK(m)  (((m) & 0xF000) == S_IFLNK)

#define O_RDONLY    0x0
#define O_WRONLY    0x1
#define O_RDWR      0x2
#define O_CREAT     0x40
#define O_TRUNC     0x200
#define O_APPEND    0x400
#define O_NONBLOCK  0x800
#define O_CLOEXEC   0x80000

#define SEEK_SET   0
#define SEEK_CUR   1
#define SEEK_END   2

struct vfs_inode;
struct vfs_file;
struct vfs_dentry;

/* ── directory entry for readdir ─────────────────────────── */
typedef struct {
    uint64_t ino;
    char     name[256];
    mode_t   mode;
    uint32_t next_off; /* opaque cursor for next readdir call */
} vfs_dirent_t;

/* ── file stat ───────────────────────────────────────────── */
typedef struct {
    uint64_t ino;
    mode_t   mode;
    uint32_t nlink;
    uid_t    uid;
    gid_t    gid;
    uint64_t size;
    dev_t    rdev;
} vfs_stat_t;

/* ── inode operations (directory / metadata) ─────────────── */
typedef struct {
    struct vfs_inode *(*lookup)(struct vfs_inode *dir, const char *name);
    int               (*create)(struct vfs_inode *dir, const char *name, mode_t mode);
    int               (*mkdir)(struct vfs_inode *dir, const char *name, mode_t mode);
    int               (*unlink)(struct vfs_inode *dir, const char *name);
    int               (*readdir)(struct vfs_inode *dir, uint32_t idx, vfs_dirent_t *out);
    int               (*stat)(struct vfs_inode *inode, vfs_stat_t *out);
    int               (*rename)(struct vfs_inode *old_dir, const char *old_name,
                                struct vfs_inode *new_dir, const char *new_name);
    int               (*readlink)(struct vfs_inode *inode, char *buf, size_t bufsize);
    int               (*symlink)(struct vfs_inode *dir, const char *name, const char *target);
} vfs_iops_t;

/* ── file operations (open fd) ───────────────────────────── */
typedef struct {
    int     (*open)(struct vfs_inode *inode, struct vfs_file *file, int flags);
    ssize_t (*read)(struct vfs_file *file, void *buf, size_t len);
    ssize_t (*write)(struct vfs_file *file, const void *buf, size_t len);
    int     (*ioctl)(struct vfs_file *file, uint32_t cmd, uint64_t arg);
    off_t   (*seek)(struct vfs_file *file, off_t off, int whence);
    int     (*close)(struct vfs_file *file);
    int     (*poll)(struct vfs_file *file); /* 1=readable, 0=not */
    uint64_t (*mmap)(struct vfs_file *file, off_t off); /* returns physical address */
} vfs_fops_t;

/* ── inode ───────────────────────────────────────────────── */
typedef struct vfs_inode {
    uint64_t     ino;
    mode_t       mode;
    uid_t        uid;
    gid_t        gid;
    uint64_t     size;
    uint32_t     nlink;
    dev_t        rdev;
    vfs_iops_t  *iops;
    vfs_fops_t  *fops;
    void        *priv;      /* filesystem-private data */
    uint32_t     refcount;
    uint32_t     no_cache;  /* 1: VFS must not cache children of this dir */
    uint32_t     cached;    /* 1: owned by dentry tree or pipe; 0: free when last fd closes */
} vfs_inode_t;

/* ── open file instance ──────────────────────────────────── */
typedef struct vfs_file {
    vfs_inode_t *inode;
    off_t        pos;
    int          flags;
    uint32_t     refcount;
    char         path[256];
} vfs_file_t;

/* ── directory entry (path tree node) ───────────────────── */
typedef struct vfs_dentry {
    char              name[256];
    vfs_inode_t      *inode;
    struct vfs_dentry *parent;
    struct vfs_dentry *child;    /* first child */
    struct vfs_dentry *next;     /* next sibling */
} vfs_dentry_t;

/* ── mount ───────────────────────────────────────────────── */
typedef struct vfs_mount {
    vfs_dentry_t      *mountpoint;
    vfs_dentry_t      *root;
    const char        *fstype;
    void              *priv;
    struct vfs_mount  *next;
} vfs_mount_t;

/* ── file descriptor table ───────────────────────────────── */
#define FD_MAX      1024   /* soft default for RLIMIT_NOFILE; table grows beyond this */
#define FD_INIT_CAP 64

typedef struct {
    vfs_file_t **files;
    int          cap;
    uint64_t    *open_bm;
    uint64_t    *cloexec;
} fd_table_t;

/* ── VMA (virtual memory area) ───────────────────────────── */
#define VMA_READ   0x1
#define VMA_WRITE  0x2
#define VMA_EXEC   0x4
#define VMA_USER   0x8
#define VMA_ANON   0x10
#define VMA_SHARED 0x20  /* MAP_SHARED — pages not COW'd on fork */

typedef struct vma {
    uint64_t    vaddr;
    uint64_t    size;
    uint32_t    flags;
    struct vma *next;
} vma_t;

/* ── VFS interface ───────────────────────────────────────── */
void       vfs_init(void);
int        vfs_mount(const char *path, const char *fstype, void *data);
int        vfs_open(const char *path, int flags, mode_t mode);
int        vfs_close(int fd);
ssize_t    vfs_read(int fd, void *buf, size_t len);
ssize_t    vfs_write(int fd, const void *buf, size_t len);
off_t      vfs_seek(int fd, off_t off, int whence);
int        vfs_mkdir(const char *path, mode_t mode);
int        vfs_unlink(const char *path);
int        vfs_kernel_install(const char *path, const void *data, size_t len);
int        vfs_symlink(const char *target, const char *linkpath);

int        vfs_stat(const char *path, vfs_stat_t *out);
int        vfs_readdir(const char *path, uint32_t idx, vfs_dirent_t *out);
int        vfs_rename(const char *old_path, const char *new_path);

/* fd_table helpers */
fd_table_t *fd_table_create(void);
fd_table_t *fd_table_copy(fd_table_t *old);
void        fd_table_destroy(fd_table_t *t);
int         fd_alloc(fd_table_t *t, vfs_file_t *f);
int         fd_alloc_at(fd_table_t *t, vfs_file_t *f, int fd);
int         fd_alloc_from(fd_table_t *t, vfs_file_t *f, int start);
void        fd_free(fd_table_t *t, int fd);
vfs_file_t *fd_get(fd_table_t *t, int fd);

static inline int fd_is_cloexec(fd_table_t *t, int fd)
{ return fd >= 0 && fd < t->cap && !!(t->cloexec[fd/64] & (1ULL << (fd%64))); }

static inline void fd_set_cloexec(fd_table_t *t, int fd, int v)
{
    if (fd < 0 || fd >= t->cap) return;
    if (v) t->cloexec[fd/64] |=  (1ULL << (fd%64));
    else   t->cloexec[fd/64] &= ~(1ULL << (fd%64));
}

/* pipe IPC */
int vfs_pipe(int pipefd[2]);

/* internal: dentry ops */
vfs_dentry_t *vfs_dentry_resolve(const char *path);
vfs_dentry_t *vfs_dentry_create(vfs_dentry_t *parent, const char *name, vfs_inode_t *inode);

#endif
