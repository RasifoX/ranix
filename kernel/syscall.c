#include <kernel/syscall.h>
#include <kernel/proc.h>
#include <kernel/elf.h>
#include <kernel/vfs.h>
#include <kernel/hal.h>
#include <kernel/kprintf.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/vm.h>
#include <kernel/lib/string.h>
#include <kernel/sync.h>
#include <kernel/tty/fb_tty.h>
#include <drivers/serial/uart.h>
#include <kernel/board.h>

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

/* Linux ABI stat struct */
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

static int resolve_path(char *out, int dirfd, const char *upath);

static long sys_write(int fd, const void *buf, size_t len)
{
	if (!access_ok(buf, len)) return -14;

	/* If a real VFS file is installed at fd (e.g. via dup2), use it */
	if (fd >= 0 && fd <= 2) {
		process_t *p = task_current();
		if (p->fds && fd_get(p->fds, fd))
			return vfs_write(fd, buf, len);
	}

	if (fd == 0 || fd == 1 || fd == 2) {
		char kbuf[256];
		size_t remaining = len;
		const char *uptr = buf;
		while (remaining > 0) {
			size_t chunk = remaining > sizeof(kbuf) ? sizeof(kbuf) : remaining;
			if (copy_from_user(kbuf, uptr, chunk) != 0)
				return -14;
			
			for (size_t i = 0; i < chunk; i++) {
				char c = kbuf[i];
				fb_tty_putchar(c);
				if (c == '\n') uart_putc('\r');
				uart_putc(c);
			}
			uptr += chunk;
			remaining -= chunk;
		}
		fb_tty_flush();
		return (long)len;
	}
	return vfs_write(fd, buf, len);
}

/* count display columns for line[0..bytes] (UTF-8 codepoints = columns) */
static int line_cols(const char *line, int bytes)
{
	int cols = 0;
	for (int i = 0; i < bytes; ) {
		unsigned char c = (unsigned char)line[i];
		if      (c < 0x80)                    { i += 1; }
		else if ((c & 0xE0) == 0xC0)          { i += 2; }
		else if ((c & 0xF0) == 0xE0)          { i += 3; }
		else                                   { i += 1; }
		cols++;
	}
	return cols;
}

/* step one UTF-8 codepoint left from pos, return new pos */
static int utf8_step_left(const char *line, int pos)
{
	if (pos <= 0) return 0;
	pos--;
	while (pos > 0 && ((unsigned char)line[pos] & 0xC0) == 0x80) pos--;
	return pos;
}

/* step one UTF-8 codepoint right from pos, return new pos */
static int utf8_step_right(const char *line, int pos, int len)
{
	if (pos >= len) return len;
	pos++;
	while (pos < len && ((unsigned char)line[pos] & 0xC0) == 0x80) pos++;
	return pos;
}

static char tty_prompt[128] = "$ ";
static int  tty_prompt_len  = 2;
#define PROMPT     tty_prompt
#define PROMPT_LEN tty_prompt_len

static void tty_out(const char *s, int n)
{
	fb_tty_write(s, (size_t)n);
	for (int i = 0; i < n; i++) {
		if (s[i] == '\n') uart_putc('\r');
		uart_putc(s[i]);
	}
}
static void tty_out_s(const char *s)
{
	int n = 0; while (s[n]) n++;
	tty_out(s, n);
}

static void uart_goto_col(int col)  /* 1-based absolute column */
{
	uart_putc('\033'); uart_putc('[');
	if (col >= 100) uart_putc((char)('0' + col / 100));
	if (col >= 10)  uart_putc((char)('0' + (col / 10) % 10));
	uart_putc((char)('0' + col % 10));
	uart_putc('G');
}

static void tty_redraw(const char *line, int len, int pos, int old_cols)
{
	int new_cols = line_cols(line, len);
	int cur_cols = line_cols(line, pos);

	tty_out_s("\r");
	tty_out_s(PROMPT);
	tty_out(line, len);
	for (int i = new_cols; i < old_cols; i++) tty_out_s(" ");

	fb_tty_set_col(PROMPT_LEN + cur_cols);       /* fb: direct cursor jump */
	uart_goto_col(PROMPT_LEN + cur_cols + 1);    /* uart: absolute col (1-based) */
	fb_tty_flush();
}

static long sys_read(int fd, void *buf, size_t len)
{
	if (!access_ok(buf, len)) return -14;

	/* If a real VFS file is installed at fd (e.g. via dup2), use it */
	if (fd == 0) {
		process_t *p = task_current();
		if (p->fds && fd_get(p->fds, 0))
			return vfs_read(0, buf, len);
	}

	if (fd == 0) {
		static char line[256];
		static int  llen = 0, pos = 0;
		static int  prev_display_cols = 0;

		fb_tty_show_cursor(1);
		fb_tty_flush();              /* show cursor immediately at prompt */
		uart_putc('\033'); uart_putc('['); uart_putc('?');
		uart_putc('2'); uart_putc('5'); uart_putc('h'); /* UART: show cursor */

		while (1) {
			while (!uart_has_data()) {
				fb_tty_cursor_flush();
				task_sleep();
			}
			unsigned char c = (unsigned char)uart_getc();

			if (c == '\r' || c == '\n') {
				fb_tty_show_cursor(0);
				tty_out_s("\n");
				fb_tty_flush();
				line[llen++] = '\n';
				size_t n = (size_t)llen < len ? (size_t)llen : len;
				arch_user_access_begin();
				memmove(buf, line, n);
				arch_user_access_end();
				llen = pos = prev_display_cols = 0;
				return (long)n;
			}

			if (c == 0x1B) { /* ESC — arrow key sequence */
				while (!uart_has_data()) task_sleep();
				if (uart_getc() != '[') continue;
				while (!uart_has_data()) task_sleep();
				unsigned char arrow = (unsigned char)uart_getc();
				if (arrow == 'D' && pos > 0) {       /* left */
					pos = utf8_step_left(line, pos);
					fb_tty_set_col(PROMPT_LEN + line_cols(line, pos));
					fb_tty_flush();
					uart_putc('\033'); uart_putc('['); uart_putc('D');
				} else if (arrow == 'C' && pos < llen) { /* right */
					pos = utf8_step_right(line, pos, llen);
					fb_tty_set_col(PROMPT_LEN + line_cols(line, pos));
					fb_tty_flush();
					uart_putc('\033'); uart_putc('['); uart_putc('C');
				}
				continue;
			}

			if (c == 127 || c == '\b') { /* backspace */
				if (pos > 0) {
					int newpos = utf8_step_left(line, pos);
					int del    = pos - newpos;
					memmove(line + newpos, line + pos, (size_t)(llen - pos));
					llen -= del;
					pos   = newpos;
					tty_redraw(line, llen, pos, prev_display_cols);
					prev_display_cols = line_cols(line, llen);
				}
				continue;
			}

			if (c >= 0x20 || (c & 0x80)) { /* printable or UTF-8 continuation */
				if (llen < 254) {
					memmove(line + pos + 1, line + pos, (size_t)(llen - pos));
					line[pos] = (char)c;
					llen++;
					pos++;
					int dc = line_cols(line, llen);
					tty_redraw(line, llen, pos, prev_display_cols);
					prev_display_cols = dc;
				}
				continue;
			}
		}
	}
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

static long sys_open(const char *path, int flags, mode_t mode)
{
	char kpath[PROC_NAME_MAX];
	if (copy_from_user(kpath, path, PROC_NAME_MAX) != 0)
		return -14;
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
	if (fd < 0 || fd >= FD_MAX) return -9;
	if (fd < 3) {
		process_t *p = task_current();
		if (!p->fds || !fd_get(p->fds, fd)) return 0;
	}
	return vfs_close(fd) == 0 ? 0 : -9;
}

static long sys_lseek(int fd, off_t off, int whence)
{
	off_t r = vfs_seek(fd, off, whence);
	return r < 0 ? -29 : (long)r;
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
		if (!page) return (long)p->brk;
		memset(page, 0, PAGE_SIZE);
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
		for (uint64_t v = va; v < va + len; v += PAGE_SIZE) {
			void *pg = buddy_alloc(0);
			if (!pg) return -12;
			memset(pg, 0, PAGE_SIZE);
			vm_map(p->vm, v, (uint64_t)pg, vmflags);
		}
		{
			uint32_t vf = VMA_USER | VMA_ANON;
			if (prot & PROT_READ)  vf |= VMA_READ;
			if (prot & PROT_WRITE) vf |= VMA_WRITE;
			if (prot & PROT_EXEC)  vf |= VMA_EXEC;
			vma_add(p, va, len, vf);
		}
		return (long)va;
	}

	/* File-backed mmap */
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f || !f->inode->fops->read) return -9;

	uint64_t file_off = off;
	for (uint64_t v = va; v < va + len; v += PAGE_SIZE) {
		void *pg = buddy_alloc(0);
		if (!pg) return -12;
		memset(pg, 0, PAGE_SIZE);

		/* read one page from file at current offset via a temporary file handle */
		vfs_file_t tmp_f = *f;
		tmp_f.pos = (off_t)file_off;
		ssize_t nr = f->inode->fops->read(&tmp_f, pg, PAGE_SIZE);
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

#define USER_STACK_TOP_   0x800000UL
#define USER_STACK_PAGES_ 8
#define USER_MMAP_BASE_   0x10000000UL

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
#define EXEC_MAXARGS 32
#define EXEC_MAXSTRTOTAL 4096
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
	if (vfs_stat(kpath, &st) != 0) return -2;
	if (st.size == 0)             return -2;

	uint8_t *buf = kmalloc(st.size);
	if (!buf) return -12;

	int fd = vfs_open(kpath, O_RDONLY, 0);
	if (fd < 0) { kfree(buf); return -2; }
	vfs_read(fd, buf, st.size);
	vfs_close(fd);

	/* build new address space */
	vm_space_t *new_vm = vm_create();
	if (!new_vm) { kfree(buf); return -12; }

	elf_info_t ei = {0};
	if (elf_load(new_vm, buf, (size_t)st.size, &ei) != 0) {
		kfree(buf);
		vm_destroy(new_vm);
		return -8;
	}
	kfree(buf);

	/* map user stack */
	for (int j = 0; j < USER_STACK_PAGES_; j++) {
		void *pg = buddy_alloc(0);
		if (!pg) { vm_destroy(new_vm); return -12; }
		memset(pg, 0, PAGE_SIZE);
		uint64_t sva = USER_STACK_TOP_ - (uint64_t)(USER_STACK_PAGES_ - j) * PAGE_SIZE;
		vm_map(new_vm, sva, (uint64_t)pg, VM_USER | VM_READ | VM_WRITE);
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
		for (int i = 3; i < FD_MAX; i++) {
			vfs_file_t *xf = fd_get(p->fds, i);
			if (xf && (xf->flags & O_CLOEXEC)) {
				fd_free(p->fds, i);
				if (--xf->refcount == 0) {
					if (xf->inode->fops && xf->inode->fops->close)
						xf->inode->fops->close(xf);
					kfree(xf);
				}
			}
		}
	}

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

/* push one auxv key/value pair (16 bytes each, stack grows down) */
#define PUSH_AUX(k, v) do { \
	sp -= 16; \
	((uint64_t *)sp)[0] = (uint64_t)(k); \
	((uint64_t *)sp)[1] = (uint64_t)(v); \
} while (0)

	PUSH_AUX(0,  0);                        /* AT_NULL */
	PUSH_AUX(31, at_execfn_addr);           /* AT_EXECFN */
	PUSH_AUX(25, at_random_addr);           /* AT_RANDOM */
	PUSH_AUX(23, 0);                        /* AT_SECURE */
	PUSH_AUX(17, 100);                      /* AT_CLKTCK */
#if defined(__aarch64__)
	PUSH_AUX(16, 3);                        /* AT_HWCAP: FP + ASIMD */
#else
	PUSH_AUX(16, 0);                        /* AT_HWCAP */
#endif
	PUSH_AUX(14, 0);                        /* AT_EGID */
	PUSH_AUX(13, 0);                        /* AT_GID */
	PUSH_AUX(12, 0);                        /* AT_EUID */
	PUSH_AUX(11, 0);                        /* AT_UID */
	PUSH_AUX(9,  ei.entry);                 /* AT_ENTRY */
	PUSH_AUX(7,  0);                        /* AT_BASE (static: no interp) */
	PUSH_AUX(6,  PAGE_SIZE);               /* AT_PAGESZ */
	PUSH_AUX(5,  ei.phnum);                 /* AT_PHNUM */
	PUSH_AUX(4,  ei.phentsize);             /* AT_PHENT */
	PUSH_AUX(3,  ei.phdr_va);              /* AT_PHDR */
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

	return 0;
}

static long sys_rt_sigaction(int sig, const sigaction_t *act, sigaction_t *oldact, size_t sigsetsize)
{
	if (sig <= 0 || sig > NSIG || sigsetsize != 8) return -22;
	if (sig == SIGKILL) return -22;

	process_t *p = task_current();
	if (oldact) {
		if (copy_to_user(oldact, &p->sig_actions[sig - 1], sizeof(sigaction_t)) != 0)
			return -14;
	}
	if (act) {
		if (copy_from_user(&p->sig_actions[sig - 1], act, sizeof(sigaction_t)) != 0)
			return -14;
	}
	return 0;
}

static long sys_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oldset, size_t sigsetsize)
{
	if (sigsetsize != 8) return -22;
	process_t *p = task_current();
	if (oldset) {
		if (copy_to_user(oldset, &p->sig_mask, sizeof(uint64_t)) != 0)
			return -14;
	}
	if (set) {
		uint64_t new_set;
		if (copy_from_user(&new_set, set, sizeof(uint64_t)) != 0)
			return -14;
		
		if (how == 0) p->sig_mask = new_set;      /* SIG_BLOCK */
		else if (how == 1) p->sig_mask |= new_set;
		else if (how == 2) p->sig_mask &= ~new_set;
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
	/* Restore saved ELR and LR from the mini-frame we pushed */
	uint64_t usp     = ctx->sp;
	ctx->elr         = *(uint64_t *)usp;
	ctx->regs[30]    = *(uint64_t *)(usp + 8);
	ctx->sp          = usp + 16;
#elif defined(__x86_64__)
	/* Restore saved RIP from stack */
	ctx->rip         = *(uint64_t *)ctx->rsp;
	ctx->rsp        += 8;
#endif
	/* Unblock the signal that was being handled */
	/* We don't track which signal was being handled per-frame, so unblock all
	   non-permanently-masked signals that were auto-blocked */
	p->sig_mask = 0;
}

static long sys_pipe2(int *pipefd, int flags)
{
	int kfds[2];
	if (vfs_pipe(kfds) != 0) return -1;
	/* propagate O_CLOEXEC and O_NONBLOCK to the file instances */
	if (flags & (O_CLOEXEC | O_NONBLOCK)) {
		process_t *p = task_current();
		for (int i = 0; i < 2; i++) {
			vfs_file_t *f = fd_get(p->fds, kfds[i]);
			if (f) f->flags |= flags & (O_CLOEXEC | O_NONBLOCK);
		}
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
			int count = task_count();
			for (int i = 0; i < count; i++) {
				process_t *c = task_get(i);
				if (c && c->ppid == p->pid) {
					if (c->state == PROC_DEAD && !c->reaped) { child = c; break; }
					if (c->state != PROC_DEAD) any_alive = 1;
				}
			}
			if (child) {
				child->reaped = 1;
				if (status && (uint64_t)status > 0x1000)
					*status = (child->exit_code & 0xFF) << 8;
				return (long)child->pid;
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
	if (status && (uint64_t)status > 0x1000)
		*status = (child->exit_code & 0xFF) << 8;
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
	(void)pid; (void)resource;
	if (old_limit) { uint64_t limits[2] = { 8192 * 1024, 8192 * 1024 }; memcpy(old_limit, limits, 16); }
	return 0;
}

/* ── path helper ─────────────────────────────────────────── */

extern volatile uint64_t kernel_ticks;

static int resolve_path(char *out, int dirfd, const char *upath)
{
	char kp[PROC_NAME_MAX];
	if (copy_from_user(kp, upath, PROC_NAME_MAX) != 0) return -14;
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
	if (copy_from_user(kp, path, PROC_NAME_MAX) != 0) return -14;
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
	if (copy_from_user(kp, path, PROC_NAME_MAX) != 0) return -14;
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
	if (copy_from_user(ko, old, PROC_NAME_MAX) != 0) return -14;
	if (copy_from_user(kn, new, PROC_NAME_MAX) != 0) return -14;
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
	if (copy_from_user(kp, path, PROC_NAME_MAX) != 0) return -14;
	return vfs_unlink(kp) == 0 ? 0 : -2;
}

/* ── stat syscalls ───────────────────────────────────────── */

static void fill_linux_stat(linux_stat_t *st, const vfs_stat_t *vs)
{
	memset(st, 0, sizeof(*st));
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
	if (copy_from_user(kp, path, PROC_NAME_MAX) != 0) return -14;
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
		if (copy_from_user(kp, path, 1) == 0 && kp[0] == '\0')
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
	if (copy_from_user(kp, path, PROC_NAME_MAX) != 0) return -14;
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
	(void)path; (void)len; return 0;
}

static long sys_ftruncate(int fd, off_t len)
{
	(void)fd; (void)len; return 0;
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
	if (total > 0) {
		arch_user_access_begin();
		memcpy(buf, kbuf, offset);
		arch_user_access_end();
	}
	kfree(kbuf);
	return total;
}

/* ── dup / fcntl ─────────────────────────────────────────── */

static long sys_dup(int oldfd)
{
	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, oldfd);
	if (!f) return -9;
	f->refcount++;
	int newfd = fd_alloc(p->fds, f);
	if (newfd < 0) { f->refcount--; return -24; }
	return newfd;
}

static long sys_dup2(int oldfd, int newfd)
{
	if (newfd < 0 || newfd >= FD_MAX) return -9;
	process_t *p = task_current();
	vfs_file_t *src;
	if (oldfd >= 0 && oldfd < 3 && !(p->fds && fd_get(p->fds, oldfd)))
		return -9; /* can't dup bare UART stdin/stdout */
	src = p->fds ? fd_get(p->fds, oldfd) : NULL;
	if (!src) return -9;
	if (oldfd == newfd) return newfd;
	if (p->fds->bitmap & (1ULL << newfd)) vfs_close(newfd);
	src->refcount++;
	p->fds->files[newfd] = src;
	p->fds->bitmap |= (1ULL << newfd);
	return newfd;
}

static long sys_dup3(int oldfd, int newfd, int flags)
{
	long r = sys_dup2(oldfd, newfd);
	if (r >= 0 && (flags & O_CLOEXEC)) {
		process_t *p = task_current();
		vfs_file_t *f = fd_get(p->fds, newfd);
		if (f) f->flags |= O_CLOEXEC;
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
		for (int i = (int)arg; i < FD_MAX; i++) {
			if (!(p->fds->bitmap & (1ULL << i))) {
				f->refcount++;
				p->fds->files[i] = f;
				p->fds->bitmap |= (1ULL << i);
				return i;
			}
		}
		return -24;
	}
	case F_GETFD: {
		vfs_file_t *f = fd_get(p->fds, fd);
		if (!f) return -9;
		return (f->flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
	}
	case F_SETFD: {
		vfs_file_t *f = fd_get(p->fds, fd);
		if (!f) return -9;
		if (arg & FD_CLOEXEC) f->flags |= O_CLOEXEC;
		else                  f->flags &= ~O_CLOEXEC;
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
	arch_user_access_begin();
	memcpy(buf, p->cwd, n + 1);
	arch_user_access_end();
	return (long)(n + 1);
}

/* ── process info ────────────────────────────────────────── */

static long sys_getuid(void)  { return (long)task_current()->uid; }
static long sys_geteuid(void) { return (long)task_current()->uid; }
static long sys_getgid(void)  { return (long)task_current()->gid; }
static long sys_getegid(void) { return (long)task_current()->gid; }
static long sys_getppid(void) { return (long)task_current()->ppid; }
static long sys_getpgrp(void) { return (long)task_current()->pid; }
static long sys_getpgid(pid_t pid) { (void)pid; return (long)task_current()->pid; }
static long sys_setsid(void)  { return (long)task_current()->pid; }
static long sys_setpgid(pid_t pid, pid_t pgid) { (void)pid; (void)pgid; return 0; }
static long sys_setuid(uid_t uid) { task_current()->uid = uid; return 0; }
static long sys_setgid(gid_t gid) { task_current()->gid = gid; return 0; }
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

static long sys_uname(linux_utsname_t *uts)
{
	if (!uts) return -14;
	linux_utsname_t k;
	memset(&k, 0, sizeof(k));
	memcpy(k.sysname,    "Linux",   6);
	memcpy(k.nodename,   "ranix",   6);
	memcpy(k.release,    "6.1.0",   6);
	memcpy(k.version,    "Ranix",   6);
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

#define FUTEX_SLOTS 32
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
	while (kernel_ticks < end) task_sleep();
	return 0;
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
	uint64_t zeros[12] = {0};
	copy_to_user(info, zeros, sizeof(zeros));
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

typedef struct { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; } linux_winsize_t;

static long sys_ioctl(int fd, uint32_t cmd, uint64_t arg)
{
	/* Forward to VFS if file has ioctl */
	if (fd >= 3) {
		process_t  *p = task_current();
		vfs_file_t *f = fd_get(p->fds, fd);
		if (f && f->inode->fops && f->inode->fops->ioctl)
			return f->inode->fops->ioctl(f, cmd, arg);
	}
	switch (cmd) {
	case TIOCGWINSZ: {
		linux_winsize_t ws = {24, 80, 0, 0};
		return copy_to_user((void *)(uintptr_t)arg, &ws, sizeof(ws));
	}
	case TCGETS: {
		uint8_t termios[60]; memset(termios, 0, 60);
		return copy_to_user((void *)(uintptr_t)arg, termios, 60);
	}
	case TCSETS: case TCSETSW: case TCSETSF: return 0;
	case TIOCGPGRP: { int pgid = (int)task_current()->pid; return copy_to_user((void *)(uintptr_t)arg, &pgid, 4); }
	case TIOCSPGRP: return 0;
	case TIOCSPROMPT: {
		char kp[128];
		if (copy_from_user(kp, (const char *)(uintptr_t)arg, 127) != 0) return -14;
		kp[127] = '\0';
		int n = 0; while (kp[n]) n++;
		memcpy(tty_prompt, kp, (size_t)(n + 1));
		tty_prompt_len = n;
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

static int fd_is_readable(int fd)
{
	if (fd == 0) {
		process_t  *p = task_current();
		vfs_file_t *f = p->fds ? fd_get(p->fds, 0) : NULL;
		if (f) {
			if (f->inode->fops && f->inode->fops->poll)
				return f->inode->fops->poll(f);
			return 1;
		}
		return uart_has_data() ? 1 : 0;
	}
	process_t  *p = task_current();
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return 0;
	if (f->inode->fops && f->inode->fops->poll)
		return f->inode->fops->poll(f);
	return 1; /* regular file: always readable */
}

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
			if ((kfds[i].events & POLLIN) && fd_is_readable(kfds[i].fd))
				kfds[i].revents |= POLLIN;
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
	(void)writefds; (void)exceptfds;
	if (nfds <= 0) return 0;

	uint64_t rfds = 0;
	if (readfds && copy_from_user(&rfds, readfds, 8) != 0) return -14;

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
	uint64_t result = 0;
	while (1) {
		result = 0; ready = 0;
		for (int i = 0; i < nfds && i < 64; i++) {
			if ((rfds & (1ULL << i)) && fd_is_readable(i)) {
				result |= (1ULL << i);
				ready++;
			}
		}
		if (ready > 0 || kernel_ticks >= deadline) break;
		task_sleep();
	}

	if (readfds) copy_to_user(readfds, &result, 8);
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

/* ── symlink / link / readlink (stubs) ───────────────────── */

static long sys_readlink(const char *path, char *buf, size_t bufsiz)
{
	(void)path; (void)buf; (void)bufsiz; return -22;
}
static long sys_readlinkat(int d, const char *path, char *buf, size_t sz)
{
	(void)d; return sys_readlink(path, buf, sz);
}
static long sys_symlink(const char *t, const char *lp) { (void)t; (void)lp; return -1; }
static long sys_symlinkat(const char *t, int d, const char *lp)
{
	(void)d; return sys_symlink(t, lp);
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
	case SYS_ARCH_READ:    ret = sys_read((int)arg0, (void*)arg1, (size_t)arg2); break;
	case SYS_ARCH_WRITE:   ret = sys_write((int)arg0, (void*)arg1, (size_t)arg2); break;
	case SYS_ARCH_WRITEV:  ret = sys_writev((int)arg0, (iovec_t*)arg1, (int)arg2); break;
	case SYS_ARCH_CLOSE:   ret = sys_close((int)arg0); break;
	case SYS_ARCH_LSEEK:   ret = sys_lseek((int)arg0, (off_t)arg1, (int)arg2); break;
	case SYS_ARCH_IOCTL:   ret = sys_ioctl((int)arg0, (uint32_t)arg1, (uint64_t)arg2); break;
	case SYS_ARCH_DUP:     ret = sys_dup((int)arg0); break;
	case SYS_ARCH_DUP3:    ret = sys_dup3((int)arg0, (int)arg1, (int)arg2); break;
	case SYS_ARCH_FCNTL:   ret = sys_fcntl((int)arg0, (int)arg1, (uint64_t)arg2); break;
	case SYS_ARCH_PIPE2:   ret = sys_pipe2((int*)arg0, (int)arg1); break;
	/* ── open / stat ─────────────────────────────────────── */
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
	case SYS_ARCH_READLINKAT: ret = sys_readlinkat((int)arg0, (const char*)arg1, (char*)arg2, (size_t)arg3); break;
	case SYS_ARCH_SYMLINKAT: ret = sys_symlinkat((const char*)arg0, (int)arg1, (const char*)arg2); break;
	case SYS_ARCH_LINKAT:   ret = sys_linkat((int)arg0, (const char*)arg1, (int)arg2, (const char*)arg3, 0); break;
	case SYS_ARCH_FACCESSAT: ret = sys_faccessat((int)arg0, (const char*)arg1, (int)arg2, (int)arg3); break;
	/* ── poll / select ───────────────────────────────────── */
	case SYS_ARCH_PSELECT6: ret = sys_pselect6((int)arg0, (uint64_t*)arg1, (uint64_t*)arg2, (uint64_t*)arg3, (linux_timespec_t*)arg4, NULL); break;
	case SYS_ARCH_PPOLL:    ret = sys_ppoll((linux_pollfd_t*)arg0, (uint64_t)arg1, (linux_timespec_t*)arg2, NULL); break;
	/* ── memory ──────────────────────────────────────────── */
	case SYS_ARCH_BRK:     ret = sys_brk((uint64_t)arg0); break;
	case SYS_ARCH_MMAP:    ret = sys_mmap((uint64_t)arg0, (size_t)arg1, (int)arg2, (int)arg3, (int)arg4, (uint64_t)arg5); break;
	case SYS_ARCH_MUNMAP:  ret = sys_munmap((uint64_t)arg0, (size_t)arg1); break;
	case SYS_ARCH_MPROTECT: ret = sys_mprotect((uint64_t)arg0, (size_t)arg1, (int)arg2); break;
	case SYS_ARCH_PRLIMIT64: ret = sys_prlimit64((pid_t)arg0, (int)arg1, (void*)arg2, (void*)arg3); break;
	/* ── process ─────────────────────────────────────────── */
	case SYS_ARCH_EXIT:        ret = sys_exit((int)arg0); break;
	case SYS_ARCH_EXIT_GROUP:  ret = sys_exit((int)arg0); break;
	case SYS_ARCH_GETPID:      ret = sys_getpid(); break;
	case SYS_ARCH_GETPPID:     ret = sys_getppid(); break;
	case SYS_ARCH_GETUID:      ret = sys_getuid(); break;
	case SYS_ARCH_GETEUID:     ret = sys_geteuid(); break;
	case SYS_ARCH_GETGID:      ret = sys_getgid(); break;
	case SYS_ARCH_GETEGID:     ret = sys_getegid(); break;
	case SYS_ARCH_SETUID:      ret = sys_setuid((uid_t)arg0); break;
	case SYS_ARCH_SETGID:      ret = sys_setgid((gid_t)arg0); break;
	case SYS_ARCH_SETPGID:     ret = sys_setpgid((pid_t)arg0, (pid_t)arg1); break;
	case SYS_ARCH_GETPGID:     ret = sys_getpgid((pid_t)arg0); break;
	case SYS_ARCH_SETSID:      ret = sys_setsid(); break;
	case SYS_ARCH_UMASK:       ret = sys_umask((mode_t)arg0); break;
	case SYS_ARCH_SET_TID_ADDR: ret = (long)task_current()->pid; break;
	case SYS_ARCH_KILL:        ret = sys_kill((pid_t)arg0, (int)arg1); break;
	case SYS_ARCH_TGKILL:     ret = sys_tgkill((pid_t)arg0, (pid_t)arg1, (int)arg2); break;
	case SYS_ARCH_FUTEX:      ret = sys_futex((uint32_t*)arg0, (int)arg1, (uint32_t)arg2, (linux_timespec_t*)arg3, (uint32_t*)arg4, (uint32_t)arg5); break;
	case SYS_ARCH_GETPGRP:    ret = (long)task_current()->pid; break;
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
		if (flags & CLONE_VM) {
			/* Thread: share address space */
			uint64_t newsp  = (uint64_t)arg1;
			uint32_t *ctid  = (flags & CLONE_CTID) ? (uint32_t*)arg3 : NULL;
			uint64_t tls    = (flags & CLONE_SETTLS) ? (uint64_t)arg4 : 0;
			ret = (long)proc_clone_thread(newsp, tls, ctid);
		} else if (flags & CLONE_VFORK) {
			ret = sys_vfork();
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
	case SYS_ARCH_READLINK: ret = sys_readlink((const char*)arg0, (char*)arg1, (size_t)arg2); break;
	case SYS_ARCH_RENAME:  ret = sys_rename((const char*)arg0, (const char*)arg1); break;
	case SYS_ARCH_CHMOD:   ret = sys_chmod((const char*)arg0, (mode_t)arg1); break;
	case SYS_ARCH_CHOWN:   ret = sys_chown((const char*)arg0, (uid_t)arg1, (gid_t)arg2); break;
	case SYS_ARCH_TRUNCATE: ret = sys_truncate((const char*)arg0, (off_t)arg1); break;
	case SYS_ARCH_GETPGRP: ret = sys_getpgrp(); break;
	case SYS_ARCH_ARCH_PRCTL: ret = sys_arch_prctl((int)arg0, (uint64_t)arg1); break;
	case SYS_ARCH_FUTEX:  ret = sys_futex((uint32_t*)arg0, (int)arg1, (uint32_t)arg2, (linux_timespec_t*)arg3, (uint32_t*)arg4, (uint32_t)arg5); break;
	case SYS_ARCH_TGKILL: ret = sys_tgkill((pid_t)arg0, (pid_t)arg1, (int)arg2); break;
	case SYS_ARCH_SET_ROBUST_LIST: ret = 0; break;
	case SYS_ARCH_GET_ROBUST_LIST: ret = -38; break;
	case SYS_ARCH_RT_SIGRETURN: sys_rt_sigreturn(ctx); return;
#endif
	default:
		ret = -38; /* ENOSYS */
		break;
	}

	CTX_SYSCALL_RET(ctx, ret);
	task_current()->ctx = ctx;
}
