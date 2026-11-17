#!/bin/bash
# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
# Swap under real pressure: a 256 MB machine fills twice its
# RAM with a pattern and gets every page back intact, first on zram (pages
# that compress), then on a swap disk (half the pages random). Then RAM and a
# small swap run out: the OOM killer ends the hog, and the machine stays up.
# The initramfs is the suite's, with the large files left out.
#
# Usage: ARCH=x86_64 bash tests/swapboot.sh   (after make ARCH=x86_64 initramfs build/ranix.elf)
set -u
cd "$(dirname "$0")/.."
B=build/swapboot
LOG=build/serial-swapboot.log
QEMU=${QEMU_CMD:-qemu-system-x86_64}
rm -rf "$B"; mkdir -p "$B/iso/boot/grub"
cp -a build/initramfs "$B/root"
find "$B/root" -type f -size +1M ! -name busybox ! -name bash ! -name test_swap ! -name init -delete
cp tests/swapboot_init.sh "$B/root/test.sh"
(cd "$B/root" && find . -print0 | cpio --null -o -H newc --owner=0:0 > ../iso/boot/initramfs.cpio 2>/dev/null)
rm -rf "$B/root"
echo "[swapboot] initramfs $(du -k "$B/iso/boot/initramfs.cpio" | cut -f1) kB"
cp build/ranix.elf "$B/iso/boot/ranix.elf"
printf 'set timeout=0\nset default=0\nmenuentry "Ranix" {\n  multiboot2 /boot/ranix.elf ranix.disk.rw=all\n  module2 /boot/initramfs.cpio\n}\n' > "$B/iso/boot/grub/grub.cfg"
grub-mkrescue -o "$B/swap.iso" "$B/iso" >/dev/null 2>&1
truncate -s 1G "$B/swapdisk.img"
rm -f "$LOG"
timeout 900 $QEMU -smp 2 -m 256M -display none -no-reboot -serial file:"$LOG" -boot d -cdrom "$B/swap.iso" \
    -drive file="$B/swapdisk.img",if=none,id=sw0,format=raw -device nvme,drive=sw0,serial=swap0 || true
rm -rf "$B"
grep -a -E "^SB|^FAIL|hog:|oom:|swap:|zram:" "$LOG" | head -40
if grep -aq "^SB-RESULT: 3/3" "$LOG" && ! grep -aqi "kernel panic" "$LOG"; then
	echo "[swapboot] PASS"
	exit 0
fi
echo "[swapboot] FAIL"
exit 1
