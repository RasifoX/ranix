#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SOCK_PATH "/tmp/test_unix_path.sock"

int main(void)
{
    unlink(SOCK_PATH);

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { puts("FAIL: server-socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        puts("FAIL: bind");
        return 1;
    }
    if (listen(srv, 1) < 0) {
        puts("FAIL: listen");
        return 1;
    }

    pid_t child = fork();
    if (child < 0) { puts("FAIL: fork"); return 1; }

    if (child == 0) {
        close(srv);
        int cli = socket(AF_UNIX, SOCK_STREAM, 0);
        if (cli < 0) _exit(1);
        if (connect(cli, (struct sockaddr *)&addr, sizeof(addr)) < 0) _exit(1);
        const char *msg = "hello-unix";
        write(cli, msg, strlen(msg));
        close(cli);
        _exit(0);
    }

    int cli = accept(srv, NULL, NULL);
    if (cli < 0) {
        puts("FAIL: accept");
        waitpid(child, NULL, 0);
        return 1;
    }

    char buf[32] = {0};
    int n = read(cli, buf, sizeof(buf) - 1);

    int st;
    waitpid(child, &st, 0);
    close(cli);
    close(srv);
    unlink(SOCK_PATH);

    if (n <= 0 || strcmp(buf, "hello-unix") != 0) {
        printf("FAIL: got='%s'\n", buf);
        return 1;
    }
    puts("OK");
    return 0;
}
