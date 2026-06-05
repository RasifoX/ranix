/* terminal job control over a pty. A session-leader child (L) owns the pty as
 * its controlling terminal and runs the job-control checks against background
 * grandchildren whose pgrp is non-orphaned (parent L is in the same session,
 * different pgrp): a background read stops with SIGTTIN, a background write with
 * TOSTOP stops with SIGTTOU, the foreground job proceeds, and a master hangup
 * delivers SIGHUP to the foreground group. Everything is alarm-bounded so a
 * stuck stop or a missing wakeup can never wedge the suite. */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#ifndef TIOCGPTN
#define TIOCGPTN  0x80045430
#endif
#ifndef TIOCSPTLCK
#define TIOCSPTLCK 0x40045431
#endif

static int open_pts(int *mp)
{
	int m = open("/dev/ptmx", O_RDWR);
	if (m < 0) return -1;
	int n = -1;
	if (ioctl(m, TIOCGPTN, &n) != 0 || n < 0) return -1;
	int zero = 0; ioctl(m, TIOCSPTLCK, &zero);
	char path[32];
	snprintf(path, sizeof(path), "/dev/pts/%d", n);
	int s = open(path, O_RDWR);
	if (s < 0) { close(m); return -1; }
	*mp = m;
	return s;
}

static void say(const char *w) { printf("%s\n", w); }
static void gate_wait(int fd) { char b; read(fd, &b, 1); }
static void gate_go(int fd)   { write(fd, "g", 1); }

/* The session leader: owns master m + slave s, with s as its controlling tty
 * and L's own pgrp as the initial foreground group. */
static int session_body(int m, int s)
{
	if (setsid() < 0) { say("FAIL setsid"); return 1; }
	if (ioctl(s, TIOCSCTTY, 0) != 0) { say("FAIL sctty"); return 1; }
	pid_t lead = getpgrp();
	ioctl(s, TIOCSPGRP, &lead);   /* foreground = the leader's group */

	/* ── background read → SIGTTIN ───────────────────────────────────────── */
	int dn[2]; if (pipe(dn) != 0) { say("FAIL pipe"); return 1; }
	pid_t b = fork();
	if (b == 0) {
		setpgid(0, 0);            /* own pgrp, != foreground → background */
		gate_wait(dn[0]);
		alarm(15);
		char buf[4];
		read(s, buf, sizeof(buf));   /* background read → SIGTTIN (stop) */
		_exit(42);
	}
	setpgid(b, b);
	gate_go(dn[1]);

	int st = 0;
	if (waitpid(b, &st, WUNTRACED) != b || !WIFSTOPPED(st) || WSTOPSIG(st) != SIGTTIN) {
		say("FAIL ttin-stop"); return 1;
	}
	ioctl(s, TIOCSPGRP, &b);      /* b becomes the foreground group */
	kill(b, SIGCONT);
	write(m, "hi\n", 3);
	if (waitpid(b, &st, 0) != b || !WIFEXITED(st) || WEXITSTATUS(st) != 42) {
		say("FAIL ttin-resume"); return 1;
	}
	ioctl(s, TIOCSPGRP, &lead);

	/* ── background write with TOSTOP → SIGTTOU ──────────────────────────── */
	struct termios tio;
	tcgetattr(s, &tio); tio.c_lflag |= TOSTOP; tcsetattr(s, TCSANOW, &tio);

	int dn2[2]; if (pipe(dn2) != 0) { say("FAIL pipe2"); return 1; }
	pid_t b2 = fork();
	if (b2 == 0) {
		setpgid(0, 0);
		gate_wait(dn2[0]);
		alarm(15);
		write(s, "x", 1);        /* background write + TOSTOP → SIGTTOU (stop) */
		_exit(7);
	}
	setpgid(b2, b2);
	gate_go(dn2[1]);

	if (waitpid(b2, &st, WUNTRACED) != b2 || !WIFSTOPPED(st) || WSTOPSIG(st) != SIGTTOU) {
		say("FAIL ttou-stop"); return 1;
	}
	ioctl(s, TIOCSPGRP, &b2);
	kill(b2, SIGCONT);
	if (waitpid(b2, &st, 0) != b2 || !WIFEXITED(st) || WEXITSTATUS(st) != 7) {
		say("FAIL ttou-resume"); return 1;
	}
	ioctl(s, TIOCSPGRP, &lead);

	/* ── foreground read succeeds (sanity) ───────────────────────────────── */
	write(m, "yo\n", 3);
	char fb[4];
	if (read(s, fb, sizeof(fb)) <= 0) { say("FAIL fg-read"); return 1; }

	return 0;
}

int main(void)
{
	alarm(25);

	int m, s = open_pts(&m);
	if (s < 0) { say("FAIL pts-open"); return 0; }

	pid_t lead = fork();
	if (lead == 0) _exit(session_body(m, s));
	if (lead < 0) { say("FAIL fork"); return 0; }

	int st = 0;
	if (waitpid(lead, &st, 0) != lead || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		/* the leader already printed the specific FAIL line */
		if (WIFEXITED(st) && WEXITSTATUS(st) != 0) return 0;
		say("FAIL leader"); return 0;
	}
	close(s); close(m);

	/* ── hangup: closing the master SIGHUPs the foreground group ─────────── */
	int m3, s3 = open_pts(&m3);
	if (s3 < 0) { say("FAIL pts3"); return 0; }
	int g3[2]; if (pipe(g3) != 0) { say("FAIL pipe3"); return 0; }
	pid_t c3 = fork();
	if (c3 == 0) {
		close(m3);                /* don't pin the master; only the parent closes it */
		setsid();
		ioctl(s3, TIOCSCTTY, 0);
		signal(SIGHUP, SIG_DFL);
		gate_go(g3[1]);
		alarm(15);
		char b[4];
		read(s3, b, sizeof(b));   /* blocks; the hangup interrupts then kills us */
		_exit(99);
	}
	if (c3 < 0) { say("FAIL fork3"); return 0; }
	gate_wait(g3[0]);
	usleep(50000);
	close(s3);
	close(m3);                    /* last master fd → hangup → SIGHUP to fg pgrp */
	if (waitpid(c3, &st, 0) != c3 || !WIFSIGNALED(st) || WTERMSIG(st) != SIGHUP) {
		say("FAIL hangup"); return 0;
	}

	say("OK");
	return 0;
}
