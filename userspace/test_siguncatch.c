/* SIGKILL and SIGSTOP are uncatchable: sigaction() must reject them (EINVAL),
 * while an ordinary signal like SIGTERM is accepted. */
#include <signal.h>
#include <stdio.h>

int main(void)
{
	struct sigaction sa;
	__builtin_memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;

	int k = sigaction(SIGKILL, &sa, 0);
	int s = sigaction(SIGSTOP, &sa, 0);
	int t = sigaction(SIGTERM, &sa, 0);

	puts((k < 0 && s < 0 && t == 0) ? "OK" : "FAIL");
	return 0;
}
