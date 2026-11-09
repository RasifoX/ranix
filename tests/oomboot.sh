#!/bin/bash
# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
# An initramfs bigger than memory must not take the kernel down. The boot
# initramfs gets a file that cannot fit next to its own archive in a 768 MB
# machine; the kernel leaves that file out, says so, and boots: /test.sh runs.
# Before, the half-written file kept every page it got, init could not even be
# read in, and the boot ended in a panic.
#
# Usage: ARCH=x86_64 bash tests/oomboot.sh   (after make ARCH=x86_64 initramfs build/ranix.elf)
set -u
cd "$(dirname "$0")/.."
B=build/oomboot
LOG=build/serial-oomboot.log
QEMU=${QEMU_CMD:-qemu-system-x86_64}
rm -rf "$B"; mkdir -p "$B/iso/boot/grub"
cp -a build/initramfs "$B/root"
printf '#!/bin/bash\necho OOMBOOT-OK\n[ -e /zz-filler ] && echo OOMBOOT-FILLER-PRESENT\npoweroff -f\n' > "$B/root/test.sh"
head -c 300M /dev/zero > "$B/root/zz-filler"
(cd "$B/root" && find . -print0 | cpio --null -o -H newc --owner=0:0 > ../iso/boot/initramfs.cpio 2>/dev/null)
rm -rf "$B/root"
cp build/ranix.elf "$B/iso/boot/ranix.elf"
printf 'set timeout=0\nset default=0\nmenuentry "Ranix" {\n  multiboot2 /boot/ranix.elf\n  module2 /boot/initramfs.cpio\n}\n' > "$B/iso/boot/grub/grub.cfg"
grub-mkrescue -o "$B/oom.iso" "$B/iso" >/dev/null 2>&1
rm -f "$LOG"
timeout 300 $QEMU -smp 2 -m 768M -display none -no-reboot -serial file:"$LOG" -boot d -cdrom "$B/oom.iso" || true
rm -rf "$B"
grep -a "initrd:" "$LOG" | grep -v "buddy out of memory" | head -3
if grep -aq "OOMBOOT-OK" "$LOG" && ! grep -aq "kernel panic" "$LOG"; then
	echo "[oomboot] PASS: booted, the file that did not fit was left out"
	exit 0
fi
grep -a -E "stop |where |who " "$LOG" | head -3
echo "[oomboot] FAIL"
exit 1
