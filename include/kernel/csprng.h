#ifndef KERNEL_CSPRNG_H
#define KERNEL_CSPRNG_H

#include <uapi/types.h>

/* ChaCha20-based DRBG. Seeded from the best entropy available (RDSEED/RDRAND on
 * x86, counter/jitter otherwise) and reseeded periodically. Replaces the old
 * splitmix64-from-ticks generator behind getrandom and /dev/urandom. */
void csprng_init(void);
void csprng_bytes(void *out, size_t len);
/* mix external entropy (e.g. interrupt timing) into the pool */
void csprng_add_entropy(const void *data, size_t len);

#endif
