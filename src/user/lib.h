#ifndef USER_LIB_H
#define USER_LIB_H

#include <stdint.h>
#include <stddef.h>

void exit(int code);
int read(int fd, char *buf, int count);
int write(int fd, const char *buf, int count);

void print(const char *str);
void putchar(char c);
char getchar();

int u_strcmp(const char* s1, const char* s2);
void u_memset(void* s, int c, size_t n);

#endif