#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(__x86_64__)
#include <sys/user.h>
typedef struct user_regs_struct ptrace_regs;
#define REG_PC(r) ((r).rip)
#else
/* aarch64 */
struct arm_pt_regs { unsigned long long regs[31]; unsigned long long sp, pc, pstate; };
typedef struct arm_pt_regs ptrace_regs;
#define REG_PC(r) ((r).pc)
#endif

int main(void)
{
    /* Two sync pipes: parent→child (go) and child→parent (ready) */
    int p2c[2], c2p[2];
    if (pipe(p2c) || pipe(c2p)) { puts("FAIL: pipe"); return 1; }

    pid_t child = fork();
    if (child < 0) { puts("FAIL: fork"); return 1; }

    if (child == 0) {
        close(p2c[1]); close(c2p[0]);
        /* Tell parent we are ready after PTRACE_TRACEME */
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        char c = 'R';
        write(c2p[1], &c, 1);       /* signal: TRACEME done */
        read(p2c[0], &c, 1);        /* wait: parent armed syscall tracing */
        close(p2c[0]); close(c2p[1]);
        /* Now do a bunch of syscalls so parent can catch one */
        for (int i = 0; i < 8; i++) getpid();
        _exit(0);
    }

    close(p2c[0]); close(c2p[1]);

    /* Wait for child's TRACEME */
    char c;
    if (read(c2p[0], &c, 1) != 1) { puts("FAIL: sync-read"); return 1; }

    /* Enable syscall tracing on child */
    if (ptrace(PTRACE_SYSCALL, child, NULL, NULL) < 0) {
        puts("FAIL: ptrace-syscall-set");
        write(p2c[1], "x", 1);
        waitpid(child, NULL, 0);
        return 1;
    }

    /* Release child */
    write(p2c[1], "x", 1);
    close(p2c[1]); close(c2p[0]);

    /* Wait for first syscall-entry stop */
    int status;
    pid_t w = waitpid(child, &status, 0);
    if (w != child) { puts("FAIL: waitpid-ret"); return 1; }

    if (!WIFSTOPPED(status)) {
        puts("FAIL: not-stopped");
        ptrace(PTRACE_CONT, child, NULL, NULL);
        waitpid(child, NULL, 0);
        return 1;
    }

    /* Read registers */
    ptrace_regs regs;
    memset(&regs, 0, sizeof(regs));
    if (ptrace(PTRACE_GETREGS, child, NULL, &regs) < 0) {
        puts("FAIL: getregs");
        ptrace(PTRACE_CONT, child, NULL, NULL);
        waitpid(child, NULL, 0);
        return 1;
    }

    if (REG_PC(regs) == 0) {
        puts("FAIL: pc-is-zero");
        ptrace(PTRACE_CONT, child, NULL, NULL);
        waitpid(child, NULL, 0);
        return 1;
    }

    /* Continue until child exits */
    ptrace(PTRACE_CONT, child, NULL, NULL);
    while (waitpid(child, &status, 0) > 0) {
        if (WIFEXITED(status) || WIFSIGNALED(status)) break;
        if (WIFSTOPPED(status))
            ptrace(PTRACE_CONT, child, NULL, NULL);
    }

    puts("OK");
    return 0;
}
