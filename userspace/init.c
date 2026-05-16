#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

int main(void)
{
    close(0); close(1); close(2);
    open("/dev/console", O_RDWR);
    dup(0);
    dup(0);

    while (1) {
        pid_t pid = vfork();
        if (pid == 0) {
            char *bash_argv[] = { "/bin/bash", "--norc", "--noprofile", NULL };
            char *envp[] = { "PATH=/bin:/mnt/usr/bin:/mnt/bin", "HOME=/root", "PWD=/", "TERM=linux", NULL };
            execve("/bin/bash", bash_argv, envp);
            /* bash yoksa sh'a düş */
            char *sh_argv[] = { "/bin/sh", NULL };
            execve("/bin/sh", sh_argv, envp);
            _exit(127);
        }
        if (pid < 0) { sleep(1); continue; }
        int status;
        waitpid(pid, &status, 0);
    }
}
