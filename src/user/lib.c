#include "lib.h"

static inline int syscall(int num, int a1, int a2, int a3)
{
    int ret;
    asm volatile("int $0x80"
                 : "=a"(ret)
                 : "a"(num), "b"(a1), "c"(a2), "d"(a3)
                 : "memory");
    return ret;
}

void exit(int code)
{
    syscall(1, code, 0, 0);
}

int read(int fd, char *buf, int count)
{
    return syscall(3, fd, (int)buf, count);
}

int write(int fd, const char *buf, int count)
{
    return syscall(4, fd, (int)buf, count);
}

void print(const char *str)
{
    int len = 0;
    while (str[len])
        len++;
    write(1, str, len);
}

void putchar(char c)
{
    write(1, &c, 1);
}

char getchar()
{
    char c = 0;
    while (read(0, &c, 1) <= 0)
        ;
    return c;
}

int u_strcmp(const char* s1, const char* s2)
{
    while (*s1 && (*s1 == *s2))
    {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

void u_memset(void* s, int c, size_t n)
{
    char *p = (char *)s;
    for (size_t i = 0; i < n; i++)
        p[i] = (char)c;
}