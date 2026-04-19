#include <kernel/syscall.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <kernel/kprintf.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/vm.h>
#include <kernel/lib/string.h>

#define PAGE_SIZE    4096UL
#define AT_FDCWD     (-100)

/* mmap flags */
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

/* mmap prot */
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

/* Linux ABI stat struct — must match kernel uapi exactly */
typedef struct {
	uint64_t st_dev;
	uint64_t st_ino;
	uint32_t st_mode;
	uint32_t st_nlink;
	uint32_t st_uid;
	uint32_t st_gid;
	uint64_t st_rdev;
	uint64_t __pad1;
	int64_t  st_size;
	int32_t  st_blksize;
	int32_t  __pad2;
	int64_t  st_blocks;
	int64_t  st_atime_sec;
	uint64_t st_atime_nsec;
	int64_t  st_mtime_sec;
	uint64_t st_mtime_nsec;
	int64_t  st_ctime_sec;
	uint64_t st_ctime_nsec;
	uint32_t __unused[2];
} linux_stat_t;

typedef struct {
	void  *iov_base;
	size_t iov_len;
} iovec_t;

/* ── helpers ─────────────────────────────────────────────── */

static long sys_write(int fd, const void *buf, size_t len)
{
	if (fd == 0 || fd == 1 || fd == 2) {
		const char *p = buf;
		for (size_t i = 0; i < len; i++)
			kprintf("%c", p[i]);
		return (long)len;
	}
	return vfs_write(fd, buf, len);
}

static long sys_read(int fd, void *buf, size_t len)
{
	return vfs_read(fd, buf, len);
}

static long sys_writev(int fd, const iovec_t *iov, int cnt)
{
	long total = 0;
	for (int i = 0; i < cnt; i++) {
		if (!iov[i].iov_base || iov[i].iov_len == 0) continue;
		long r = sys_write(fd, iov[i].iov_base, iov[i].iov_len);
		if (r < 0) return r;
		total += r;
	}
	return total;
}

static long sys_open(const char *path, int flags, mode_t mode)
{
	long fd = vfs_open(path, flags, mode);
	return fd < 0 ? -2 : fd;   /* -ENOENT */
}

static long sys_openat(int dirfd, const char *path, int flags, mode_t mode)
{
	(void)dirfd;   /* only absolute paths / AT_FDCWD supported */
	return sys_open(path, flags, mode);
}

static long sys_close(int fd)
{
	if (fd < 3) return 0;   /* don't close stdin/stdout/stderr */
	return vfs_close(fd) == 0 ? 0 : -9;   /* -EBADF */
}

static long sys_lseek(int fd, off_t off, int whence)
{
	off_t r = vfs_seek(fd, off, whence);
	return r < 0 ? -29 : (long)r;   /* -ESPIPE on error */
}

static long sys_fstat(int fd, linux_stat_t *st)
{
	if (!st) return -14;   /* -EFAULT */
	memset(st, 0, sizeof(*st));

	if (fd >= 0 && fd <= 2) {
		st->st_mode     = 0x2190;   /* S_IFCHR | 0110 — char dev */
		st->st_nlink    = 1;
		st->st_rdev     = 0x0501;   /* major 5, minor 1 — /dev/console */
		st->st_blksize  = 1024;
		return 0;
	}

	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return -9;   /* -EBADF */

	vfs_stat_t vs = {0};
	if (f->inode->iops && f->inode->iops->stat)
		f->inode->iops->stat(f->inode, &vs);

	st->st_mode    = vs.mode;
	st->st_ino     = vs.ino;
	st->st_size    = (int64_t)vs.size;
	st->st_nlink   = vs.nlink ? vs.nlink : 1;
	st->st_uid     = vs.uid;
	st->st_gid     = vs.gid;
	st->st_blksize = 4096;
	st->st_blocks  = (vs.size + 511) / 512;
	return 0;
}

static long sys_exit(int code)
{
	process_t *p = task_current();
	p->exit_code  = code;
	task_exit();
	return 0;
}

static long sys_getpid(void)
{
	return (long)task_current()->pid;
}

static long sys_brk(uint64_t addr)
{
	process_t *p = task_current();

	if (addr == 0 || addr <= p->brk)
		return (long)p->brk;

	uint64_t new_brk = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

	for (uint64_t va = p->brk; va < new_brk; va += PAGE_SIZE) {
		void *page = buddy_alloc(0);
		if (!page) return (long)p->brk;
		memset(page, 0, PAGE_SIZE);
		if (vm_map(p->vm, va, (uint64_t)page, VM_USER | VM_READ | VM_WRITE) != 0) {
			buddy_free((uint64_t)page, 0);
			return (long)p->brk;
		}
	}

	p->brk = new_brk;
	return (long)new_brk;
}

static long sys_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, uint64_t off)
{
	(void)fd; (void)off;

	if (!(flags & MAP_ANONYMOUS)) return -38;   /* -ENOSYS: file-backed not yet */
	if (len == 0)                 return -22;   /* -EINVAL */

	process_t *p = task_current();
	len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

	uint64_t va;
	if ((flags & MAP_FIXED) && addr)
		va = addr & ~(PAGE_SIZE - 1);
	else {
		va = p->mmap_next;
		p->mmap_next += len;
	}

	uint32_t vmflags = VM_USER;
	if (prot & PROT_READ)  vmflags |= VM_READ;
	if (prot & PROT_WRITE) vmflags |= VM_WRITE;
	if (prot & PROT_EXEC)  vmflags |= VM_EXEC;

	for (uint64_t v = va; v < va + len; v += PAGE_SIZE) {
		void *pg = buddy_alloc(0);
		if (!pg) return -12;   /* -ENOMEM */
		memset(pg, 0, PAGE_SIZE);
		if (vm_map(p->vm, v, (uint64_t)pg, vmflags) != 0) {
			buddy_free((uint64_t)pg, 0);
			return -12;
		}
	}

	return (long)va;
}

#if defined(__x86_64__)
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004
static long sys_arch_prctl(int code, uint64_t addr)
{
	if (code == ARCH_SET_FS) {
		__asm__ volatile("wrmsr"
			:: "c"(0xC0000100U),
			   "a"((uint32_t)addr),
			   "d"((uint32_t)(addr >> 32)));
		return 0;
	}
	if (code == ARCH_GET_FS) {
		uint32_t lo, hi;
		__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100U));
		*(uint64_t *)addr = ((uint64_t)hi << 32) | lo;
		return 0;
	}
	return -22;
}
#endif

static long sys_munmap(uint64_t addr, size_t len)
{
	process_t *p  = task_current();
	addr          = addr & ~(PAGE_SIZE - 1);
	len           = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE)
		vm_unmap(p->vm, va);
	return 0;
}

/* ── dispatcher ──────────────────────────────────────────── */

void syscall_handle(arch_context_t *ctx)
{
	long n    = (long)CTX_SYSCALL_N(ctx);
	long arg0 = (long)CTX_SYSCALL_ARG0(ctx);
	long arg1 = (long)CTX_SYSCALL_ARG1(ctx);
	long arg2 = (long)CTX_SYSCALL_ARG2(ctx);
	long arg3 = (long)CTX_SYSCALL_ARG3(ctx);
	long ret  = -38;   /* -ENOSYS default */

	switch (n) {
	case SYS_ARCH_READ:          ret = sys_read((int)arg0, (void*)arg1, (size_t)arg2); break;
	case SYS_ARCH_WRITE:         ret = sys_write((int)arg0, (void*)arg1, (size_t)arg2); break;
	case SYS_ARCH_WRITEV:        ret = sys_writev((int)arg0, (iovec_t*)arg1, (int)arg2); break;
#if defined(SYS_ARCH_OPEN)
	case SYS_ARCH_OPEN:          ret = sys_open((const char*)arg0, (int)arg1, (mode_t)arg2); break;
#endif
	case SYS_ARCH_OPENAT:        ret = sys_openat((int)arg0, (const char*)arg1, (int)arg2, (mode_t)arg3); break;
	case SYS_ARCH_CLOSE:         ret = sys_close((int)arg0); break;
	case SYS_ARCH_LSEEK:         ret = sys_lseek((int)arg0, (off_t)arg1, (int)arg2); break;
	case SYS_ARCH_FSTAT:         ret = sys_fstat((int)arg0, (linux_stat_t*)arg1); break;
	case SYS_ARCH_EXIT:          ret = sys_exit((int)arg0); break;
	case SYS_ARCH_EXIT_GROUP:    ret = sys_exit((int)arg0); break;
	case SYS_ARCH_GETPID:        ret = sys_getpid(); break;
	case SYS_ARCH_BRK:           ret = sys_brk((uint64_t)arg0); break;
	case SYS_ARCH_MMAP:          ret = sys_mmap((uint64_t)arg0, (size_t)arg1, (int)arg2,
	                                             (int)arg3, 0, 0); break;
	case SYS_ARCH_MUNMAP:        ret = sys_munmap((uint64_t)arg0, (size_t)arg1); break;
	case SYS_ARCH_SET_TID_ADDR:  ret = (long)task_current()->pid; break;
	case SYS_ARCH_RT_SIGACTION:     ret = 0; break;
	case SYS_ARCH_RT_SIGPROCMASK:   ret = 0; break;
	case SYS_ARCH_IOCTL:            ret = -25; break;   /* -ENOTTY */
#if defined(SYS_ARCH_ARCH_PRCTL)
	case SYS_ARCH_ARCH_PRCTL:      ret = sys_arch_prctl((int)arg0, (uint64_t)arg1); break;
#endif
	default:
		kprintf("[syscall] unknown #%ld\n", n);
		break;
	}

	CTX_SYSCALL_RET(ctx, ret);
}
