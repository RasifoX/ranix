#ifndef KERNEL_BUG_H
#define KERNEL_BUG_H

#include <kernel/kprintf.h>

/* Kernel-invariant violations are bugs in the kernel itself: make them loud
 * (the branded panic screen) rather than limping on in a corrupted state. The
 * file:line is baked into the message so a photo of the panic pinpoints it.
 * (External errors — userspace, drivers, I/O, OOM — must be contained, not
 * BUG()'d.) */

#define _BUG_STR2(x) #x
#define _BUG_STR(x)  _BUG_STR2(x)
#define _BUG_AT      "  @ " __FILE__ ":" _BUG_STR(__LINE__)

#define BUG(msg)        panic_report("BUG: " msg _BUG_AT, 0)
#define BUG_ON(cond)    do { if (cond)    panic_report("BUG_ON(" #cond ")" _BUG_AT, 0); } while (0)
#define KASSERT(cond)   do { if (!(cond)) panic_report("assert(" #cond ")" _BUG_AT, 0); } while (0)

#endif
