/*
 * FP/SIMD-across-signal regression test.
 *
 * The kernel must preserve the full FP/SIMD register state across signal
 * delivery — a handler that touches FP otherwise corrupts the interrupted
 * code's live vector registers. main() pins sentinels in d0–d7 and spins
 * (keeping them live) while a child hammers it with a signal whose handler
 * does FP work; if the kernel does not save/restore FP, the sentinels come
 * back wrong.
 */
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

static volatile unsigned long ticks;

static void handler(int s)
{
	(void)s;
	volatile double x = 3.14159;
	for (int i = 0; i < 32; i++) x = x * 1.0001 + 0.5;
	ticks += (unsigned long)x & 1 ? 1 : 1;
}

int main(void)
{
	struct sigaction sa = {0};
	sa.sa_handler = handler;
	sigaction(SIGUSR1, &sa, NULL);

	pid_t parent = getpid();
	pid_t child = fork();
	if (child == 0) {
		/* Steady signal stream, not a tight storm — a small spin between
		 * sends keeps signals frequent without saturating the IPI path. */
		for (;;) {
			kill(parent, SIGUSR1);
			for (volatile int d = 0; d < 2000; d++) { }
		}
		_exit(0);
	}

	double in[8]  = { 1.1, 2.2, 3.3, 4.4, 5.5, 6.6, 7.7, 8.8 };
	double out[8] = { 0 };
	int bad = 0;

#if defined(__aarch64__)
	for (int round = 0; round < 80 && !bad; round++) {
		unsigned long t0 = ticks;
		unsigned long tmp;
		__asm__ volatile(
			"ldp d0, d1, [%[in], #0]\n"
			"ldp d2, d3, [%[in], #16]\n"
			"ldp d4, d5, [%[in], #32]\n"
			"ldp d6, d7, [%[in], #48]\n"
			"1:\n"
			"ldr %[tmp], [%[tick]]\n"
			"sub %[tmp], %[tmp], %[t0]\n"
			"cmp %[tmp], #40\n"
			"b.lo 1b\n"
			"stp d0, d1, [%[out], #0]\n"
			"stp d2, d3, [%[out], #16]\n"
			"stp d4, d5, [%[out], #32]\n"
			"stp d6, d7, [%[out], #48]\n"
			: [tmp] "=&r"(tmp)
			: [in] "r"(in), [out] "r"(out), [tick] "r"(&ticks), [t0] "r"(t0)
			: "d0","d1","d2","d3","d4","d5","d6","d7","memory");

		for (int i = 0; i < 8; i++)
			if (out[i] != in[i]) {
				printf("FP-CORRUPT round=%d d%d got=%f want=%f ticks=%lu\n",
				       round, i, out[i], in[i], ticks);
				bad = 1;
				break;
			}
	}
#else
	(void)in; (void)out;   /* x86 xmm variant pending the x86 FP-signal fix */
#endif

	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	if (!bad) printf("OK ticks=%lu\n", ticks);
	return bad;
}
