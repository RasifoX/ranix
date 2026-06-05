#ifndef VFS_H
#define VFS_H

#include <uapi/types.h>
#include <kernel/sync.h>
#include <kernel/config.h>

/* ── mode bits ───────────────────────────────────────────── */
#define S_IFREG  0x8000
#define S_IFDIR  0x4000
#define S_IFCHR  0x2000
#define S_IFBLK  0x6000
#define S_IFIFO  0x1000
#define S_IFLNK  0xA000
#define S_ISUID  04000
#define S_ISGID  02000
#define S_ISVTX  01000

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
#define O_EXCL      0x80
#define O_NOCTTY    0x100
#define O_TRUNC     0x200
#define O_APPEND    0x400
#define O_NONBLOCK  0x800
/* O_DIRECTORY/O_NOFOLLOW differ between the x86_64 and the generic (aarch64) ABI */
#if defined(__x86_64__)
#define O_DIRECTORY 0x10000
#define O_NOFOLLOW  0x20000
#else
#define O_DIRECTORY 0x4000
#define O_NOFOLLOW  0x8000
#endif
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
    int64_t  atime, mtime, ctime;
    uint32_t atime_nsec, mtime_nsec, ctime_nsec;
} vfs_stat_t;

/* ── filesystem statistics (statfs) ──────────────────────── */
typedef struct {
    uint32_t bsize;     /* block size in bytes */
    uint64_t blocks;    /* total data blocks */
    uint64_t bfree;     /* free blocks */
    uint64_t bavail;    /* free blocks available to unprivileged users */
    uint64_t files;     /* total inodes */
    uint64_t ffree;     /* free inodes */
    uint32_t namelen;   /* max filename length */
} vfs_statfs_t;

/* ── inode operations (directory / metadata) ─────────────── */
typedef struct {
    struct vfs_inode *(*lookup)(struct vfs_inode *dir, const char *name);
    int               (*create)(struct vfs_inode *dir, const char *name, mode_t mode);
    int               (*mkdir)(struct vfs_inode *dir, const char *name, mode_t mode);
    int               (*mknod)(struct vfs_inode *dir, const char *name, mode_t mode, dev_t dev);
    int               (*unlink)(struct vfs_inode *dir, const char *name);
    int               (*readdir)(struct vfs_inode *dir, uint32_t idx, vfs_dirent_t *out);
    int               (*stat)(struct vfs_inode *inode, vfs_stat_t *out);
    int               (*rename)(struct vfs_inode *old_dir, const char *old_name,
                                struct vfs_inode *new_dir, const char *new_name);
    int               (*readlink)(struct vfs_inode *inode, char *buf, size_t bufsize);
    int               (*symlink)(struct vfs_inode *dir, const char *name, const char *target);
    int               (*truncate)(struct vfs_inode *inode, off_t len);
    int               (*link)(struct vfs_inode *dir, const char *name, struct vfs_inode *target);
    int               (*statfs)(struct vfs_inode *inode, vfs_statfs_t *out);
    void              (*evict)(struct vfs_inode *inode); /* free a deferred-unlink inode at last close */
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
    int      mmap_cacheable; /* 1: mmap() backing is normal write-back RAM owned by the inode
                              *    (ramfs/tmpfs) — map cacheable + NOFREE so a per-mapping unmap
                              *    never frees the file's buffer. 0: device/hugetlb, map NOCACHE. */
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
    void        *i_sb;      /* per-mount superblock (tmpfs/overlay); NULL otherwise */
    uint32_t     refcount;
    uint32_t     open_refs;       /* live open file descriptions referencing this inode */
    uint32_t     deferred_unlink; /* nlink hit 0 while still open; evict at last close */
    uint32_t     mmap_pinned; /* 1: backing buffer is live in a MAP_SHARED mapping; ramfs/tmpfs
                               *    must not relocate it on grow (would dangle the mapping). */
    uint32_t     no_cache;  /* 1: VFS must not cache children of this dir */
    uint32_t     cached;    /* 1: owned by dentry tree or pipe; 0: free when last fd closes */
    void        *xattrs;    /* extended attributes: vfs_xattr_t list (NULL if none) */
    int64_t      atime, mtime, ctime;
    uint32_t     atime_nsec, mtime_nsec, ctime_nsec;
    void        *flock_ex;   /* vfs_file holding an exclusive flock, or NULL */
    int          flock_sh;   /* count of shared flock holders */
} vfs_inode_t;

/* ── open file instance ──────────────────────────────────── */
typedef struct vfs_file {
    vfs_inode_t *inode;
    off_t        pos;
    int          flags;
    uint32_t     refcount;
    char         path[256];
    void        *priv;       /* per-open private data (e.g. captured namespace) */
    int          flock_held; /* this open file's flock: 0, LOCK_SH(1) or LOCK_EX(2) */
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
    const char        *fstype;        /* points at fstype_name (self-owned copy) */
    char               fstype_name[32];
    void              *priv;
    void             (*kill_sb)(void *priv);   /* umount destructor (tmpfs/overlay); may be NULL */
    void             (*get_sb)(void *priv);    /* bump sb refcount when a namespace clones the mount */
    vfs_inode_t       *orig_inode;
    struct vfs_mount  *next;
} vfs_mount_t;

/* ── file descriptor table ───────────────────────────────── */
#define FD_MAX      CONFIG_FD_MAX   /* soft default for RLIMIT_NOFILE; table grows beyond this */
#define FD_INIT_CAP CONFIG_FD_INIT_CAP

typedef struct {
    vfs_file_t **files;
    int          cap;
    uint64_t    *open_bm;
    uint64_t    *cloexec;
    spinlock_t   lock;      /* guards files[]/open_bm/cloexec and the per-fd refcount bumps */
    int          refcount;  /* number of threads sharing this table */
} fd_table_t;

/* ── VMA (virtual memory area) ───────────────────────────── */
#define VMA_READ   0x1
#define VMA_WRITE  0x2
#define VMA_EXEC   0x4
#define VMA_USER   0x8
#define VMA_ANON   0x10
#define VMA_SHARED 0x20  /* MAP_SHARED — pages not COW'd on fork */
#define VMA_HUGE   0x40  /* MAP_HUGETLB — backed by 2 MB blocks */
#define VMA_HUGE_1G 0x80 /* MAP_HUGETLB | MAP_HUGE_1GB — 1 GB blocks */
#define VMA_LOCKED 0x100 /* mlock/MAP_LOCKED — prefaulted, exempt from future reclaim */

typedef struct vma {
    uint64_t    vaddr;
    uint64_t    size;
    uint32_t    flags;
    struct vma *next;
} vma_t;

/* ── VFS interface ───────────────────────────────────────── */
void       vfs_init(void);
int        vfs_mount(const char *path, const char *fstype, void *data);
const char *vfs_fstype_name(int idx);

/* mount namespace — each holds a private list of mounts; processes following
 * different namespaces see different mount trees. */
typedef struct mnt_namespace {
    int            refcount;
    unsigned long  inum;
    struct vfs_mount *mounts;
} mnt_namespace_t;

extern mnt_namespace_t init_mnt_ns;
mnt_namespace_t *current_mnt_ns(void);
mnt_namespace_t *mnt_ns_get(mnt_namespace_t *ns);
void             mnt_ns_put(mnt_namespace_t *ns);
mnt_namespace_t *mnt_ns_clone(mnt_namespace_t *old);
struct vfs_file;
mnt_namespace_t *nsfile_to_mnt(struct vfs_file *f);
int        vfs_umount(const char *path);
size_t     vfs_proc_mounts(char *buf, size_t sz);
const char *vfs_path_fstype(const char *path);
int        vfs_statfs_path(const char *path, vfs_statfs_t *out);   /* per-fs statfs op; <0 if none */
int        vfs_statfs_inode(vfs_inode_t *in, vfs_statfs_t *out);
size_t     vfs_proc_mountinfo(char *buf, size_t sz);
int        vfs_open(const char *path, int flags, mode_t mode);
int        vfs_open_fds(fd_table_t *fds, uid_t uid, gid_t gid, mode_t umask,
                        const char *path, int flags, mode_t mode);
int        vfs_close(int fd);
void       vfs_file_put(vfs_file_t *f);
int        vfs_flock_try(vfs_file_t *f, int cmd);   /* cmd: 1=LOCK_SH 2=LOCK_EX */
void       vfs_flock_release(vfs_file_t *f);
int        vfs_posix_setlk(void *inode, int pid, int type, int64_t start, int64_t end);
void       vfs_posix_unlock(void *inode, int pid, int64_t start, int64_t end);
int        vfs_posix_getlk(void *inode, int pid, int *type, int64_t *start, int64_t *end, int *holder_pid);
void       vfs_posix_drop_pid(int pid);
void       vfs_posix_drop_pid_inode(void *inode, int pid);
ssize_t    vfs_read(int fd, void *buf, size_t len);
ssize_t    vfs_write(int fd, const void *buf, size_t len);
off_t      vfs_seek(int fd, off_t off, int whence);
int        vfs_mkdir(const char *path, mode_t mode);
int        vfs_mkfifo(const char *path, mode_t mode);
int        vfs_mknod(const char *path, mode_t mode, dev_t dev);
void       fifo_init_inode(vfs_inode_t *in);
int        vfs_unlink(const char *path);
int        vfs_kernel_install(const char *path, const void *data, size_t len);
int        vfs_symlink(const char *target, const char *linkpath);

int        vfs_stat(const char *path, vfs_stat_t *out);
int        vfs_lstat(const char *path, vfs_stat_t *out);
void       ranix_walltime(int64_t *sec, int64_t *nsec);
int        vfs_readdir(const char *path, uint32_t idx, vfs_dirent_t *out);
int        vfs_rename(const char *old_path, const char *new_path);
int        vfs_chmod(const char *path, mode_t mode);
int        vfs_chown(const char *path, uid_t uid, gid_t gid);
int        vfs_chown_inode(vfs_inode_t *in, uid_t uid, gid_t gid);
int        vfs_link(const char *oldpath, const char *newpath);

/* fd_table helpers */
fd_table_t *fd_table_create(void);
fd_table_t *fd_table_copy(fd_table_t *old);
fd_table_t *fd_table_get(fd_table_t *t);   /* increment refcount, return t */
void        fd_table_put(fd_table_t *t);    /* decrement refcount; destroy when 0 */
void        fd_table_destroy(fd_table_t *t); /* unconditional destroy (legacy) */
int         fd_alloc(fd_table_t *t, vfs_file_t *f);
int         fd_alloc_at(fd_table_t *t, vfs_file_t *f, int fd);
int         fd_alloc_from(fd_table_t *t, vfs_file_t *f, int start);
void        fd_free(fd_table_t *t, int fd);
vfs_file_t *fd_get(fd_table_t *t, int fd);
vfs_file_t *fget(fd_table_t *t, int fd);   /* fd_get + take a reference; release with fput */
void        fput(vfs_file_t *f);           /* drop a fget reference (frees the file at refcount 0) */
vfs_file_t *fd_remove(fd_table_t *t, int fd); /* atomically detach fd; caller fputs the result */

static inline int fd_is_cloexec(fd_table_t *t, int fd)
{ return fd >= 0 && fd < t->cap && !!(t->cloexec[fd/64] & (1ULL << (fd%64))); }

/* inotify VFS hook — set by inotify subsystem at first init */
typedef void (*vfs_inotify_fn_t)(const char *path, uint32_t mask, const char *name);
extern vfs_inotify_fn_t vfs_inotify_hook;

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

/* extended attributes — generic per-inode key/value store */
#define XATTR_CREATE  1   /* fail (EEXIST) if the attribute already exists */
#define XATTR_REPLACE 2   /* fail (ENODATA) if the attribute does not exist */
int     vfs_inode_setxattr(vfs_inode_t *in, const char *name, const void *val, size_t len, int flags);
ssize_t vfs_inode_getxattr(vfs_inode_t *in, const char *name, void *val, size_t size);
ssize_t vfs_inode_listxattr(vfs_inode_t *in, char *list, size_t size);
int     vfs_inode_removexattr(vfs_inode_t *in, const char *name);
void    vfs_inode_free_xattrs(vfs_inode_t *in);

#endif
