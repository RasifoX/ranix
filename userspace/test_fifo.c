/* named pipes (FIFO): mkfifo creates an S_IFIFO node; open() blocks until both
 * a reader and a writer are present (rendezvous), then data flows like a pipe.
 * Used by shells and assorted IPC. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define FIFO "/tmp/myfifo"

int main(void)
{
	unlink(FIFO);
	if (mkfifo(FIFO, 0666) != 0) { printf("fifo-fail mkfifo\n"); return 0; }

	struct stat s;
	if (stat(FIFO, &s) != 0 || !S_ISFIFO(s.st_mode)) { printf("fifo-fail stat\n"); return 0; }

	pid_t pid = fork();
	if (pid == 0) {
		int w = open(FIFO, O_WRONLY);          /* blocks until reader present */
		if (w < 0) _exit(2);
		if (write(w, "hello fifo", 10) != 10) _exit(3);
		close(w);
		_exit(0);
	}

	int r = open(FIFO, O_RDONLY);              /* blocks until writer present */
	if (r < 0) { printf("fifo-fail open-rd\n"); return 0; }
	char buf[16] = {0};
	ssize_t n = read(r, buf, sizeof(buf));
	close(r);
	if (n != 10 || memcmp(buf, "hello fifo", 10) != 0) { printf("fifo-fail data n=%zd [%s]\n", n, buf); return 0; }

	int st; waitpid(pid, &st, 0);
	printf("%s\n", (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? "OK" : "fifo-fail child");
	return 0;
}
