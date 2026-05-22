#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>

int main(void)
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) _exit(1);

    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = __builtin_bswap16(8080);
    addr.sin_addr.s_addr = 0;

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) _exit(1);
    if (listen(srv, 5) < 0) _exit(1);

    write(1, "echo-server listening :8080\n", 28);

    while (1) {
        int cli = accept(srv, NULL, NULL);
        if (cli < 0) continue;

        char buf[1024];
        int n;
        while ((n = (int)read(cli, buf, sizeof(buf))) > 0)
            write(cli, buf, (size_t)n);

        close(cli);
    }
}
