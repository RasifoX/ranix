#ifndef KERNEL_CONFIG_H
#define KERNEL_CONFIG_H

/* Scheduler */
#define SCHED_QUANTUM   10      /* timer ticks per process timeslice */

/* Memory */
#define KSTACK_SIZE     8192    /* kernel stack per process (bytes) */
#define DENTRY_MAX      4096    /* max cached dentries */

/* VFS */
#define FD_TABLE_INIT   16      /* initial fd table capacity */

#endif
