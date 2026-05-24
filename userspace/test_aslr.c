#include <sys/mman.h>
#include <stdio.h>

int main(void)
{
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { puts("mmap-failed"); return 1; }
    printf("%p\n", p);
    return 0;
}
