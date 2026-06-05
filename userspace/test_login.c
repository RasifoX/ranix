/* Full login chain over a pts: a session leader makes the pts its controlling
 * tty and execs `login test`; the parent answers the password prompt against
 * /etc/shadow ($6$ SHA-512 crypt) and confirms the session shell runs as uid
 * 1000. Exercises getty/login -> crypt(shadow) -> setuid -> interactive shell,
 * beyond su (which root performs without a password). alarm()-bounded; the
 * master is non-blocking so a stalled prompt can never wedge the read. */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#ifndef TIOCGPTN
#define TIOCGPTN   0x80045430
#endif
#ifndef TIOCSPTLCK
#define TIOCSPTLCK 0x40045431
#endif

static void on_alarm(int s) { (void)s; write(1, "FAIL timeout\n", 13); _exit(0); }

int main(void)
{
	signal(SIGALRM, on_alarm);
	alarm(15);

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
		execl("/bin/login", "login", "test", (char *)NULL);
		_exit(127);
	}

	fcntl(m, F_SETFL, O_NONBLOCK);
	char acc[2048]; int len = 0; acc[0] = 0;
	int sent_pw = 0, sent_id = 0, ok = 0;
	for (int i = 0; i < 60; i++) {
		char b[256];
		ssize_t r = read(m, b, sizeof(b));
		if (r > 0 && len < (int)sizeof(acc) - 1) {
			int c = r; if (c > (int)sizeof(acc) - 1 - len) c = sizeof(acc) - 1 - len;
			memcpy(acc + len, b, c); len += c; acc[len] = 0;
		}
		if (!sent_pw && strstr(acc, "assword")) { write(m, "ranixpw\n", 8); sent_pw = 1; }
		else if (sent_pw && !sent_id && strstr(acc, "\n")) { write(m, "id -u\n", 6); sent_id = 1; }
		if (sent_id && strstr(acc, "1000")) { ok = 1; write(m, "exit\n", 5); break; }
		usleep(200000);
	}

	puts(ok ? "OK" : "FAIL");
	kill(pid, 9);
	{ int st; waitpid(pid, &st, 0); }
	return 0;
}
