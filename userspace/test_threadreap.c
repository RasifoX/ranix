/* A joined thread gives back its pid. If exited threads wait for a parent that
 * never collects them, a long-lived process exhausts the pid space and nothing on
 * the machine can fork. Threads are also not children: wait(-1) must skip them. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define ROUNDS 33000L       /* past PID_MAX */

static void *quick(void *a) { return a; }

int main(void)
{
	pthread_t t;
	int st;
	if (pthread_create(&t, NULL, quick, NULL)) { puts("FAIL create"); return 1; }
	pthread_join(t, NULL);
	errno = 0;
	if (waitpid(-1, &st, WNOHANG) != -1 || errno != ECHILD) {
		printf("FAIL thread seen by wait, errno %d\n", errno);
		return 1;
	}
	pid_t c = fork();
	if (c == 0) _exit(7);
	if (pthread_create(&t, NULL, quick, NULL)) { puts("FAIL create"); return 1; }
	pthread_join(t, NULL);
	if (waitpid(-1, &st, 0) != c || !WIFEXITED(st) || WEXITSTATUS(st) != 7) { puts("FAIL child"); return 1; }
	for (long i = 0; i < ROUNDS; i++) {
		int r = pthread_create(&t, NULL, quick, NULL);
		if (r) { printf("FAIL at %ld: %s\n", i, strerror(r)); return 1; }
		pthread_join(t, NULL);
	}
	puts("OK");
	return 0;
}
