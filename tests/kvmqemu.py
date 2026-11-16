#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
"""QEMU with -enable-kvm inside Ranix: an unmodified VMM on /dev/kvm.

Ranix boots in a VM whose CPU has VMX (-cpu host on a VT-x host); its initramfs
carries Alpine's qemu-system-x86_64 and a 64 KB firmware image. Inside, QEMU
runs that firmware as a KVM guest: the reset vector jumps to code that writes a
line to the emulated COM1, which QEMU prints, and leaves through isa-debug-exit,
whose code QEMU exits with. The line and the exit code show the guest ran on
Ranix's KVM, through QEMU's own start-up (capabilities, CPUID, MSRs, memory
slots, vcpu state) and its port I/O devices.

The machine is isapc with kernel-irqchip=off: QEMU keeps its interrupt
controllers in userspace, and refuses a userspace local APIC with KVM on the
machines that have one.

QEMU comes from Alpine (v3.21, the same release Ranix's apk tests use), fetched
once into .cache with the apk.static the build already has.

Usage: python3 tests/kvmqemu.py   (after make ARCH=x86_64 iso; QEMU_CMD as for
make test, with -enable-kvm -cpu host)"""
import os, shlex, shutil, signal, subprocess, sys, time

B = "build/kvmqemu"
QEMU = shlex.split(os.environ.get("QEMU_CMD", "qemu-system-x86_64 -enable-kvm -cpu host"))
ALPINE = "https://dl-cdn.alpinelinux.org/alpine/v3.21"
APK = ".cache/apk-x86_64"
QROOT = ".cache/qemu-alpine-x86_64"
MARK = "KVMQEMU-ICERDE-4127"

FIRMWARE = r'''
	.code16
	.text
	.org 0xE000
start:
	mov %%cs, %%ax
	mov %%ax, %%ds
	mov $0x3f8, %%dx
	mov $msg, %%si
1:	lodsb
	test %%al, %%al
	jz 2f
	out %%al, %%dx
	jmp 1b
2:	mov $0x501, %%dx
	mov $0x10, %%al
	out %%al, %%dx
	cli
3:	hlt
	jmp 3b
msg:	.asciz "%(mark)s\n"
	.org 0xFFF0
	ljmp $0xF000, $start
	.org 0x10000
'''

GUEST = r'''#!/bin/bash
echo "KVMQ basla"
L="/alp/lib/ld-musl-x86_64.so.1 --library-path /alp/usr/lib:/alp/lib"
cd /tmp
timeout 60 $L /alp/usr/bin/qemu-system-x86_64 -enable-kvm -machine isapc,accel=kvm,kernel-irqchip=off \
    -nodefaults -display none -serial stdio -m 16 -bios /fw.bin \
    -device isa-debug-exit,iobase=0x501,iosize=1 > /tmp/q.out 2>&1
echo "KVMQ rc $?"
sed 's/^/KVMQ out /' /tmp/q.out
echo KVMQ-SON
'''


def fetch_qemu():
    if os.path.exists(f"{QROOT}/usr/bin/qemu-system-x86_64"):
        return
    tmp = QROOT + ".tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(f"{tmp}/etc/apk")
    subprocess.run([f"{APK}/apk.static", "--root", os.path.abspath(tmp), "--keys-dir", os.path.abspath(f"{APK}/keys"),
                    "--arch", "x86_64", "--initdb", "--no-scripts", "--no-cache",
                    "-X", f"{ALPINE}/main", "-X", f"{ALPINE}/community", "add", "qemu-system-x86_64"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)   # as non-root it cannot chown: not needed
    if not os.path.exists(f"{tmp}/usr/bin/qemu-system-x86_64"):
        sys.exit("kvmqemu: could not fetch Alpine's qemu-system-x86_64")
    os.rename(tmp, QROOT)


def main():
    fetch_qemu()
    shutil.rmtree(B, ignore_errors=True)
    os.makedirs(B)
    open(f"{B}/fw.S", "w").write(FIRMWARE % {"mark": MARK})
    subprocess.run(["as", "--32", "-o", f"{B}/fw.o", f"{B}/fw.S"], check=True)
    subprocess.run(["objcopy", "-O", "binary", f"{B}/fw.o", f"{B}/fw.bin"], check=True)

    root = f"{B}/root"
    shutil.copytree("build/initramfs", root, symlinks=True, copy_function=os.link)
    for f in ("test.sh", "reboot-on-exit"):
        if os.path.lexists(f"{root}/{f}"):
            os.unlink(f"{root}/{f}")
    # QEMU and its libraries only: the firmware is ours
    os.makedirs(f"{root}/alp/usr/bin")
    shutil.copytree(f"{QROOT}/lib", f"{root}/alp/lib", symlinks=True, copy_function=os.link)
    shutil.copytree(f"{QROOT}/usr/lib", f"{root}/alp/usr/lib", symlinks=True, copy_function=os.link)
    os.link(f"{QROOT}/usr/bin/qemu-system-x86_64", f"{root}/alp/usr/bin/qemu-system-x86_64")
    shutil.copy(f"{B}/fw.bin", f"{root}/fw.bin")
    open(f"{root}/test.sh", "w").write(GUEST)
    os.chmod(f"{root}/test.sh", 0o755)
    subprocess.run(f"cd {root} && find . -print0 | cpio --null -o -H newc --owner=0:0 > ../initramfs.cpio 2>/dev/null",
                   shell=True, check=True)
    shutil.rmtree(root)
    iso = f"{B}/iso"
    os.makedirs(f"{iso}/boot/grub")
    shutil.copy("build/ranix.elf", f"{iso}/boot/ranix.elf")
    os.link(f"{B}/initramfs.cpio", f"{iso}/boot/initramfs.cpio")
    open(f"{iso}/boot/grub/grub.cfg", "w").write(
        'set timeout=0\nset default=0\nmenuentry "Ranix" {\n  multiboot2 /boot/ranix.elf\n'
        '  module2 /boot/initramfs.cpio\n}\n')
    subprocess.run(["grub-mkrescue", "-o", f"{B}/q.iso", iso], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    shutil.rmtree(iso)

    log = f"{B}/serial.log"
    t0 = time.time()
    p = subprocess.Popen(QEMU + ["-smp", "2", "-m", "2G", "-display", "none", "-serial", f"file:{log}", "-no-reboot",
                                 "-boot", "d", "-cdrom", f"{B}/q.iso", "-vga", "none"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    try:
        end = time.time() + 240
        while time.time() < end and p.poll() is None:
            t = open(log, "rb").read().decode("utf-8", "replace") if os.path.exists(log) else ""
            if "KVMQ-SON" in t:
                break
            time.sleep(1)
    finally:
        if p.poll() is None:
            os.killpg(p.pid, 9)
            p.wait()
    out = open(log, "rb").read().decode("utf-8", "replace").replace("\r", "") if os.path.exists(log) else ""
    lines = [l for l in out.split("\n") if l.startswith("KVMQ")]
    for l in lines:
        print(l)
    rc = next((l.split()[2] for l in lines if l.startswith("KVMQ rc ")), "")
    ok = [("the VM booted and ran the test", "KVMQ basla" in out, ""),
          ("QEMU printed the guest's line", f"KVMQ out {MARK}" in out, ""),
          ("QEMU exited with the guest's code", rc == "33", rc)]
    for name, good, why in ok:
        print(("PASS: " if good else "FAIL: ") + name + ("" if good else f"  ({why})"))
    print(f"[kvmqemu] {sum(1 for _, g, _ in ok if g)}/{len(ok)} in {time.time() - t0:.0f}s")
    return 0 if all(g for _, g, _ in ok) else 1


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(124))
    sys.exit(main())
