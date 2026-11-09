/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef VFS_H
#define VFS_H

#include <uapi/types.h>
#include <kernel/sync.h>
#include <kernel/config.h>
#include <kernel/time.h>   /* ranix_walltime for timestamps */

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

/* *at() flags — arch-generic in the Linux uapi (asm-generic/fcntl.h) */
#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x0100
#define AT_EACCESS          0x0200
#define AT_REMOVEDIR        0x0200
#define AT_SYMLINK_FOLLOW   0x0400
#define AT_EMPTY_PATH       0x1000

#define SEEK_SET   0
#define SEEK_CUR   1
#define SEEK_END   2

/* ── mount flags (subset of the Linux MS_* ABI) ──────────── */
#define MS_RDONLY  0x0001
#define MS_NOSUID  0x0002
#define MS_NODEV   0x0004
#define MS_NOEXEC  0x0008
#define MS_REMOUNT 0x0020

struct vfs_inode;
struct vfs_iattr;
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
    uint32_t sdev;           /* the filesystem it is on (minor of major 0), set by the VFS; 0 unknown */
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
    int               (*setattr)(struct vfs_inode *in, const struct vfs_iattr *a);  /* NULL: in-core edit only */
    void              (*release)(struct vfs_inode *in); /* this in-core copy is being freed; may run under vfs_lock */
    struct vfs_file  *(*link_file)(struct vfs_inode *link); /* a link to an open file (procfs fd/N):
                                                              * that file, pinned (fput), or NULL */
} vfs_iops_t;

/* A metadata change. Every chmod, chown, utimens and xattr write comes through
 * vfs_inode_setattr, so a filesystem sees them all at one hook. */
#define ATTR_MODE         0x01
#define ATTR_UID          0x02
#define ATTR_GID          0x04
#define ATTR_ATIME        0x08
#define ATTR_MTIME        0x10
#define ATTR_CTIME        0x20
#define ATTR_XATTR_SET    0x40
#define ATTR_XATTR_REMOVE 0x80

typedef struct vfs_iattr {
    uint32_t    valid;      /* ATTR_* */
    mode_t      mode;       /* permission bits only */
    uid_t       uid;
    gid_t       gid;
    int64_t     atime, mtime, ctime;
    uint32_t    atime_nsec, mtime_nsec, ctime_nsec;
    const char *xname;
    const void *xval;
    size_t      xlen;
    int         xflags;
} vfs_iattr_t;

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
    int      mmap_keep_pages; /* kernel owns the pages: unmap must not free them */
    int mmap_wc;      /* map write-combining: a framebuffer */
    int poll_keyed;   /* its readiness changes come through poll_notify_key(inode->priv) */
    int      mmap_cacheable; /* 1: mmap() backing is normal write-back RAM owned by the inode
                              *    (ramfs/tmpfs) — map cacheable + NOFREE so a per-mapping unmap
                              *    never frees the file's buffer. 0: device/hugetlb, map NOCACHE. */
    /* Page of the file at off with one reference for the mapping, for shared and
     * private maps alike; a private one is mapped read-only and copied on write.
     * shared_write: stores through the mapping must reach the file. */
    uint64_t (*mmap_page)(struct vfs_file *file, off_t off, int shared_write);
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
    uint32_t     deferred_unlink; /* no name left; evict once out of every file and walk */
    uint32_t     no_cache;  /* 1: VFS must not cache children of this dir */
    uint32_t     cached;    /* 1: owned by dentry tree or pipe; 0: free when last fd closes */
    uint32_t     priv_static; /* 1: priv borrows static/caller-owned data — do not kfree it */
    void        *xattrs;    /* extended attributes: vfs_xattr_t list (NULL if none) */
    int64_t      atime, mtime, ctime;
    uint32_t     atime_nsec, mtime_nsec, ctime_nsec;
    void        *flock_ex;   /* vfs_file holding an exclusive flock, or NULL */
    int          flock_sh;   /* count of shared flock holders */
    int          in_grave;   /* queued for iops->evict */
    void        *retired_by; /* who queued it: a debugging aid */
    int          dotdot_lookup; /* no_cache dir: lookup("..") names its parent, NULL at the mount root */
    int          lookup_copy;   /* made by lookup for one dentry: the cache may drop both, nothing is lost */
    struct vfs_inode *grave_next;
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
    unsigned long mnt_flags; /* MS_* of the mount it was opened through; 0 if no path */
    char        *lpath;      /* kmalloc'd absolute path when it does not fit path[]; else NULL */
    void        *bdev;       /* block device of the filesystem it is on, held while open */
    uint32_t     sdev;       /* st_dev of that filesystem, for fstat */
    struct vfs_dirpos *at;   /* an open directory: where it stands; see vfs_file_dirpath */
    const void  *opened_root; /* the root its path starts from; compared, never followed (walk_fdlink) */
} vfs_file_t;

/* ── directory entry (path tree node) ───────────────────── */
typedef struct vfs_dentry {
    char              name[256];
    vfs_inode_t      *inode;
    struct vfs_dentry *parent;
    struct vfs_dentry *child;    /* first child */
    struct vfs_dentry *next;     /* next sibling */
    uint32_t           pins;     /* mounts holding this dentry; unlink and umount refuse */
    uint32_t           retired;  /* waiting in the grave: a walk may still read it */
    struct vfs_dentry *grave_next;   /* its own link: next stays what a walker follows */
    struct vfs_dentry **pprev;   /* the link pointing here (parent->child or a sibling's next); NULL if in no list */
    struct vfs_dentry *lru_prev, *lru_next;   /* cache order, newest first; under vfs_lock */
    uint32_t           keep;     /* some task's root: never evicted */
    uint32_t           holds;    /* open directories standing here: not evicted, and freed
                                  * after the last one closes; under grave_lock */
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
    int              (*busy)(void *priv);      /* umount asks: nonzero while still in use. Under
                                                * vfs_lock, so it must not sleep; may be NULL */
    vfs_inode_t       *orig_inode;
    unsigned long      flags;         /* per-mount MS_RDONLY/NOSUID/NODEV/NOEXEC */
    int                pin_root;      /* root belongs to another mount's tree (session views) */
    int                ro_lock;       /* read-only for good: no remount may lift it */
    char               mnt_path[256]; /* absolute mountpoint path, for flag/fstype lookup */
    void              *bdev;          /* block device under it, counted in its users; else NULL */
    uint32_t           sdev;          /* st_dev of everything on it (major 0); a bind shares its source's */
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
    int          rlim_nofile; /* RLIMIT_NOFILE soft cap: fd_alloc refuses fd >= this (EMFILE) */
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
#define VMA_NOWRITE 0x200 /* MAP_SHARED of a read-only fd: mprotect must not add write */
#define VMA_NOEXEC  0x400 /* file from a noexec mount: mprotect must not add exec (Linux !VM_MAYEXEC) */

typedef struct vma {
    uint64_t    vaddr;
    uint64_t    size;
    uint32_t    flags;
    struct vma *next;
} vma_t;

/* ── VFS interface ───────────────────────────────────────── */
void       vfs_init(void);
int        vfs_mount(const char *path, const char *fstype, void *data);
int        vfs_mount_flags(const char *path, const char *fstype, void *data, unsigned long flags);
int        vfs_remount(const char *path, unsigned long flags);
unsigned long vfs_path_mnt_flags(const char *path);   /* MS_* of the mount covering path */
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
mnt_namespace_t *mnt_ns_rooted(vfs_dentry_t *root, unsigned long flags);
vfs_mount_t     *vfs_mount_at(mnt_namespace_t *ns, vfs_dentry_t *mp, const char *path,
                              const char *fstype, void *data, unsigned long flags);
struct vfs_file;
mnt_namespace_t *nsfile_to_mnt(struct vfs_file *f);
int              vfs_bind(mnt_namespace_t *ns, vfs_dentry_t *mp, vfs_dentry_t *dir,
                          const char *path, unsigned long flags);
vfs_dentry_t    *vfs_follow_in(mnt_namespace_t *ns, vfs_dentry_t *d);
int        vfs_umount(const char *path);
void       vfs_inode_free(vfs_inode_t *in);    /* a lookup result no dentry took; no-op if cached */
/* Something a path walk may still be reading. fn(d) runs once every syscall that
 * had walked a path when vfs_defer was called has finished, the caller's own
 * included. d is the caller's. */
typedef struct vfs_deferred {
    struct vfs_deferred *next;
    void               (*fn)(struct vfs_deferred *d);
} vfs_deferred_t;
void       vfs_defer(vfs_deferred_t *d);
void       vfs_inode_orphan(vfs_inode_t *in);  /* last name gone: iops->evict follows, in time */
struct process;
void       vfs_walk_done(struct process *p);
void       fifo_free_inode(vfs_inode_t *in);
int        vfs_may_write(vfs_inode_t *in);   /* 0, or -13: DAC for the current task */
int        vfs_may_enter(vfs_inode_t *dir);  /* 0, or -13: search permission on dir */
int        vfs_may_search(vfs_inode_t *dir); /* 1, or 0 and the syscall reports EACCES */
int        vfs_path_searchable(const char *path); /* 0 unless a directory on the way refuses */
struct process;
void       vfs_walk_anchor(struct process *p, const char *path);
int        vfs_walk_from(struct process *p, vfs_dentry_t *top);
int        vfs_file_dirpath(vfs_file_t *f, char *out, vfs_dentry_t **top);
void       vfs_dentry_pin(vfs_dentry_t *d);
void       vfs_dentry_keep(vfs_dentry_t *d);   /* d becomes a task's root: never evicted */
vfs_dentry_t *vfs_empty_root(void);           /* an empty root nothing can be made in */
void       vfs_dentry_unpin(vfs_dentry_t *d);
void       vfs_dentry_free_tree(vfs_dentry_t *root, int free_inodes);
/* Per-mount state for a filesystem that keeps none of its own: drops the tree on
 * the last unmount, after release(root_inode) if there is one. */
int        vfs_simple_sb(vfs_mount_t *mnt, void (*release)(vfs_inode_t *root));
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
int        vfs_posix_setlk(void *inode, long pid, int type, int64_t start, int64_t end);
void       vfs_posix_unlock(void *inode, long pid, int64_t start, int64_t end);
int        vfs_posix_getlk(void *inode, long pid, int *type, int64_t *start, int64_t *end, int *holder_pid);
void       vfs_posix_drop_pid(int pid);
void       vfs_posix_drop_pid_inode(void *inode, int pid);
void       vfs_posix_drop_ofd(vfs_file_t *f);
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
uint32_t vfs_path_sdev(const char *path);   /* st_dev minor of the filesystem path is on */
int        vfs_readdir(const char *path, uint32_t idx, vfs_dirent_t *out);
int        vfs_rename(const char *old_path, const char *new_path);
int        vfs_chmod(const char *path, mode_t mode);
int        vfs_chown(const char *path, uid_t uid, gid_t gid);
int        vfs_lchown(const char *path, uid_t uid, gid_t gid);
int        vfs_lutimes(const char *path, vfs_iattr_t *a, int explicit_times);
int        vfs_lchmod(const char *path, mode_t mode);
int        vfs_chown_inode(vfs_inode_t *in, uid_t uid, gid_t gid);
int        vfs_chmod_inode(vfs_inode_t *in, mode_t mode);
int        vfs_utimes_inode(vfs_inode_t *in, vfs_iattr_t *a, int explicit_times);
int        vfs_inode_setattr(vfs_inode_t *in, const vfs_iattr_t *a);
int        vfs_iattr_apply(vfs_inode_t *in, const vfs_iattr_t *a);  /* the in-core edit */
int        ext2_sync_all(void);                 /* the write-back cache reaches the disk */
void       ext2_shutdown_all(void);             /* last flush; the disks read as unmounted */
uint64_t   ext2_shrink(uint64_t want);          /* drop clean blocks; never sleeps */
int        vfs_link(const char *oldpath, const char *newpath);

/* fd_table helpers */
fd_table_t *fd_table_create(void);
fd_table_t *fd_table_copy(fd_table_t *old);
void       fd_table_keep_only(fd_table_t *t, const int *keep, int n);
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
/* The result of a path through no_cache directories is not in the dentry tree:
 * it lives in a slot of the calling task until that task's next resolve. Only its
 * inode means anything, and a dentry kept past that must not be one. */
vfs_dentry_t *vfs_dentry_resolve(const char *path);
vfs_dentry_t *vfs_dentry_child(vfs_dentry_t *dir, const char *name);
int        vfs_dentry_readonly(vfs_dentry_t *d);
int           vfs_dentry_is_volatile(const vfs_dentry_t *d);
int           vfs_dentry_count(void);
vfs_dentry_t *vfs_dentry_create(vfs_dentry_t *parent, const char *name, vfs_inode_t *inode);

/* extended attributes — generic per-inode key/value store */
#define XATTR_CREATE  1   /* fail (EEXIST) if the attribute already exists */
#define XATTR_REPLACE 2   /* fail (ENODATA) if the attribute does not exist */
int     vfs_inode_setxattr(vfs_inode_t *in, const char *name, const void *val, size_t len, int flags);
ssize_t vfs_inode_getxattr(vfs_inode_t *in, const char *name, void *val, size_t size);
ssize_t vfs_inode_listxattr(vfs_inode_t *in, char *list, size_t size);
int     vfs_inode_removexattr(vfs_inode_t *in, const char *name);
void    vfs_inode_free_xattrs(vfs_inode_t *in);

void vfs_register_fs(const char *name, int (*mount)(vfs_mount_t *, void *));
int  vfs_file_is_socket(vfs_file_t *f);          /* sys/syscall_net.c */

/* filesystems without a header of their own, registered by vfs_init() */
void fat_register(void);
void exfat_register(void);
void ntfs_register(void);
void iso9660_register(void);
void sysfs_register(void);
void hugetlbfs_register(void);

/* sysfs subtrees that drivers provide */
vfs_inode_t *drm_sysfs_root(const char *where);   /* drivers/gpu/drm/drm_sysfs.c */
vfs_inode_t *thermal_sysfs_file(void);            /* drivers/thermal/thermal_guard.c */
vfs_inode_t *casper_ec_sysfs_file(void);          /* drivers/platform/casper_ec_node.c */

#endif
