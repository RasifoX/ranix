#include <stdint.h>

static inline int _syscall(int num, int a1, int a2, int a3) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a1), "c"(a2), "d"(a3));
    return ret;
}

void exit(int code) {
    _syscall(1, code, 0, 0);
}

int write(int fd, const char* buf, uint32_t len) {
    return _syscall(4, fd, (int)buf, (int)len);
}

void print(const char* str) {
    int len = 0;
    while(str[len]) len++;
    write(1, str, len);
}