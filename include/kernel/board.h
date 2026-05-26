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

/* Per-CPU IRQ/timer init called by each secondary CPU after arch_secondary_init(). */
void board_secondary_irq_init(void);

/* Phase 4 — Register platform drivers in driver framework. */
void board_drivers_register(void);

board_config_t *board_get_config(void);

/* Iterate over all physical RAM regions. Calls cb(base, size, ud) for each. */
void board_foreach_mem_region(void (*cb)(uint64_t, uint64_t, void *), void *ud);

/* Return a platform-specific reserved region that must be excluded from the
 * buddy allocator (e.g. multiboot2 info on x86_64).  *size==0 if none. */
void board_get_reserved(uint64_t *base, uint32_t *size);

/* Trigger a hardware reset / system reboot. Does not return. */
void board_reset(void);

/* Power off the machine. Does not return. */
void board_poweroff(void);

#endif
