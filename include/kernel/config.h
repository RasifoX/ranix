#ifndef KERNEL_CONFIG_H
#define KERNEL_CONFIG_H

/* Scheduler */
#define SCHED_QUANTUM   10      /* timer ticks per process timeslice */

/* Memory */
#define KSTACK_SIZE     16384   /* kernel stack per process (bytes) */
#define DENTRY_MAX      4096    /* max cached dentries */

/* VFS */
#define FD_TABLE_INIT   16      /* initial fd table capacity */
#define PIPE_BUF_SIZE   4096    /* pipe ring buffer size (bytes) */
#define CMDLINE_MAX     512     /* kernel command line max length */

/* IPC */
#define FUTEX_SLOTS     256     /* max simultaneous futex waiters */

#endif
