#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char body[] = "RANIX-HTTP-OK\n";

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8080;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) return 1;
    if (listen(s, 4) < 0) return 1;
    for (;;) {
        int c = accept(s, 0, 0);
        if (c < 0) continue;
        char req[1024];
        read(c, req, sizeof(req));
        char hdr[128];
        int n = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.0 200 OK\r\nContent-Length: %zu\r\n"
                         "Content-Type: text/plain\r\n\r\n",
                         sizeof(body) - 1);
        write(c, hdr, n);
        write(c, body, sizeof(body) - 1);
        close(c);
    }
    return 0;
}
