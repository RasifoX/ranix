#include <kernel/syscall.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <kernel/kprintf.h>
static long sys_write(int fd, const void *buf, size_t len)
{
	if (fd == 1 || fd == 2) {
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

static long sys_exit(int code)
{
	process_t *p = task_current();
	p->exit_code = code;
	task_exit();
	return 0;   /* unreachable */
}

static long sys_getpid(void)
{
	return (long)task_current()->pid;
}

void syscall_handle(arch_context_t *ctx)
{
	long n    = (long)CTX_SYSCALL_N(ctx);
	long arg0 = (long)CTX_SYSCALL_ARG0(ctx);
	long arg1 = (long)CTX_SYSCALL_ARG1(ctx);
	long arg2 = (long)CTX_SYSCALL_ARG2(ctx);
	long ret  = -1;

	switch (n) {
	case SYS_ARCH_READ:   ret = sys_read((int)arg0, (void*)arg1, (size_t)arg2); break;
	case SYS_ARCH_WRITE:  ret = sys_write((int)arg0, (void*)arg1, (size_t)arg2); break;
	case SYS_ARCH_EXIT:   ret = sys_exit((int)arg0); break;
	case SYS_ARCH_GETPID: ret = sys_getpid(); break;
	default:
		kprintf("[syscall] unknown #%d\n", (int)n);
		ret = -38;  /* -ENOSYS */
	}

	CTX_SYSCALL_RET(ctx, ret);
}
