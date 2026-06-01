#ifndef GPT_H
#define GPT_H

#include <kernel/block.h>

/* Parse the GPT of dev (header at LBA 1), register each used partition as
 * "<dev>p0", "<dev>p1", ... Returns 0 if at least one partition was found. */
int gpt_probe(block_device_t *dev);

#endif
