/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef KERNEL_INPUT_DEV_H
#define KERNEL_INPUT_DEV_H

#include <uapi/types.h>

/*
 * One input device, one node: /dev/input/eventN (N from 1; event0 is the
 * system's keyboard and mouse stream) and, for joysticks, /dev/input/jsN.
 * Both 0660 root:input. Each open gets every event from then on, so a game, the
 * FRC Driver Station under Wine and the desktop can all read one pad.
 *
 * A driver fills the capabilities, adds the device, reports values (only
 * changes are passed on) and ends each report with input_dev_sync. When the
 * hardware leaves, input_dev_gone hides the nodes; open readers get ENODEV and
 * the slot is reused once they close.
 */

#define INPUT_DEV_KEYS 0x300
typedef struct input_dev_info {
	char     name[64];
	uint16_t bustype, vendor, product, version;
	uint64_t keybits[INPUT_DEV_KEYS / 64];
	uint64_t absbits;
	int32_t  absmin[64], absmax[64];
	int      joystick;         /* also a /dev/input/jsN */
} input_dev_info_t;

struct input_dev;
struct input_dev *input_dev_add(const input_dev_info_t *info);
void input_dev_report(struct input_dev *d, uint16_t type, uint16_t code, int32_t value);
void input_dev_sync(struct input_dev *d);
void input_dev_gone(struct input_dev *d);

#endif
