/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef SHA256_H
#define SHA256_H

#include <uapi/types.h>

#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE  64

typedef struct {
	uint32_t h[8];
	uint64_t total;                  /* bytes hashed so far */
	uint8_t  block[SHA256_BLOCK_SIZE];
	uint32_t fill;                   /* bytes waiting in block */
} sha256_t;

void sha256_init(sha256_t *c);
void sha256_update(sha256_t *c, const void *data, size_t len);
void sha256_final(sha256_t *c, uint8_t out[SHA256_DIGEST_SIZE]);
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);

/* Checks crc32 and sha256 against published vectors. 0 when they match. */
int  hash_selftest(void);

#endif
