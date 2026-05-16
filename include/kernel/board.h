#ifndef BOARD_H
#define BOARD_H

#include <uapi/types.h>
#include <kernel/fb.h>

#define TIMER_HZ 100

typedef struct {
	const char *name;
	uint64_t    uart_base;
	uint64_t    gic_dist_base;
	uint64_t    gic_cpu_base;
	uint64_t    local_base;
	uint64_t    mbox_base;
	uint64_t    sd_base;
	uint32_t    bus_offset;
	uint64_t    mem_base;
	uint64_t    mem_size;
	uint32_t    fb_width;
	uint32_t    fb_height;
} board_config_t;

/* Phase 1 — UART only, safe to call before MMU or heap */
void board_early_init(void);

/* Phase 2 — Allocate framebuffer; must run before mmu_init().
 * Fills *out and returns 0 on success, -1 if no framebuffer. */
int board_fb_alloc(fb_info_t *out);

/* Phase 3 — Set up irqchip and timer; runs after heap_init(). */
void board_irq_init(void);

/* Phase 4 — Register platform drivers in driver framework. */
void board_drivers_register(void);

board_config_t *board_get_config(void);

/* Trigger a hardware reset / system reboot. Does not return. */
void board_reset(void);

#endif
