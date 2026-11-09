/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/asess.h>
#include <kernel/capabilities.h>
#include <kernel/cgroup.h>
#include <kernel/hal.h>
#include <kernel/lib/string.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/mmu.h>
#include <kernel/sync.h>
#include <kernel/vfs.h>
#include <kernel/errno.h>

/*
 * Agent sessions: a process tree confined from its first instruction. The only
 * way in is ASESS_SPAWN, a fork whose child is complete before it can run. There
 * is no call to enter from a running process and none to leave, and every path
 * that creates a task copies p->asess.
 *
 * Confinement is one syscall gate plus a private set of namespaces. The file
 * view is the caller's directory as the task's root, with a mount table of its
 * own (see asess_create): read-only and nosuid below root, private tmpfs on /tmp,
 * filtered proc and devfs, and at most one directory bound writable. The creator's
 * mounts are not in that table, so a mount on the host side is simply absent here. Teardown is the pid namespace's:
 * when the child, pid 1 there, dies, the namespace kills the rest. Closing the
 * last sfd kills the session too.
 *
 * Memory is a cgroup of its own under /cgroup/asess, entered by ASESS_SPAWN and
 * not named from inside, since cgroupfs is not in the view. It covers user pages,
 * page tables, and file data the session writes to tmpfs, ramfs, memfd or SysV
 * shm. It does not cover kernel objects: pipes, sockets, the heap.
 */

typedef struct asess {
	uint64_t         id;
	uint32_t         refcount;      /* the sfd plus every live task */
	int              spawned;
	uid_t            owner, uid;
	gid_t            gid;
	uint32_t         tasks, max_tasks;
	uint32_t         ever_ran;
	vfs_dentry_t    *root;
	mnt_namespace_t *mnt_ns;
	void            *cg;
	int             *keep;          /* fds surviving into the child */
	uint32_t         nkeep;
	uint32_t         denied;
	struct { int nr; int pid; } recent[ASESS_DENIALS_KEPT];   /* ring, by denied */
	char             label[ASESS_LABEL_MAX];
	struct asess    *next;
} asess_t;

static asess_t   *asess_list;
static uint64_t   asess_next_id = 1;
static spinlock_t asess_lock = SPINLOCK_INIT;   /* inside sched_lock on the fork path */

/* Default deny: a syscall added to Ranix is refused in sessions until it is
 * listed here. GATE_ARGS entries are decided by gate_args from the argument
 * registers; a check that needs user memory would race a sibling thread, so it
 * belongs at the point of use, as CLONE_NEW* does in do_fork. Not listed on
 * purpose, beside the design's list: capset (nothing to raise), inotify (watches
 * match path strings across views), setpriority and sched_setscheduler/setparam
 * (they take no privilege here), adjtimex. */
enum { GATE_DENY = 0, GATE_ALLOW, GATE_ARGS };

#define A(name) [SYS_ARCH_##name] = GATE_ALLOW
static const uint8_t gate[SYS_ARCH_ASESS + 1] = {
	A(READ), A(WRITE), A(READV), A(WRITEV), A(PREAD64), A(PWRITE64), A(CLOSE),
	A(CLOSE_RANGE), A(LSEEK), A(IOCTL), A(DUP), A(DUP3), A(FCNTL), A(PIPE2), A(FLOCK),
	A(SENDFILE), A(SPLICE), A(COPY_FILE_RANGE), A(FSYNC), A(FDATASYNC), A(SYNC),
	A(SYNC_FILE_RANGE), A(FALLOCATE), A(FTRUNCATE), A(FADVISE64),
	A(EPOLL_CREATE1), A(EPOLL_CTL), A(EPOLL_PWAIT), A(PSELECT6), A(PPOLL),
	A(EVENTFD2), A(TIMERFD_CREATE), A(TIMERFD_SETTIME), A(TIMERFD_GETTIME),
	A(SIGNALFD4), A(MEMFD_CREATE),

	A(OPENAT), A(FSTAT), A(NEWFSTATAT), A(STATX), A(GETDENTS64), A(CHDIR), A(FCHDIR),
	A(GETCWD), A(MKDIRAT), A(UNLINKAT), A(RENAMEAT), A(RENAMEAT2), A(FCHMOD), A(FCHMODAT),
	A(FCHOWN), A(FCHOWNAT), A(READLINKAT), A(SYMLINKAT), A(LINKAT), A(FACCESSAT),
	A(FACCESSAT2), A(UTIMENSAT), A(UMASK), A(STATFS), A(FSTATFS), A(GETXATTR),
	A(LGETXATTR), A(FGETXATTR), A(LISTXATTR), A(LLISTXATTR), A(SETXATTR), A(LSETXATTR),
	A(REMOVEXATTR),

	A(BRK), A(MMAP), A(MUNMAP), A(MPROTECT), A(MREMAP), A(MADVISE), A(MSYNC), A(MLOCK),
	A(MLOCK2), A(MUNLOCK), A(MLOCKALL), A(MUNLOCKALL), A(MEMBARRIER),

	A(CLONE), A(CLONE3), A(VFORK), A(EXECVE), A(EXECVEAT), A(EXIT), A(EXIT_GROUP),
	A(WAIT4), A(WAITID), A(GETPID), A(GETPPID), A(GETTID), A(GETPGID), A(SETPGID),
	A(GETSID), A(SETSID), A(SET_TID_ADDR), A(SET_ROBUST_LIST), A(GET_ROBUST_LIST), A(RSEQ),
	A(FUTEX), A(GETUID), A(GETEUID), A(GETGID), A(GETEGID), A(GETGROUPS), A(SETGROUPS),
	A(SETUID), A(SETGID), A(SETREUID), A(SETREGID), A(SETRESUID), A(SETRESGID),
	A(GETRESUID), A(GETRESGID), A(SETFSUID), A(SETFSGID), A(CAPGET), A(GETRLIMIT),
	A(SETRLIMIT), A(PRLIMIT64), A(GETRUSAGE), A(GETPRIORITY), A(PRCTL), A(PERSONALITY),
	A(SECCOMP), A(SCHED_YIELD), A(SCHED_GETAFFINITY), A(SCHED_SETAFFINITY),
	A(SCHED_GETSCHEDULER), A(SCHED_GETPARAM), A(SCHED_GET_PRIORITY_MAX),
	A(SCHED_GET_PRIORITY_MIN), A(GETCPU), A(GETRANDOM), A(UNAME), A(SYSINFO), A(TIMES),
	A(PTRACE), A(PROCESS_VM_READV), A(PROCESS_VM_WRITEV),

	A(RT_SIGACTION), A(RT_SIGPROCMASK), A(RT_SIGRETURN), A(RT_SIGPENDING), A(RT_SIGSUSPEND),
	A(SIGALTSTACK), A(KILL), A(TKILL), A(TGKILL), A(RT_SIGQUEUEINFO), A(RT_TGSIGQUEUEINFO),
	A(PIDFD_OPEN), A(PIDFD_SEND_SIGNAL),

	A(CLOCK_GETTIME), A(CLOCK_GETRES), A(CLOCK_NANOSLEEP), A(GETTIMEOFDAY), A(NANOSLEEP),
	A(GETITIMER), A(SETITIMER),

	A(BIND), A(CONNECT), A(LISTEN), A(ACCEPT), A(ACCEPT4), A(SENDTO), A(RECVFROM),
	A(SENDMSG), A(RECVMSG), A(GETSOCKNAME), A(GETPEERNAME), A(SETSOCKOPT), A(GETSOCKOPT),
	A(SHUTDOWN), A(SOCKETPAIR),

	A(SHMGET), A(SHMAT), A(SHMDT), A(SHMCTL), A(SEMGET), A(SEMOP), A(SEMCTL), A(MSGGET),
	A(MSGSND), A(MSGRCV), A(MSGCTL), A(MQ_OPEN), A(MQ_UNLINK), A(MQ_TIMEDSEND),
	A(MQ_TIMEDRECEIVE), A(MQ_GETSETATTR),

	A(ASESS),

	[SYS_ARCH_SOCKET]  = GATE_ARGS,
	[SYS_ARCH_MKNODAT] = GATE_ARGS,
#if defined(__x86_64__)
	A(OPEN), A(CREAT), A(STAT), A(LSTAT), A(ACCESS), A(READLINK), A(MKDIR), A(RMDIR),
	A(UNLINK), A(RENAME), A(LINK), A(SYMLINK), A(CHMOD), A(CHOWN), A(LCHOWN), A(TRUNCATE),
	A(PIPE), A(DUP2), A(POLL), A(SELECT), A(EPOLL_CREATE), A(EPOLL_WAIT), A(EVENTFD),
	A(PAUSE), A(ALARM), A(FORK), A(GETPGRP), A(ARCH_PRCTL), A(TIME_SYSCALL),
	[SYS_ARCH_MKNOD] = GATE_ARGS,
#endif
};
#undef A

#define AF_NETLINK_ 16
#define SOCK_RAW_   3
#define S_IFMT_     0170000

static int mknod_mode_ok(mode_t mode)
{
	return (mode & S_IFMT_) == 0 || S_ISREG(mode) || S_ISFIFO(mode);
}

static int gate_args(long nr, long a0, long a1, long a2)
{
	switch (nr) {
	case SYS_ARCH_SOCKET:
		/* netlink is always SOCK_RAW; its writes need CAP_NET_ADMIN */
		return (a1 & 0xf) != SOCK_RAW_ || a0 == AF_NETLINK_;
	case SYS_ARCH_MKNODAT:
		return mknod_mode_ok((mode_t)a2);
#if defined(__x86_64__)
	case SYS_ARCH_MKNOD:
		return mknod_mode_ok((mode_t)a1);
#endif
	}
	return 0;
}

int asess_syscall_allowed(process_t *p, long nr, long a0, long a1, long a2)
{
	if (nr >= 0 && nr < (long)sizeof(gate)) {
		if (gate[nr] == GATE_ALLOW) return 1;
		if (gate[nr] == GATE_ARGS && gate_args(nr, a0, a1, a2)) return 1;
	}
	asess_t *s = p->asess;
	uint64_t f = spin_lock_irq(&asess_lock);
	uint32_t slot = s->denied++ % ASESS_DENIALS_KEPT;
	s->recent[slot].nr  = (int)nr;
	s->recent[slot].pid = task_vpid(p);
	spin_unlock_irq(&asess_lock, f);
	return 0;
}

/* Pipes, sockets and terminals carry bytes only. Anything with a path or a
 * capability behind it (a file or directory, an sfd, a pidfd, a namespace fd, a
 * device) would reach past the view, so it stays out. */
int asess_file_allowed(vfs_file_t *f)
{
	vfs_inode_t *in = f->inode;
	if (!in) return 0;
	if (S_ISFIFO(in->mode) || vfs_file_is_socket(f)) return 1;
	if (!S_ISCHR(in->mode)) return 0;
	unsigned major = in->rdev >> 8, minor = in->rdev & 0xff;
	return major == 4 || major == 136 || (major == 5 && minor < 2) ||
	       (major == 1 && minor == 3);   /* /dev/null */
}

static void asess_put(asess_t *s)
{
	uint64_t f = spin_lock_irq(&asess_lock);
	int last = --s->refcount == 0;
	if (last)
		for (asess_t **pp = &asess_list; *pp; pp = &(*pp)->next)
			if (*pp == s) { *pp = s->next; break; }
	spin_unlock_irq(&asess_lock, f);
	if (!last) return;
	mnt_ns_put(s->mnt_ns);
	cgroup_put(s->cg);
	kfree(s->keep);
	kfree(s);
}

void *asess_cgroup(asess_t *s)
{
	return s->cg;
}

/* ── fork path ──────────────────────────────────────────────── */

/* Runs on the caller, on a child nothing can reach yet. p->asess comes last, in
 * asess_fork, so a failure here leaves no half-entered task. */
int asess_prepare_child(process_t *c, asess_t *s)
{
	/* c->fds is private now, so no sibling of the caller can swap a file in
	 * behind this check */
	for (uint32_t i = 0; i < s->nkeep; i++) {
		vfs_file_t *f = fd_get(c->fds, s->keep[i]);
		if (!f) return -EBADF;
		if (!asess_file_allowed(f)) return -EINVAL;
	}

	c->uid = c->euid = c->suid = s->uid;
	c->gid = c->egid = c->sgid = s->gid;
	c->fs_uid_set = c->fs_gid_set = 0;
	c->ngroups = 0;
	c->cap_permitted = c->cap_effective = c->cap_inheritable = 0;
	c->no_new_privs = 1;

	fd_table_keep_only(c->fds, s->keep, (int)s->nkeep);

	mnt_ns_put(c->mnt_ns);
	c->mnt_ns = mnt_ns_get(s->mnt_ns);
	c->fs->root = s->root;   /* the fork gave it a private copy (no CLONE_FS) */
	fs_set_cwd(c->fs, "/");
	c->ctty = NULL; c->ctty_fops = NULL;
	return 0;
}

int asess_fork(process_t *child, process_t *parent, asess_t *enter)
{
	asess_t *s = enter ? enter : parent->asess;
	if (!s) return 0;
	/* a task the pid namespace's teardown walk has already passed would outlive it */
	if (!enter && parent->pid_ns && parent->pid_ns->dead) return -1;
	uint64_t f = spin_lock_irq(&asess_lock);
	int ok = s->tasks < s->max_tasks;
	if (ok) { s->tasks++; s->refcount++; s->ever_ran = 1; child->asess = s; }
	spin_unlock_irq(&asess_lock, f);
	return ok ? 0 : -1;
}

/* undoes asess_fork; the parent or the sfd still holds s, so this is never the last */
void asess_fork_cancel(process_t *child)
{
	asess_t *s = child->asess;
	if (!s) return;
	child->asess = NULL;
	uint64_t f = spin_lock_irq(&asess_lock);
	s->tasks--; s->refcount--;
	spin_unlock_irq(&asess_lock, f);
}

void asess_task_exit(asess_t *s)
{
	if (!s) return;
	uint64_t f = spin_lock_irq(&asess_lock);
	s->tasks--;
	spin_unlock_irq(&asess_lock, f);
	asess_put(s);
}

/* ── introspection ──────────────────────────────────────────── */

/* Under asess_lock: the task may exit meanwhile, and its session can only be freed
 * after the exit path takes this lock, by which time p->asess is NULL. */
uint64_t asess_id_of(process_t *p)
{
	uint64_t f = spin_lock_irq(&asess_lock);
	uint64_t id = p->asess ? p->asess->id : 0;
	spin_unlock_irq(&asess_lock, f);
	return id;
}

/* inside: the caller's own session; outside: its owner or root */
static int asess_visible(const asess_t *s)
{
	process_t *c = task_current();
	if (c->asess) return c->asess == s;
	return c->euid == 0 || c->euid == s->owner;
}

static int asess_state(const asess_t *s)
{
	return !s->ever_ran ? ASESS_STATE_CREATED : s->tasks ? ASESS_STATE_RUNNING : ASESS_STATE_DEAD;
}

int asess_view(uint64_t id, asess_view_t *out)
{
	int r = -1;
	uint64_t f = spin_lock_irq(&asess_lock);
	for (asess_t *s = asess_list; s; s = s->next) {
		if (s->id != id) continue;
		if (asess_visible(s)) {
			out->id = s->id;
			out->state = asess_state(s);
			out->uid = s->uid;
			out->tasks = s->tasks;
			out->max_tasks = s->max_tasks;
			out->denied = s->denied;
			cgroup_mem_usage(s->cg, &out->mem_cur, &out->mem_max);
			memcpy(out->label, s->label, sizeof(out->label));
			uint32_t n = s->denied < ASESS_DENIALS_KEPT ? s->denied : ASESS_DENIALS_KEPT;
			out->nrecent = n;
			for (uint32_t i = 0; i < n; i++) {
				uint32_t slot = (s->denied - n + i) % ASESS_DENIALS_KEPT;
				out->recent[i].nr  = s->recent[slot].nr;
				out->recent[i].pid = s->recent[slot].pid;
			}
			r = 0;
		}
		break;
	}
	spin_unlock_irq(&asess_lock, f);
	return r;
}

uint64_t asess_nth_visible(uint32_t idx)
{
	uint64_t id = 0;
	uint64_t f = spin_lock_irq(&asess_lock);
	for (asess_t *s = asess_list; s && !id; s = s->next)
		if (asess_visible(s) && idx-- == 0) id = s->id;
	spin_unlock_irq(&asess_lock, f);
	return id;
}

/* ── syscall ────────────────────────────────────────────────── */

/* Nothing outside can reach the session once its last sfd is gone, so it ends
 * with its supervisor. The design freezes it instead; that waits for the freeze
 * work. */
static int sfd_close(vfs_file_t *f)
{
	task_kill_session(f->inode->priv);
	asess_put(f->inode->priv);
	f->inode->priv = NULL;
	return 0;
}

static vfs_fops_t sfd_fops = { .close = sfd_close };

static int sfd_alloc(asess_t *s)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	vfs_file_t  *f  = kmalloc(sizeof(*f));
	if (!in || !f) { kfree(in); kfree(f); return -ENOMEM; }
	memset(in, 0, sizeof(*in));
	memset(f, 0, sizeof(*f));
	in->fops = &sfd_fops;
	in->priv = s;
	in->priv_static = 1;
	in->refcount = 1;
	f->inode = in;
	int fd = fd_alloc(task_current()->fds, f);
	if (fd < 0) { kfree(in); kfree(f); return fd; }
	fd_set_cloexec(task_current()->fds, fd, 1);
	return fd;
}

/* mount fstype on the directory path names directly under root. A name that is
 * missing, or not a real directory, stays absent: the view only ever loses. */
static vfs_mount_t *view_mount(asess_t *s, const char *path, const char *fstype,
                               void *data, unsigned long flags, int *err)
{
	vfs_dentry_t *mp = vfs_dentry_child(s->root, path + 1);
	if (!mp || !mp->inode || !S_ISDIR(mp->inode->mode)) return NULL;
	vfs_mount_t *m = vfs_mount_at(s->mnt_ns, mp, path, fstype, data, flags);
	if (!m) *err = -12;
	return m;
}

static void put_decimal(char *out, unsigned v)
{
	char rev[12]; int n = 0;
	do { rev[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (n) *out++ = rev[--n];
	*out = '\0';
}

/* The dentry rel names in the view, not following symlinks: the caller named a
 * real location, and a link in the path could aim it elsewhere. A missing name is
 * made where the view is writable, which means /tmp. */
static vfs_dentry_t *view_walk(asess_t *s, const char *rel, int *err)
{
	vfs_dentry_t *cur = s->root;
	char name[256];
	int in_view_fs = 0;
	while (*rel) {
		const char *end = rel;
		while (*end && *end != '/') end++;
		size_t n = (size_t)(end - rel);
		if (n == 0 || n >= sizeof(name) || (rel[0] == '.' && (n == 1 || (n == 2 && rel[1] == '.')))) {
			*err = -22;
			return NULL;
		}
		memcpy(name, rel, n);
		name[n] = '\0';
		rel = *end ? end + 1 : end;

		vfs_dentry_t *top = vfs_follow_in(s->mnt_ns, cur);
		in_view_fs |= top != cur;
		vfs_dentry_t *c = vfs_dentry_child(top, name);
		if (!c && in_view_fs && top->inode->iops->mkdir &&
		    top->inode->iops->mkdir(top->inode, name, 0755) == 0) {
			c = vfs_dentry_child(top, name);
			if (c && c->inode) { c->inode->uid = s->uid; c->inode->gid = s->gid; }
		}
		if (!c || !c->inode) { *err = -2; return NULL; }
		if (S_ISLNK(c->inode->mode)) { *err = -40; return NULL; }
		if (!S_ISDIR(c->inode->mode)) { *err = -20; return NULL; }
		cur = c;
	}
	return cur;
}

/* vfs_dentry_resolve hands back the task's reused slot once the dentry cache is
 * full (or below a no_cache directory), and a mount or p->root must not hold
 * that. Walk the canonical path again through vfs_dentry_child, which always
 * makes real dentries. Where that cannot be done (no_cache, a symlink, "." or
 * ".." in the path) the slot is returned and the caller refuses it as before. */
static vfs_dentry_t *resolve_stable(const char *path)
{
	vfs_dentry_t *d = vfs_dentry_resolve(path);
	if (!d || !vfs_dentry_is_volatile(d)) return d;
	vfs_dentry_t *v = d;   /* a no_cache tree has no real dentries: stays volatile */
	mnt_namespace_t *ns = current_mnt_ns();
	vfs_dentry_t *cur = vfs_dentry_resolve("/");
	if (!cur || vfs_dentry_is_volatile(cur)) return v;
	char name[256];
	const char *p = path;
	while (*p) {
		while (*p == '/') p++;
		if (!*p) break;
		const char *end = p;
		while (*end && *end != '/') end++;
		size_t n = (size_t)(end - p);
		if (n >= sizeof(name) || (p[0] == '.' && (n == 1 || (n == 2 && p[1] == '.')))) return v;
		memcpy(name, p, n);
		name[n] = '\0';
		p = end;
		cur = vfs_follow_in(ns, cur);
		if (!cur->inode || !S_ISDIR(cur->inode->mode) || !vfs_may_search(cur->inode)) return v;
		vfs_dentry_t *c = vfs_dentry_child(cur, name);
		if (!c || !c->inode || S_ISLNK(c->inode->mode)) return v;
		cur = c;
	}
	return vfs_follow_in(ns, cur);
}

/* dir is the place in the view; src, if given, the host directory bound there.
 * Without src the place binds to itself. */
static int view_bind(asess_t *s, const char *root, const char *dir, const char *src)
{
	vfs_dentry_t *d = resolve_stable(src ? src : dir);
	if (!d || !d->inode) return -ENOENT;
	if (!S_ISDIR(d->inode->mode)) return -ENOTDIR;
	if (vfs_dentry_is_volatile(d)) return -EINVAL;   /* the slot is reused, the mount outlives it */

	size_t l = strlen(root);
	while (l && root[l - 1] == '/') l--;
	const char *rel = dir;
	if (strncmp(dir, root, l) == 0 && (dir[l] == '/' || dir[l] == '\0')) rel = dir + l;
	if (rel[0] != '/' && rel[0] != '\0') return -EINVAL;
	if (rel[0] == '/') rel++;

	int err = 0;
	vfs_dentry_t *mp = view_walk(s, rel, &err);
	if (!mp) return err;
	if (vfs_follow_in(s->mnt_ns, mp) != mp) return -EBUSY;   /* the view has a mount there */
	if (src && mp == s->root) return -EBUSY;   /* staging the whole root is not supported */
	return vfs_bind(s->mnt_ns, mp == d && !src ? NULL : mp, d, rel[0] ? rel - 1 : "/",
	                MS_NOSUID | MS_NODEV);
}

static int view_build(asess_t *s, unsigned tmp_mib)
{
	const unsigned long hard = MS_NOSUID | MS_NODEV;
	int err = 0;
	s->mnt_ns = mnt_ns_rooted(s->root, MS_RDONLY | hard);
	if (!s->mnt_ns) return -ENOMEM;

	char opt[24];
	memcpy(opt, "size=", 5);
	put_decimal(opt + 5, tmp_mib);
	int l = (int)strlen(opt);
	opt[l] = 'M'; opt[l + 1] = '\0';
	vfs_mount_t *tmp = view_mount(s, "/tmp", "tmpfs", opt, hard, &err);
	if (tmp && tmp->root && tmp->root->inode) {
		tmp->root->inode->uid  = s->uid;
		tmp->root->inode->gid  = s->gid;
		tmp->root->inode->mode = S_IFDIR | 01777;
	}
	/* no noexec: a link in proc counts for both mounts, and /proc/self/exe has to run */
	if (!err) view_mount(s, "/proc", "proc", ASESS_MOUNT_OPT, MS_RDONLY | hard, &err);
	if (!err) view_mount(s, "/dev", "devfs", ASESS_MOUNT_OPT, MS_NOSUID | MS_NOEXEC, &err);
	return err;
}

/* The defaults of TASARIM 10.2: min(R/2, R-1 GiB) a session, with a floor, and
 * min(1 GiB, R/4) of R held back from all sessions together. */
static int budget_build(asess_t *s, const struct asess_attr *a)
{
	const long gib = 1L << 30, floor = (long)ASESS_MEM_FLOOR_MIB << 20;
	long ram = (long)(buddy_total_pages() * PAGE_SIZE);
	long mem = ram / 2 < ram - gib ? ram / 2 : ram - gib;
	if (mem < floor) mem = floor;
	if (mem > ram) return -ENOMEM;
	if (a->mem_max) mem = (long)a->mem_max;
	long reserve = gib < ram / 4 ? gib : ram / 4;

	char name[24];
	put_decimal(name, (unsigned)s->id);
	s->cg = cgroup_session_create(name, mem, s->max_tasks,
	                              (long)(buddy_free_pages() * PAGE_SIZE), reserve);
	return s->cg ? 0 : -12;
}

static int cred_for(asess_t *s, const struct asess_attr *a)
{
	process_t *c = task_current();
	if (a->uid && a->uid != c->euid && !capable_task(c, CAP_SETUID)) return -1;
	if (a->gid && a->gid != c->egid && !capable_task(c, CAP_SETGID)) return -1;
	s->owner = c->euid;
	s->uid = a->uid ? a->uid : c->euid;
	s->gid = a->gid ? a->gid : c->egid;
	/* exec gives uid 0 every capability back and vfs_check_perm lets it past
	 * every mode, so no session runs as root */
	return s->uid && s->gid ? 0 : -1;
}

static char *path_in(uint64_t upath, long *err)
{
	char *path = kmalloc(PATH_MAX);
	*err = -12;
	if (!path) return NULL;
	*err = -14;
	if (strncpy_from_user(path, (const char *)(uintptr_t)upath, PATH_MAX) < 0) { kfree(path); return NULL; }
	path[PATH_MAX - 1] = '\0';
	*err = -22;
	if (path[0] != '/') { kfree(path); return NULL; }
	return path;
}

static long asess_create(const void *uattr, size_t size)
{
	struct asess_attr a;
	if (size != sizeof(a)) return -EINVAL;
	if (copy_from_user(&a, uattr, sizeof(a)) != 0) return -EFAULT;
	if (a.size != sizeof(a) || a.flags || a.net_mode != ASESS_NET_NONE) return -EINVAL;
	if (a.nkeep > 64) return -EINVAL;

	asess_t *s = kmalloc(sizeof(*s));
	if (!s) return -ENOMEM;
	memset(s, 0, sizeof(*s));
	s->refcount = 1;
	s->max_tasks = a.max_tasks ? a.max_tasks : ASESS_DEFAULT_TASKS;
	memcpy(s->label, a.label, sizeof(s->label));
	s->label[sizeof(s->label) - 1] = '\0';

	long r = cred_for(s, &a);
	if (r) goto fail;

	r = -12;
	if (a.nkeep) {
		s->keep = kmalloc(a.nkeep * sizeof(int));
		if (!s->keep) goto fail;
		r = -14;
		if (copy_from_user(s->keep, (const void *)(uintptr_t)a.keep_fds, a.nkeep * sizeof(int)) != 0)
			goto fail;
		s->nkeep = a.nkeep;
	}

	char *path = path_in(a.root, &r);
	if (!path) goto fail;
	vfs_dentry_t *d = resolve_stable(path);
	r = -2;
	if (!d || !d->inode) goto fail_path;
	r = -20;
	if (!S_ISDIR(d->inode->mode)) goto fail_path;
	r = -22;
	if (vfs_dentry_is_volatile(d)) goto fail_path;   /* the slot is reused, p->root outlives it */
	vfs_dentry_keep(d);
	s->root = d;

	r = view_build(s, a.tmp_mib ? a.tmp_mib : ASESS_DEFAULT_TMP_MIB);
	if (r) goto fail_path;
	r = -22;
	if (a.rw_src && !a.rw_dir) goto fail_path;
	if (a.rw_dir) {
		char *dir = path_in(a.rw_dir, &r);
		if (!dir) goto fail_path;
		char *src = NULL;
		if (a.rw_src && !(src = path_in(a.rw_src, &r))) { kfree(dir); goto fail_path; }
		r = view_bind(s, path, dir, src);
		kfree(src);
		kfree(dir);
		if (r) goto fail_path;
	}
	kfree(path);

	uint64_t f = spin_lock_irq(&asess_lock);
	s->id = asess_next_id++;
	spin_unlock_irq(&asess_lock, f);
	r = budget_build(s, &a);
	if (r) goto fail;

	f = spin_lock_irq(&asess_lock);
	s->next = asess_list;
	asess_list = s;
	spin_unlock_irq(&asess_lock, f);

	r = sfd_alloc(s);
	if (r >= 0) return r;
	asess_put(s);
	return r;
fail_path:
	kfree(path);
fail:
	if (s->mnt_ns) mnt_ns_put(s->mnt_ns);
	kfree(s->keep);
	kfree(s);
	return r;
}

static long asess_spawn(asess_t *s)
{
	process_t *c = task_current();
	if (c->tracer) return -1;   /* the tracer would hold the child from outside */

	uint64_t f = spin_lock_irq(&asess_lock);
	int busy = s->spawned;
	s->spawned = 1;
	spin_unlock_irq(&asess_lock, f);
	if (busy) return -EBUSY;   /* pid 1 of a session exists once */

	long pid = proc_fork_asess(s);
	if (pid < 0) s->spawned = 0;
	return pid;
}

static long asess_info(asess_t *s, void *uinfo, size_t size)
{
	struct asess_info in;
	if (size != sizeof(in)) return -EINVAL;
	uint64_t f = spin_lock_irq(&asess_lock);
	in.id = s->id;
	in.state = (unsigned)asess_state(s);
	in.tasks = s->tasks;
	in.max_tasks = s->max_tasks;
	in.denied = s->denied;
	spin_unlock_irq(&asess_lock, f);
	long cur, max;
	cgroup_mem_usage(s->cg, &cur, &max);
	in.mem_cur = (unsigned long long)cur;
	in.mem_max = (unsigned long long)max;
	return copy_to_user(uinfo, &in, sizeof(in)) ? -14 : 0;
}

long sys_asess(unsigned int op, int sfd, void *arg, size_t size)
{
	process_t *c = task_current();
	if (c->asess) {
		if (op == ASESS_INFO && sfd == -1) return asess_info(c->asess, arg, size);
		return -EPERM;   /* no nesting, and nothing here to steer from inside */
	}
	if (op == ASESS_CREATE) return asess_create(arg, size);

	/* the file reference keeps s alive if a sibling closes sfd meanwhile */
	vfs_file_t *f = fget(c->fds, sfd);
	if (!f) return -EBADF;
	long r = -9;
	if (f->inode && f->inode->fops == &sfd_fops) {
		asess_t *s = f->inode->priv;
		r = op == ASESS_SPAWN ? asess_spawn(s)
		  : op == ASESS_INFO  ? asess_info(s, arg, size) : -22;
	}
	fput(f);
	return r;
}
