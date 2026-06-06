#include <kernel/lib/string.h>
#include <kernel/hal.h>
#include <kernel/proc.h>
#include <kernel/mm/mmu.h>
#include <kernel/extable.h>
#include <asm/mmu.h>

extern uint64_t arch_vm_get_paddr(uint64_t *root, uint64_t vaddr);

/* Byte copy whose load/store fault to the exit label via __ex_table, so an
 * unmapped/bad user address returns the not-copied count instead of panicking.
 * n is decremented only after both accesses succeed, so on a fault it still
 * holds the bytes left to copy. */
static unsigned long __copy_user(void *dst, const void *src, unsigned long n)
{
	uaccess_begin();
#if defined(__x86_64__)
	__asm__ volatile(
	".Lcu_loop%=:\n"
	"	test	%[n], %[n]\n"
	"	jz	.Lcu_out%=\n"
	".Lcu_ld%=:\n"
	"	movb	(%[src]), %%al\n"
	".Lcu_st%=:\n"
	"	movb	%%al, (%[dst])\n"
	"	inc	%[src]\n"
	"	inc	%[dst]\n"
	"	dec	%[n]\n"
	"	jmp	.Lcu_loop%=\n"
	".Lcu_out%=:\n"
	_ASM_EXTABLE(".Lcu_ld%=", ".Lcu_out%=")
	_ASM_EXTABLE(".Lcu_st%=", ".Lcu_out%=")
	: [n] "+r"(n), [src] "+r"(src), [dst] "+r"(dst)
	:: "al", "memory", "cc");
#else
	__asm__ volatile(
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
	if (!access_ok(src, 1)) return -1;
	process_t *p = task_current();
	uint64_t cur_pg = (uint64_t)-1;
	uaccess_begin();
	for (size_t i = 0; i < n; i++) {
		uint64_t pg = ((uint64_t)src + i) & PAGE_MASK;
		if (pg != cur_pg) {
			if (p && p->vm && !arch_vm_get_paddr(p->vm->root, pg)) {
				uaccess_end();
				return -1;
			}
			cur_pg = pg;
		}
		dst[i] = src[i];
		if (src[i] == '\0') { uaccess_end(); return (long)i; }
	}
	uaccess_end();
	dst[n - 1] = '\0';
	return (long)n;
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