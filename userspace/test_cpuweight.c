/* cgroup cpu controller: cpu.weight round-trips, and a process placed in a
 * weighted cgroup still schedules and runs to completion (the weight scales
 * its time slice without breaking the scheduler). */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

int main(void)
{
	mkdir("/cgroup/cw", 0755);
	int fd = open("/cgroup/cw/cpu.weight", O_WRONLY);
	if (fd < 0) { printf("cpuw-fail open\n"); return 0; }
	write(fd, "250\n", 4); close(fd);

	char buf[16] = {0};
	fd = open("/cgroup/cw/cpu.weight", O_RDONLY); read(fd, buf, sizeof(buf) - 1); close(fd);

	pid_t pid = fork();
	if (pid == 0) {
		int f = open("/cgroup/cw/cgroup.procs", O_WRONLY);
		char b[16]; int n = sprintf(b, "%d\n", (int)getpid());
		write(f, b, n); close(f);
		volatile long s = 0;
		for (long i = 0; i < 2000000; i++) s += i;   /* run with the weighted slice */
		_exit(s ? 7 : 0);
	}
	int st; waitpid(pid, &st, 0);

	if (strcmp(buf, "250\n") == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 7)
		printf("cpuw-ok\n");
	else
		printf("cpuw-fail w=[%s] st=%d\n", buf, WEXITSTATUS(st));
	return 0;
}
