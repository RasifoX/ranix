#include "string.h"
#include <stdint.h>

void *memcpy(void *dest, const void *src, size_t n)
{
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;

    if (n >= 4)
    {
        uint32_t *pdest32 = (uint32_t *)dest;
        const uint32_t *psrc32 = (const uint32_t *)src;
        size_t n32 = n / 4;

        while (n32--)
        {
            *pdest32++ = *psrc32++;
        }

        pdest = (uint8_t *)pdest32;
        psrc = (const uint8_t *)psrc32;
        n %= 4;
    }

    while (n--)
    {
        *pdest++ = *psrc++;
    }

    return dest;
}

void *memset(void *s, int c, size_t n)
{
    uint8_t *p = (uint8_t *)s;
    uint8_t val = (uint8_t)c;

    if (n >= 4)
    {
        uint32_t val32 = val | (val << 8) | (val << 16) | (val << 24);
        uint32_t *p32 = (uint32_t *)s;
        size_t n32 = n / 4;

        while (n32--)
        {
            *p32++ = val32;
        }

        p = (uint8_t *)p32;
        n %= 4;
    }

    while (n--)
    {
        *p++ = val;
    }

    return s;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;

    for (size_t i = 0; i < n; i++)
    {
        if (p1[i] != p2[i])
        {
            return p1[i] - p2[i];
        }
    }

    return 0;
}

size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len])
        len++;
    return len;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2))
    {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}