#!/bin/sh
# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
# Build and run the NVIDIA probe against its PCI/MMIO model natively; any host compiler works.
set -e
D=$(cd "$(dirname "$0")" && pwd)
O=$(mktemp -d)
trap 'rm -rf "$O"' EXIT
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I"$D/stub" "$D/model.c" \
	"$D/../../drivers/gpu/nvidia/nv_chip.c" "$D/../../drivers/gpu/nvidia/nv_probe.c" -o "$O/nv_model"
"$O/nv_model"
