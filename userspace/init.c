#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

static void write_str(const char *s)
{
    int n = 0; while (s[n]) n++;
    write(1, s, n);
}

int main(void)
{
    write_str("[init] PID 1 started\n");
    while (1) {
        write_str("[init] calling fork\n");
        pid_t pid = vfork();
        if (pid == 0) {
            char *argv[] = { "/bin/sh", NULL };
            execve("/bin/sh", argv, NULL);
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
