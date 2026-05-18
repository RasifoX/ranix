#include <unistd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#define TIOCSPGRP 0x5410

int main(void)
{
    close(0); close(1); close(2);
    int tfd = open("/dev/console", O_RDWR);
    if (tfd != 0) { dup2(tfd, 0); close(tfd); }
    dup2(0, 1);
    dup2(0, 2);

    /* start echo_server in background */
    {
        pid_t spid = fork();
        if (spid == 0) {
            setsid();
            char *argv[] = { "/bin/echo_server", NULL };
            char *envp[] = { NULL };
            execve("/bin/echo_server", argv, envp);
            _exit(1);
        }
    }

    while (1) {
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            int t = open("/dev/console", O_RDWR);
            if (t >= 0) {
                dup2(t, 0); dup2(t, 1); dup2(t, 2);
                if (t > 2) close(t);
            }
            pid_t mypid = getpid();
            ioctl(0, TIOCSPGRP, &mypid);

            char *bash_argv[] = { "/bin/bash", "--norc", "--noprofile", NULL };
            char *envp[] = { "PATH=/bin:/mnt/usr/bin:/mnt/bin", "HOME=/root", "PWD=/", "TERM=linux", NULL };
            execve("/bin/bash", bash_argv, envp);
            char *sh_argv[] = { "/bin/sh", NULL };
            execve("/bin/sh", sh_argv, envp);
            _exit(127);
        }
        if (pid < 0) { sleep(1); continue; }
        int status;
        waitpid(pid, &status, 0);
    }
}
