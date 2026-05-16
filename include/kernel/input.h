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
void input_report_abs(uint16_t code, int32_t value);
void input_sync(void);
void input_inject_ascii(uint8_t c);

#endif
