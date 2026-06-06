/* no_new_privs across exec: a uid-1000 caller that sets PR_SET_NO_NEW_PRIVS
 * must NOT gain uid 0 when it execs the suid-root helper, and the flag must
 * survive fork+exec. Run via `su test -c /bin/test_nnp`. Prints OK on success. */
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#endif
#ifndef PR_GET_NO_NEW_PRIVS
#define PR_GET_NO_NEW_PRIVS 39
#endif

/* run the suid helper, return the euid it reported */
static int helper_euid(void)
{
    int pfd[2];
    if (pipe(pfd)) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(pfd[1], 1);
        close(pfd[0]); close(pfd[1]);
        execl("/bin/test_suidhelper", "test_suidhelper", (char *)0);
        _exit(127);
    }
    close(pfd[1]);
    char buf[64]; int n = (int)read(pfd[0], buf, sizeof(buf) - 1);
    close(pfd[0]);
    waitpid(pid, NULL, 0);
    if (n <= 0) return -1;
    buf[n] = 0;
    char *eq = strchr(buf, '=');
    return eq ? atoi(eq + 1) : -1;
}

int main(void)
{
    if (geteuid() == 0) { puts("FAIL: running as root"); return 1; }

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) { puts("FAIL: prctl-set"); return 1; }
    if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) { puts("FAIL: prctl-get"); return 1; }

    /* direct exec of the suid helper must not elevate */
    int e = helper_euid();
    if (e != (int)geteuid()) { printf("FAIL: suid-elevated euid=%d\n", e); return 1; }

    /* fork: child inherits NNP, its exec must also stay unelevated */
    pid_t c = fork();
    if (c < 0) { puts("FAIL: fork"); return 1; }
    if (c == 0) {
        if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) _exit(2);
        int ce = helper_euid();
        _exit(ce == (int)geteuid() ? 0 : 3);
    }
    int st = 0;
    waitpid(c, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { puts("FAIL: child-elevated-or-lost-nnp"); return 1; }

    puts("OK");
    return 0;
}
