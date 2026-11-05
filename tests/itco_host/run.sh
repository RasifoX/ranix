#!/bin/sh
# Build and run the Sunrise Point register model natively; any host compiler works.
set -e
D=$(cd "$(dirname "$0")" && pwd)
O=$(mktemp -d)
${CC:-gcc} -std=gnu11 -Wall -g -I"$D/stub" "$D/model.c" "$D/../../drivers/watchdog/itco.c" -o "$O/itco_model"
"$O/itco_model"
rc=$?
rm -rf "$O"
exit $rc
