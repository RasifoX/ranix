#ifndef INITRD_H
#define INITRD_H

#include <stdint.h>
#include "fs.h"

fs_node_t *initialise_initrd(uint32_t location, uint32_t end);

#endif