#!/bin/bash
# One KVM run. Under the shared Casper lock, in ~/ranix-nt, -smp 2 -m 2G (KILIT.md).
# usage: rig.sh TAG SECONDS GUESTSCRIPT "cmdline" -- qemu disk args...
export PATH=/usr/sbin:/sbin:$PATH
O=~/ranix-nt; T=$1; S=$2; G=$3; X=$4; shift 5
A=$O/t/run-$T; L=$A/serial.log
mkdir -p $A/ird $A/iso/boot/grub; rm -f $L
if [ ! -f $O/t/base.cpio ] || [ -n "$REBASE" ]; then cp $O/build/initramfs.cpio $O/t/base.cpio; fi
if [ ! -d $O/t/base ]; then mkdir $O/t/base; (cd $O/t/base; cpio -idm --quiet < $O/t/base.cpio 2>/dev/null); fi
rsync -a --delete $O/t/base/ $A/ird/
cp $O/t/ubin/* $A/ird/bin/ 2>/dev/null
cp $G $A/ird/test.sh; [ -n "$EXTRA" ] && cp -r $EXTRA $A/ird/
(cd $A/ird; find . | cpio -o -H newc --quiet) > $A/iso/boot/initramfs.cpio
cp ${KERNEL:-$O/build/ranix.elf} $A/iso/boot/ranix.elf
printf 'set timeout=0\nmenuentry "R" {\n multiboot2 /boot/ranix.elf ranix.deadline=%s panic=5 %s\n module2 /boot/initramfs.cpio\n}\n' $((S+30)) "$X" > $A/iso/boot/grub/grub.cfg
grub-mkrescue -o $A/i.iso $A/iso 2>/dev/null
timeout $((S+40)) sg kvm -c "qemu-system-x86_64 -enable-kvm -cpu host -m 2G -smp 2 -display none -serial file:$L -no-reboot -boot d -device isa-debug-exit,iobase=0x501,iosize=1 -cdrom $A/i.iso $*"
echo qemu=$?
