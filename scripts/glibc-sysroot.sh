#!/bin/sh
# A glibc for build hosts without one (a musl system such as Ranix's own root).
# The suite needs a stock loader, libc and GNU echo to test the glibc path, and
# these come from Debian's packages instead of the host.
#
#   glibc-sysroot.sh <cache-dir> <suite> <dest>
set -eu

CACHE="$1"; SUITE="$2"; DEST="$3"
MIRROR="http://deb.debian.org/debian"
DB="$CACHE/pkgdb-$SUITE-amd64.tsv"
DEBDIR="$CACHE/deb-glibc-amd64"

sh scripts/fetch-deb.sh "$CACHE" "$SUITE" amd64 hello >/dev/null   # builds the index
mkdir -p "$DEBDIR"
rm -rf "$DEST"; mkdir -p "$DEST"
for p in libc6 libc6-dev linux-libc-dev coreutils; do
	fn=$(grep -m1 "^$p	" "$DB" | cut -f2)
	[ -n "$fn" ] || { echo "ERROR: package not found: $p" >&2; exit 1; }
	out="$DEBDIR/$(basename "$fn")"
	if [ ! -s "$out" ]; then
		wget -q "$MIRROR/$fn" -O "$out" || { rm -f "$out"; echo "ERROR: download failed: $p" >&2; exit 1; }
	fi
	dpkg-deb -x "$out" "$DEST"
done
# Debian's link is absolute, which leaves the sysroot
ln -sf ../lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 "$DEST/lib64/ld-linux-x86-64.so.2"
touch "$DEST/.stamp"
