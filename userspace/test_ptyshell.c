/* interactive shell over a pts: open ptmx, get slave, fork a session leader that
 * makes the pts its controlling tty and execs an interactive -sh; the parent
 * drives the master, types a command, and checks the echoed/output bytes.
 * Bounded by alarm() so a stuck line discipline can never hang the suite. */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#ifndef TIOCGPTN
#define TIOCGPTN   0x80045430
#endif
#ifndef TIOCSPTLCK
#define TIOCSPTLCK 0x40045431
#endif

static void on_alarm(int s) { (void)s; write(2, "FAIL timeout\n", 13); _exit(0); }

int main(void)
{
	signal(SIGALRM, on_alarm);
	alarm(10);

	int m = open("/dev/ptmx", O_RDWR);
	if (m < 0) { puts("FAIL ptmx"); return 0; }
	int n = -1;
	if (ioctl(m, TIOCGPTN, &n) != 0 || n < 0) { puts("FAIL ptn"); return 0; }
	int zero = 0; ioctl(m, TIOCSPTLCK, &zero);

	char path[32];
	snprintf(path, sizeof(path), "/dev/pts/%d", n);

	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		int s = open(path, O_RDWR);
		if (s < 0) _exit(127);
		ioctl(s, TIOCSCTTY, 0);
		dup2(s, 0); dup2(s, 1); dup2(s, 2);
		if (s > 2) close(s);
		close(m);
		execl("/bin/sh", "-sh", (char *)NULL);
		_exit(127);
	}

	/* Give the shell a moment, drain its prompt, then type a command. */
	usleep(200000);
	const char *cmd = "echo RDY$((20+2))\n";
	write(m, cmd, strlen(cmd));

	char acc[4096]; size_t got = 0;
	while (got < sizeof(acc) - 1) {
		char b[256];
		ssize_t r = read(m, b, sizeof(b));
		if (r <= 0) break;
		memcpy(acc + got, b, (size_t)r); got += (size_t)r;
		acc[got] = 0;
		if (strstr(acc, "RDY22")) break;
	}
	acc[got] = 0;

	const char *exit_cmd = "exit\n";
	write(m, exit_cmd, strlen(exit_cmd));

	int st; waitpid(pid, &st, 0);

	if (strstr(acc, "RDY22")) puts("OK");
	else puts("FAIL no-output");
	return 0;
}
