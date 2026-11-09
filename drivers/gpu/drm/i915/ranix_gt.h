/* SPDX-License-Identifier: MIT */
#ifndef RANIX_GT_H
#define RANIX_GT_H
#include <uapi/types.h>

int ranix_gt_command(const char *cmd, size_t len);   /* "audit", "rps on|off", "rps max <MHz>",
                                                     * "irq on|off", "spin <us>" */
int ranix_gt_status(char *o);
int ranix_power_status(char *o);                     /* one line, < 1000 bytes */
int gt_freq_mhz(uint32_t *act, uint32_t *req);       /* 0 while the engine is off */

/* the render engine for clients */
typedef struct gt_ctx gt_ctx_t;
int       gt_start(void);                      /* first use; 0, or <0 and the GT stays off */
int       gt_usable(void);
gt_ctx_t *gt_ctx_create(uint64_t pml4);         /* pml4 0: GGTT-only context */
void      gt_ctx_destroy(gt_ctx_t *c);          /* waits for its requests */
int       gt_submit(gt_ctx_t *c, uint64_t addr, uint32_t *seqno);
int       gt_wait_seqno(uint32_t seqno, int64_t timeout_ns);
int       gt_seqno_busy(uint32_t seqno);
uint32_t  gt_seqno_done(void);
uint32_t  gt_reset_count(void);
void      gt_tick(void);
#endif
