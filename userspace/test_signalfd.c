#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

static int fail = 0;
static void check(const char *name, int ok)
{
    if (ok) printf("  OK: %s\n", name);
    else    { printf("  FAIL: %s\n", name); fail = 1; }
}

int main(void)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    int sfd = signalfd(-1, &mask, SFD_CLOEXEC);
    check("signalfd-create", sfd >= 0);
    if (sfd < 0) { puts("FAIL"); return 1; }

    int efd = epoll_create1(0);
    check("epoll-create", efd >= 0);

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = sfd };
    check("epoll-ctl-add", epoll_ctl(efd, EPOLL_CTL_ADD, sfd, &ev) == 0);

    kill(getpid(), SIGUSR1);

    struct epoll_event out;
    int n = epoll_wait(efd, &out, 1, 1000);
    check("epoll-wait-got-event", n == 1);
    check("epoll-event-is-sfd",   n == 1 && out.data.fd == sfd);

    struct signalfd_siginfo info;
    ssize_t r = read(sfd, &info, sizeof(info));
    check("signalfd-read",   r == (ssize_t)sizeof(info));
    check("signalfd-signo",  r == (ssize_t)sizeof(info) && info.ssi_signo == SIGUSR1);

    close(sfd);
    close(efd);
    printf(fail ? "FAIL\n" : "OK\n");
    return fail;
}
