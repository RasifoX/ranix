/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef DRIVERS_INPUT_SYNAPTICS_H
#define DRIVERS_INPUT_SYNAPTICS_H
#if __has_include(<uapi/types.h>)
#include <uapi/types.h>
#else
#include <stdint.h>
#endif

/* Absolute-mode Synaptics packets turned into pointer motion, taps and scrolling.
 * Pure logic, no hardware: tests/host_synaptics.c drives it from a host program. */
typedef void (*syn_emit_t)(int dx, int dy, int wheel, int buttons);
/* Two-finger scrolling as it goes, 120 = one wheel notch: v up, h right (fingers'
 * direction); stop = the fingers left the pad after scrolling. */
typedef void (*syn_scroll_t)(int v120, int h120, int stop);

typedef struct {
	int      down, fingers, maxf, moved, clicked, buttons;
	int      lx, ly, rx, ry, sy, px, py;
	int      settle, moving, dragging, tapped;
	uint64_t t0, tap_t, click_t;
	int      fx, fy, ax, ay, lock, scrolled;   /* fine scroll: remainders, travel, axis lock */
	syn_scroll_t scroll;                       /* NULL: wheel notches only */
} syn_t;

extern int syn_tap;      /* a touch is a click */
extern int syn_tap3;     /* three fingers tapped or clicking: the middle button (else the right) */
extern int syn_middle;   /* both physical buttons together: the middle button */
void syn_packet(syn_t *s, const uint8_t p[6], uint64_t ms, syn_emit_t emit);
/* "tap 1", "tap3 0", "middle 1": 0 when set, -1 when not understood */
int syn_option(const char *k, unsigned long n);
#endif
