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
	return vfs_open(kpath, flags, mode);
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
	process_t *p = task_current();
	return (long)(p->tgid ? p->tgid : p->pid);
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
		if (child->ptrace_stop && !child->ptrace_stop_reported) {
			child->ptrace_stop_reported = 1;
			if (status && (uint64_t)status > 0x1000)
				*status = (5 << 8) | 0x7f; /* WIFSTOPPED | SIGTRAP */
			return (long)pid;
		}
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
		if (pa) {
			if (prot == 0)
				vm_unmap(p->vm, va);
			else
				vm_map(p->vm, va, pa, vmflags);
		} else if (prot != 0) {
			void *pg = buddy_alloc(0);
			if (!pg) return -12;
			memset((void *)PHYS_TO_VIRT(pg), 0, PAGE_SIZE);
			vm_map(p->vm, va, (uint64_t)pg, vmflags);
		}
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


#include "sys/syscall_fs.c"
#include "sys/syscall_proc.c"
#include "sys/syscall_io.c"
#include "sys/syscall_event.c"
#include "sys/syscall_net.c"
#include "sys/syscall_misc.c"
#include "sys/syscall_ptrace.c"
#include "sys/syscall_ipc.c"

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

	task_current()->ctx = ctx;
	ptrace_stop_check(ctx, 0);


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
			/* Linux ABI: x86_64 clone(flags,newsp,ptid,ctid,tls)
			 *            aarch64 clone(flags,newsp,ptid,tls,ctid) */
#if defined(__aarch64__)
			uint64_t tls    = (flags & CLONE_SETTLS) ? (uint64_t)arg3 : 0;
			uint32_t *ctid  = (flags & CLONE_CTID)   ? (uint32_t*)arg4 : NULL;
#else
			uint32_t *ctid  = (flags & CLONE_CTID)   ? (uint32_t*)arg3 : NULL;
			uint64_t tls    = (flags & CLONE_SETTLS) ? (uint64_t)arg4 : 0;
#endif
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
	case SYS_ARCH_LINK:    ret = sys_linkat(AT_FDCWD, (const char*)arg0, AT_FDCWD, (const char*)arg1, 0); break;
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
	case SYS_ARCH_UMOUNT2:     ret = sys_umount2((const char*)arg0, (int)arg1); break;
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
	case SYS_ARCH_INOTIFY_INIT1:      ret = sys_inotify_init1((int)arg0); break;
	case SYS_ARCH_INOTIFY_ADD_WATCH:  ret = sys_inotify_add_watch((int)arg0, (const char*)arg1, (uint32_t)arg2); break;
	case SYS_ARCH_INOTIFY_RM_WATCH:   ret = sys_inotify_rm_watch((int)arg0, (int)arg1); break;
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
	case SYS_ARCH_STATFS:    ret = sys_statfs((const char*)arg0, (kernel_statfs_t*)arg1); break;
	case SYS_ARCH_FSTATFS:   ret = sys_fstatfs((int)arg0, (kernel_statfs_t*)arg1); break;
	case SYS_ARCH_CAPGET:    ret = sys_capget((void*)arg0, (void*)arg1); break;
	case SYS_ARCH_CAPSET:    ret = sys_capset((void*)arg0, (const void*)arg1); break;
	case SYS_ARCH_GETXATTR:  ret = sys_getxattr((const char*)arg0,(const char*)arg1,(void*)arg2,(size_t)arg3); break;
	case SYS_ARCH_LGETXATTR: ret = sys_lgetxattr((const char*)arg0,(const char*)arg1,(void*)arg2,(size_t)arg3); break;
	case SYS_ARCH_FGETXATTR: ret = sys_fgetxattr((int)arg0,(const char*)arg1,(void*)arg2,(size_t)arg3); break;
	case SYS_ARCH_SETXATTR:  ret = sys_setxattr((const char*)arg0,(const char*)arg1,(const void*)arg2,(size_t)arg3,(int)arg4); break;
	case SYS_ARCH_LSETXATTR: ret = sys_lsetxattr((const char*)arg0,(const char*)arg1,(const void*)arg2,(size_t)arg3,(int)arg4); break;
	case SYS_ARCH_LISTXATTR: ret = sys_listxattr((const char*)arg0,(char*)arg1,(size_t)arg2); break;
	case SYS_ARCH_LLISTXATTR:ret = sys_llistxattr((const char*)arg0,(char*)arg1,(size_t)arg2); break;
	case SYS_ARCH_REMOVEXATTR:ret = sys_removexattr((const char*)arg0,(const char*)arg1); break;
	case SYS_ARCH_PTRACE:    ret = sys_ptrace((long)arg0, (pid_t)arg1, (uint64_t)arg2, (uint64_t)arg3); break;
	/* SysV IPC */
	case SYS_ARCH_SHMGET:   ret = sys_shmget((key_t)arg0, (size_t)arg1, (int)arg2); break;
	case SYS_ARCH_SHMAT:    ret = sys_shmat((int)arg0, (const void*)arg1, (int)arg2); break;
	case SYS_ARCH_SHMDT:    ret = sys_shmdt((const void*)arg0); break;
	case SYS_ARCH_SHMCTL:   ret = sys_shmctl((int)arg0, (int)arg1, (void*)arg2); break;
	case SYS_ARCH_SEMGET:   ret = sys_semget((key_t)arg0, (int)arg1, (int)arg2); break;
	case SYS_ARCH_SEMOP:    ret = sys_semop((int)arg0, (const sembuf_t*)arg1, (size_t)arg2); break;
	case SYS_ARCH_SEMCTL:   ret = sys_semctl((int)arg0, (int)arg1, (int)arg2, (uint64_t)arg3); break;
	/* POSIX IPC */
	case SYS_ARCH_MQ_OPEN:         ret = sys_mq_open((const char*)arg0,(int)arg1,(mode_t)arg2,(mq_attr_t*)arg3); break;
	case SYS_ARCH_MQ_UNLINK:       ret = sys_mq_unlink((const char*)arg0); break;
	case SYS_ARCH_MQ_TIMEDSEND:    ret = sys_mq_timedsend((int)arg0,(const char*)arg1,(size_t)arg2,(uint32_t)arg3,(const void*)arg4); break;
	case SYS_ARCH_MQ_TIMEDRECEIVE: ret = sys_mq_timedreceive((int)arg0,(char*)arg1,(size_t)arg2,(uint32_t*)arg3,(const void*)arg4); break;
	case SYS_ARCH_MQ_GETSETATTR:   ret = sys_mq_getsetattr((int)arg0,(const mq_attr_t*)arg1,(mq_attr_t*)arg2); break;
	default:
		kprintf("syscall: unimplemented %ld (pid=%u)\n",
		        n, task_current()->pid);
		ret = -38; /* ENOSYS */
		break;
	}

	CTX_SYSCALL_RET(ctx, ret);
	task_current()->ctx = ctx;
	ptrace_stop_check(ctx, 1);
}
