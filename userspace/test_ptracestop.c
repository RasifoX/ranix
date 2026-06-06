#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>

/* PTRACE_ATTACH a child that is blocked mid-syscall (sleeping in a long
 * nanosleep), confirm the tracer gets the stop notification without wedging,
 * PTRACE_CONT resumes it, and a PTRACE_PEEKDATA works. alarm() bounds the run so
 * a regression is a test timeout, not a suite hang. */

static void on_alarm(int s) { (void)s; _exit(2); }

int main(void)
{
    signal(SIGALRM, on_alarm);
    alarm(15);

    /* sync pipe: child tells parent it is about to block in the syscall */
    int c2p[2];
    if (pipe(c2p)) { puts("FAIL: pipe"); return 1; }

    pid_t child = fork();
    if (child < 0) { puts("FAIL: fork"); return 1; }

    if (child == 0) {
        close(c2p[0]);
        char c = 'R';
        write(c2p[1], &c, 1);
        /* Block in a syscall — this is the sleeping/paused tracee the ATTACH
         * SIGSTOP must interrupt and stop cleanly. */
        for (;;) {
            struct timespec ts = { 100, 0 };
            nanosleep(&ts, NULL);
        }
        _exit(0);
    }

    close(c2p[1]);
    char c;
    if (read(c2p[0], &c, 1) != 1) { puts("FAIL: sync"); return 1; }
    /* give the child a moment to actually enter nanosleep */
    struct timespec ts = { 0, 50 * 1000 * 1000 };
    nanosleep(&ts, NULL);

    if (ptrace(PTRACE_ATTACH, child, NULL, NULL) < 0) {
        puts("FAIL: attach");
        kill(child, SIGKILL); waitpid(child, NULL, 0);
        return 1;
    }

    /* The ATTACH-induced SIGSTOP must stop the sleeping tracee and notify us. */
    int status;
    pid_t w = waitpid(child, &status, 0);
    if (w != child) { puts("FAIL: waitpid"); kill(child, SIGKILL); return 1; }
    if (!WIFSTOPPED(status)) {
        puts("FAIL: not-stopped");
        kill(child, SIGKILL); waitpid(child, NULL, 0);
        return 1;
    }

    /* PEEK a known-mapped word: read the stopped tracee's PC via GETREGS, then
     * PEEKDATA at it — confirms the tracer can read the tracee's memory. */
#if defined(__x86_64__)
    struct { unsigned long long r[27]; } regs;
#else
    struct { unsigned long long regs[31], sp, pc, pstate; } regs;
#endif
    memset(&regs, 0, sizeof(regs));
    if (ptrace(PTRACE_GETREGS, child, NULL, &regs) < 0) {
        puts("FAIL: getregs");
        kill(child, SIGKILL); waitpid(child, NULL, 0);
        return 1;
    }
#if defined(__x86_64__)
    uintptr_t pc = (uintptr_t)regs.r[16];   /* rip */
#else
    uintptr_t pc = (uintptr_t)regs.pc;
#endif
    errno = 0;
    long val = ptrace(PTRACE_PEEKDATA, child, (void *)(pc & ~7UL), NULL);
    if (val == -1 && errno != 0) {
        puts("FAIL: peek");
        kill(child, SIGKILL); waitpid(child, NULL, 0);
        return 1;
    }

    /* Resume and tear down. */
    if (ptrace(PTRACE_CONT, child, NULL, NULL) < 0) {
        puts("FAIL: cont");
        kill(child, SIGKILL); waitpid(child, NULL, 0);
        return 1;
    }

    /* child is back in its infinite nanosleep loop; kill and reap. */
    kill(child, SIGKILL);
    waitpid(child, &status, 0);

    puts("OK");
    return 0;
}
