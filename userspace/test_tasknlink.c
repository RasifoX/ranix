/* /proc/self/task link count is 2 + threads, like Linux. Chromium's sandbox
 * (GPU process too, even with --no-sandbox) stops a thread and waits until
 * st_nlink of /proc/self/task says it is gone; a fixed 2 aborted it with
 * "Stopped thread does not disappear in /proc". */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

static int fd_[2];
static volatile pid_t tid_seen;

static void *th(void *a)
{
	char c;
	(void)a;
	tid_seen = gettid();
	if (read(fd_[0], &c, 1) < 0) return NULL;
	return NULL;
}

static long nl(void)
{
	struct stat st;
	return stat("/proc/self/task/", &st) == 0 ? (long)st.st_nlink : -1;
}

int main(void)
{
	if (nl() != 3) { printf("FAIL single %ld\n", nl()); return 0; }
	if (pipe(fd_)) { printf("FAIL pipe\n"); return 0; }
	pthread_t t;
	pthread_create(&t, NULL, th, NULL);
	usleep(50000);
	if (nl() != 4) { printf("FAIL two %ld\n", nl()); return 0; }
	char dir[64];
	struct stat ds;
	snprintf(dir, sizeof dir, "/proc/self/task/%d/", (int)tid_seen);
	if (stat(dir, &ds) != 0) { printf("FAIL thread dir missing\n"); return 0; }
	if (write(fd_[1], "x", 1) != 1) { printf("FAIL write\n"); return 0; }
	pthread_join(t, NULL);
	for (int i = 0; i < 100 && nl() != 3; i++) usleep(10000);
	if (nl() != 3) { printf("FAIL after join %ld\n", nl()); return 0; }
	if (stat(dir, &ds) == 0) { printf("FAIL exited thread still in task/\n"); return 0; }
	printf("OK\n");
	return 0;
}
