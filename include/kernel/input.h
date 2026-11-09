/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef KERNEL_INPUT_H
#define KERNEL_INPUT_H

#include <uapi/types.h>
#include <kernel/vfs.h>

#define EV_SYN  0x00
#define EV_KEY  0x01
#define EV_REL  0x02
#define EV_ABS  0x03

struct input_event {
	struct {
		int64_t tv_sec;
		int64_t tv_usec;
	} time;
	uint16_t type;
	uint16_t code;
	int32_t  value;
};

void input_core_init(void);
void input_report_key(uint16_t code, int32_t value);

/* What the keyboard's keys type: /sys/kernel/keymap, or ranix.kbd=tr at boot */
#define KEYMAP_US 0
#define KEYMAP_TR 1
extern int input_keymap;
void input_report_abs(uint16_t code, int32_t value);
void input_sync(void);
void input_inject_ascii(uint8_t c);

/* Where input came from (docs: arastirma/ajan-insan-ozellikleri.md, item 3).
 * hw: a real keyboard, pad, mouse, USB device or the serial console. injected:
 * bytes software wrote into /dev/input/rawkbd or /dev/input/mice (they come
 * back through the i8042 and would otherwise look like a person typing).
 * Drivers set the source around each byte they report; input_report_key()
 * puts an EV_MSC/MSC_RANIX_SOURCE value 1 in front of every injected key on
 * evdev, and /dev/input/source counts both kinds for the desk and fjord-sys. */
#define INPUT_SRC_HW    0
#define INPUT_SRC_INJECTED 1
#define EV_MSC           0x04
#define MSC_RANIX_SOURCE 0x06   /* free in Linux's MSC_* range; value 1 = injected */
void input_set_source(int src);
int  input_get_source(void);
void input_activity(int src, int pointer);   /* one key or pointer event happened */


#endif
