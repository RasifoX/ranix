#ifndef KERNEL_CMDLINE_H
#define KERNEL_CMDLINE_H

#include <uapi/types.h>

void        cmdline_init(const char *str);
const char *cmdline_str(void);

/* Return value of "key=<value>" token, written into buf (NUL-terminated).
   Returns buf on match, NULL if key not present.
   If key appears as a bare flag ("quiet") with no =, returns "". */
const char *cmdline_get(const char *key, char *buf, size_t bufsz);

/* Quick boolean test: is a bare flag present? */
int cmdline_has(const char *flag);

#endif
