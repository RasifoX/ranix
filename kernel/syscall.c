#include <kernel/syscall.h>
#include <kernel/proc.h>
#include <kernel/elf.h>
#include <kernel/vfs.h>
#include <kernel/hal.h>
#include <kernel/kprintf.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/vm.h>
#include <kernel/lib/string.h>
#include <kernel/sync.h>
#include <kernel/tty/fb_tty.h>
#include <drivers/serial/uart.h>
#include <kernel/board.h>
#include <kernel/net/socket.h>
#include <kernel/net/udp.h>
#include <kernel/net/icmp.h>
#include <kernel/net/ip.h>
#include <kernel/net/tcp.h>
#include <kernel/net/net.h>
#include <kernel/config.h>

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

/* Linux ABI stat struct — layout differs between x86_64 and aarch64 */
#if defined(__x86_64__)
typedef struct {
	uint64_t st_dev;
	uint64_t st_ino;
	uint64_t st_nlink;   /* x86_64: nlink (8B) before mode */
	uint32_t st_mode;
	uint32_t st_uid;
	uint32_t st_gid;
	uint32_t __pad0;
	uint64_t st_rdev;
	int64_t  st_size;
	int64_t  st_blksize;
	int64_t  st_blocks;
	int64_t  st_atime_sec;
	uint64_t st_atime_nsec;
	int64_t  st_mtime_sec;
	uint64_t st_mtime_nsec;
	int64_t  st_ctime_sec;
	uint64_t st_ctime_nsec;
	uint64_t __unused[3];
} linux_stat_t;
#else /* aarch64: mode (4B) before nlink (4B), extra pad after rdev */
typedef struct {
	uint64_t st_dev;
	uint64_t st_ino;
	uint32_t st_mode;    /* aarch64: mode before nlink */
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
	uint32_t __unused4;
	uint32_t __unused5;
} linux_stat_t;
#endif

typedef struct {
	void  *iov_base;
	size_t iov_len;
} iovec_t;

/* ── helpers ─────────────────────────────────────────────── */

static int resolve_path(char *out, int dirfd, const char *upath);

static long sys_write(int fd, const void *buf, size_t len)
{
	if (!access_ok(buf, len)) return -14;
	return vfs_write(fd, buf, len);
}

static long sys_read(int fd, void *buf, size_t len)
{
	if (!access_ok(buf, len)) return -14;
	return vfs_read(fd, buf, len);
}

static long sys_writev(int fd, const iovec_t *iov, int cnt)
{
	if (!access_ok(iov, sizeof(iovec_t) * cnt)) return -14;

	long total = 0;
	for (int i = 0; i < cnt; i++) {
		if (!iov[i].iov_base || iov[i].iov_len == 0) continue;
		long r = sys_write(fd, iov[i].iov_base, iov[i].iov_len);
		if (r < 0) return r;
		total += r;
	}
	return total;
}

static long sys_sendfile(int out_fd, int in_fd, long *offset, size_t count)
{
	char buf[4096];
	size_t sent = 0;
	if (offset) {
		long off;
		if (copy_from_user(&off, offset, sizeof(off)) != 0) return -14;
		vfs_seek(in_fd, off, 0);
	}
	while (sent < count) {
		size_t n = count - sent;
		if (n > sizeof(buf)) n = sizeof(buf);
		long r = vfs_read(in_fd, buf, n);
		if (r <= 0) break;
		long w = vfs_write(out_fd, buf, (size_t)r);
		if (w < 0) return w;
		sent += (size_t)w;
	}
	if (offset) {
		long off = (long)vfs_seek(in_fd, 0, 1);
		copy_to_user(offset, &off, sizeof(off));
	}
	return (long)sent;
}

static long sys_readv(int fd, const iovec_t *iov, int cnt)
{
	if (!access_ok(iov, sizeof(iovec_t) * cnt)) return -14;

	long total = 0;
	for (int i = 0; i < cnt; i++) {
		if (!iov[i].iov_base || iov[i].iov_len == 0) continue;
		long r = sys_read(fd, iov[i].iov_base, iov[i].iov_len);
		if (r < 0) return r;
		if (r == 0) break;
		total += r;
	}
	return total;
}

static long sys_open(const char *path, int flags, mode_t mode)
{
	char kpath[PROC_NAME_MAX];
	int r = resolve_path(kpath, AT_FDCWD, path);
	if (r) return r;
	long fd = vfs_open(kpath, flags, mode);
	return fd < 0 ? -2 : fd;
}

static long sys_openat(int dirfd, const char *path, int flags, mode_t mode)
{
	char kpath[PROC_NAME_MAX];
	int r = resolve_path(kpath, dirfd, path);
	if (r) return r;
	long fd = vfs_open(kpath, flags, mode);
	return fd < 0 ? -2 : fd;
}

static long sys_close(int fd)
{
	return vfs_close(fd) == 0 ? 0 : -9;
}

static long sys_lseek(int fd, off_t off, int whence)
{
	return (long)vfs_seek(fd, off, whence);
}

static long sys_fstat(int fd, linux_stat_t *st)
{
	if (!st) return -14;
	memset(st, 0, sizeof(*st));

	if (fd >= 0 && fd <= 2) {
		st->st_mode     = 0x2190;
		st->st_nlink    = 1;
		st->st_rdev     = 0x0501;
		st->st_blksize  = 1024;
		return 0;
	}

	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return -9;

	vfs_stat_t vs = {0};
	if (f->inode->iops && f->inode->iops->stat)
		f->inode->iops->stat(f->inode, &vs);

	st->st_dev     = 1;  /* single root device */
	st->st_rdev    = vs.rdev;
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

static void vma_add(process_t *p, uint64_t vaddr, uint64_t size, uint32_t flags)
{
	vma_t *v = kmalloc(sizeof(*v));
	if (!v) return;
	v->vaddr = vaddr;
	v->size  = size;
	v->flags = flags;
	v->next  = p->vmas;
	p->vmas  = v;
}

static void vma_free_all(process_t *p)
{
	vma_t *v = p->vmas;
	while (v) {
		vma_t *next = v->next;
		kfree(v);
		v = next;
	}
	p->vmas = NULL;
}

static long sys_exit(int code)
{
	process_t *p = task_current();
	p->exit_code  = code;
	vma_free_all(p);
	task_exit();
	return 0;
}

static long sys_reboot(int magic1, int magic2, int cmd, void *arg)
{
	(void)arg;
	kprintf("sys_reboot: magic1=%x magic2=%x cmd=%x\n",
	        (uint32_t)magic1, (uint32_t)magic2, (uint32_t)cmd);
	if ((uint32_t)magic1 != 0xfee1dead) { kprintf("bad magic1\n"); return -22; }
	if ((uint32_t)magic2 != 0x28121969 &&
	    (uint32_t)magic2 != 0x05121996 &&
	    (uint32_t)magic2 != 0x16041998 &&
	    (uint32_t)magic2 != 0x20112000) { kprintf("bad magic2\n"); return -22; }
	kprintf("reboot: system going down\n");
	if (cmd == 0x4321fedc) /* LINUX_REBOOT_CMD_POWER_OFF */
		board_poweroff();
	board_reset();
	for (;;) __asm__ volatile("" ::: "memory");
	return 0;
}

static long sys_syslog(int type, char *buf, int len)
{
	if ((type == 2 || type == 3 || type == 4) && buf && len > 0) {
		uint64_t phys = (uint64_t)buddy_alloc(1);
		if (!phys) return -12;
		char *tmp = (char *)PHYS_TO_VIRT(phys);
		size_t cap = (size_t)len < 8192 ? (size_t)len : 8192;
		size_t n = kprintf_read_log(tmp, cap);
		long r = copy_to_user(buf, tmp, n) == 0 ? (long)n : -14;
		buddy_free(phys, 1);
		return r;
	}
	if (type == 10)
		return (long)kprintf_log_len();
	return 0;
}

static long sys_utimensat(int dirfd, const char *path, const void *times, int flags)
{
	(void)times; (void)flags;
	if (!path) return 0;
	char kp[PROC_NAME_MAX];
	int r = resolve_path(kp, dirfd, path);
	if (r) return r;
	vfs_stat_t st;
	if (vfs_stat(kp, &st) != 0) return -2; /* ENOENT */
	return 0;
}

static long sys_getpid(void)
{
	return (long)task_current()->pid;
}

static long sys_brk(uint64_t addr)
{
	process_t *p = task_current();
	if (addr == 0) return (long)p->brk;

	uint64_t new_brk = (addr + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

	if (new_brk < p->brk) {
		/* Shrink: free pages above new_brk */
		for (uint64_t va = new_brk; va < p->brk; va += PAGE_SIZE)
			vm_unmap(p->vm, va);
		p->brk = new_brk;
		return (long)new_brk;
	}

	for (uint64_t va = p->brk; va < new_brk; va += PAGE_SIZE) {
		void *page = buddy_alloc(0);
		if (!page) { kprintf("brk: ENOMEM pid=%u brk=%lx new=%lx free=%lu\n", p->pid, p->brk, new_brk, pmm_free_pages()); return (long)p->brk; }
		memset((void *)PHYS_TO_VIRT(page), 0, PAGE_SIZE);
		vm_map(p->vm, va, (uint64_t)page, VM_USER | VM_READ | VM_WRITE);
	}
	p->brk = new_brk;
	return (long)new_brk;
}

static long sys_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, uint64_t off)
{
	if (len == 0) return -22;
	process_t *p = task_current();
	len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

	uint64_t va;
	if ((flags & MAP_FIXED) && addr) va = addr & ~(uint64_t)(PAGE_SIZE - 1);
	else { va = p->mmap_next; p->mmap_next += len; }

	uint32_t vmflags = VM_USER;
	if (prot & PROT_READ)  vmflags |= VM_READ;
	if (prot & PROT_WRITE) vmflags |= VM_WRITE;
	if (prot & PROT_EXEC)  vmflags |= VM_EXEC;

	if (flags & MAP_ANONYMOUS) {
		if (prot == 0) {
			/* PROT_NONE: make pages inaccessible by unmapping them */
			for (uint64_t v = va; v < va + len; v += PAGE_SIZE)
				vm_unmap(p->vm, v);
		} else {
			for (uint64_t v = va; v < va + len; v += PAGE_SIZE) {
				void *pg = buddy_alloc(0);
				if (!pg) { kprintf("mmap: ENOMEM pid=%u va=%lx len=%zu free=%lu\n", p->pid, va, len, pmm_free_pages()); return -12; }
				memset((void *)PHYS_TO_VIRT(pg), 0, PAGE_SIZE);
				vm_map(p->vm, v, (uint64_t)pg, vmflags);
			}
		}
		{
			uint32_t vf = VMA_USER | VMA_ANON;
			if (prot & PROT_READ)  vf |= VMA_READ;
			if (prot & PROT_WRITE) vf |= VMA_WRITE;
			if (prot & PROT_EXEC)  vf |= VMA_EXEC;
			if (flags & MAP_SHARED) vf |= VMA_SHARED;
			vma_add(p, va, len, vf);
		}
		return (long)va;
	}

	/* File-backed mmap */
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return -9;

	/* If it's a device mapping via mmap callback */
	if (f->inode->fops && f->inode->fops->mmap) {
		uint64_t file_off = off;
		uint32_t dev_vmflags = vmflags | VM_NOCACHE;
		for (uint64_t v = va; v < va + len; v += PAGE_SIZE) {
			uint64_t phys = f->inode->fops->mmap(f, (off_t)file_off);
			if (!phys) {
				/* Mapping failed, cleanup is complex here, but we return error */
				return -22; /* EINVAL */
			}
			vm_map(p->vm, v, phys, dev_vmflags);
			file_off += PAGE_SIZE;
		}
		{
			uint32_t vf = VMA_USER;
			if (prot & PROT_READ)  vf |= VMA_READ;
			if (prot & PROT_WRITE) vf |= VMA_WRITE;
			if (prot & PROT_EXEC)  vf |= VMA_EXEC;
			vma_add(p, va, len, vf);
		}
		return (long)va;
	}

	if (!f->inode->fops->read) return -9;

	uint64_t file_off = off;
	for (uint64_t v = va; v < va + len; v += PAGE_SIZE) {
		void *pg = buddy_alloc(0);
		if (!pg) return -12;
		void *pg_virt = (void *)PHYS_TO_VIRT(pg);
		memset(pg_virt, 0, PAGE_SIZE);

		/* read one page from file at current offset via a temporary file handle */
		vfs_file_t tmp_f = *f;
		tmp_f.pos = (off_t)file_off;
		ssize_t nr = f->inode->fops->read(&tmp_f, pg_virt, PAGE_SIZE);
		(void)nr;
		file_off += PAGE_SIZE;

		vm_map(p->vm, v, (uint64_t)pg, vmflags);

#if defined(__aarch64__)
		if (prot & PROT_EXEC) {
			uintptr_t pp = (uintptr_t)pg;
			for (uintptr_t l = pp; l < pp + PAGE_SIZE; l += 64)
				__asm__ volatile("dc civac, %0" :: "r"(l) : "memory");
			__asm__ volatile("dsb ish; ic iallu; dsb ish; isb" ::: "memory");
		}
#endif
	}
	{
		uint32_t vf = VMA_USER;
		if (prot & PROT_READ)  vf |= VMA_READ;
		if (prot & PROT_WRITE) vf |= VMA_WRITE;
		if (prot & PROT_EXEC)  vf |= VMA_EXEC;
		vma_add(p, va, len, vf);
	}
	return (long)va;
}

#define USER_STACK_TOP_   ARCH_USER_STACK_TOP
#define USER_STACK_PAGES_ ARCH_USER_STACK_PAGES
#define USER_MMAP_BASE_   ARCH_USER_MMAP_BASE

static long sys_execve(const char *path, char * const argv[], char * const envp[])
{
	if (!path) return -14;

	process_t *p = task_current();

	/* copy path to kernel buffer */
	char kpath[PROC_NAME_MAX];
	int i = 0;
	while (i < PROC_NAME_MAX - 1 && path[i]) {
		kpath[i] = path[i];
		i++;
	}
	kpath[i] = '\0';

	/* copy argv/envp strings to kernel before VM switch */
#define EXEC_MAXARGS 64
#define EXEC_MAXSTRTOTAL 16384
	int argc = 0, envc = 0;
	char *kstrs = kmalloc(EXEC_MAXSTRTOTAL);
	char *kargv[EXEC_MAXARGS + 1];
	char *kenvp[EXEC_MAXARGS + 1];
	int koff = 0;
	if (kstrs) {
		if (argv) {
			for (; argc < EXEC_MAXARGS && argv[argc]; argc++) {
				const char *s = argv[argc];
				int n = 0;
				while (s[n] && koff + n < EXEC_MAXSTRTOTAL - 1) n++;
				memcpy(kstrs + koff, s, n);
				kstrs[koff + n] = '\0';
				kargv[argc] = kstrs + koff;
				koff += n + 1;
			}
		}
		if (envp) {
			for (; envc < EXEC_MAXARGS && envp[envc]; envc++) {
				const char *s = envp[envc];
				int n = 0;
				while (s[n] && koff + n < EXEC_MAXSTRTOTAL - 1) n++;
				memcpy(kstrs + koff, s, n);
				kstrs[koff + n] = '\0';
				kenvp[envc] = kstrs + koff;
				koff += n + 1;
			}
		}
	}

	/* read file from VFS */
	vfs_stat_t st = {0};
	if (vfs_stat(kpath, &st) != 0) { kfree(kstrs); return -2; }
	if (st.size == 0)              { kfree(kstrs); return -2; }

	uint8_t *buf = kmalloc(st.size);
	if (!buf) { kfree(kstrs); return -12; }

	int fd = vfs_open(kpath, O_RDONLY, 0);
	if (fd < 0) { kfree(buf); kfree(kstrs); return -2; }
	vfs_read(fd, buf, st.size);
	vfs_close(fd);

	/* build new address space */
	vm_space_t *new_vm = vm_create();
	if (!new_vm) { kfree(buf); kfree(kstrs); return -12; }

	elf_info_t ei = {0};
	if (elf_load(new_vm, buf, (size_t)st.size, &ei) != 0) {
		kfree(buf);
		vm_destroy(new_vm);
		kfree(kstrs);
		return -8;
	}
	kfree(buf);

	/* map user stack */
	for (int j = 0; j < USER_STACK_PAGES_; j++) {
		void *pg = buddy_alloc(0);
		if (!pg) { vm_destroy(new_vm); kfree(kstrs); return -12; }
		memset((void *)PHYS_TO_VIRT(pg), 0, PAGE_SIZE);
		uint64_t sva = USER_STACK_TOP_ - (uint64_t)(USER_STACK_PAGES_ - j) * PAGE_SIZE;
		vm_map(new_vm, sva, (uint64_t)pg, VM_USER | VM_READ | VM_WRITE);
	}
	{
		void *guard = buddy_alloc(0);
		if (guard) {
			memset((void *)PHYS_TO_VIRT(guard), 0, PAGE_SIZE);
			uint64_t guard_va = USER_STACK_TOP_ - (uint64_t)(USER_STACK_PAGES_ + 1) * PAGE_SIZE;
			vm_map(new_vm, guard_va, (uint64_t)guard, VM_USER | VM_READ);
		}
	}

	/* update process name */
	const char *last_slash = kpath;
	for (int j = 0; kpath[j]; j++) if (kpath[j] == '/') last_slash = kpath + j + 1;
	
	int nlen = 0;
	while (last_slash[nlen] && nlen < PROC_NAME_MAX - 1) {
		p->name[nlen] = last_slash[nlen];
		nlen++;
	}
	p->name[nlen] = '\0';

	/* close O_CLOEXEC file descriptors before exec */
	if (p->fds) {
		for (int i = 0; i < p->fds->cap; i++) {
			if (!fd_is_cloexec(p->fds, i)) continue;
			vfs_file_t *xf = fd_get(p->fds, i);
			if (!xf) { fd_set_cloexec(p->fds, i, 0); continue; }
			fd_free(p->fds, i);
			if (--xf->refcount == 0) {
				if (xf->inode->fops && xf->inode->fops->close)
					xf->inode->fops->close(xf);
				kfree(xf);
			}
		}
	}

	/* reset signal state: SIG_IGN stays, user handlers reset to SIG_DFL */
	for (int i = 0; i < NSIG; i++) {
		if (p->sig_actions[i].sa_handler != (void (*)(int))1 /* SIG_IGN */)
			p->sig_actions[i] = (sigaction_t){0};
	}
	p->sig_mask    = 0;
	p->sig_pending = 0;

	/* switch to new address space, freeing the old one */
	vma_free_all(p);
	vm_space_t *old_vm = p->vm;
	p->vm        = new_vm;
	p->brk       = ei.load_end;
	p->mmap_next = USER_MMAP_BASE_;
	arch_vm_switch((uint64_t)new_vm->root);
	if (old_vm) vm_destroy(old_vm);

	/* record initial VMAs */
	vma_add(p, USER_STACK_TOP_ - USER_STACK_PAGES_ * PAGE_SIZE,
	        USER_STACK_PAGES_ * PAGE_SIZE, VMA_USER | VMA_READ | VMA_WRITE);
	vma_add(p, ei.load_bias, ei.load_end - ei.load_bias, VMA_USER | VMA_READ | VMA_EXEC);

	/* build SysV ABI initial stack */
	uint64_t uargv[EXEC_MAXARGS + 1];
	uint64_t uenvp[EXEC_MAXARGS + 1];
	memset(uargv, 0, sizeof(uargv));
	memset(uenvp, 0, sizeof(uenvp));

	uint64_t sp = USER_STACK_TOP_;
	if (kstrs) {
		for (int j = envc - 1; j >= 0; j--) {
			int n = 0; while (kenvp[j][n]) n++;
			sp -= (uint64_t)(n + 1);
			memcpy((void *)sp, kenvp[j], (size_t)(n + 1));
			uenvp[j] = sp;
		}
		for (int j = argc - 1; j >= 0; j--) {
			int n = 0; while (kargv[j][n]) n++;
			sp -= (uint64_t)(n + 1);
			memcpy((void *)sp, kargv[j], (size_t)(n + 1));
			uargv[j] = sp;
		}
		kfree(kstrs);
	}
	sp &= ~15ULL;

	/* 16 pseudo-random bytes for AT_RANDOM */
	sp -= 16;
	{
		extern volatile uint64_t kernel_ticks;
		uint64_t rs = (uint64_t)(uintptr_t)p ^ kernel_ticks ^ ei.entry;
		uint64_t *rp = (uint64_t *)sp;
		rs = rs * 6364136223846793005ULL + 1442695040888963407ULL;
		rp[0] = rs;
		rs = rs * 6364136223846793005ULL + 1442695040888963407ULL;
		rp[1] = rs;
	}
	uint64_t at_random_addr = sp;

	/* execfn string */
	{
		int plen = 0; while (kpath[plen]) plen++;
		sp -= (uint64_t)(plen + 1);
		memcpy((void *)sp, kpath, (size_t)(plen + 1));
	}
	uint64_t at_execfn_addr = sp;

	sp &= ~15ULL;

	/* padding before auxv keeps the final sp 16-byte aligned;
	   must NOT be placed between envp[] and auxv — musl would read it as AT_NULL */
	if ((argc + envc + 3) % 2 != 0) {
		sp -= 8;
		*(uint64_t *)sp = 0;
	}

/* push one auxv key/value pair (16 bytes each, stack grows down) */
#define PUSH_AUX(k, v) do { \
	sp -= 16; \
	((uint64_t *)sp)[0] = (uint64_t)(k); \
	((uint64_t *)sp)[1] = (uint64_t)(v); \
} while (0)

	PUSH_AUX(0,  0);
	PUSH_AUX(31, at_execfn_addr);
	PUSH_AUX(25, at_random_addr);
	PUSH_AUX(23, 0);
	PUSH_AUX(17, TIMER_HZ);
#if defined(__aarch64__)
	PUSH_AUX(16, 3);
#else
	PUSH_AUX(16, 0);
#endif
	PUSH_AUX(14, 0);
	PUSH_AUX(13, 0);
	PUSH_AUX(12, 0);
	PUSH_AUX(11, 0);
	PUSH_AUX(9,  ei.entry);
	PUSH_AUX(7,  0);
	PUSH_AUX(6,  PAGE_SIZE);
	PUSH_AUX(5,  ei.phnum);
	PUSH_AUX(4,  ei.phentsize);
	PUSH_AUX(3,  ei.phdr_va);
#undef PUSH_AUX

	/* envp[] */
	sp -= 8; *(uint64_t *)sp = 0;
	for (int j = envc - 1; j >= 0; j--) { sp -= 8; *(uint64_t *)sp = uenvp[j]; }
	/* argv[] */
	sp -= 8; *(uint64_t *)sp = 0;
	for (int j = argc - 1; j >= 0; j--) { sp -= 8; *(uint64_t *)sp = uargv[j]; }
	/* argc */
	sp -= 8; *(uint64_t *)sp = (uint64_t)argc;

	/* reinitialise context frame */
	arch_context_init_user(p->ctx, ei.entry, sp);

	/* reset TLS */
	p->tls_base = 0;
#if defined(__aarch64__)
	__asm__ volatile("msr tpidr_el0, xzr");
#elif defined(__x86_64__)
	__asm__ volatile("wrmsr" :: "c"(0xC0000100U), "a"(0U), "d"(0U));
#endif

	/* wake vfork parent — child now has its own address space */
	proc_vfork_exec_notify();

	return 0;
}

static long sys_rt_sigaction(int sig, const sigaction_t *act, sigaction_t *oldact, size_t sigsetsize)
{
	if (sig <= 0 || sig > NSIG || sigsetsize < 8) return -22;
	if (sig == SIGKILL) return -22;

	/* Linux struct sigaction: handler, flags, restorer, mask[N]
	 * Our sigaction_t mirrors this layout. User struct may have larger
	 * sa_mask (16 bytes for 128-bit RT sigset); we only store 64 bits. */
	size_t copy_sz = sizeof(sigaction_t);
	/* If user struct is larger (sigsetsize > 8), user's sa_mask is at offset
	 * 24..24+sigsetsize. We read/write only the first 8 bytes of sa_mask
	 * (offset 24..32 = sizeof sigaction_t). */

	process_t *p = task_current();
	if (oldact) {
		if (copy_to_user(oldact, &p->sig_actions[sig - 1], copy_sz) != 0)
			return -14;
	}
	if (act) {
		if (copy_from_user(&p->sig_actions[sig - 1], act, copy_sz) != 0)
			return -14;
	}
	return 0;
}

static long sys_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oldset, size_t sigsetsize)
{
	if (sigsetsize < 8) return -22;
	process_t *p = task_current();
	if (oldset) {
		if (copy_to_user(oldset, &p->sig_mask, sizeof(uint64_t)) != 0)
			return -14;
	}
	if (set) {
		uint64_t new_set;
		if (copy_from_user(&new_set, set, sizeof(uint64_t)) != 0)
			return -14;
		
		if (how == 0) p->sig_mask |= new_set;        /* SIG_BLOCK */
		else if (how == 1) p->sig_mask &= ~new_set;  /* SIG_UNBLOCK */
		else if (how == 2) p->sig_mask = new_set;    /* SIG_SETMASK */
		else return -22;
		
		p->sig_mask &= ~(1UL << (SIGKILL - 1));
	}
	return 0;
}

static long sys_kill(pid_t pid, int sig)
{
	process_t *p = proc_find(pid);
	if (!p) return -3; /* ESRCH */
	task_signal(p, sig);
	return 0;
}

/* tgkill: simplified — thread groups not implemented, just route to kill */
static long sys_tgkill(pid_t tgid, pid_t tid, int sig)
{
	(void)tgid;
	return sys_kill(tid, sig);
}


static void sys_rt_sigreturn(arch_context_t *ctx)
{
	process_t *p = task_current();
#if defined(__aarch64__)
	/* Restore full frame saved by signal_dispatch */
	uint64_t usp  = ctx->sp;
	ctx->elr      = *(uint64_t *)(usp +   0);
	ctx->regs[30] = *(uint64_t *)(usp +   8);
	p->sig_mask   = *(uint64_t *)(usp +  16);
	for (int _i = 0; _i <= 18; _i++)
		ctx->regs[_i] = *(uint64_t *)(usp + 32 + (uint64_t)_i * 8);
	ctx->regs[29] = *(uint64_t *)(usp + 184);
	ctx->spsr     = *(uint64_t *)(usp + 192);
	ctx->sp       = usp + 256;
#elif defined(__x86_64__)
	/* After handler ret + restorer syscall, ctx->rsp = frame_base+8.
	 * Frame layout matches signal_dispatch: offsets from frame_base+8. */
	uint64_t base = ctx->rsp;
	p->sig_mask   = *(uint64_t *)(base +   0);
	ctx->rip      = *(uint64_t *)(base +   8);
	ctx->rsp      = *(uint64_t *)(base +  16);
	ctx->rflags   = *(uint64_t *)(base +  24);
	ctx->rax      = *(uint64_t *)(base +  32);
	ctx->rbx      = *(uint64_t *)(base +  40);
	ctx->rcx      = *(uint64_t *)(base +  48);
	ctx->rdx      = *(uint64_t *)(base +  56);
	ctx->rsi      = *(uint64_t *)(base +  64);
	ctx->rdi      = *(uint64_t *)(base +  72);
	ctx->rbp      = *(uint64_t *)(base +  80);
	ctx->r8       = *(uint64_t *)(base +  88);
	ctx->r9       = *(uint64_t *)(base +  96);
	ctx->r10      = *(uint64_t *)(base + 104);
	ctx->r11      = *(uint64_t *)(base + 112);
	ctx->r12      = *(uint64_t *)(base + 120);
	ctx->r13      = *(uint64_t *)(base + 128);
	ctx->r14      = *(uint64_t *)(base + 136);
	ctx->r15      = *(uint64_t *)(base + 144);
#endif
}

static long sys_pipe2(int *pipefd, int flags)
{
	int kfds[2];
	if (vfs_pipe(kfds) != 0) return -1;
	process_t *p = task_current();
	if (flags & O_NONBLOCK) {
		for (int i = 0; i < 2; i++) {
			vfs_file_t *f = fd_get(p->fds, kfds[i]);
			if (f) f->flags |= O_NONBLOCK;
		}
	}
	if (flags & O_CLOEXEC) {
		fd_set_cloexec(p->fds, kfds[0], 1);
		fd_set_cloexec(p->fds, kfds[1], 1);
	}
	if (copy_to_user(pipefd, kfds, sizeof(int) * 2) != 0) return -14;
	return 0;
}

static long sys_fork(void)
{
	return (long)proc_fork();
}

static long sys_vfork(void)
{
	return (long)proc_vfork();
}

#define WNOHANG 1

static long sys_waitpid(pid_t pid, int *status, int flags)
{
	process_t *p = task_current();
	int wnohang  = (flags & WNOHANG) != 0;

	if (pid == -1) {
		while (1) {
			process_t *child = NULL;
			int any_alive = 0;
			proc_find_zombie(p->pid, &child, &any_alive);
			if (child) {
				int st = child->killed_by_sig
				       ? (child->exit_code & 0x7F)
				       : (child->exit_code & 0xFF) << 8;
				pid_t result = child->pid;
				proc_reap(child);
				if (status && (uint64_t)status > 0x1000)
					*status = st;
				return (long)result;
			}
			if (!any_alive) return -10; /* ECHILD */
			if (wnohang)    return 0;
			task_sleep();
		}
	}

	process_t *child = proc_find(pid);
	if (!child || child->ppid != p->pid) return -10;

	while (child->state != PROC_DEAD) {
		if (wnohang) return 0;
		task_sleep();
	}

	child->reaped = 1;
	int st = child->killed_by_sig
	       ? (child->exit_code & 0x7F)
	       : (child->exit_code & 0xFF) << 8;
	proc_reap(child);
	if (status && (uint64_t)status > 0x1000)
		*status = st;
	return (long)pid;
}

#if defined(__x86_64__)
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002

static long sys_arch_prctl(int code, uint64_t addr)
{
	if (code == ARCH_SET_FS) {
		__asm__ volatile("wrmsr" :: "c"(0xC0000100U), "a"((uint32_t)addr), "d"((uint32_t)(addr >> 32)));
		task_current()->tls_base = addr;
		return 0;
	}
	if (code == ARCH_SET_GS) {
		/* When kernel is running, USER GS is stored in KERNEL_GS_BASE MSR. */
		__asm__ volatile("wrmsr" :: "c"(0xC0000102U), "a"((uint32_t)addr), "d"((uint32_t)(addr >> 32)));
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
	/* remove fully-covered VMAs */
	vma_t **pp = &p->vmas;
	while (*pp) {
		vma_t *v = *pp;
		if (v->vaddr >= addr && v->vaddr + v->size <= addr + len) {
			*pp = v->next;
			kfree(v);
		} else {
			pp = &v->next;
		}
	}
	return 0;
}

static long sys_mprotect(uint64_t addr, size_t len, int prot)
{
	if (len == 0) return 0;
	process_t *p = task_current();
	addr &= ~(uint64_t)(PAGE_SIZE - 1);
	len  = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

	uint32_t vmflags = VM_USER;
	if (prot & PROT_READ)  vmflags |= VM_READ;
	if (prot & PROT_WRITE) vmflags |= VM_WRITE;
	if (prot & PROT_EXEC)  vmflags |= VM_EXEC;

	extern uint64_t arch_vm_get_paddr(uint64_t *root, uint64_t vaddr);
	for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE) {
		uint64_t pa = arch_vm_get_paddr(p->vm->root, va);
		if (pa) vm_map(p->vm, va, pa, vmflags);
	}
	return 0;
}
static long sys_prlimit64(pid_t pid, int resource, const void *new_limit, void *old_limit)
{
	(void)pid; (void)new_limit;
	if (old_limit) {
		uint64_t limits[2];
		switch (resource) {
		case 3:  /* RLIMIT_STACK  */ limits[0] = limits[1] = 8UL << 20; break;
		case 7:  /* RLIMIT_NOFILE */ limits[0] = limits[1] = FD_MAX; break;
		default: limits[0] = limits[1] = (uint64_t)-1; break; /* RLIM_INFINITY */
		}
		copy_to_user(old_limit, limits, 16);
	}
	return 0;
}

static long sys_madvise(uint64_t addr, size_t len, int advice)
{
	(void)addr; (void)len; (void)advice;
	return 0;
}

static long sys_msync(uint64_t addr, size_t len, int flags)
{
	(void)addr; (void)len; (void)flags;
	return 0;
}

static long sys_sched_yield(void) { yield(); return 0; }

static long sys_sched_setaffinity(pid_t pid, size_t len, const void *mask)
{
	(void)pid; (void)len; (void)mask;
	return 0;
}

static long sys_sched_getaffinity(pid_t pid, size_t len, void *mask)
{
	(void)pid;
	if (len >= 8 && access_ok(mask, len)) {
		uint64_t m = 1;
		copy_to_user(mask, &m, 8);
	}
	return 0;
}

#define PR_SET_NAME    15
#define PR_GET_NAME    16
#define PR_SET_DUMPABLE 4
#define PR_GET_DUMPABLE 3
#define PR_SET_PDEATHSIG 1
#define PR_SET_CHILD_SUBREAPER 36

static long sys_prctl(int option, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg3; (void)arg4; (void)arg5;
	process_t *p = task_current();
	switch (option) {
	case PR_SET_NAME: {
		char name[16];
		if (strncpy_from_user(name, (const void *)(uintptr_t)arg2, 16) >= 0) {
			name[15] = '\0';
			int len = 0;
			while (name[len] && len < PROC_NAME_MAX - 1) len++;
			memcpy(p->name, name, len);
			p->name[len] = '\0';
		}
		return 0;
	}
	case PR_GET_NAME: {
		char name[16];
		memset(name, 0, sizeof(name));
		int len = 0;
		while (p->name[len] && len < 15) { name[len] = p->name[len]; len++; }
		copy_to_user((void *)(uintptr_t)arg2, name, 16);
		return 0;
	}
	case PR_GET_DUMPABLE: return 0;
	default: return 0;
	}
}

static uint64_t getrandom_state = 0xdeadbeefcafebabe;

static long sys_getrandom(void *buf, size_t len, unsigned int flags)
{
	(void)flags;
	if (!access_ok(buf, len)) return -14;
	extern volatile uint64_t kernel_ticks;
	getrandom_state ^= kernel_ticks;
	uint8_t *dst = buf;
	for (size_t i = 0; i < len; i++) {
		getrandom_state = getrandom_state * 6364136223846793005ULL + 1442695040888963407ULL;
		dst[i] = (uint8_t)(getrandom_state >> 33);
	}
	return (long)len;
}

typedef struct {
	uint32_t stx_mask;
	uint32_t stx_blksize;
	uint64_t stx_attributes;
	uint32_t stx_nlink;
	uint32_t stx_uid;
	uint32_t stx_gid;
	uint16_t stx_mode;
	uint16_t __pad1;
	uint64_t stx_ino;
	uint64_t stx_size;
	uint64_t stx_blocks;
	uint64_t stx_attributes_mask;
	int64_t  stx_atime_sec; uint32_t stx_atime_nsec; int32_t _a;
	int64_t  stx_btime_sec; uint32_t stx_btime_nsec; int32_t _b;
	int64_t  stx_ctime_sec; uint32_t stx_ctime_nsec; int32_t _c;
	int64_t  stx_mtime_sec; uint32_t stx_mtime_nsec; int32_t _m;
	uint32_t stx_rdev_major;
	uint32_t stx_rdev_minor;
	uint32_t stx_dev_major;
	uint32_t stx_dev_minor;
} linux_statx_t;

static long sys_statx(int dirfd, const char *path, int flags, unsigned int mask, linux_statx_t *buf)
{
	(void)flags; (void)mask;
	char kp[PROC_NAME_MAX];
	int r = resolve_path(kp, dirfd, path);
	if (r) return r;

	vfs_stat_t ks;
	r = vfs_stat(kp, &ks);
	if (r < 0) return -2;

	linux_statx_t sx;
	memset(&sx, 0, sizeof(sx));
	sx.stx_mask       = 0x7FF;
	sx.stx_blksize    = 4096;
	sx.stx_nlink      = ks.nlink;
	sx.stx_uid        = ks.uid;
	sx.stx_gid        = ks.gid;
	sx.stx_mode       = (uint16_t)ks.mode;
	sx.stx_ino        = ks.ino;
	sx.stx_size       = (uint64_t)ks.size;
	sx.stx_blocks     = (ks.size + 511) / 512;
	sx.stx_dev_major  = 0;
	sx.stx_dev_minor  = 1;
	sx.stx_rdev_major = (uint32_t)(ks.rdev >> 8);
	sx.stx_rdev_minor = (uint32_t)(ks.rdev & 0xFF);
	return copy_to_user(buf, &sx, sizeof(sx));
}

/* ── path helper ─────────────────────────────────────────── */

extern volatile uint64_t kernel_ticks;

static int resolve_path(char *out, int dirfd, const char *upath)
{
	char kp[PROC_NAME_MAX];
	if (strncpy_from_user(kp, upath, PROC_NAME_MAX) < 0) return -14;
	if (kp[0] == '/') { memcpy(out, kp, PROC_NAME_MAX); return 0; }
	process_t *p = task_current();
	const char *base = (dirfd == AT_FDCWD) ? p->cwd : "/";
	int blen = strlen(base), plen = strlen(kp);
	if (blen + 1 + plen >= PROC_NAME_MAX) return -36;
	memcpy(out, base, blen);
	if (blen == 0 || out[blen - 1] != '/') out[blen++] = '/';
	memcpy(out + blen, kp, plen);
	out[blen + plen] = '\0';
	return 0;
}

/* ── fs syscalls ─────────────────────────────────────────── */

static long sys_mkdir(const char *path, mode_t mode)
{
	char kp[PROC_NAME_MAX];
	if (strncpy_from_user(kp, path, PROC_NAME_MAX) < 0) return -14;
	return vfs_mkdir(kp, mode);
}

static long sys_mkdirat(int dirfd, const char *path, mode_t mode)
{
	char kp[PROC_NAME_MAX];
	int r = resolve_path(kp, dirfd, path);
	if (r) return r;
	return vfs_mkdir(kp, mode);
}

static long sys_unlink(const char *path)
{
	char kp[PROC_NAME_MAX];
	if (strncpy_from_user(kp, path, PROC_NAME_MAX) < 0) return -14;
	return vfs_unlink(kp) == 0 ? 0 : -2;
}

static long sys_unlinkat(int dirfd, const char *path, int flags)
{
	(void)flags;
	char kp[PROC_NAME_MAX];
	int r = resolve_path(kp, dirfd, path);
	if (r) return r;
	return vfs_unlink(kp) == 0 ? 0 : -2;
}

static long sys_rename(const char *old, const char *new)
{
	char ko[PROC_NAME_MAX], kn[PROC_NAME_MAX];
	if (strncpy_from_user(ko, old, PROC_NAME_MAX) < 0) return -14;
	if (strncpy_from_user(kn, new, PROC_NAME_MAX) < 0) return -14;
	return vfs_rename(ko, kn) == 0 ? 0 : -2;
}

static long sys_renameat(int olddirfd, const char *old, int newdirfd, const char *newp)
{
	char ko[PROC_NAME_MAX], kn[PROC_NAME_MAX];
	int r = resolve_path(ko, olddirfd, old);  if (r) return r;
	    r = resolve_path(kn, newdirfd, newp); if (r) return r;
	return vfs_rename(ko, kn) == 0 ? 0 : -2;
}

static long sys_rmdir(const char *path)
{
	char kp[PROC_NAME_MAX];
	if (strncpy_from_user(kp, path, PROC_NAME_MAX) < 0) return -14;
	return vfs_unlink(kp) == 0 ? 0 : -2;
}

/* ── stat syscalls ───────────────────────────────────────── */

static void fill_linux_stat(linux_stat_t *st, const vfs_stat_t *vs)
{
	memset(st, 0, sizeof(*st));
	st->st_dev     = 1;
	st->st_mode    = vs->mode;
	st->st_ino     = vs->ino;
	st->st_size    = (int64_t)vs->size;
	st->st_nlink   = vs->nlink ? vs->nlink : 1;
	st->st_uid     = vs->uid;
	st->st_gid     = vs->gid;
	st->st_blksize = 4096;
	st->st_blocks  = (vs->size + 511) / 512;
	st->st_rdev    = vs->rdev;
}

static long sys_stat_path(const char *path, linux_stat_t *st)
{
	if (!st) return -14;
	char kp[PROC_NAME_MAX];
	int r = resolve_path(kp, AT_FDCWD, path);
	if (r) return r;
	vfs_stat_t vs = {0};
	if (vfs_stat(kp, &vs) != 0) return -2;
	linux_stat_t ks;
	fill_linux_stat(&ks, &vs);
	return copy_to_user(st, &ks, sizeof(ks));
}

#define AT_EMPTY_PATH 0x1000

static long sys_newfstatat(int dirfd, const char *path, linux_stat_t *st, int flags)
{
	if (!st) return -14;
	char kp[PROC_NAME_MAX];
	if (flags & AT_EMPTY_PATH) {
		/* stat the fd itself */
		if (strncpy_from_user(kp, path, 2) >= 0 && kp[0] == '\0')
			return sys_fstat(dirfd, st);
	}
	int r = resolve_path(kp, dirfd, path);
	if (r) return r;
	vfs_stat_t vs = {0};
	if (vfs_stat(kp, &vs) != 0) return -2;
	linux_stat_t ks;
	fill_linux_stat(&ks, &vs);
	return copy_to_user(st, &ks, sizeof(ks));
}

static long sys_access(const char *path, int mode)
{
	(void)mode;
	char kp[PROC_NAME_MAX];
	int r = resolve_path(kp, AT_FDCWD, path);
	if (r) return r;
	vfs_stat_t vs = {0};
	return vfs_stat(kp, &vs) == 0 ? 0 : -2;
}

static long sys_faccessat(int dirfd, const char *path, int mode, int flags)
{
	(void)flags; (void)mode;
	char kp[PROC_NAME_MAX];
	int r = resolve_path(kp, dirfd, path);
	if (r) return r;
	vfs_stat_t vs = {0};
	return vfs_stat(kp, &vs) == 0 ? 0 : -2;
}

static long sys_truncate(const char *path, off_t len)
{
	char kp[256];
	int r = resolve_path(kp, AT_FDCWD, path);
	if (r) return r;
	vfs_dentry_t *de = vfs_dentry_resolve(kp);
	if (!de || !de->inode) return -2;
	vfs_inode_t *in = de->inode;
	if (!in->iops || !in->iops->truncate) return -22;
	return in->iops->truncate(in, len);
}

static long sys_ftruncate(int fd, off_t len)
{
	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return -9;
	vfs_inode_t *in = f->inode;
	if (!in->iops || !in->iops->truncate) return -22;
	return in->iops->truncate(in, len);
}

/* ── getdents64 ──────────────────────────────────────────── */

#define DT_FIFO  1
#define DT_CHR   2
#define DT_DIR   4
#define DT_BLK   6
#define DT_REG   8
#define DT_LNK  10
#define DT_SOCK 12

static long sys_getdents64(int fd, void *buf, size_t bufsize)
{
	if (!access_ok(buf, bufsize)) return -14;
	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return -9;
	vfs_inode_t *inode = f->inode;
	if (!S_ISDIR(inode->mode)) return -20;
	if (!inode->iops || !inode->iops->readdir) return -95;

	uint8_t *kbuf = kmalloc(bufsize > 4096 ? 4096 : bufsize);
	if (!kbuf) return -12;
	size_t bufcap = bufsize > 4096 ? 4096 : bufsize;

	size_t  offset = 0;
	long    total  = 0;
	uint32_t idx   = (uint32_t)f->pos;

	while (1) {
		vfs_dirent_t vde;
		if (inode->iops->readdir(inode, idx, &vde) != 0) break;
		int namelen = strlen(vde.name);
		size_t reclen = (19 + namelen + 1 + 7) & ~7UL;
		if (offset + reclen > bufcap) break;

		uint8_t *e = kbuf + offset;
		memset(e, 0, reclen);
		*(uint64_t *)(e + 0)  = vde.ino;
		*(int64_t  *)(e + 8)  = (int64_t)vde.next_off;
		*(uint16_t *)(e + 16) = (uint16_t)reclen;
		e[18] = S_ISREG(vde.mode)  ? DT_REG  :
		        S_ISDIR(vde.mode)  ? DT_DIR  :
		        S_ISLNK(vde.mode)  ? DT_LNK  :
		        S_ISCHR(vde.mode)  ? DT_CHR  :
		        S_ISFIFO(vde.mode) ? DT_FIFO : 0;
		memcpy(e + 19, vde.name, namelen);
		e[19 + namelen] = '\0';
		offset += reclen;
		total  += (long)reclen;
		idx = vde.next_off;
	}

	f->pos = (off_t)idx;
	if (total > 0)
		copy_to_user(buf, kbuf, offset);
	kfree(kbuf);
	return total;
}

/* ── dup / fcntl ─────────────────────────────────────────── */

static long sys_dup(int oldfd)
{
	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, oldfd);
	if (!f) return -9;
	int newfd = fd_alloc(p->fds, f);
	if (newfd < 0) return -24;
	fd_set_cloexec(p->fds, newfd, 0);
	return newfd;
}

static long sys_dup2(int oldfd, int newfd)
{
	if (newfd < 0) return -9;
	process_t *p = task_current();
	vfs_file_t *src = p->fds ? fd_get(p->fds, oldfd) : NULL;
	if (!src) return -9;
	if (oldfd == newfd) return newfd;
	if (fd_get(p->fds, newfd)) vfs_close(newfd);
	if (fd_alloc_at(p->fds, src, newfd) < 0) return -24;
	fd_set_cloexec(p->fds, newfd, 0);
	return newfd;
}

static long sys_dup3(int oldfd, int newfd, int flags)
{
	long r = sys_dup2(oldfd, newfd);
	if (r >= 0) {
		process_t *p = task_current();
		fd_set_cloexec(p->fds, newfd, !!(flags & O_CLOEXEC));
	}
	return r;
}

#define F_DUPFD    0
#define F_GETFD    1
#define F_SETFD    2
#define F_GETFL    3
#define F_SETFL    4
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC 1

static long sys_fcntl(int fd, int cmd, uint64_t arg)
{
	process_t  *p = task_current();
	switch (cmd) {
	case F_DUPFD:
	case F_DUPFD_CLOEXEC: {
		/* dup to first fd >= arg */
		vfs_file_t *f = fd_get(p->fds, fd);
		if (!f) return -9;
		int nfd = fd_alloc_from(p->fds, f, (int)arg);
		if (nfd < 0) return -24;
		if (cmd == F_DUPFD_CLOEXEC) fd_set_cloexec(p->fds, nfd, 1);
		return nfd;
	}
	case F_GETFD: {
		vfs_file_t *f = fd_get(p->fds, fd);
		if (!f) return -9;
		return fd_is_cloexec(p->fds, fd) ? FD_CLOEXEC : 0;
	}
	case F_SETFD: {
		if (!fd_get(p->fds, fd)) return -9;
		fd_set_cloexec(p->fds, fd, !!(arg & FD_CLOEXEC));
		return 0;
	}
	case F_GETFL: {
		vfs_file_t *f = fd_get(p->fds, fd);
		return f ? (long)f->flags : -9;
	}
	case F_SETFL: {
		vfs_file_t *f = fd_get(p->fds, fd);
		if (!f) return -9;
		f->flags = (f->flags & ~0xFFF) | ((int)arg & 0xFFF);
		return 0;
	}
	default: return 0;
	}
}

/* ── chdir / getcwd ──────────────────────────────────────── */

static long sys_chdir(const char *path)
{
	char kp[PROC_NAME_MAX];
	int r = resolve_path(kp, -100 /*AT_FDCWD*/, path);
	if (r) return r;
	/* resolve . and .. via dentry so we get the canonical path */
	vfs_dentry_t *d = vfs_dentry_resolve(kp);
	if (!d) return -2;
	if (!S_ISDIR(d->inode->mode)) return -20;
	/* rebuild canonical path by walking up the dentry tree */
	char canon[PROC_NAME_MAX];
	char tmp[PROC_NAME_MAX];
	canon[0] = '\0';
	vfs_dentry_t *cur = d;
	while (cur && cur->parent) {
		int tlen = strlen(cur->name);
		int clen = strlen(canon);
		if (tlen + 1 + clen >= PROC_NAME_MAX) return -36;
		memmove(tmp + tlen + 1, canon, (size_t)(clen + 1));
		tmp[0] = '/';
		memcpy(tmp + 1, cur->name, (size_t)tlen);
		memcpy(canon, tmp, (size_t)(tlen + 1 + clen + 1));
		cur = cur->parent;
	}
	if (canon[0] == '\0') { canon[0] = '/'; canon[1] = '\0'; }
	process_t *p = task_current();
	int n = strlen(canon);
	if (n >= 256) return -36;
	memcpy(p->cwd, canon, (size_t)(n + 1));
	return 0;
}

static long sys_getcwd(char *buf, size_t size)
{
	if (!buf || size == 0) return -14;
	process_t *p = task_current();
	int n = strlen(p->cwd);
	if ((size_t)(n + 1) > size) return -34; /* ERANGE */
	copy_to_user(buf, p->cwd, n + 1);
	return (long)(n + 1);
}

/* ── process info ────────────────────────────────────────── */

static long sys_getuid(void)  { return (long)task_current()->uid; }
static long sys_geteuid(void) { return (long)task_current()->uid; }
static long sys_getgid(void)  { return (long)task_current()->gid; }
static long sys_getegid(void) { return (long)task_current()->gid; }
static long sys_getppid(void) { return (long)task_current()->ppid; }
static long sys_gettid(void)  { return (long)task_current()->pid; }
static long sys_getgroups(int size, uint64_t *list) {
	if (size == 0) return 1;
	if (size < 1) return -22;
	if (!access_ok(list, (size_t)size * sizeof(uint64_t))) return -14;
	/* single supplementary group = gid */
	uint64_t gid = (uint64_t)task_current()->gid;
	return copy_to_user(list, &gid, sizeof(gid)) == 0 ? 1 : -14;
}
static long sys_getrlimit(int res, void *rlim) {
	if (!access_ok(rlim, 16)) return -14;
	uint64_t inf = (uint64_t)-1;
	if (res == 7) { uint64_t pair[2] = {FD_MAX, FD_MAX}; return copy_to_user(rlim, pair, 16) == 0 ? 0 : -14; }
	uint64_t pair[2] = {inf, inf};
	return copy_to_user(rlim, pair, 16) == 0 ? 0 : -14;
}
static long sys_setrlimit(int res, const void *rlim) { (void)res; (void)rlim; return 0; }
static long sys_getpgrp(void) { return (long)task_current()->pgrp; }
static long sys_getpgid(pid_t pid) {
	process_t *p = pid ? proc_find(pid) : task_current();
	return p ? (long)p->pgrp : -3;
}
static long sys_getsid(pid_t pid) {
	process_t *p = pid ? proc_find(pid) : task_current();
	return p ? (long)p->sid : -3;
}
static long sys_setsid(void) {
	process_t *p = task_current();
	p->pgrp = (pid_t)p->pid;
	p->sid  = (pid_t)p->pid;
	return (long)p->pid;
}
static long sys_setpgid(pid_t pid, pid_t pgid) {
	process_t *p = pid ? proc_find(pid) : task_current();
	if (!p) return -3;
	p->pgrp = pgid ? pgid : (pid_t)p->pid;
	return 0;
}
static long sys_setuid(uid_t uid) { task_current()->uid = uid; return 0; }
static long sys_setgid(gid_t gid) { task_current()->gid = gid; return 0; }

static long sys_getpriority(int which, int who)
{
	(void)which; (void)who;
	return 20 - task_current()->nice;  /* POSIX: returns 20 - nice */
}
static long sys_setpriority(int which, int who, int prio)
{
	(void)which; (void)who;
	int nice = 20 - prio;
	if (nice < -20) nice = -20;
	if (nice >  19) nice =  19;
	task_current()->nice = nice;
	return 0;
}

static long sys_sigaltstack(const stack_t *uss, stack_t *uoss)
{
	process_t *p = task_current();
	if (uoss) {
		if (copy_to_user(uoss, &p->altstack, sizeof(stack_t)) != 0)
			return -14;
	}
	if (uss) {
		stack_t ss;
		if (copy_from_user(&ss, uss, sizeof(stack_t)) != 0)
			return -14;
		if (ss.ss_flags & SS_DISABLE) {
			p->altstack.ss_flags = SS_DISABLE;
			p->altstack.ss_sp    = NULL;
			p->altstack.ss_size  = 0;
		} else {
			if (!ss.ss_sp || ss.ss_size < 2048) return -22;
			p->altstack = ss;
		}
	}
	return 0;
}
static long sys_umask(mode_t mask)
{
	process_t *p = task_current();
	mode_t old = p->umask;
	p->umask = mask & 0777;
	return (long)old;
}

/* ── uname ───────────────────────────────────────────────── */

typedef struct {
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
	char domainname[65];
} linux_utsname_t;

static char ranix_hostname[65] = "ranix";

static long sys_uname(linux_utsname_t *uts)
{
	if (!uts) return -14;
	linux_utsname_t k;
	memset(&k, 0, sizeof(k));
	memcpy(k.sysname,    "Ranix",   6);
	memcpy(k.nodename,   ranix_hostname, sizeof(ranix_hostname));
	memcpy(k.release,    "1.0.0",   6);
	memcpy(k.version,    "#1 SMP",  7);
#if defined(__x86_64__)
	memcpy(k.machine,    "x86_64",  7);
#else
	memcpy(k.machine,    "aarch64", 8);
#endif
	memcpy(k.domainname, "(none)",  7);
	return copy_to_user(uts, &k, sizeof(k));
}

/* ── time syscalls ───────────────────────────────────────── */

typedef struct { int64_t tv_sec; int64_t tv_nsec; } linux_timespec_t;
typedef struct { int64_t tv_sec; int64_t tv_usec; } linux_timeval_t;

/* ── futex ───────────────────────────────────────────────────── */

#define FUTEX_WAIT         0
#define FUTEX_WAKE         1
#define FUTEX_PRIVATE_FLAG 128

static struct {
	uint32_t  *uaddr;
	process_t *waiter;
} futex_table[FUTEX_SLOTS];
static spinlock_t futex_lock = SPINLOCK_INIT;

static long sys_futex(uint32_t *uaddr, int op, uint32_t val,
                      linux_timespec_t *timeout, uint32_t *uaddr2, uint32_t val3)
{
	(void)uaddr2; (void)val3;
	int cmd = op & ~FUTEX_PRIVATE_FLAG;

	if (cmd == FUTEX_WAIT) {
		uint64_t f = spin_lock_irq(&futex_lock);
		uint32_t cur;
		if (copy_from_user(&cur, uaddr, sizeof(cur)) != 0) {
			spin_unlock_irq(&futex_lock, f);
			return -14;
		}
		if (cur != val) {
			spin_unlock_irq(&futex_lock, f);
			return -11; /* EAGAIN */
		}
		int slot = -1;
		for (int i = 0; i < FUTEX_SLOTS; i++) {
			if (!futex_table[i].waiter) { slot = i; break; }
		}
		if (slot < 0) { spin_unlock_irq(&futex_lock, f); return -11; }
		futex_table[slot].uaddr  = uaddr;
		futex_table[slot].waiter = task_current();
		spin_unlock_irq(&futex_lock, f);

		if (timeout) {
			linux_timespec_t kts;
			if (copy_from_user(&kts, timeout, sizeof(kts)) == 0) {
				uint64_t end = kernel_ticks +
				    (uint64_t)kts.tv_sec * TIMER_HZ +
				    (uint64_t)kts.tv_nsec / (1000000000 / TIMER_HZ);
				while (futex_table[slot].waiter && kernel_ticks < end)
					task_sleep();
			}
		} else {
			while (futex_table[slot].waiter)
				task_sleep();
		}

		f = spin_lock_irq(&futex_lock);
		futex_table[slot].waiter = NULL;
		futex_table[slot].uaddr  = NULL;
		spin_unlock_irq(&futex_lock, f);
		return 0;
	}

	if (cmd == FUTEX_WAKE) {
		uint64_t f = spin_lock_irq(&futex_lock);
		int woken = 0;
		for (int i = 0; i < FUTEX_SLOTS && woken < (int)val; i++) {
			if (futex_table[i].uaddr == uaddr && futex_table[i].waiter) {
				task_wake(futex_table[i].waiter);
				futex_table[i].waiter = NULL;
				futex_table[i].uaddr  = NULL;
				woken++;
			}
		}
		spin_unlock_irq(&futex_lock, f);
		return woken;
	}

	return -38;
}

static long sys_clock_gettime(int clkid, linux_timespec_t *tp)
{
	(void)clkid;
	if (!tp) return -14;
	linux_timespec_t k;
	k.tv_sec  = (int64_t)(kernel_ticks / TIMER_HZ);
	k.tv_nsec = (int64_t)((kernel_ticks % TIMER_HZ) * (1000000000 / TIMER_HZ));
	return copy_to_user(tp, &k, sizeof(k));
}

static long sys_gettimeofday(linux_timeval_t *tv, void *tz)
{
	(void)tz;
	if (!tv) return 0;
	linux_timeval_t k;
	k.tv_sec  = (int64_t)(kernel_ticks / TIMER_HZ);
	k.tv_usec = (int64_t)((kernel_ticks % TIMER_HZ) * (1000000 / TIMER_HZ));
	return copy_to_user(tv, &k, sizeof(k));
}

static long sys_nanosleep(linux_timespec_t *req, linux_timespec_t *rem)
{
	(void)rem;
	if (!req) return -14;
	linux_timespec_t k;
	if (copy_from_user(&k, req, sizeof(k)) != 0) return -14;
	uint64_t ticks = (uint64_t)k.tv_sec * TIMER_HZ +
	                 (uint64_t)k.tv_nsec / (1000000000 / TIMER_HZ);
	uint64_t end = kernel_ticks + (ticks ? ticks : 1);
	process_t *cp = task_current();
	while (kernel_ticks < end) {
		task_sleep();
		if (cp->sig_pending & ~cp->sig_mask) return -4; /* EINTR */
	}
	return 0;
}

typedef struct { int64_t tv_sec; int64_t tv_usec; } itimerval_tv_t;
typedef struct { itimerval_tv_t it_interval; itimerval_tv_t it_value; } itimerval_t;

static long sys_setitimer(int which, const itimerval_t *new_v, itimerval_t *old_v)
{
    if (which != 0 /* ITIMER_REAL */) return 0; /* ignore VIRTUAL/PROF */
    process_t *p = task_current();
    if (old_v) {
        itimerval_t oval;
        uint64_t hz = TIMER_HZ;
        oval.it_value.tv_sec    = (int64_t)(p->itimer_value / hz);
        oval.it_value.tv_usec   = (int64_t)((p->itimer_value % hz) * (1000000 / hz));
        oval.it_interval.tv_sec = (int64_t)(p->itimer_interval / hz);
        oval.it_interval.tv_usec= (int64_t)((p->itimer_interval % hz) * (1000000 / hz));
        copy_to_user(old_v, &oval, sizeof(oval));
    }
    if (new_v) {
        itimerval_t nval;
        if (copy_from_user(&nval, new_v, sizeof(nval)) != 0) return -14;
        uint64_t hz = TIMER_HZ;
        p->itimer_value    = (uint64_t)nval.it_value.tv_sec * hz +
                             (uint64_t)nval.it_value.tv_usec * hz / 1000000;
        p->itimer_interval = (uint64_t)nval.it_interval.tv_sec * hz +
                             (uint64_t)nval.it_interval.tv_usec * hz / 1000000;
    }
    return 0;
}

static long sys_getitimer(int which, itimerval_t *val)
{
    return sys_setitimer(which, NULL, val);
}

static long sys_times(void *buf)
{
	if (buf) {
		int64_t zeros[4] = {0};
		copy_to_user(buf, zeros, sizeof(zeros));
	}
	return (long)kernel_ticks;
}

static long sys_sysinfo(void *info)
{
	if (!info) return 0;
	/* struct sysinfo layout (Linux ABI) */
	struct {
		int64_t  uptime;
		uint64_t loads[3];
		uint64_t totalram;
		uint64_t freeram;
		uint64_t sharedram;
		uint64_t bufferram;
		uint64_t totalswap;
		uint64_t freeswap;
		uint16_t procs;
		uint8_t  _pad[22];
		uint32_t mem_unit;
	} si = {0};
	si.uptime    = (int64_t)(kernel_ticks / TIMER_HZ);
	si.totalram  = buddy_total_pages() * PAGE_SIZE;
	si.freeram   = buddy_free_pages()  * PAGE_SIZE;
	si.procs     = (uint16_t)task_count();
	si.mem_unit  = 1;
	copy_to_user(info, &si, sizeof(si));
	return 0;
}

/* ── ioctl ───────────────────────────────────────────────── */

#define TIOCGWINSZ  0x5413
#define TCGETS      0x5401
#define TIOCSPROMPT 0x54FF  /* Ranix: set TTY prompt string */
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCSPGRP  0x5410
#define TIOCGPGRP  0x540F

static pid_t tty_fg_pgrp = 1;

typedef struct { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; } linux_winsize_t;

static long sys_ioctl(int fd, uint32_t cmd, uint64_t arg)
{
	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (f) {
		if (f->inode && f->inode->fops && f->inode->fops->ioctl)
			return f->inode->fops->ioctl(f, cmd, arg);
		return -25; /* fd exists but has no ioctl (pipe, regular file…) */
	}
	/* fd not in table — fallback for stdin/stdout before /dev/console open */
	switch (cmd) {
	case TIOCGWINSZ: {
		linux_winsize_t ws = {24, 80, 0, 0};
		return copy_to_user((void *)(uintptr_t)arg, &ws, sizeof(ws));
	}
	case TCGETS: {
		uint8_t termios[36]; memset(termios, 0, 36);
		return copy_to_user((void *)(uintptr_t)arg, termios, 36);
	}
	case TCSETS: case TCSETSW: case TCSETSF: return 0;
	case TIOCGPGRP: { int pgid = (int)tty_fg_pgrp; return copy_to_user((void *)(uintptr_t)arg, &pgid, 4); }
	case TIOCSPGRP: { int pgid; if (copy_from_user(&pgid, (void *)(uintptr_t)arg, 4) != 0) return -14; tty_fg_pgrp = pgid; return 0; }
	case TIOCSPROMPT: {
		return 0;
	}
	default: return -25; /* ENOTTY */
	}
}

/* ── poll / select ───────────────────────────────────────── */

#define POLLIN  0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020

typedef struct { int fd; short events; short revents; } linux_pollfd_t;

static int fd_poll_bits(int fd)
{
	if (fd == 0) {
		process_t  *p = task_current();
		vfs_file_t *f = p->fds ? fd_get(p->fds, 0) : NULL;
		if (f) {
			if (f->inode->fops && f->inode->fops->poll)
				return f->inode->fops->poll(f);
			return 0x0005; /* POLLIN|POLLOUT */
		}
		return uart_has_data() ? 0x0001 : 0;
	}
	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return 0;
	if (f->inode->fops && f->inode->fops->poll)
		return f->inode->fops->poll(f);
	return 0x0005; /* regular file: always readable+writable */
}

static int fd_is_readable(int fd) { return fd_poll_bits(fd) & 0x0001; }
static int fd_is_writable(int fd) { return fd_poll_bits(fd) & 0x0004; }

static long sys_poll(linux_pollfd_t *fds, uint64_t nfds, int timeout_ms)
{
	if (!fds || nfds == 0) return 0;
	if (!access_ok(fds, nfds * sizeof(linux_pollfd_t))) return -14;

	linux_pollfd_t *kfds = kmalloc(nfds * sizeof(linux_pollfd_t));
	if (!kfds) return -12;
	if (copy_from_user(kfds, fds, nfds * sizeof(linux_pollfd_t)) != 0) {
		kfree(kfds); return -14;
	}

	uint64_t deadline = (timeout_ms < 0) ? (uint64_t)-1
	                  : (timeout_ms == 0) ? 0
	                  : kernel_ticks + (uint64_t)timeout_ms * TIMER_HZ / 1000 + 1;
	int ready = 0;
	while (1) {
		ready = 0;
		for (uint64_t i = 0; i < nfds; i++) {
			kfds[i].revents = 0;
			if (kfds[i].fd < 0) continue;
			int bits = fd_poll_bits(kfds[i].fd);
			if ((kfds[i].events & POLLIN)  && (bits & POLLIN))  kfds[i].revents |= POLLIN;
			if ((kfds[i].events & POLLOUT) && (bits & POLLOUT)) kfds[i].revents |= POLLOUT;
			if (kfds[i].revents) ready++;
		}
		if (ready > 0 || kernel_ticks >= deadline) break;
		task_sleep();
	}

	copy_to_user(fds, kfds, nfds * sizeof(linux_pollfd_t));
	kfree(kfds);
	return ready;
}

static long sys_select(int nfds, uint64_t *readfds, uint64_t *writefds,
                       uint64_t *exceptfds, linux_timeval_t *timeout)
{
	(void)exceptfds;
	if (nfds <= 0) return 0;

	uint64_t rfds = 0, wfds = 0;
	if (readfds  && copy_from_user(&rfds, readfds,  8) != 0) return -14;
	if (writefds && copy_from_user(&wfds, writefds, 8) != 0) return -14;

	uint64_t deadline = (uint64_t)-1;
	if (timeout) {
		linux_timeval_t tv;
		if (copy_from_user(&tv, timeout, sizeof(tv)) == 0 && (tv.tv_sec || tv.tv_usec))
			deadline = kernel_ticks + (uint64_t)tv.tv_sec * TIMER_HZ +
			           (uint64_t)tv.tv_usec * TIMER_HZ / 1000000 + 1;
		else if (timeout)
			deadline = 0;
	}

	int ready = 0;
	uint64_t rres = 0, wres = 0;
	while (1) {
		rres = 0; wres = 0; ready = 0;
		for (int i = 0; i < nfds && i < 64; i++) {
			if ((rfds & (1ULL << i)) && fd_is_readable(i)) { rres |= (1ULL << i); ready++; }
			if ((wfds & (1ULL << i)) && fd_is_writable(i)) { wres |= (1ULL << i); ready++; }
		}
		if (ready > 0 || kernel_ticks >= deadline) break;
		task_sleep();
	}

	if (readfds)  copy_to_user(readfds,  &rres, 8);
	if (writefds) copy_to_user(writefds, &wres, 8);
	return ready;
}

/* pselect6 / ppoll */
static long sys_pselect6(int nfds, uint64_t *r, uint64_t *w, uint64_t *e,
                         linux_timespec_t *ts, void *sigmask)
{
	(void)sigmask;
	linux_timeval_t tv = {0};
	linux_timeval_t *tvp = NULL;
	if (ts) {
		linux_timespec_t k;
		if (copy_from_user(&k, ts, sizeof(k)) == 0) {
			tv.tv_sec = k.tv_sec; tv.tv_usec = k.tv_nsec / 1000;
			tvp = &tv;
		}
	}
	return sys_select(nfds, r, w, e, tvp);
}

static long sys_ppoll(linux_pollfd_t *fds, uint64_t nfds,
                      linux_timespec_t *ts, void *sigmask)
{
	(void)sigmask;
	int timeout_ms = -1;
	if (ts) {
		linux_timespec_t k;
		if (copy_from_user(&k, ts, sizeof(k)) == 0)
			timeout_ms = (int)(k.tv_sec * 1000 + k.tv_nsec / 1000000);
	}
	return sys_poll(fds, nfds, timeout_ms);
}

/* ── epoll ───────────────────────────────────────────────── */

#define EPOLLIN    0x00000001
#define EPOLLOUT   0x00000004
#define EPOLLERR   0x00000008
#define EPOLLHUP   0x00000010
#define EPOLLRDHUP 0x00002000
#define EPOLLET    0x80000000
#define EPOLLONESHOT 0x40000000

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

typedef struct {
	uint32_t events;
	uint64_t data;
} __attribute__((packed)) linux_epoll_event_t;

#define EPOLL_MAX_WATCH 64

typedef struct {
	spinlock_t lock;
	int        fds[EPOLL_MAX_WATCH];
	uint32_t   ev[EPOLL_MAX_WATCH];
	uint64_t   data[EPOLL_MAX_WATCH];
} epoll_t;

static int epoll_close(vfs_file_t *file)
{
	epoll_t     *ep = (epoll_t *)file->inode->priv;
	vfs_inode_t *in = file->inode;
	kfree(ep);
	kfree(in);
	return 0;
}

static vfs_fops_t epoll_fops = { .close = epoll_close };

static epoll_t *epfd_get(int epfd)
{
	process_t  *p = task_current();
	vfs_file_t *f = p->fds ? fd_get(p->fds, epfd) : NULL;
	if (!f || !f->inode || f->inode->fops != &epoll_fops) return NULL;
	return (epoll_t *)f->inode->priv;
}

static long sys_epoll_create1(int flags)
{
	(void)flags;
	epoll_t *ep = kmalloc(sizeof(*ep));
	if (!ep) return -12;
	memset(ep, 0, sizeof(*ep));
	ep->lock = (spinlock_t)SPINLOCK_INIT;
	for (int i = 0; i < EPOLL_MAX_WATCH; i++) ep->fds[i] = -1;

	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) { kfree(ep); return -12; }
	memset(in, 0, sizeof(*in));
	in->fops     = &epoll_fops;
	in->priv     = ep;
	in->refcount = 1;

	vfs_file_t *f = kmalloc(sizeof(*f));
	if (!f) { kfree(ep); kfree(in); return -12; }
	memset(f, 0, sizeof(*f));
	f->inode    = in;
	f->refcount = 1;

	process_t *p = task_current();
	int fd = fd_alloc(p->fds, f);
	if (fd < 0) { kfree(f); kfree(ep); kfree(in); return -12; }
	return fd;
}

static long sys_epoll_ctl(int epfd, int op, int fd, linux_epoll_event_t *uevent)
{
	epoll_t *ep = epfd_get(epfd);
	if (!ep) return -9;

	linux_epoll_event_t ev = {0};
	if (op != EPOLL_CTL_DEL && uevent)
		copy_from_user(&ev, uevent, sizeof(ev));

	uint64_t f = spin_lock_irq(&ep->lock);

	if (op == EPOLL_CTL_ADD) {
		for (int i = 0; i < EPOLL_MAX_WATCH; i++) {
			if (ep->fds[i] == fd) { spin_unlock_irq(&ep->lock, f); return -17; }
		}
		for (int i = 0; i < EPOLL_MAX_WATCH; i++) {
			if (ep->fds[i] < 0) {
				ep->fds[i]  = fd;
				ep->ev[i]   = ev.events;
				ep->data[i] = ev.data;
				spin_unlock_irq(&ep->lock, f);
				return 0;
			}
		}
		spin_unlock_irq(&ep->lock, f);
		return -28;
	}

	for (int i = 0; i < EPOLL_MAX_WATCH; i++) {
		if (ep->fds[i] != fd) continue;
		if (op == EPOLL_CTL_DEL) {
			ep->fds[i] = -1;
		} else if (op == EPOLL_CTL_MOD) {
			ep->ev[i]   = ev.events;
			ep->data[i] = ev.data;
		}
		spin_unlock_irq(&ep->lock, f);
		return 0;
	}

	spin_unlock_irq(&ep->lock, f);
	return (op == EPOLL_CTL_MOD || op == EPOLL_CTL_DEL) ? -2 : -22;
}

static long sys_epoll_pwait(int epfd, linux_epoll_event_t *uevents, int maxevents,
                            int timeout_ms, void *sigmask)
{
	(void)sigmask;
	epoll_t *ep = epfd_get(epfd);
	if (!ep) return -9;
	if (!uevents || maxevents <= 0) return -22;
	if (!access_ok(uevents, (size_t)maxevents * sizeof(linux_epoll_event_t))) return -14;

	uint64_t deadline = (timeout_ms < 0) ? (uint64_t)-1
	                  : (timeout_ms == 0) ? 0
	                  : kernel_ticks + (uint64_t)timeout_ms * TIMER_HZ / 1000 + 1;

	linux_epoll_event_t *kev = kmalloc((size_t)maxevents * sizeof(*kev));
	if (!kev) return -12;

	int ready = 0;
	while (1) {
		ready = 0;
		uint64_t f = spin_lock_irq(&ep->lock);
		for (int i = 0; i < EPOLL_MAX_WATCH && ready < maxevents; i++) {
			if (ep->fds[i] < 0) continue;
			int bits = fd_poll_bits(ep->fds[i]);
			int r = 0;
			if ((ep->ev[i] & EPOLLIN)  && (bits & POLLIN))  r |= EPOLLIN;
			if ((ep->ev[i] & EPOLLOUT) && (bits & POLLOUT)) r |= EPOLLOUT;
			if (r) {
				kev[ready].events = (uint32_t)r;
				kev[ready].data   = ep->data[i];
				ready++;
			}
		}
		spin_unlock_irq(&ep->lock, f);
		if (ready > 0 || kernel_ticks >= deadline) break;
		task_sleep();
	}

	copy_to_user(uevents, kev, (size_t)ready * sizeof(*kev));
	kfree(kev);
	return ready;
}

/* ── symlink / readlink ───────────────────────────────────── */

static long sys_symlink(const char *t, const char *lp)
{
	char kt[512], klp[512];
	if (strncpy_from_user(kt,  t,  sizeof(kt))  < 0) return -14;
	if (strncpy_from_user(klp, lp, sizeof(klp)) < 0) return -14;
	return vfs_symlink(kt, klp);
}
static long sys_symlinkat(const char *t, int d, const char *lp)
{
	char kt[512];
	if (strncpy_from_user(kt, t, sizeof(kt)) < 0) return -14;
	char klp[512];
	int r = resolve_path(klp, d, lp);
	if (r) return r;
	return vfs_symlink(kt, klp);
}
static long sys_link(const char *o, const char *n) { (void)o; (void)n; return -1; }
static long sys_linkat(int od, const char *o, int nd, const char *n, int f)
{
	(void)od; (void)nd; (void)f; return sys_link(o, n);
}

/* ── chmod / chown (stubs) ───────────────────────────────── */

static long sys_chmod(const char *p, mode_t m)  { (void)p; (void)m; return 0; }
static long sys_fchmod(int fd, mode_t m)         { (void)fd; (void)m; return 0; }
static long sys_fchmodat(int d, const char *p, mode_t m, int f)
{ (void)d; (void)p; (void)m; (void)f; return 0; }
static long sys_chown(const char *p, uid_t u, gid_t g)  { (void)p; (void)u; (void)g; return 0; }
static long sys_fchown(int fd, uid_t u, gid_t g)         { (void)fd; (void)u; (void)g; return 0; }
static long sys_fchownat(int d, const char *p, uid_t u, gid_t g, int f)
{ (void)d; (void)p; (void)u; (void)g; (void)f; return 0; }

/* ── eventfd ─────────────────────────────────────────────── */

#define EFD_SEMAPHORE 1
#define EFD_NONBLOCK  O_NONBLOCK
#define EFD_CLOEXEC   O_CLOEXEC

typedef struct {
    uint64_t   count;
    int        flags;
    spinlock_t lock;
    wait_queue_t rd_wait;
    wait_queue_t wr_wait;
} eventfd_t;

static ssize_t eventfd_read_vfs(vfs_file_t *file, void *buf, size_t len)
{
    if (len < 8) return -22;
    eventfd_t *efd = (eventfd_t *)file->inode->priv;
    while (1) {
        uint64_t f = spin_lock_irq(&efd->lock);
        if (efd->count > 0) {
            uint64_t val;
            if (efd->flags & EFD_SEMAPHORE) {
                val = 1;
                efd->count--;
            } else {
                val = efd->count;
                efd->count = 0;
            }
            wake_up(&efd->wr_wait);
            spin_unlock_irq(&efd->lock, f);
            copy_to_user(buf, &val, 8);
            return 8;
        }
        if (file->flags & O_NONBLOCK) {
            spin_unlock_irq(&efd->lock, f);
            return -11;
        }
        sleep_on_locked(&efd->rd_wait, &efd->lock, f);
    }
}

static ssize_t eventfd_write_vfs(vfs_file_t *file, const void *buf, size_t len)
{
    if (len < 8) return -22;
    eventfd_t *efd = (eventfd_t *)file->inode->priv;
    uint64_t val = 0;
    if (copy_from_user(&val, buf, 8) != 0) return -14;
    if (val == (uint64_t)-1) return -22; /* EINVAL */
    while (1) {
        uint64_t f = spin_lock_irq(&efd->lock);
        if ((uint64_t)-2 - efd->count >= val) {
            efd->count += val;
            wake_up(&efd->rd_wait);
            spin_unlock_irq(&efd->lock, f);
            return 8;
        }
        if (file->flags & O_NONBLOCK) {
            spin_unlock_irq(&efd->lock, f);
            return -11;
        }
        sleep_on_locked(&efd->wr_wait, &efd->lock, f);
    }
}

static int eventfd_poll_vfs(vfs_file_t *file)
{
    eventfd_t *efd = (eventfd_t *)file->inode->priv;
    uint64_t f = spin_lock_irq(&efd->lock);
    uint64_t cnt = efd->count;
    spin_unlock_irq(&efd->lock, f);
    int r = 0;
    if (cnt > 0)              r |= 0x0001; /* POLLIN  */
    if (cnt < (uint64_t)-2)   r |= 0x0004; /* POLLOUT */
    return r;
}

static int eventfd_close_vfs(vfs_file_t *file)
{
    eventfd_t   *efd = (eventfd_t *)file->inode->priv;
    vfs_inode_t *in  = file->inode;
    kfree(efd);
    kfree(in);
    return 0;
}

static vfs_fops_t eventfd_fops = {
    .read  = eventfd_read_vfs,
    .write = eventfd_write_vfs,
    .poll  = eventfd_poll_vfs,
    .close = eventfd_close_vfs,
};

static long sys_eventfd2(unsigned int initval, int flags)
{
    eventfd_t *efd = kmalloc(sizeof(*efd));
    if (!efd) return -12;
    efd->count = initval;
    efd->flags = flags;
    efd->lock  = (spinlock_t)SPINLOCK_INIT;
    wait_queue_init(&efd->rd_wait);
    wait_queue_init(&efd->wr_wait);

    vfs_inode_t *in = kmalloc(sizeof(*in));
    if (!in) { kfree(efd); return -12; }
    memset(in, 0, sizeof(*in));
    in->fops     = &eventfd_fops;
    in->priv     = efd;
    in->refcount = 1;

    vfs_file_t *f = kmalloc(sizeof(*f));
    if (!f) { kfree(in); kfree(efd); return -12; }
    memset(f, 0, sizeof(*f));
    f->inode    = in;
    f->flags    = (flags & EFD_NONBLOCK) ? O_NONBLOCK : 0;
    f->refcount = 1;

    process_t *p = task_current();
    int fd = fd_alloc(p->fds, f);
    if (fd < 0) { kfree(f); kfree(in); kfree(efd); return -12; }
    return fd;
}

/* ── timerfd ─────────────────────────────────────────────── */

typedef struct {
    int        clockid;
    int        flags;
    uint64_t   interval_ns;  /* 0 = one-shot */
    uint64_t   expire_tick;  /* absolute kernel_ticks, 0 = disarmed */
    uint64_t   expirations;  /* accumulated un-read expirations */
    spinlock_t lock;
    wait_queue_t rd_wait;
} timerfd_t;

/* Called from sched_tick — check all timerfds */
void timerfd_tick(void);

static timerfd_t **timerfd_table;
static int         timerfd_cap;
static int         timerfd_cnt;
static spinlock_t  timerfd_lock = SPINLOCK_INIT;

static void timerfd_register(timerfd_t *t)
{
    uint64_t f = spin_lock_irq(&timerfd_lock);
    if (timerfd_cnt >= timerfd_cap) {
        int nc = timerfd_cap ? timerfd_cap * 2 : 16;
        timerfd_t **nb = kmalloc((size_t)nc * sizeof(timerfd_t *));
        if (nb) {
            memset(nb, 0, (size_t)nc * sizeof(timerfd_t *));
            if (timerfd_table) {
                memcpy(nb, timerfd_table,
                       (size_t)timerfd_cap * sizeof(timerfd_t *));
                kfree(timerfd_table);
            }
            timerfd_table = nb;
            timerfd_cap   = nc;
        }
    }
    for (int i = 0; i < timerfd_cap; i++) {
        if (!timerfd_table[i]) { timerfd_table[i] = t; timerfd_cnt++; break; }
    }
    spin_unlock_irq(&timerfd_lock, f);
}

static void timerfd_unregister(timerfd_t *t)
{
    uint64_t f = spin_lock_irq(&timerfd_lock);
    for (int i = 0; i < timerfd_cap; i++) {
        if (timerfd_table[i] == t) { timerfd_table[i] = NULL; timerfd_cnt--; break; }
    }
    spin_unlock_irq(&timerfd_lock, f);
}

void timerfd_tick(void)
{
    extern volatile uint64_t kernel_ticks;
    uint64_t f = spin_lock_irq(&timerfd_lock);
    for (int i = 0; i < timerfd_cap; i++) {
        timerfd_t *t = timerfd_table[i];
        if (!t || !t->expire_tick) continue;
        if (kernel_ticks < t->expire_tick) continue;

        uint64_t tf2 = spin_lock_irq(&t->lock);
        t->expirations++;
        if (t->interval_ns) {
            uint64_t ticks = (t->interval_ns * TIMER_HZ + 999999999UL) / 1000000000UL;
            if (!ticks) ticks = 1;
            t->expire_tick += ticks;
        } else {
            t->expire_tick = 0;
        }
        wake_up(&t->rd_wait);
        spin_unlock_irq(&t->lock, tf2);
    }
    spin_unlock_irq(&timerfd_lock, f);
}

static ssize_t timerfd_read_vfs(vfs_file_t *file, void *buf, size_t len)
{
    if (len < 8) return -22;
    timerfd_t *t = (timerfd_t *)file->inode->priv;
    while (1) {
        uint64_t f = spin_lock_irq(&t->lock);
        if (t->expirations > 0) {
            uint64_t val = t->expirations;
            t->expirations = 0;
            spin_unlock_irq(&t->lock, f);
            copy_to_user(buf, &val, 8);
            return 8;
        }
        if (file->flags & O_NONBLOCK) {
            spin_unlock_irq(&t->lock, f);
            return -11;
        }
        sleep_on_locked(&t->rd_wait, &t->lock, f);
    }
}

static int timerfd_poll_vfs(vfs_file_t *file)
{
    timerfd_t *t = (timerfd_t *)file->inode->priv;
    uint64_t f = spin_lock_irq(&t->lock);
    uint64_t exp = t->expirations;
    spin_unlock_irq(&t->lock, f);
    return exp ? 0x0001 : 0;
}

static int timerfd_close_vfs(vfs_file_t *file)
{
    timerfd_t   *t  = (timerfd_t *)file->inode->priv;
    vfs_inode_t *in = file->inode;
    timerfd_unregister(t);
    kfree(t);
    kfree(in);
    return 0;
}

static vfs_fops_t timerfd_fops = {
    .read  = timerfd_read_vfs,
    .poll  = timerfd_poll_vfs,
    .close = timerfd_close_vfs,
};

typedef struct { int32_t tv_sec; int32_t tv_nsec; } linux_itimerspec_old_t;
typedef struct { linux_timespec_t it_interval; linux_timespec_t it_value; } linux_itimerspec_t;

static long sys_timerfd_create(int clockid, int flags)
{
    timerfd_t *t = kmalloc(sizeof(*t));
    if (!t) return -12;
    memset(t, 0, sizeof(*t));
    t->clockid = clockid;
    t->flags   = flags;
    t->lock    = (spinlock_t)SPINLOCK_INIT;
    wait_queue_init(&t->rd_wait);

    vfs_inode_t *in = kmalloc(sizeof(*in));
    if (!in) { kfree(t); return -12; }
    memset(in, 0, sizeof(*in));
    in->fops     = &timerfd_fops;
    in->priv     = t;
    in->refcount = 1;

    vfs_file_t *f = kmalloc(sizeof(*f));
    if (!f) { kfree(in); kfree(t); return -12; }
    memset(f, 0, sizeof(*f));
    f->inode    = in;
    f->flags    = (flags & O_NONBLOCK) ? O_NONBLOCK : 0;
    f->refcount = 1;

    process_t *p = task_current();
    int fd = fd_alloc(p->fds, f);
    if (fd < 0) { kfree(f); kfree(in); kfree(t); return -12; }

    timerfd_register(t);
    return fd;
}

static timerfd_t *fd_to_timerfd(int fd)
{
    process_t  *p = task_current();
    vfs_file_t *f = fd_get(p->fds, fd);
    if (!f || !f->inode || f->inode->fops != &timerfd_fops) return NULL;
    return (timerfd_t *)f->inode->priv;
}

static long sys_timerfd_settime(int fd, int flags,
                                const linux_itimerspec_t *unew,
                                linux_itimerspec_t *uold)
{
    timerfd_t *t = fd_to_timerfd(fd);
    if (!t) return -9;
    linux_itimerspec_t kn;
    if (copy_from_user(&kn, unew, sizeof(kn)) != 0) return -14;

    extern volatile uint64_t kernel_ticks;

    uint64_t new_interval_ns = (uint64_t)kn.it_interval.tv_sec * 1000000000UL
                               + (uint64_t)kn.it_interval.tv_nsec;
    uint64_t new_value_ns    = (uint64_t)kn.it_value.tv_sec    * 1000000000UL
                               + (uint64_t)kn.it_value.tv_nsec;

    if (uold) {
        linux_itimerspec_t ko = {{0,0},{0,0}};
        /* return old value (approximate — we store absolute tick) */
        if (t->expire_tick > kernel_ticks) {
            uint64_t rem_ns = ((t->expire_tick - kernel_ticks) * 1000000000UL) / TIMER_HZ;
            ko.it_value.tv_sec  = (int64_t)(rem_ns / 1000000000UL);
            ko.it_value.tv_nsec = (int64_t)(rem_ns % 1000000000UL);
        }
        ko.it_interval.tv_sec  = (int64_t)(t->interval_ns / 1000000000UL);
        ko.it_interval.tv_nsec = (int64_t)(t->interval_ns % 1000000000UL);
        copy_to_user(uold, &ko, sizeof(ko));
    }

    uint64_t lf = spin_lock_irq(&t->lock);
    t->interval_ns = new_interval_ns;
    t->expirations = 0;
    if (new_value_ns == 0) {
        t->expire_tick = 0;  /* disarm */
    } else {
        uint64_t ticks = (new_value_ns * TIMER_HZ + 999999999UL) / 1000000000UL;
        if (!ticks) ticks = 1;
        if (flags & 1 /* TFD_TIMER_ABSTIME */) {
            /* absolute: convert from ns-since-boot to ticks */
            t->expire_tick = (new_value_ns * TIMER_HZ + 999999999UL) / 1000000000UL;
        } else {
            t->expire_tick = kernel_ticks + ticks;
        }
    }
    spin_unlock_irq(&t->lock, lf);
    return 0;
}

static long sys_timerfd_gettime(int fd, linux_itimerspec_t *ucur)
{
    timerfd_t *t = fd_to_timerfd(fd);
    if (!t) return -9;
    extern volatile uint64_t kernel_ticks;

    linux_itimerspec_t ko = {{0,0},{0,0}};
    uint64_t lf = spin_lock_irq(&t->lock);
    if (t->expire_tick > kernel_ticks) {
        uint64_t rem_ns = ((t->expire_tick - kernel_ticks) * 1000000000UL) / TIMER_HZ;
        ko.it_value.tv_sec  = (int64_t)(rem_ns / 1000000000UL);
        ko.it_value.tv_nsec = (int64_t)(rem_ns % 1000000000UL);
    }
    ko.it_interval.tv_sec  = (int64_t)(t->interval_ns / 1000000000UL);
    ko.it_interval.tv_nsec = (int64_t)(t->interval_ns % 1000000000UL);
    spin_unlock_irq(&t->lock, lf);
    copy_to_user(ucur, &ko, sizeof(ko));
    return 0;
}

/* ── memfd_create ────────────────────────────────────────── */

static long sys_memfd_create(const char *uname, unsigned int flags)
{
    (void)uname; (void)flags;
    /* Create an anonymous tmpfs file backed by ramfs */
    /* Use ramfs to allocate an inode; return an fd */
    extern vfs_inode_t *ramfs_create_anon(void);
    vfs_inode_t *in = ramfs_create_anon();
    if (!in) return -12;

    vfs_file_t *f = kmalloc(sizeof(*f));
    if (!f) { /* can't easily free ramfs inode here; leak minimal */ return -12; }
    memset(f, 0, sizeof(*f));
    f->inode    = in;
    f->flags    = O_RDWR;
    f->refcount = 1;

    process_t *p = task_current();
    int fd = fd_alloc(p->fds, f);
    if (fd < 0) { kfree(f); return -12; }
    return fd;
}

/* ── signalfd (stub: returns fd that is never readable) ─── */

static long sys_signalfd4(int fd, const uint64_t *mask, size_t sigsetsize, int flags)
{
    (void)mask; (void)sigsetsize; (void)flags;
    /* If fd == -1, create a new signalfd (stub, never delivers events).
     * Many programs use signalfd only as a fallback; returning a valid fd
     * that never becomes readable is enough to not crash them. */
    if (fd != -1) return 0; /* modifying existing fd: no-op */
    /* Reuse eventfd infrastructure with count=0, non-writable from outside */
    return sys_eventfd2(0, flags & O_NONBLOCK);
}

/* ── socket syscalls ─────────────────────────────────────── */

static int sock_close_vfs(vfs_file_t *file)
{
    socket_t    *s  = (socket_t *)file->inode->priv;
    vfs_inode_t *in = file->inode;

    if (s->domain == AF_UNIX) {
        socket_t *peer = s->unix_peer;
        s->unix_peer = NULL;
        if (peer && peer->unix_peer == s) {
            peer->unix_peer = NULL;
            /* wake peer's blocked recv — empty queue + no peer = EOF */
            uint64_t f = spin_lock_irq(&peer->recv_lock);
            process_t *w = peer->recv_waiter;
            peer->recv_waiter = NULL;
            spin_unlock_irq(&peer->recv_lock, f);
            if (w) task_wake(w);
            /* wake peer's blocked send */
            process_t *sw = peer->connect_waiter;
            peer->connect_waiter = NULL;
            if (sw) task_wake(sw);
        }
        sock_free(s);
        kfree(in);
        return 0;
    }

    if (s->type == SOCK_STREAM &&
        s->tcp_state != TCP_CLOSED && s->tcp_state != TCP_LISTEN) {
        tcp_close(s);
        while (s->tcp_state != TCP_CLOSED &&
               s->tcp_state != TCP_TIME_WAIT) {
            s->close_waiter = task_current();
            task_sleep();
        }
    }
    sock_free(s);
    kfree(in);
    return 0;
}

static int sock_poll_vfs(vfs_file_t *file)
{
    socket_t *s = (socket_t *)file->inode->priv;
    if (!s) return 0;
    uint64_t f = spin_lock_irq(&s->recv_lock);
    int has_data = (s->recv_head != NULL);
    spin_unlock_irq(&s->recv_lock, f);
    int eof;
    if (s->domain == AF_UNIX)
        eof = (s->unix_peer == NULL);
    else
        eof = (s->type == SOCK_STREAM &&
               (s->tcp_state == TCP_CLOSE_WAIT || s->tcp_state == TCP_CLOSED ||
                s->tcp_state == TCP_LAST_ACK   || s->tcp_state == TCP_TIME_WAIT));
    return ((has_data || eof) ? 0x0001 : 0) | 0x0004;
}

static ssize_t sock_read_vfs(vfs_file_t *file, void *buf, size_t len)
{
    socket_t *s = (socket_t *)file->inode->priv;
    if (!s) return -9;
    /* blocking read: wait for data in recv queue */
    while (1) {
        uint64_t f = spin_lock_irq(&s->recv_lock);
        skb_t *skb = s->recv_head;
        if (skb) {
            s->recv_head = skb->next;
            if (!s->recv_head) s->recv_tail = NULL;
            spin_unlock_irq(&s->recv_lock, f);
            size_t n = skb->len < len ? skb->len : len;
            if (n) copy_to_user(buf, skb->data, n);
            skb_free(skb);
            return (ssize_t)(n ? n : 0);  /* 0 = EOF (FIN) */
        }
        /* EOF: TCP half-closed or AF_UNIX peer disconnected */
        int eof = (s->domain == AF_UNIX)
            ? (s->unix_peer == NULL)
            : (s->type == SOCK_STREAM &&
               (s->tcp_state == TCP_CLOSE_WAIT || s->tcp_state == TCP_CLOSED ||
                s->tcp_state == TCP_TIME_WAIT  || s->tcp_state == TCP_LAST_ACK));
        if (eof) {
            spin_unlock_irq(&s->recv_lock, f);
            return 0;
        }
        s->recv_waiter = task_current();
        spin_unlock_irq(&s->recv_lock, f);
        task_sleep();
        process_t *p = task_current();
        if (p->sig_pending & ~p->sig_mask) { s->recv_waiter = NULL; return -4; }
    }
}

static ssize_t sock_write_vfs(vfs_file_t *file, const void *buf, size_t len)
{
    socket_t *s = (socket_t *)file->inode->priv;
    if (!s) return -9;
    if (s->type == SOCK_STREAM || (s->domain == AF_UNIX && s->type == SOCK_DGRAM)) {
        uint8_t *kbuf = kmalloc(len);
        if (!kbuf) return -12;
        if (copy_from_user(kbuf, buf, len) != 0) { kfree(kbuf); return -14; }
        int rc;
        if (s->domain == AF_UNIX) {
            rc = unix_send(s, kbuf, (uint16_t)len);
        } else {
            while ((rc = tcp_send(s, kbuf, (uint16_t)len)) == -11) {
                s->connect_waiter = task_current();
                task_sleep();
                process_t *p = task_current();
                if (p->sig_pending & ~p->sig_mask) { s->connect_waiter = NULL; kfree(kbuf); return -4; }
            }
        }
        kfree(kbuf);
        return rc < 0 ? rc : (ssize_t)len;
    }
    return -38;
}

static vfs_fops_t sock_fops = {
    .read  = sock_read_vfs,
    .write = sock_write_vfs,
    .close = sock_close_vfs,
    .poll  = sock_poll_vfs,
};

static socket_t *fd_to_sock(int fd)
{
    process_t  *p = task_current();
    vfs_file_t *f = fd_get(p->fds, fd);
    if (!f || !f->inode || f->inode->fops != &sock_fops) return NULL;
    return (socket_t *)f->inode->priv;
}

static int sock_to_fd(socket_t *s)
{
    vfs_inode_t *in = kmalloc(sizeof(*in));
    if (!in) return -12;
    memset(in, 0, sizeof(*in));
    in->fops     = &sock_fops;
    in->priv     = s;
    in->refcount = 1;

    vfs_file_t *f = kmalloc(sizeof(*f));
    if (!f) { kfree(in); return -12; }
    memset(f, 0, sizeof(*f));
    f->inode    = in;
    f->refcount = 1;

    process_t *p = task_current();
    int fd = fd_alloc(p->fds, f);
    if (fd < 0) { kfree(f); kfree(in); return -12; }
    return fd;
}

static long sys_socket(int domain, int type, int proto)
{
    if (domain != AF_INET && domain != AF_UNIX) return -97;
    if (type != SOCK_DGRAM && type != SOCK_STREAM && type != SOCK_RAW) return -22;
    socket_t *s = sock_alloc(domain, type, proto);
    if (!s) return -12;
    s->dev = net_dev(0);
    if (type == SOCK_RAW) sock_register(s);
    int fd = sock_to_fd(s);
    if (fd < 0) { sock_free(s); return fd; }
    return fd;
}

static long sys_bind(int fd, const void *uaddr, uint32_t addrlen)
{
    (void)addrlen;
    socket_t *s = fd_to_sock(fd);
    if (!s) return -9;
    sockaddr_in_t sa;
    if (copy_from_user(&sa, uaddr, sizeof(sa)) != 0) return -14;
    s->local_ip   = ntohl(sa.sin_addr);
    s->local_port = ntohs(sa.sin_port);
    sock_register(s);
    return 0;
}

static long sys_connect(int fd, const void *uaddr, uint32_t addrlen)
{
    (void)addrlen;
    socket_t *s = fd_to_sock(fd);
    if (!s) return -9;
    sockaddr_in_t sa;
    if (copy_from_user(&sa, uaddr, sizeof(sa)) != 0) return -14;
    s->remote_ip   = ntohl(sa.sin_addr);
    s->remote_port = ntohs(sa.sin_port);
    if (s->type == SOCK_DGRAM) return 0;
    if (s->type != SOCK_STREAM) return -38;

    int rc = tcp_connect(s);
    if (rc < 0) return rc;

    /* block until ESTABLISHED or RST */
    while (s->tcp_state == TCP_SYN_SENT) {
        s->connect_waiter = task_current();
        task_sleep();
        process_t *p = task_current();
        if (p->sig_pending & ~p->sig_mask) { s->connect_waiter = NULL; return -4; }
    }
    return (s->tcp_state == TCP_ESTABLISHED) ? 0 : -111; /* ECONNREFUSED */
}

static long sys_sendto(int fd, const void *buf, size_t len, int flags,
                       const void *uaddr, uint32_t addrlen)
{
    (void)flags; (void)addrlen;
    socket_t *s = fd_to_sock(fd);
    if (!s || !s->dev) return -9;

    uint32_t dst_ip; uint16_t dst_port;
    if (uaddr) {
        sockaddr_in_t sa;
        if (copy_from_user(&sa, uaddr, sizeof(sa)) != 0) return -14;
        dst_ip   = ntohl(sa.sin_addr);
        dst_port = ntohs(sa.sin_port);
    } else {
        dst_ip = s->remote_ip; dst_port = s->remote_port;
    }

    if (len > SKB_DATA_MAX) len = SKB_DATA_MAX;
    uint8_t *kbuf = kmalloc(len);
    if (!kbuf) return -12;
    if (copy_from_user(kbuf, buf, len) != 0) { kfree(kbuf); return -14; }

    long r = -38;
    if (s->type == SOCK_STREAM) {
        /* TCP: ignore dst from uaddr, use connected remote */
        int rc;
        while ((rc = tcp_send(s, kbuf, (uint16_t)len)) == -11) {
            s->connect_waiter = task_current();
            task_sleep();
            process_t *p = task_current();
            if (p->sig_pending & ~p->sig_mask) { s->connect_waiter = NULL; kfree(kbuf); return -4; }
        }
        r = rc < 0 ? rc : (long)len;
    } else if (s->type == SOCK_DGRAM) {
        int rc = udp_tx(s->dev, dst_ip, s->local_port, dst_port, kbuf, (uint16_t)len);
        r = rc < 0 ? rc : (long)len;
    } else if (s->type == SOCK_RAW && s->proto == 1 /* IPPROTO_ICMP */) {
        if (len >= sizeof(icmp_hdr_t)) {
            icmp_hdr_t *ih = (icmp_hdr_t *)kbuf;
            ih->checksum = 0;
            ih->checksum = inet_cksum(kbuf, len);
            /* retry loop: first attempt sends ARP request; subsequent ones hit cached MAC */
            for (int attempt = 0; attempt < 64; attempt++) {
                skb_t *skb = skb_alloc();
                if (!skb) { r = -12; break; }
                memcpy(skb->data, kbuf, len);
                skb->len = (uint16_t)len;
                int rc = ip_tx(s->dev, skb, dst_ip, 1 /* IPPROTO_ICMP */);
                if (rc >= 0) { r = (long)len; break; }
                if (rc != -1) { r = rc; break; }
                net_poll();  /* drain RX — ARP reply may arrive */
            }
        }
    }
    kfree(kbuf);
    return r;
}

static long sys_recvfrom(int fd, void *buf, size_t len, int flags,
                         void *uaddr, uint32_t *addrlen)
{
    (void)addrlen;
#define MSG_DONTWAIT 0x40
    int nonblock = (flags & MSG_DONTWAIT);
    socket_t *s = fd_to_sock(fd);
    if (!s) return -9;

    while (1) {
        uint64_t f = spin_lock_irq(&s->recv_lock);
        skb_t *skb = s->recv_head;
        if (skb) {
            s->recv_head = skb->next;
            if (!s->recv_head) s->recv_tail = NULL;
            spin_unlock_irq(&s->recv_lock, f);
            size_t n = skb->len < len ? skb->len : len;
            copy_to_user(buf, skb->data, n);
            if (uaddr) {
                sockaddr_in_t sa;
                memset(&sa, 0, sizeof(sa));
                sa.sin_family = AF_INET;
                sa.sin_port   = htons(s->remote_port);
                if (s->type == SOCK_RAW && skb->len >= 20) {
                    /* extract src IP from the prepended IP header (network byte order) */
                    uint32_t src_net;
                    memcpy(&src_net, skb->data + 12, 4);
                    sa.sin_addr = src_net;
                } else {
                    sa.sin_addr = htonl(s->remote_ip);
                }
                copy_to_user(uaddr, &sa, sizeof(sa));
            }
            skb_free(skb);
            return (long)n;
        }
        if (nonblock) { spin_unlock_irq(&s->recv_lock, f); return -11; /* EAGAIN */ }
        s->recv_waiter = task_current();
        spin_unlock_irq(&s->recv_lock, f);
        task_sleep();
        process_t *p = task_current();
        if (p->sig_pending & ~p->sig_mask) { s->recv_waiter = NULL; return -4; }
    }
}

static long sys_getsockname(int fd, void *uaddr, uint32_t *uaddrlen)
{
    socket_t *s = fd_to_sock(fd);
    if (!s) return -9;
    sockaddr_in_t sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(s->local_port);
    sa.sin_addr   = htonl(s->local_ip ? s->local_ip : (s->dev ? s->dev->ip : 0));
    copy_to_user(uaddr, &sa, sizeof(sa));
    if (uaddrlen) { uint32_t l = sizeof(sa); copy_to_user(uaddrlen, &l, 4); }
    return 0;
}

static long sys_getpeername(int fd, void *uaddr, uint32_t *addrlen)
{
    socket_t *s = fd_to_sock(fd);
    if (!s) return -9;
    sockaddr_in_t sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(s->remote_port);
    sa.sin_addr   = htonl(s->remote_ip);
    copy_to_user(uaddr, &sa, sizeof(sa));
    if (addrlen) { uint32_t l = sizeof(sa); copy_to_user(addrlen, &l, 4); }
    return 0;
}

static long sys_setsockopt(int fd, int lv, int opt, const void *val, uint32_t len)
{ (void)fd; (void)lv; (void)opt; (void)val; (void)len; return 0; }

static long sys_getsockopt(int fd, int lv, int opt, void *val, uint32_t *len)
{ (void)fd; (void)lv; (void)opt; (void)val; (void)len; return -38; }

static long sys_listen(int fd, int bl)
{
    socket_t *s = fd_to_sock(fd);
    if (!s) return -9;
    if (s->type != SOCK_STREAM) return -95; /* EOPNOTSUPP */
    return tcp_listen(s, bl);
}

static long sys_accept4(int fd, void *ua, uint32_t *al, int fl)
{
    (void)fl;
    socket_t *ls = fd_to_sock(fd);
    if (!ls) return -9;
    if (ls->tcp_state != TCP_LISTEN) return -22;

    /* wait for a completed connection */
    while (!ls->accept_head) {
        ls->accept_waiter = task_current();
        task_sleep();
        process_t *p = task_current();
        if (p->sig_pending & ~p->sig_mask) { ls->accept_waiter = NULL; return -4; }
    }

    /* dequeue from accept list (accept_head chained via accept_head field of child) */
    socket_t *ns = ls->accept_head;
    ls->accept_head = ns->accept_head;
    if (!ls->accept_head) ls->accept_tail = NULL;
    ns->accept_head = NULL;

    /* fill peer addr if requested */
    if (ua) {
        sockaddr_in_t sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port   = htons(ns->remote_port);
        sa.sin_addr   = htonl(ns->remote_ip);
        copy_to_user(ua, &sa, sizeof(sa));
        if (al) { uint32_t l = sizeof(sa); copy_to_user(al, &l, 4); }
    }

    int nfd = sock_to_fd(ns);
    if (nfd < 0) { sock_free(ns); return nfd; }
    return nfd;
}

static long sys_shutdown(int fd, int how)
{
    (void)how;
    socket_t *s = fd_to_sock(fd);
    if (s && s->type == SOCK_STREAM) tcp_close(s);
    return 0;
}

/* ── readlinkat via VFS ──────────────────────────────────── */

static long sys_readlinkat_vfs(int dirfd, const char *upath, char *ubuf, size_t bufsiz)
{
    char kp[256];
    int r = resolve_path(kp, dirfd, upath);
    if (r) return r;

    /* Split into parent dir and filename — look up last component without following */
    char parent_path[256];
    const char *slash = kp + strlen(kp) - 1;
    while (slash > kp && *slash != '/') slash--;
    int plen = (int)(slash - kp);
    if (plen == 0) { parent_path[0] = '/'; parent_path[1] = '\0'; }
    else           { memcpy(parent_path, kp, plen); parent_path[plen] = '\0'; }
    const char *fname = slash + 1;

    vfs_dentry_t *parent = vfs_dentry_resolve(parent_path);
    if (!parent || !parent->inode) return -2;
    if (!parent->inode->iops || !parent->inode->iops->lookup) return -2;

    vfs_inode_t *in = parent->inode->iops->lookup(parent->inode, fname);
    if (!in) return -2;
    if (!in->iops || !in->iops->readlink) {
        if (!in->cached) kfree(in);
        return -22;
    }

    char kbuf[256];
    int n = in->iops->readlink(in, kbuf, sizeof(kbuf));
    if (!in->cached) kfree(in);
    if (n < 0) return n;
    size_t slen = strlen(kbuf);
    if (slen > bufsiz) slen = bufsiz;
    copy_to_user(ubuf, kbuf, slen);
    return (long)slen;
}

/* ── batch: missing syscalls ─────────────────────────────── */

static long sys_pread64(int fd, void *buf, size_t count, off_t offset)
{
	if (!access_ok(buf, count)) return -14;
	off_t saved = vfs_seek(fd, 0, 1);
	if (saved < 0) return -9;
	vfs_seek(fd, offset, 0);
	long n = vfs_read(fd, buf, count);
	vfs_seek(fd, saved, 0);
	return n;
}

static long sys_pwrite64(int fd, const void *buf, size_t count, off_t offset)
{
	if (!access_ok(buf, count)) return -14;
	off_t saved = vfs_seek(fd, 0, 1);
	if (saved < 0) return -9;
	vfs_seek(fd, offset, 0);
	long n = vfs_write(fd, buf, count);
	vfs_seek(fd, saved, 0);
	return n;
}

static long sys_mremap(uint64_t old_addr, size_t old_size, size_t new_size,
                       int flags, uint64_t new_addr)
{
	(void)flags; (void)new_addr;
	if (new_size == 0) return -22;
	if (new_size <= old_size) return (long)old_addr;
	/* growing: allocate new, copy, unmap old */
	long na = sys_mmap(0, new_size, 3 /*RW*/, 0x22 /*MAP_ANON|MAP_PRIVATE*/, -1, 0);
	if (na < 0) return -12;
	memcpy((void *)na, (void *)old_addr, old_size);
	sys_munmap(old_addr, old_size);
	return na;
}

static long sys_pause(void)
{
	task_sleep();
	return -4; /* EINTR */
}

static long sys_alarm(unsigned int seconds)
{
	process_t *p = task_current();
	unsigned int remaining = (unsigned int)(p->itimer_value / TIMER_HZ);
	p->itimer_value    = (uint64_t)seconds * TIMER_HZ;
	p->itimer_interval = 0;
	return (long)remaining;
}

static long sys_socketpair(int domain, int type, int protocol, int *sv)
{
    (void)protocol;
    if (domain != AF_UNIX) return -97;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -22;

    socket_t *s0 = sock_alloc(AF_UNIX, type, 0);
    socket_t *s1 = sock_alloc(AF_UNIX, type, 0);
    if (!s0 || !s1) { sock_free(s0); sock_free(s1); return -12; }

    s0->unix_peer = s1;
    s1->unix_peer = s0;
    s0->tcp_state = TCP_ESTABLISHED;
    s1->tcp_state = TCP_ESTABLISHED;

    int fd0 = sock_to_fd(s0);
    if (fd0 < 0) { sock_free(s0); sock_free(s1); return fd0; }
    int fd1 = sock_to_fd(s1);
    if (fd1 < 0) {
        /* fd0 already allocated; free s1, let fd0 cleanup handle s0 */
        sock_free(s1); s0->unix_peer = NULL;
        return fd1;
    }

    int kbuf[2] = { fd0, fd1 };
    if (copy_to_user(sv, kbuf, sizeof(kbuf)) != 0) return -14;
    return 0;
}

static long sys_flock(int fd, int operation)
{
	(void)fd; (void)operation;
	return 0;
}

static long sys_fsync(int fd)    { (void)fd; return 0; }
static long sys_fdatasync(int fd){ (void)fd; return 0; }

static long sys_fchdir(int fd)
{
	process_t *p = task_current();
	if (!p->fds) return -9;
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return -9;
	if (!f->inode || (f->inode->mode & 0xF000) != 0x4000) return -20; /* ENOTDIR */
	memcpy(p->cwd, f->path, sizeof(p->cwd));
	p->cwd[sizeof(p->cwd) - 1] = '\0';
	return 0;
}

static long sys_creat(const char *path, mode_t mode)
{
	return sys_open(path, 0101 /* O_CREAT|O_WRONLY */ | 01000 /* O_TRUNC */, mode);
}

static long sys_lchown(const char *path, uid_t uid, gid_t gid)
{
	return sys_chown(path, uid, gid);
}

static long sys_getrusage(int who, void *usage)
{
	(void)who;
	if (!access_ok(usage, 144)) return -14;
	memset(usage, 0, 144); /* struct rusage is 144 bytes */
	return 0;
}

static long sys_setreuid(uid_t ruid, uid_t euid)
{
	process_t *p = task_current();
	if (ruid != (uid_t)-1) p->uid = ruid;
	if (euid != (uid_t)-1) p->uid = euid;
	return 0;
}

static long sys_setregid(gid_t rgid, gid_t egid)
{
	process_t *p = task_current();
	if (rgid != (gid_t)-1) p->gid = rgid;
	if (egid != (gid_t)-1) p->gid = egid;
	return 0;
}

static long sys_setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
	process_t *p = task_current();
	if (ruid != (uid_t)-1) p->uid = ruid;
	if (euid != (uid_t)-1) p->uid = euid;
	(void)suid;
	return 0;
}

static long sys_getresuid(uid_t *ruid, uid_t *euid, uid_t *suid)
{
	uid_t u = task_current()->uid;
	if (copy_to_user(ruid, &u, sizeof(u))) return -14;
	if (copy_to_user(euid, &u, sizeof(u))) return -14;
	if (copy_to_user(suid, &u, sizeof(u))) return -14;
	return 0;
}

static long sys_setresgid(gid_t rgid, gid_t egid, gid_t sgid)
{
	process_t *p = task_current();
	if (rgid != (gid_t)-1) p->gid = rgid;
	if (egid != (gid_t)-1) p->gid = egid;
	(void)sgid;
	return 0;
}

static long sys_getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid)
{
	gid_t g = task_current()->gid;
	if (copy_to_user(rgid, &g, sizeof(g))) return -14;
	if (copy_to_user(egid, &g, sizeof(g))) return -14;
	if (copy_to_user(sgid, &g, sizeof(g))) return -14;
	return 0;
}

static long sys_rt_sigpending(uint64_t *uset, size_t sigsetsize)
{
	(void)sigsetsize;
	if (!access_ok(uset, sizeof(uint64_t))) return -14;
	uint64_t pending = task_current()->sig_pending;
	return copy_to_user(uset, &pending, sizeof(pending)) == 0 ? 0 : -14;
}

static long sys_rt_sigsuspend(const uint64_t *unewmask, size_t sigsetsize)
{
	(void)sigsetsize;
	uint64_t newmask;
	if (copy_from_user(&newmask, unewmask, sizeof(newmask)) != 0) return -14;
	process_t *p = task_current();
	uint64_t oldmask = p->sig_mask;
	p->sig_mask = newmask;
	task_sleep();
	p->sig_mask = oldmask;
	return -4; /* EINTR */
}

static long sys_mknod(const char *path, mode_t mode, uint64_t dev)
{
	(void)dev;
	if ((mode & 0xF000) == 0x8000 /* S_IFREG */)
		return sys_open(path, 0101 | 01000, mode & 0777);
	return -1; /* EPERM for device nodes */
}

static long sys_mknodat(int dirfd, const char *path, mode_t mode, uint64_t dev)
{
	char kpath[256];
	if (resolve_path(kpath, dirfd, path) != 0) return -2;
	return sys_mknod(kpath, mode, dev);
}

static long sys_mlock(uint64_t addr, size_t len)   { (void)addr; (void)len; return 0; }
static long sys_munlock(uint64_t addr, size_t len) { (void)addr; (void)len; return 0; }
static long sys_mlock2(uint64_t addr, size_t len, int flags)
	{ (void)addr; (void)len; (void)flags; return 0; }

static long sys_sync(void) { return 0; }

static long sys_mount(const char *source, const char *target, const char *fstype,
                      unsigned long flags, const void *data)
{
	(void)source; (void)target; (void)fstype; (void)flags; (void)data;
	return -1; /* EPERM — no userspace mount yet */
}

static long sys_sethostname(const char *name, size_t len)
{
	if (len >= sizeof(ranix_hostname)) return -22;
	if (!access_ok(name, len)) return -14;
	if (copy_from_user(ranix_hostname, name, len) != 0) return -14;
	ranix_hostname[len] = '\0';
	return 0;
}

static long sys_tkill(pid_t tid, int sig) { return sys_kill(tid, sig); }

static long sys_time_syscall(long *tloc)
{
	long t = (long)(kernel_ticks / TIMER_HZ);
	if (tloc && access_ok(tloc, sizeof(long)))
		copy_to_user(tloc, &t, sizeof(t));
	return t;
}

static long sys_clock_nanosleep(int clkid, int flags,
                                linux_timespec_t *req, linux_timespec_t *rem)
{
	(void)clkid; (void)flags;
	return sys_nanosleep(req, rem);
}

static long sys_waitid(int idtype, int id, void *infop, int options, void *rusage)
{
	(void)infop; (void)rusage;
	pid_t wpid = (idtype == 0) ? -1 : (idtype == 1) ? (pid_t)id : -(pid_t)id;
	int status;
	long r = sys_waitpid(wpid, &status, options);
	return (r < 0) ? r : 0;
}

static long sys_splice(int fd_in, off_t *off_in, int fd_out, off_t *off_out,
                       size_t len, unsigned int flags)
{
	(void)flags;
	char buf[4096];
	size_t done = 0;
	while (done < len) {
		size_t chunk = len - done;
		if (chunk > sizeof(buf)) chunk = sizeof(buf);
		if (off_in) {
			off_t o; copy_from_user(&o, off_in, sizeof(o));
			vfs_seek(fd_in, o, 0);
		}
		long r = vfs_read(fd_in, buf, chunk);
		if (r <= 0) { if (done == 0) return r; break; }
		if (off_in) {
			off_t o; copy_from_user(&o, off_in, sizeof(o));
			o += r; copy_to_user(off_in, &o, sizeof(o));
		}
		if (off_out) {
			off_t o; copy_from_user(&o, off_out, sizeof(o));
			vfs_seek(fd_out, o, 0);
		}
		long w = vfs_write(fd_out, buf, (size_t)r);
		if (w < 0) { if (done == 0) return w; break; }
		if (off_out) {
			off_t o; copy_from_user(&o, off_out, sizeof(o));
			o += w; copy_to_user(off_out, &o, sizeof(o));
		}
		done += (size_t)w;
	}
	return (long)done;
}

static long sys_fallocate(int fd, int mode, off_t offset, off_t len)
{
	(void)fd; (void)mode; (void)offset; (void)len;
	return 0;
}

static long sys_renameat2(int odfd, const char *op, int ndfd, const char *np, unsigned int flags)
{
	(void)flags;
	return sys_renameat(odfd, op, ndfd, np);
}

static long sys_copy_file_range(int fd_in, off_t *off_in, int fd_out, off_t *off_out,
                                size_t len, unsigned int flags)
{
	(void)off_in; (void)off_out; (void)flags;
	char buf[4096];
	size_t done = 0;
	while (done < len) {
		size_t chunk = len - done; if (chunk > sizeof(buf)) chunk = sizeof(buf);
		long r = vfs_read(fd_in, buf, chunk);
		if (r <= 0) break;
		long w = vfs_write(fd_out, buf, (size_t)r);
		if (w < 0) { if (done == 0) return w; break; }
		done += (size_t)w;
	}
	return (long)done;
}

void syscall_handle(arch_context_t *ctx)
{
	long n    = (long)CTX_SYSCALL_N(ctx);
	long arg0 = (long)CTX_SYSCALL_ARG0(ctx);
	long arg1 = (long)CTX_SYSCALL_ARG1(ctx);
	long arg2 = (long)CTX_SYSCALL_ARG2(ctx);
	long arg3 = (long)CTX_SYSCALL_ARG3(ctx);
	long arg4 = (long)CTX_SYSCALL_ARG4(ctx);
	long arg5 = (long)CTX_SYSCALL_ARG5(ctx);
	long ret  = -38;
	(void)arg5;


	switch (n) {
	/* ── I/O ─────────────────────────────────────────────── */
	case SYS_ARCH_READ:     ret = sys_read((int)arg0, (void*)arg1, (size_t)arg2); break;
	case SYS_ARCH_WRITE:    ret = sys_write((int)arg0, (void*)arg1, (size_t)arg2); break;
	case SYS_ARCH_SENDFILE: ret = sys_sendfile((int)arg0, (int)arg1, (long*)arg2, (size_t)arg3); break;
	case SYS_ARCH_READV:   ret = sys_readv((int)arg0, (const iovec_t*)arg1, (int)arg2); break;
	case SYS_ARCH_WRITEV:  ret = sys_writev((int)arg0, (const iovec_t*)arg1, (int)arg2); break;
	case SYS_ARCH_CLOSE:   ret = sys_close((int)arg0); break;
	case SYS_ARCH_LSEEK:   ret = sys_lseek((int)arg0, (off_t)arg1, (int)arg2); break;
	case SYS_ARCH_IOCTL:   ret = sys_ioctl((int)arg0, (uint32_t)arg1, (uint64_t)arg2); break;
	case SYS_ARCH_DUP:     ret = sys_dup((int)arg0); break;
	case SYS_ARCH_DUP3:    ret = sys_dup3((int)arg0, (int)arg1, (int)arg2); break;
	case SYS_ARCH_FCNTL:   ret = sys_fcntl((int)arg0, (int)arg1, (uint64_t)arg2); break;
	case SYS_ARCH_PIPE2:   ret = sys_pipe2((int*)arg0, (int)arg1); break;
#if defined(__x86_64__)
	case SYS_ARCH_PIPE:    ret = sys_pipe2((int*)arg0, 0); break;
#endif
	case SYS_ARCH_EPOLL_CREATE1: ret = sys_epoll_create1((int)arg0); break;
	case SYS_ARCH_EPOLL_CTL:     ret = sys_epoll_ctl((int)arg0, (int)arg1, (int)arg2, (linux_epoll_event_t*)arg3); break;
	case SYS_ARCH_EPOLL_PWAIT:   ret = sys_epoll_pwait((int)arg0, (linux_epoll_event_t*)arg1, (int)arg2, (int)arg3, (void*)arg4); break;
	/* ── open / stat ─────────────────────────────────────── */
#if defined(__x86_64__)
	case SYS_ARCH_OPEN:       ret = sys_open((const char*)arg0, (int)arg1, (mode_t)arg2); break;
#endif
	case SYS_ARCH_OPENAT:     ret = sys_openat((int)arg0, (const char*)arg1, (int)arg2, (mode_t)arg3); break;
	case SYS_ARCH_FSTAT:      ret = sys_fstat((int)arg0, (linux_stat_t*)arg1); break;
	case SYS_ARCH_NEWFSTATAT: ret = sys_newfstatat((int)arg0, (const char*)arg1, (linux_stat_t*)arg2, (int)arg3); break;
	case SYS_ARCH_GETDENTS64: ret = sys_getdents64((int)arg0, (void*)arg1, (size_t)arg2); break;
	/* ── fs ops ──────────────────────────────────────────── */
	case SYS_ARCH_CHDIR:    ret = sys_chdir((const char*)arg0); break;
	case SYS_ARCH_GETCWD:   ret = sys_getcwd((char*)arg0, (size_t)arg1); break;
	case SYS_ARCH_MKDIRAT:  ret = sys_mkdirat((int)arg0, (const char*)arg1, (mode_t)arg2); break;
	case SYS_ARCH_UNLINKAT: ret = sys_unlinkat((int)arg0, (const char*)arg1, (int)arg2); break;
	case SYS_ARCH_RENAMEAT: ret = sys_renameat((int)arg0, (const char*)arg1, (int)arg2, (const char*)arg3); break;
	case SYS_ARCH_FTRUNCATE: ret = sys_ftruncate((int)arg0, (off_t)arg1); break;
	case SYS_ARCH_FCHMOD:   ret = sys_fchmod((int)arg0, (mode_t)arg1); break;
	case SYS_ARCH_FCHMODAT: ret = sys_fchmodat((int)arg0, (const char*)arg1, (mode_t)arg2, (int)arg3); break;
	case SYS_ARCH_FCHOWN:   ret = sys_fchown((int)arg0, (uid_t)arg1, (gid_t)arg2); break;
	case SYS_ARCH_FCHOWNAT: ret = sys_fchownat((int)arg0, (const char*)arg1, (uid_t)arg2, (gid_t)arg3, 0); break;
	case SYS_ARCH_READLINKAT: ret = sys_readlinkat_vfs((int)arg0, (const char*)arg1, (char*)arg2, (size_t)arg3); break;
	case SYS_ARCH_SYMLINKAT: ret = sys_symlinkat((const char*)arg0, (int)arg1, (const char*)arg2); break;
	case SYS_ARCH_LINKAT:   ret = sys_linkat((int)arg0, (const char*)arg1, (int)arg2, (const char*)arg3, 0); break;
	case SYS_ARCH_FACCESSAT:  ret = sys_faccessat((int)arg0, (const char*)arg1, (int)arg2, (int)arg3); break;
	case SYS_ARCH_FACCESSAT2: ret = sys_faccessat((int)arg0, (const char*)arg1, (int)arg2, 0); break;
	/* ── poll / select ───────────────────────────────────── */
	case SYS_ARCH_PSELECT6: ret = sys_pselect6((int)arg0, (uint64_t*)arg1, (uint64_t*)arg2, (uint64_t*)arg3, (linux_timespec_t*)arg4, NULL); break;
	case SYS_ARCH_PPOLL:    ret = sys_ppoll((linux_pollfd_t*)arg0, (uint64_t)arg1, (linux_timespec_t*)arg2, NULL); break;
	/* ── memory ──────────────────────────────────────────── */
	case SYS_ARCH_BRK:     ret = sys_brk((uint64_t)arg0); break;
	case SYS_ARCH_MMAP:
		ret = sys_mmap((uint64_t)arg0, (size_t)arg1, (int)arg2, (int)arg3, (int)arg4, (uint64_t)arg5);
		break;
	case SYS_ARCH_MUNMAP:  ret = sys_munmap((uint64_t)arg0, (size_t)arg1); break;
	case SYS_ARCH_MPROTECT: ret = sys_mprotect((uint64_t)arg0, (size_t)arg1, (int)arg2); break;
	case SYS_ARCH_MADVISE: ret = sys_madvise((uint64_t)arg0, (size_t)arg1, (int)arg2); break;
	case SYS_ARCH_MSYNC:   ret = sys_msync((uint64_t)arg0, (size_t)arg1, (int)arg2); break;
	case SYS_ARCH_PRLIMIT64: ret = sys_prlimit64((pid_t)arg0, (int)arg1, (void*)arg2, (void*)arg3); break;
	/* ── process ─────────────────────────────────────────── */
	case SYS_ARCH_EXIT:        ret = sys_exit((int)arg0); break;
	case SYS_ARCH_EXIT_GROUP:  ret = sys_exit((int)arg0); break;
	case SYS_ARCH_REBOOT:      ret = sys_reboot((int)arg0, (int)arg1, (int)arg2, (void*)arg3); break;
	case SYS_ARCH_SYSLOG:      ret = sys_syslog((int)arg0, (char*)arg1, (int)arg2); break;
	case SYS_ARCH_UTIMENSAT:   ret = sys_utimensat((int)arg0, (const char*)arg1, (const void*)arg2, (int)arg3); break;
	case SYS_ARCH_GETPID:      ret = sys_getpid(); break;
	case SYS_ARCH_GETPPID:     ret = sys_getppid(); break;
	case SYS_ARCH_GETUID:      ret = sys_getuid(); break;
	case SYS_ARCH_GETEUID:     ret = sys_geteuid(); break;
	case SYS_ARCH_GETGID:      ret = sys_getgid(); break;
	case SYS_ARCH_GETEGID:     ret = sys_getegid(); break;
	case SYS_ARCH_GETTID:      ret = sys_gettid(); break;
	case SYS_ARCH_GETGROUPS:   ret = sys_getgroups((int)arg0, (uint64_t*)arg1); break;
	case SYS_ARCH_SETGROUPS:   ret = 0; break;
	case SYS_ARCH_GETRLIMIT:   ret = sys_getrlimit((int)arg0, (void*)arg1); break;
	case SYS_ARCH_SETRLIMIT:   ret = sys_setrlimit((int)arg0, (const void*)arg1); break;
	case SYS_ARCH_SETUID:      ret = sys_setuid((uid_t)arg0); break;
	case SYS_ARCH_SETGID:      ret = sys_setgid((gid_t)arg0); break;
	case SYS_ARCH_SETPGID:     ret = sys_setpgid((pid_t)arg0, (pid_t)arg1); break;
	case SYS_ARCH_GETPGID:     ret = sys_getpgid((pid_t)arg0); break;
	case SYS_ARCH_SETSID:      ret = sys_setsid(); break;
	case SYS_ARCH_GETSID:      ret = sys_getsid((pid_t)arg0); break;
	case SYS_ARCH_GETPRIORITY: ret = sys_getpriority((int)arg0, (int)arg1); break;
	case SYS_ARCH_SETPRIORITY: ret = sys_setpriority((int)arg0, (int)arg1, (int)arg2); break;
	case SYS_ARCH_SIGALTSTACK: ret = sys_sigaltstack((const stack_t *)arg0, (stack_t *)arg1); break;
	case SYS_ARCH_UMASK:       ret = sys_umask((mode_t)arg0); break;
	case SYS_ARCH_SET_TID_ADDR: ret = (long)task_current()->pid; break;
	case SYS_ARCH_KILL:        ret = sys_kill((pid_t)arg0, (int)arg1); break;
	case SYS_ARCH_TGKILL:     ret = sys_tgkill((pid_t)arg0, (pid_t)arg1, (int)arg2); break;
	case SYS_ARCH_FUTEX:      ret = sys_futex((uint32_t*)arg0, (int)arg1, (uint32_t)arg2, (linux_timespec_t*)arg3, (uint32_t*)arg4, (uint32_t)arg5); break;
#if defined(__x86_64__)
	case SYS_ARCH_GETPGRP:    ret = (long)task_current()->pid; break;
#endif
	case SYS_ARCH_SCHED_YIELD: ret = sys_sched_yield(); break;
	case SYS_ARCH_SCHED_SETAFFINITY: ret = sys_sched_setaffinity((pid_t)arg0, (size_t)arg1, (void*)arg2); break;
	case SYS_ARCH_SCHED_GETAFFINITY: ret = sys_sched_getaffinity((pid_t)arg0, (size_t)arg1, (void*)arg2); break;
	case SYS_ARCH_PRCTL:      ret = sys_prctl((int)arg0, (uint64_t)arg1, (uint64_t)arg2, (uint64_t)arg3, (uint64_t)arg4); break;
	case SYS_ARCH_GETRANDOM:  ret = sys_getrandom((void*)arg0, (size_t)arg1, (unsigned int)arg2); break;
	case SYS_ARCH_STATX:      ret = sys_statx((int)arg0, (const char*)arg1, (int)arg2, (unsigned int)arg3, (linux_statx_t*)arg4); break;
	case SYS_ARCH_MEMBARRIER: ret = 0; break;
	case SYS_ARCH_RSEQ:       ret = -22; break;
	case SYS_ARCH_SET_ROBUST_LIST: ret = 0; break;
	case SYS_ARCH_GET_ROBUST_LIST: ret = -38; break;
	case SYS_ARCH_RT_SIGACTION: ret = sys_rt_sigaction((int)arg0, (const sigaction_t*)arg1, (sigaction_t*)arg2, (size_t)arg3); break;
	case SYS_ARCH_RT_SIGPROCMASK: ret = sys_rt_sigprocmask((int)arg0, (const uint64_t*)arg1, (uint64_t*)arg2, (size_t)arg3); break;
	case SYS_ARCH_RT_SIGRETURN: sys_rt_sigreturn(ctx); return; /* returns directly, no ret value */
	case SYS_ARCH_VFORK:  ret = sys_vfork(); break;
#ifdef SYS_ARCH_FORK
	case SYS_ARCH_FORK:   ret = sys_fork(); break;
#endif
	case SYS_ARCH_CLONE: {
		uint64_t flags = (uint64_t)arg0;
#define CLONE_VM       0x00000100
#define CLONE_THREAD   0x00010000
#define CLONE_SETTLS   0x00080000
#define CLONE_CTID     0x00200000 /* CLONE_CHILD_CLEARTID */
#define CLONE_VFORK    0x00004000
		if (flags & CLONE_VFORK) {
			ret = sys_vfork();
		} else if (flags & CLONE_VM) {
			/* Thread: share address space */
			uint64_t newsp  = (uint64_t)arg1;
			uint32_t *ctid  = (flags & CLONE_CTID) ? (uint32_t*)arg3 : NULL;
			uint64_t tls    = (flags & CLONE_SETTLS) ? (uint64_t)arg4 : 0;
			ret = (long)proc_clone_thread(newsp, tls, ctid);
		} else {
			ret = sys_fork();
		}
		break;
	}
	case SYS_ARCH_EXECVE: ret = sys_execve((const char*)arg0, (char * const *)arg1, (char * const *)arg2); break;
	case SYS_ARCH_WAIT4:  ret = sys_waitpid((pid_t)arg0, (int*)arg1, (int)arg2); break;
	/* ── time ────────────────────────────────────────────── */
	case SYS_ARCH_CLOCK_GETTIME: ret = sys_clock_gettime((int)arg0, (linux_timespec_t*)arg1); break;
	case SYS_ARCH_GETTIMEOFDAY:  ret = sys_gettimeofday((linux_timeval_t*)arg0, (void*)arg1); break;
	case SYS_ARCH_NANOSLEEP:     ret = sys_nanosleep((linux_timespec_t*)arg0, (linux_timespec_t*)arg1); break;
	case SYS_ARCH_GETITIMER:     ret = sys_getitimer((int)arg0, (itimerval_t*)arg1); break;
	case SYS_ARCH_SETITIMER:     ret = sys_setitimer((int)arg0, (const itimerval_t*)arg1, (itimerval_t*)arg2); break;
	case SYS_ARCH_TIMES:         ret = sys_times((void*)arg0); break;
	/* ── misc ────────────────────────────────────────────── */
	case SYS_ARCH_UNAME:   ret = sys_uname((linux_utsname_t*)arg0); break;
	case SYS_ARCH_SYSINFO: ret = sys_sysinfo((void*)arg0); break;
#if defined(__x86_64__)
	case SYS_ARCH_STAT:    ret = sys_stat_path((const char*)arg0, (linux_stat_t*)arg1); break;
	case SYS_ARCH_LSTAT:   ret = sys_stat_path((const char*)arg0, (linux_stat_t*)arg1); break;
	case SYS_ARCH_ACCESS:  ret = sys_access((const char*)arg0, (int)arg1); break;
	case SYS_ARCH_SELECT:  ret = sys_select((int)arg0, (uint64_t*)arg1, (uint64_t*)arg2, (uint64_t*)arg3, (linux_timeval_t*)arg4); break;
	case SYS_ARCH_POLL:    ret = sys_poll((linux_pollfd_t*)arg0, (uint64_t)arg1, (int)arg2); break;
	case SYS_ARCH_DUP2:    ret = sys_dup2((int)arg0, (int)arg1); break;
	case SYS_ARCH_MKDIR:   ret = sys_mkdir((const char*)arg0, (mode_t)arg1); break;
	case SYS_ARCH_RMDIR:   ret = sys_rmdir((const char*)arg0); break;
	case SYS_ARCH_UNLINK:  ret = sys_unlink((const char*)arg0); break;
	case SYS_ARCH_LINK:    ret = sys_link((const char*)arg0, (const char*)arg1); break;
	case SYS_ARCH_SYMLINK: ret = sys_symlink((const char*)arg0, (const char*)arg1); break;
	case SYS_ARCH_READLINK: ret = sys_readlinkat_vfs(AT_FDCWD, (const char*)arg0, (char*)arg1, (size_t)arg2); break;
	case SYS_ARCH_RENAME:  ret = sys_rename((const char*)arg0, (const char*)arg1); break;
	case SYS_ARCH_CHMOD:   ret = sys_chmod((const char*)arg0, (mode_t)arg1); break;
	case SYS_ARCH_CHOWN:   ret = sys_chown((const char*)arg0, (uid_t)arg1, (gid_t)arg2); break;
	case SYS_ARCH_TRUNCATE: ret = sys_truncate((const char*)arg0, (off_t)arg1); break;
	case SYS_ARCH_ARCH_PRCTL: ret = sys_arch_prctl((int)arg0, (uint64_t)arg1); break;
	case SYS_ARCH_EPOLL_CREATE: ret = sys_epoll_create1(0); break;
	case SYS_ARCH_EPOLL_WAIT:   ret = sys_epoll_pwait((int)arg0, (linux_epoll_event_t*)arg1, (int)arg2, (int)arg3, NULL); break;
#endif
	/* ── sockets ─────────────────────────────────────────── */
	case SYS_ARCH_SOCKET:      ret = sys_socket((int)arg0, (int)arg1, (int)arg2); break;
	case SYS_ARCH_BIND:        ret = sys_bind((int)arg0, (const void*)arg1, (uint32_t)arg2); break;
	case SYS_ARCH_CONNECT:     ret = sys_connect((int)arg0, (const void*)arg1, (uint32_t)arg2); break;
	case SYS_ARCH_LISTEN:      ret = sys_listen((int)arg0, (int)arg1); break;
	case SYS_ARCH_ACCEPT:      ret = sys_accept4((int)arg0, (void*)arg1, (uint32_t*)arg2, 0); break;
	case SYS_ARCH_ACCEPT4:     ret = sys_accept4((int)arg0, (void*)arg1, (uint32_t*)arg2, (int)arg3); break;
	case SYS_ARCH_SENDTO:      ret = sys_sendto((int)arg0, (void*)arg1, (size_t)arg2, (int)arg3, (void*)arg4, (uint32_t)arg5); break;
	case SYS_ARCH_RECVFROM:    ret = sys_recvfrom((int)arg0, (void*)arg1, (size_t)arg2, (int)arg3, (void*)arg4, (uint32_t*)arg5); break;
	case SYS_ARCH_GETSOCKNAME: ret = sys_getsockname((int)arg0, (void*)arg1, (uint32_t*)arg2); break;
	case SYS_ARCH_GETPEERNAME: ret = sys_getpeername((int)arg0, (void*)arg1, (uint32_t*)arg2); break;
	case SYS_ARCH_SETSOCKOPT:  ret = sys_setsockopt((int)arg0, (int)arg1, (int)arg2, (void*)arg3, (uint32_t)arg4); break;
	case SYS_ARCH_GETSOCKOPT:  ret = sys_getsockopt((int)arg0, (int)arg1, (int)arg2, (void*)arg3, (uint32_t*)arg4); break;
	case SYS_ARCH_SHUTDOWN:    ret = sys_shutdown((int)arg0, (int)arg1); break;
	/* ── missing batch ───────────────────────────────────────── */
	case SYS_ARCH_PREAD64:     ret = sys_pread64((int)arg0, (void*)arg1, (size_t)arg2, (off_t)arg3); break;
	case SYS_ARCH_PWRITE64:    ret = sys_pwrite64((int)arg0, (void*)arg1, (size_t)arg2, (off_t)arg3); break;
	case SYS_ARCH_MREMAP:      ret = sys_mremap((uint64_t)arg0, (size_t)arg1, (size_t)arg2, (int)arg3, (uint64_t)arg4); break;
	case SYS_ARCH_FLOCK:       ret = sys_flock((int)arg0, (int)arg1); break;
	case SYS_ARCH_FSYNC:       ret = sys_fsync((int)arg0); break;
	case SYS_ARCH_FDATASYNC:   ret = sys_fdatasync((int)arg0); break;
	case SYS_ARCH_FCHDIR:      ret = sys_fchdir((int)arg0); break;
	case SYS_ARCH_GETRUSAGE:   ret = sys_getrusage((int)arg0, (void*)arg1); break;
	case SYS_ARCH_SETREUID:    ret = sys_setreuid((uid_t)arg0, (uid_t)arg1); break;
	case SYS_ARCH_SETREGID:    ret = sys_setregid((gid_t)arg0, (gid_t)arg1); break;
	case SYS_ARCH_SETRESUID:   ret = sys_setresuid((uid_t)arg0, (uid_t)arg1, (uid_t)arg2); break;
	case SYS_ARCH_GETRESUID:   ret = sys_getresuid((uid_t*)arg0, (uid_t*)arg1, (uid_t*)arg2); break;
	case SYS_ARCH_SETRESGID:   ret = sys_setresgid((gid_t)arg0, (gid_t)arg1, (gid_t)arg2); break;
	case SYS_ARCH_GETRESGID:   ret = sys_getresgid((gid_t*)arg0, (gid_t*)arg1, (gid_t*)arg2); break;
	case SYS_ARCH_RT_SIGPENDING: ret = sys_rt_sigpending((uint64_t*)arg0, (size_t)arg1); break;
	case SYS_ARCH_RT_SIGSUSPEND: ret = sys_rt_sigsuspend((const uint64_t*)arg0, (size_t)arg1); break;
	case SYS_ARCH_MLOCK:       ret = sys_mlock((uint64_t)arg0, (size_t)arg1); break;
	case SYS_ARCH_MUNLOCK:     ret = sys_munlock((uint64_t)arg0, (size_t)arg1); break;
	case SYS_ARCH_MLOCK2:      ret = sys_mlock2((uint64_t)arg0, (size_t)arg1, (int)arg2); break;
	case SYS_ARCH_SYNC:        ret = sys_sync(); break;
	case SYS_ARCH_MOUNT:       ret = sys_mount((const char*)arg0, (const char*)arg1, (const char*)arg2, (unsigned long)arg3, (const void*)arg4); break;
	case SYS_ARCH_SETHOSTNAME: ret = sys_sethostname((const char*)arg0, (size_t)arg1); break;
	case SYS_ARCH_TKILL:       ret = sys_tkill((pid_t)arg0, (int)arg1); break;
	case SYS_ARCH_CLOCK_NANOSLEEP: ret = sys_clock_nanosleep((int)arg0, (int)arg1, (linux_timespec_t*)arg2, (linux_timespec_t*)arg3); break;
	case SYS_ARCH_WAITID:      ret = sys_waitid((int)arg0, (int)arg1, (void*)arg2, (int)arg3, (void*)arg4); break;
	case SYS_ARCH_SPLICE:      ret = sys_splice((int)arg0, (off_t*)arg1, (int)arg2, (off_t*)arg3, (size_t)arg4, (unsigned int)arg5); break;
	case SYS_ARCH_FALLOCATE:   ret = sys_fallocate((int)arg0, (int)arg1, (off_t)arg2, (off_t)arg3); break;
	case SYS_ARCH_COPY_FILE_RANGE: ret = sys_copy_file_range((int)arg0, (off_t*)arg1, (int)arg2, (off_t*)arg3, (size_t)arg4, (unsigned int)arg5); break;
	case SYS_ARCH_SOCKETPAIR:  ret = sys_socketpair((int)arg0, (int)arg1, (int)arg2, (int*)arg3); break;
	/* ── fd-based notifications ──────────────────────────────── */
#if defined(__x86_64__)
	case SYS_ARCH_EVENTFD:
#endif
	case SYS_ARCH_EVENTFD2:    ret = sys_eventfd2((unsigned int)arg0, (int)arg1); break;
	case SYS_ARCH_TIMERFD_CREATE:  ret = sys_timerfd_create((int)arg0, (int)arg1); break;
	case SYS_ARCH_TIMERFD_SETTIME: ret = sys_timerfd_settime((int)arg0, (int)arg1, (linux_itimerspec_t*)arg2, (linux_itimerspec_t*)arg3); break;
	case SYS_ARCH_TIMERFD_GETTIME: ret = sys_timerfd_gettime((int)arg0, (linux_itimerspec_t*)arg1); break;
#if defined(__x86_64__)
	case SYS_ARCH_INOTIFY_INIT:
#endif
	case SYS_ARCH_INOTIFY_INIT1:   ret = sys_eventfd2(0, 0); break; /* stub: never readable */
	case SYS_ARCH_INOTIFY_ADD_WATCH: ret = 1; break;  /* stub: wd=1 */
	case SYS_ARCH_INOTIFY_RM_WATCH:  ret = 0; break;
	case SYS_ARCH_SIGNALFD4:   ret = sys_signalfd4((int)arg0, (const uint64_t*)arg1, (size_t)arg2, (int)arg3); break;
	case SYS_ARCH_MEMFD_CREATE: ret = sys_memfd_create((const char*)arg0, (unsigned int)arg1); break;
	/* ── arch-specific ───────────────────────────────────────── */
#if defined(__x86_64__)
	case SYS_ARCH_PAUSE:       ret = sys_pause(); break;
	case SYS_ARCH_ALARM:       ret = sys_alarm((unsigned int)arg0); break;
	case SYS_ARCH_CREAT:       ret = sys_creat((const char*)arg0, (mode_t)arg1); break;
	case SYS_ARCH_LCHOWN:      ret = sys_lchown((const char*)arg0, (uid_t)arg1, (gid_t)arg2); break;
	case SYS_ARCH_MKNOD:       ret = sys_mknod((const char*)arg0, (mode_t)arg1, (uint64_t)arg2); break;
	case SYS_ARCH_TIME_SYSCALL: ret = sys_time_syscall((long*)arg0); break;
	case SYS_ARCH_RENAMEAT2:   ret = sys_renameat2((int)arg0, (const char*)arg1, (int)arg2, (const char*)arg3, (unsigned int)arg4); break;
#else
	case SYS_ARCH_MKNODAT:     ret = sys_mknodat((int)arg0, (const char*)arg1, (mode_t)arg2, (uint64_t)arg3); break;
	case SYS_ARCH_RENAMEAT2:   ret = sys_renameat2((int)arg0, (const char*)arg1, (int)arg2, (const char*)arg3, (unsigned int)arg4); break;
#endif
	default:
		kprintf("syscall: unimplemented %ld (pid=%u)\n",
		        n, task_current()->pid);
		ret = -38; /* ENOSYS */
		break;
	}

	CTX_SYSCALL_RET(ctx, ret);
	task_current()->ctx = ctx;
}
