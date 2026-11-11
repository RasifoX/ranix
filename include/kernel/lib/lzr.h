/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef LZR_H
#define LZR_H

#include <uapi/types.h>

/* LZR: a small LZ77 compressor written for zram (kernel/lib/lzr.c).
 * The stream is a list of tokens, each led by one control byte c:
 *   c < 0x80   c + 1 literal bytes follow (1..128)
 *   c >= 0x80  copy (c & 0x7F) + 4 bytes (4..131) from d bytes back in the
 *              output, d in the next two bytes, little endian (1..65535);
 *              the copy may overlap what it writes, so runs repeat.
 * Input up to LZR_MAX_IN bytes. The table is the caller's scratch. */
#define LZR_MAX_IN     65534
#define LZR_HASH_BITS  12
#define LZR_TABLE_SIZE (1u << LZR_HASH_BITS)

/* bytes written to out, or -1 when they would not fit in cap */
long lzr_compress(const uint8_t *in, size_t n, uint8_t *out, size_t cap, uint16_t *table);
/* bytes written to out, or -1 for a damaged stream or one over cap */
long lzr_decompress(const uint8_t *in, size_t n, uint8_t *out, size_t cap);

#endif
