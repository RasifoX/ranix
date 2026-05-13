#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

int main() {
    close(0); close(1); close(2);
    open("/dev/console", O_RDWR);
    dup(0);
    dup(0);
    
    struct stat st;
    if (stat("/bin/ls", &st) == 0) {
        printf("/bin/ls exists. mode=%x (S_ISREG=%d), uid=%d\n", st.st_mode, S_ISREG(st.st_mode), st.st_uid);
    } else {
        printf("stat(/bin/ls) failed: errno=%d\n", errno);
    }

    if (access("/bin/ls", X_OK) == 0) {
        printf("access(/bin/ls, X_OK) OK\n");
    } else {
        printf("access(/bin/ls, X_OK) failed: errno=%d\n", errno);
    }

    char *argv[] = { "ls", NULL };
    char *envp[] = { NULL };
    printf("execve(/bin/ls)...\n");
    execve("/bin/ls", argv, envp);
    printf("execve failed: errno=%d\n", errno);
    return 0;
}