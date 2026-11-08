#ifndef KERNEL_KOUT_H
#define KERNEL_KOUT_H

/* Tiny bounded text builder for kernel-made reports (rescue channel, claims,
 * kill watch). The kernel has no snprintf; this is enough for "key=value" lines
 * and JSON, and never writes past cap (the last byte stays a NUL). */

#include <uapi/types.h>

typedef struct { char *buf; size_t cap, len; } kout_t;

static inline void kout_init(kout_t *o, char *buf, size_t cap)
{ o->buf = buf; o->cap = cap; o->len = 0; if (cap) buf[0] = 0; }

static inline void kout_c(kout_t *o, char c)
{ if (o->len + 1 < o->cap) { o->buf[o->len++] = c; o->buf[o->len] = 0; } }

static inline void kout_s(kout_t *o, const char *s)
{ while (s && *s) kout_c(o, *s++); }

static inline void kout_sn(kout_t *o, const char *s, size_t n)
{ for (size_t i = 0; s && i < n && s[i]; i++) kout_c(o, s[i]); }

static inline void kout_u(kout_t *o, uint64_t v)
{ char t[21]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) kout_c(o, t[--n]); }

static inline void kout_i(kout_t *o, int64_t v)
{ if (v < 0) { kout_c(o, '-'); kout_u(o, (uint64_t)(-v)); } else kout_u(o, (uint64_t)v); }

static inline void kout_x(kout_t *o, uint64_t v)
{ static const char h[] = "0123456789abcdef"; char t[17]; int n = 0;
  do { t[n++] = h[v & 15]; v >>= 4; } while (v); kout_s(o, "0x"); while (n) kout_c(o, t[--n]); }

/* JSON string body (no quotes): escapes ", \ and control bytes */
static inline void kout_js(kout_t *o, const char *s, size_t max)
{
	static const char h[] = "0123456789abcdef";
	for (size_t i = 0; s && i < max && s[i]; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '"' || c == '\\') { kout_c(o, '\\'); kout_c(o, (char)c); }
		else if (c < 0x20) { kout_s(o, "\\u00"); kout_c(o, h[c >> 4]); kout_c(o, h[c & 15]); }
		else kout_c(o, (char)c);
	}
}

#endif
