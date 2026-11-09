/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef CRC32_H
#define CRC32_H

#include <uapi/types.h>

/* CRC-32 (IEEE 802.3, reflected), the variant GPT mandates. */
uint32_t crc32(const void *data, size_t len);

#endif
