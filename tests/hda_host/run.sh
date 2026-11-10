#!/bin/sh
# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
# Build and run the HD Audio model (controller + ALC269) natively; any host compiler works.
set -e
D=$(cd "$(dirname "$0")" && pwd)
O=$(mktemp -d)
${CC:-gcc} -std=gnu11 -Wall -Wextra -Wno-unused-function -g -fsanitize=address,undefined -I"$D/stub" "$D/model.c" -o "$O/hda_model"
rc=0; "$O/hda_model" || rc=$?
rm -rf "$O"
exit $rc
