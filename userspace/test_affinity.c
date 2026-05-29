#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#ifdef __aarch64__
#define SYS_SCHED_GETAFFINITY 123
#define SYS_SCHED_SETAFFINITY 122
#else
#define SYS_SCHED_GETAFFINITY 204
#define SYS_SCHED_SETAFFINITY 203
#endif

static int get_nproc(void)
{
    uint64_t mask = 0;
    long r = syscall(SYS_SCHED_GETAFFINITY, 0, sizeof(mask), &mask);
    if (r < 0) return -1;
    int n = 0;
    for (int i = 0; i < 64; i++)
        if (mask & (1ULL << i)) n++;
    return n;
}

static volatile int thread_ran[4];
static int nproc;

static void *worker(void *arg)
{
    int id = (int)(long)arg;
    if (id < 4) thread_ran[id] = 1;
    return NULL;
}

int main(void)
{
    nproc = get_nproc();
    if (nproc < 1) {
        puts("affinity-fail");
        return 1;
    }

    printf("nproc=%d\n", nproc);
    fflush(stdout);

    /* Launch nproc threads and verify they all run */
    int n = nproc < 4 ? nproc : 4;
    pthread_t tid[4];
    memset(thread_ran, 0, sizeof(thread_ran));

    for (int i = 0; i < n; i++)
        pthread_create(&tid[i], NULL, worker, (void *)(long)i);
    for (int i = 0; i < n; i++)
        pthread_join(tid[i], NULL);

    int ok = 1;
    for (int i = 0; i < n; i++)
        if (!thread_ran[i]) { ok = 0; break; }

    puts(ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
