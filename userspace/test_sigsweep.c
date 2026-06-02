/* Signal semantics sweep: handler delivery, blocking via sigprocmask, sigpending
 * reporting, and sigaction oldact round-trip. Prints the first failure or OK. */
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n", __LINE__, #c); return 0; } } while (0)

static volatile int got;
static void handler(int s) { got = s; }

int main(void)
{
	struct sigaction sa, old;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handler;

	/* handler delivery: raise(SIGUSR1) runs the handler */
	got = 0;
	CHECK(sigaction(SIGUSR1, &sa, 0) == 0);
	CHECK(raise(SIGUSR1) == 0);
	CHECK(got == SIGUSR1);

	/* oldact round-trip: re-install and confirm the old handler is reported */
	struct sigaction sa2;
	memset(&sa2, 0, sizeof(sa2));
	sa2.sa_handler = SIG_IGN;
	CHECK(sigaction(SIGUSR1, &sa2, &old) == 0);
	CHECK(old.sa_handler == handler);

	/* blocking: block SIGUSR2, raise it, confirm it is pending and not yet
	 * delivered; then unblock and confirm delivery. */
	got = 0;
	CHECK(sigaction(SIGUSR2, &sa, 0) == 0);
	sigset_t block, oldset, pend;
	sigemptyset(&block);
	sigaddset(&block, SIGUSR2);
	CHECK(sigprocmask(SIG_BLOCK, &block, &oldset) == 0);
	CHECK(raise(SIGUSR2) == 0);
	CHECK(got == 0);                       /* blocked: not delivered yet */
	CHECK(sigpending(&pend) == 0);
	CHECK(sigismember(&pend, SIGUSR2) == 1);
	CHECK(sigprocmask(SIG_SETMASK, &oldset, 0) == 0);  /* unblock */
	CHECK(got == SIGUSR2);                 /* delivered on unblock */

	/* SIG_IGN: an ignored signal is discarded, not delivered */
	got = 0;
	CHECK(sigaction(SIGUSR1, &sa2, 0) == 0);  /* SIG_IGN */
	CHECK(raise(SIGUSR1) == 0);
	CHECK(got == 0);

	puts("OK");
	return 0;
}
