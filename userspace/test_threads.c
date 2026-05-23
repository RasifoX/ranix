#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

static pid_t main_pid;
static volatile pid_t thread_gettid_val;
static volatile pid_t thread_getpid_val;

static void *thread_fn(void *arg)
{
    (void)arg;
    thread_gettid_val = (pid_t)syscall(SYS_gettid);
    thread_getpid_val = getpid();
    return NULL;
}

int main(void)
{
    main_pid = getpid();
    pid_t main_tid = (pid_t)syscall(SYS_gettid);

    if (main_pid != main_tid) {
        write(1, "FAIL:main-pid-ne-tid\n", 21);
        return 1;
    }

    pthread_t t;
    int rc = pthread_create(&t, NULL, thread_fn, NULL);
    if (rc != 0) {
        write(1, "FAIL:create\n", 12);
        return 1;
    }
    rc = pthread_join(t, NULL);
    if (rc != 0) {
        write(1, "FAIL:join\n", 10);
        return 1;
    }

    if (thread_gettid_val == main_tid) {
        write(1, "FAIL:tid-same\n", 14);
        return 1;
    }
    if (thread_getpid_val != main_pid) {
        write(1, "FAIL:tgid\n", 10);
        return 1;
    }

    write(1, "OK\n", 3);
    return 0;
}
