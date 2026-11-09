#!/bin/sh
# Build and run the Casper EC mailbox model natively; any host compiler works.
set -e
D=$(cd "$(dirname "$0")" && pwd)
O=$(mktemp -d)
${CC:-gcc} -std=gnu11 -Wall -Wextra -g -I"$D/stub" "$D/model.c" "$D/../../drivers/platform/casper_ec.c" -o "$O/casper_ec_model"
rc=0; "$O/casper_ec_model" || rc=$?
rm -rf "$O"
exit $rc
