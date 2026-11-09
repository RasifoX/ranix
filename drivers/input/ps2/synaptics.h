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

typedef struct {
	int      down, fingers, maxf, moved, clicked, buttons;
	int      lx, ly, rx, ry, sy, px, py;
	int      settle, moving, dragging, tapped;
	uint64_t t0, tap_t, click_t;
} syn_t;

extern int syn_tap;
void syn_packet(syn_t *s, const uint8_t p[6], uint64_t ms, syn_emit_t emit);
#endif
