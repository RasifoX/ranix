#include <kernel/lib/string.h>
#include <kernel/hal.h>
#include <asm/mmu.h>

int access_ok(const void *addr, size_t n)
{
	uint64_t start = (uint64_t)addr;
	uint64_t end   = start + n;

	/* User space must be below the kernel virtual offset */
	if (start >= VIRT_OFFSET || end > VIRT_OFFSET || end < start)
		return 0;
	return 1;
}

long copy_from_user(void *dst, const void *src, size_t n)
{
	if (!access_ok(src, n)) return -1;
	arch_user_access_begin();
	memcpy(dst, src, n);
	arch_user_access_end();
	return 0;
}

long copy_to_user(void *dst, const void *src, size_t n)
{
	if (!access_ok(dst, n)) return -1;
	arch_user_access_begin();
	memcpy(dst, src, n);
	arch_user_access_end();
	return 0;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    /* fast path: both pointers 8-byte aligned */
    if (((unsigned long)d | (unsigned long)s) % sizeof(unsigned long) == 0) {
        unsigned long       *dl = (unsigned long *)d;
        const unsigned long *sl = (const unsigned long *)s;
        while (n >= sizeof(unsigned long)) {
            *dl++ = *sl++;
            n   -= sizeof(unsigned long);
        }
        d = (unsigned char *)dl;
        s = (const unsigned char *)sl;
    }

    while (n--) *d++ = *s++;

    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (d < s) {
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }

    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    unsigned char  v = (unsigned char)c;

    unsigned long fill = v;
    fill |= fill << 8;
    fill |= fill << 16;
    fill |= fill << 32;

    /* fast path: aligned */
    if (((unsigned long)d % sizeof(unsigned long)) == 0) {
        unsigned long *dl = (unsigned long *)d;
        while (n >= sizeof(unsigned long)) {
            *dl++ = fill;
            n    -= sizeof(unsigned long);
        }
        d = (unsigned char *)dl;
    }

    while (n--) *d++ = v;

    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;

    while (n--) {
        if (*pa != *pb)
            return (int)*pa - (int)*pb;
        pa++;
        pb++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = '\0';
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst + strlen(dst);
    while ((*d++ = *src++));
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst + strlen(dst);
    while (n-- && *src) *d++ = *src++;
    *d = '\0';
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : (void *)0;
}

char *strrchr(const char *s, int c)
{
    const char *last = (void *)0;
    do {
        if (*s == (char)c) last = s;
    } while (*s++);
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;

    size_t nlen = strlen(needle);
    while (*haystack) {
        if (*haystack == *needle && memcmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
        haystack++;
    }
    return (void *)0;
}