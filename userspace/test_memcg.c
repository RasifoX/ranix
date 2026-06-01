/* cgroup memory controller: a process in a cgroup with memory.max=1MB that
 * faults in 4MB is killed when the charge crosses the limit. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>

int main(void)
{
	mkdir("/cgroup/mtest", 0755);
	int fd = open("/cgroup/mtest/memory.max", O_WRONLY);
	if (fd < 0) { printf("memcg-fail open\n"); return 0; }
	write(fd, "1048576\n", 8);   /* 1 MB */
	close(fd);

	pid_t pid = fork();
	if (pid == 0) {
		fd = open("/cgroup/mtest/cgroup.procs", O_WRONLY);
		char b[16]; int n = sprintf(b, "%d\n", (int)getpid());
		write(fd, b, n); close(fd);
		/* fault in 4MB — must be killed once the charge passes 1MB */
		char *m = mmap(0, 4 * 1024 * 1024, PROT_READ | PROT_WRITE,
		               MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		for (int i = 0; i < 4 * 1024 * 1024; i += 4096) m[i] = 1;
		_exit(0);   /* reaching here means the limit was NOT enforced */
	}

	int st;
	waitpid(pid, &st, 0);
	printf("%s\n", WIFSIGNALED(st) ? "memcg-ok" : "memcg-fail");
	return 0;
}
