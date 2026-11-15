/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/lib/string.h>
#include <kernel/hal.h>
#include <kernel/proc.h>
#include <kernel/mm/mmu.h>
#include <kernel/extable.h>
#include <asm/mmu.h>
#include <kernel/mm/vm.h>


/* User copy whose faulting access exits through __ex_table, so a bad user
 * address returns the count not copied instead of panicking. It was a byte loop:
 * every read, write and exec paid ~1 ns a byte (4 KB pread 3.7x Linux, HIZ0 11-15).
 * x86_64: rep movsb (ERMS); on a fault rcx is still the bytes left.
 * aarch64: 8 bytes at a time while 8 are left; an 8-byte access that faults
 * (it may straddle into an unmapped page) falls back to bytes, so the count
 * left stays exact. */
static unsigned long __copy_user(void *dst, const void *src, unsigned long n)
{
	uaccess_begin();
#if defined(__x86_64__)
	__asm__ volatile(
	".Lcu_rep%=:\n"
	"	rep movsb\n"
	".Lcu_out%=:\n"
	_ASM_EXTABLE(".Lcu_rep%=", ".Lcu_out%=")
	: "+c"(n), "+S"(src), "+D"(dst)
	:: "memory");
#else
	__asm__ volatile(
	".Lcu_w%=:\n"
	"	cmp	%[n], #8\n"
	"	b.lo	.Lcu_loop%=\n"
	".Lcu_wld%=:\n"
	"	ldr	x8, [%[src]]\n"
	".Lcu_wst%=:\n"
	"	str	x8, [%[dst]]\n"
	"	add	%[src], %[src], #8\n"
	"	add	%[dst], %[dst], #8\n"
	"	sub	%[n], %[n], #8\n"
	"	b	.Lcu_w%=\n"
	".Lcu_loop%=:\n"
	"	cbz	%[n], .Lcu_out%=\n"
	".Lcu_ld%=:\n"
	"	ldrb	w8, [%[src]]\n"
	".Lcu_st%=:\n"
	"	strb	w8, [%[dst]]\n"
	"	add	%[src], %[src], #1\n"
	"	add	%[dst], %[dst], #1\n"
	"	sub	%[n], %[n], #1\n"
	"	b	.Lcu_loop%=\n"
	".Lcu_out%=:\n"
	_ASM_EXTABLE(".Lcu_wld%=", ".Lcu_loop%=")
	_ASM_EXTABLE(".Lcu_wst%=", ".Lcu_loop%=")
	_ASM_EXTABLE(".Lcu_ld%=", ".Lcu_out%=")
	_ASM_EXTABLE(".Lcu_st%=", ".Lcu_out%=")
	: [n] "+r"(n), [src] "+r"(src), [dst] "+r"(dst)
	:: "x8", "memory", "cc");
#endif
	uaccess_end();
	return n;
}

int user_pages_mapped(const void *addr, size_t n)
{
	if (n == 0) return 1;
	process_t *p = task_current();
	if (!p || !p->vm) return 1;
	uint64_t pg  = (uint64_t)addr & PAGE_MASK;
	uint64_t end = ((uint64_t)addr + n - 1) & PAGE_MASK;
	for (; pg <= end; pg += PAGE_SIZE) {
		if (!arch_vm_get_paddr(p->vm->root, pg))
			return 0;
	}
	return 1;
}

#ifdef ARCH_USER_VA_END
#define USER_VA_END ARCH_USER_VA_END
#else
#define USER_VA_END 0x8000000000UL
#endif

int access_ok(const void *addr, size_t n)
{
	uint64_t start = (uint64_t)addr;
	uint64_t end   = start + n;

	/* All of user space sits in page-table slot 0 (see the note in vm.c). On x86_64
	 * VIRT_OFFSET is the kernel text, with the direct map of all RAM below it. */
	if (start >= USER_VA_END || end > USER_VA_END || end < start)
		return 0;
	return 1;
}

long copy_from_user(void *dst, const void *src, size_t n)
{
	if (!access_ok(src, n)) return -1;
	return __copy_user(dst, (void *)src, n) ? -1 : 0;
}

long copy_to_user(void *dst, const void *src, size_t n)
{
	if (!access_ok(dst, n)) return -1;
	return __copy_user(dst, (void *)src, n) ? -1 : 0;
}

long __get_user_n(void *dst, const void *src, size_t n)
{
	if (!access_ok(src, n)) return -1;
	return __copy_user(dst, (void *)src, n) ? -1 : 0;
}

long __put_user_n(void *dst, const void *src, size_t n)
{
	if (!access_ok(dst, n)) return -1;
	return __copy_user(dst, (void *)src, n) ? -1 : 0;
}

long copy_fop_to_user(void *dst, const void *src, size_t n)
{
	if (!access_ok(dst, n)) { memcpy(dst, src, n); return 0; }
	return __copy_user(dst, src, n) ? -1 : 0;
}

long copy_fop_from_user(void *dst, const void *src, size_t n)
{
	if (!access_ok(src, n)) { memcpy(dst, src, n); return 0; }
	return __copy_user(dst, src, n) ? -1 : 0;
}

long strncpy_from_user(char *dst, const char *src, size_t n)
{
	if (n == 0) return -1;
	/* Small chunks through the fault-fixed copy: a sibling thread can unmap the
	 * string at any moment, so a raw byte read here could panic. dst stays
	 * untouched past the NUL. */
	char tmp[64];
	for (size_t done = 0; done < n; ) {
		uint64_t a = (uint64_t)src + done;
		size_t chunk = PAGE_SIZE - (a & (PAGE_SIZE - 1));
		if (chunk > n - done) chunk = n - done;
		if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
		if (!access_ok((void *)a, chunk)) return -1;
		if (__copy_user(tmp, (void *)a, chunk)) return -1;
		for (size_t i = 0; i < chunk; i++) {
			dst[done + i] = tmp[i];
			if (tmp[i] == '\0') return (long)(done + i);
		}
		done += chunk;
	}
	dst[n - 1] = '\0';
	return (long)n;
}

void *memcpy(void *dst, const void *src, size_t n)
{
#if defined(__x86_64__)
    /* ERMS: one rep movsb beats the loops below from ~64 bytes, aligned or not
     * (HIZ0 11-15: the unaligned path was a byte loop) */
    if (n >= 64) {
        void *d0 = dst;
        __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) :: "memory");
        return d0;
    }
#endif
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
        if ((size_t)(s - d) >= n) return memcpy(dst, src, n);   /* apart: rep movsb may run out of order */
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
#if defined(__x86_64__)
    if (n >= 64) {
        void *d0 = dst;
        __asm__ volatile("rep stosb" : "+D"(dst), "+c"(n) : "a"(c) : "memory");
        return d0;
    }
#endif
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