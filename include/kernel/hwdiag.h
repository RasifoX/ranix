/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef KERNEL_HWDIAG_H
#define KERNEL_HWDIAG_H

#include <uapi/types.h>

/* /proc/hwdiag: raw hardware health readings for fjord doctor.
 * One line per object: "<class> <name> key=value key=value ...".
 * Drivers report only what the hardware measured; interpretation (is this a
 * cable fault?) belongs to userspace. A key that cannot be read is left out,
 * or the line carries available=0 reason=<word>. */

typedef struct { char *p; size_t rem; } hw_out_t;

void hw_begin(hw_out_t *o, const char *cls, const char *name);
void hw_kv(hw_out_t *o, const char *key, uint64_t v);
void hw_ks(hw_out_t *o, const char *key, const char *s);   /* s: no spaces */
void hw_end(hw_out_t *o);

typedef void (*hwdiag_fn)(hw_out_t *o);

/* Up to HWDIAG_MAX providers; called in process context from procfs reads. */
#define HWDIAG_MAX 8
void hwdiag_register(hwdiag_fn fn);
int  hwdiag_fill(char *buf, size_t sz);

#endif
