/* SPDX-License-Identifier: MIT */
#ifndef RANIX_VM_H
#define RANIX_VM_H
#include "../drm_priv.h"
typedef struct { uint64_t pml4; } ppgtt_t;     /* physical address of the top table */
int  ppgtt_init(ppgtt_t *vm);
int  ppgtt_map(ppgtt_t *vm, uint64_t va, drm_gem_t *o);
void ppgtt_unmap(ppgtt_t *vm, uint64_t va, uint64_t size);
void ppgtt_fini(ppgtt_t *vm);
#endif
