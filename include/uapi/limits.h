#ifndef UAPI_LIMITS_H
#define UAPI_LIMITS_H

#define PATH_MAX        4096   /* longest pathname, incl. NUL */
#define NAME_MAX        255    /* longest single path component */

/* execve string area: total bytes of argv+envp (incl. NULs). Linux caps a
 * single arg at MAX_ARG_STRLEN and the whole block at ~1/4 the stack rlimit;
 * here it is bounded by the arg staging window mapped onto the new stack. */
#define MAX_ARG_STRLEN  (32 * 1024)
#define ARG_MAX         (256 * 1024)

#endif
