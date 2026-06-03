#!/bin/bash
# switch_root / disk-root proof: init pivots from the ramfs initramfs into a
# full rootfs on disk and runs it as the real /. Boot 1 writes a marker to /var
# on the disk root (DISKROOT:FRESH); boot 2 reuses the same image and the marker
# is still there (DISKROOT:SURVIVED) — proving / itself is now persistent disk,
# not ramfs. (x86_64 only — needs the AHCI disk target.)
set -u
cd "$(dirname "$0")/.."
LOG=build/serial-test.log

# Build the kernel/ISO and a fresh disk-root image.
make ARCH=x86_64 iso >/dev/null 2>&1 || { echo "[diskroot] iso build failed"; exit 1; }
rm -f build/diskroot.img
make ARCH=x86_64 build/diskroot.img >/dev/null 2>&1 || { echo "[diskroot] image build failed"; exit 1; }

boot() {
	rm -f "$LOG"
	timeout 180 qemu-system-x86_64 -cpu max \
	    -device isa-debug-exit,iobase=0x501,iosize=1 -smp 4 -m 3G -display none \
	    -serial file:"$LOG" -boot d -cdrom ranix.iso \
	    -drive id=d0,file=build/diskroot.img,format=raw,if=none \
	    -device ich9-ahci,id=ahci -device ide-hd,drive=d0,bus=ahci.0 >/dev/null 2>&1 || true
}

echo "[diskroot] boot 1 (fresh disk root)"
boot
ond1=$(grep -c 'DISKROOT:ONDISK:RANIX_DISK_ROOT' "$LOG")
b1=$(grep -o 'DISKROOT:\(FRESH\|SURVIVED:[A-Z_]*\)' "$LOG" | head -1)

echo "[diskroot] boot 2 (same disk, marker must survive)"
boot
b2=$(grep -o 'DISKROOT:\(FRESH\|SURVIVED:[A-Z_]*\)' "$LOG" | head -1)

echo "[diskroot] ondisk=$ond1 boot1=$b1 boot2=$b2"
if [ "$ond1" -ge 1 ] && [ "$b1" = "DISKROOT:FRESH" ] && [ "$b2" = "DISKROOT:SURVIVED:RANIX_DISKROOT_OK" ]; then
	echo "[diskroot] PROOF: PASS — init pivoted into the disk and / persists across reboot"
	exit 0
fi
echo "[diskroot] PROOF: FAIL"
echo "---- last serial log tail ----"; tail -20 "$LOG"
exit 1
