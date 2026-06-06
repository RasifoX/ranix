/* ptrace_may_access gate: a uid-1000 caller is denied PTRACE_ATTACH to a
 * process it doesn't own (pid 1, root) and cannot read that process's
 * /proc/<pid>/mem|maps, but may inspect a same-uid child's /proc/<pid>/maps.
 * Run via `su test -c /bin/test_ptraceperm`. Prints OK on success. */
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int file_nonempty(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    char b[64];
    ssize_t n = read(fd, b, sizeof(b));
    close(fd);
    return n > 0;
}

int main(void)
{
    if (geteuid() == 0) { puts("FAIL: running as root"); return 1; }

    /* 1. attaching to pid 1 (root) must fail with EPERM */
    errno = 0;
    if (ptrace(PTRACE_ATTACH, 1, NULL, NULL) == 0) {
        ptrace(PTRACE_DETACH, 1, NULL, NULL);
        puts("FAIL: attached to root pid 1");
        return 1;
    }
    if (errno != EPERM) { printf("FAIL: attach errno=%d want EPERM\n", errno); return 1; }

    /* 2. a root process's memory and maps must be unreadable */
    int fd = open("/proc/1/mem", O_RDONLY);
    if (fd >= 0) {
        char b; lseek(fd, 0x400000, SEEK_SET);
        if (read(fd, &b, 1) == 1) { close(fd); puts("FAIL: read /proc/1/mem"); return 1; }
        close(fd);
    }
    if (file_nonempty("/proc/1/maps")) { puts("FAIL: read root /proc/1/maps"); return 1; }

    /* 3. a same-uid child: the gate must permit inspecting its maps */
    pid_t child = fork();
    if (child < 0) { puts("FAIL: fork"); return 1; }
    if (child == 0) { sleep(30); _exit(0); }

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)child);
    int ok = file_nonempty(path);          /* own child → ptrace_may_access true */
    int own = file_nonempty("/proc/self/maps");
    kill(child, 9);
    waitpid(child, NULL, 0);

    if (!own) { puts("FAIL: own /proc/self/maps empty"); return 1; }
    if (!ok)  { puts("FAIL: child maps denied"); return 1; }

    puts("OK");
    return 0;
}
