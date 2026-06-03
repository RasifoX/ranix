#!/bin/bash
# Two-boot persistence proof: an ext2 write on the disk must survive a reboot.
#
# Boot 1 starts from a freshly formatted disk image, so the suite finds no
# /mnt/persist_marker, creates it (PERSISTMARK:FRESH), and powers off — QEMU
# flushes the image. Boot 2 reuses the SAME image (make won't regenerate an
# existing one), so the marker is already there (PERSISTMARK:SURVIVED). If the
# write never reached the backing device, boot 2 would say FRESH again.
#
# Usage: ARCH=x86_64 bash tests/persist.sh   (needs the AHCI/virtio disk target)
set -u
cd "$(dirname "$0")/.."
ARCH=${ARCH:-x86_64}
LOG=build/serial-test.log

run_boot() { make ARCH="$ARCH" test >/dev/null 2>&1 || true; }

echo "[persist] boot 1: fresh disk"
rm -f build/gpt-test.img            # force a clean, marker-free image
run_boot
b1=$(grep -o 'PERSISTMARK:[A-Z_:]*' "$LOG" | head -1)

echo "[persist] boot 2: same disk (not regenerated)"
run_boot
b2=$(grep -o 'PERSISTMARK:[A-Z_:]*' "$LOG" | head -1)

echo "[persist] boot1=$b1"
echo "[persist] boot2=$b2"
if [ "$b1" = "PERSISTMARK:FRESH" ] && [ "$b2" = "PERSISTMARK:SURVIVED:RANIX_PERSIST_OK" ]; then
	echo "[persist] PROOF: PASS — ext2 writes persist across reboot"
	exit 0
fi
echo "[persist] PROOF: FAIL"
exit 1
