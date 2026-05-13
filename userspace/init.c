#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>

static void write_str(const char *s)
{
    int n = 0; while (s[n]) n++;
    write(1, s, n);
}

int main(void)
{
    /* Initialize standard FDs if they are not open */
    close(0); close(1); close(2);
    open("/dev/console", O_RDWR);
    dup(0);                  /* fd 1 */
    dup(0);                  /* fd 2 */

    write_str("[init] PID 1 started\n");
    
    /* Run a quick test command */
    pid_t test_pid = vfork();
    if (test_pid == 0) {
        char *argv[] = { "/bin/sh", "-c", "echo '*** BUSYBOX SHELL IS ALIVE ***'", NULL };
        char *envp[] = { "PATH=/bin:/mnt/usr/bin:/mnt/bin", "HOME=/", NULL };
        execve("/bin/sh", argv, envp);
        _exit(127);
    }
    int st;
    waitpid(test_pid, &st, 0);

    while (1) {
        write_str("[init] calling fork\n");
        pid_t pid = vfork();
        if (pid == 0) {
            char *argv[] = { "/bin/sh", NULL };
            char *envp[] = { "PATH=/bin:/mnt/usr/bin:/mnt/bin", "HOME=/", NULL };
            execve("/bin/sh", argv, envp);
            _exit(127);
        }
        write_str("[init] fork returned\n");
        if (pid < 0) {
            write_str("[init] fork failed, retrying\n");
            sleep(1);
            continue;
        }
        write_str("[init] parent: waiting for child\n");
        int status;
        waitpid(pid, &status, 0);
        write_str("[init] shell exited, respawning\n");
    }
}
