#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
"""QEMU with -enable-kvm inside Ranix: an unmodified VMM on /dev/kvm.

Ranix boots in a VM whose CPU has VMX (-cpu host on a VT-x host); its initramfs
carries Alpine's qemu-system-x86_64, a 64 KB firmware image and Alpine's Linux
kernel (linux-virt). Inside, QEMU runs two KVM guests:
  - the firmware: the reset vector jumps to code that writes a line to the
    emulated COM1, which QEMU prints, and leaves through isa-debug-exit, whose
    code QEMU exits with. QEMU's own start-up (capabilities, CPUID, MSRs, memory
    slots, vcpu state) and its port I/O devices;
  - Linux, through QEMU's SeaBIOS and its -kernel loader, to an /init (busybox
    from Ranix's initramfs) that writes the kernel's name and the CPU to COM1
    and reboots: real mode to long mode, paging, MSRs, interrupts from the
    PIT through QEMU's PIC, string port I/O;
  - the same Linux on QEMU's pc machine with two CPUs and the local APICs in
    Ranix's KVM (kernel-irqchip=split): ACPI, the IOAPIC's interrupts as MSIs,
    APIC timers, INIT/SIPI to start the second CPU, IPIs.

The first two use isapc with kernel-irqchip=off: every interrupt controller
in QEMU. A machine with local APICs needs them in the kernel: QEMU refuses a
userspace local APIC with KVM.

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
KERNEL = ".cache/alpine-linux-virt-x86_64"
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
echo "KVMQ start"
L="/alp/lib/ld-musl-x86_64.so.1 --library-path /alp/usr/lib:/alp/lib"
cd /tmp
timeout 60 $L /alp/usr/bin/qemu-system-x86_64 -enable-kvm -machine isapc,accel=kvm,kernel-irqchip=off \
    -nodefaults -display none -serial stdio -m 16 -bios /fw.bin \
    -device isa-debug-exit,iobase=0x501,iosize=1 > /tmp/q.out 2>&1
echo "KVMQ rc $?"
sed 's/^/KVMQ out /' /tmp/q.out
echo "KVMQ linux start"
timeout 120 $L /alp/usr/bin/qemu-system-x86_64 -enable-kvm -machine isapc,accel=kvm,kernel-irqchip=off \
    -cpu host,-apic -nodefaults -display none -no-reboot -serial file:/tmp/linux.out -m 256 \
    -L /alp/usr/share/qemu -kernel /lnx/vmlinuz -initrd /lnx/initrd.cpio \
    -append "console=ttyS0 nolapic noapic panic=-1" > /tmp/ql.out 2>&1
echo "KVMQ linux rc $?"
grep -v "host doesn't support" /tmp/ql.out | sed 's/^/KVMQ linux err /'
grep -a "LINUXQ" /tmp/linux.out | sed 's/^/KVMQ linux out /'
echo "KVMQ smp start"
timeout 150 $L /alp/usr/bin/qemu-system-x86_64 -enable-kvm -machine pc,accel=kvm,kernel-irqchip=split \
    -cpu host -smp 2 -nodefaults -display none -no-reboot -serial file:/tmp/smp.out -m 256 \
    -L /alp/usr/share/qemu -kernel /lnx/vmlinuz -initrd /lnx/initrd.cpio \
    -append "console=ttyS0 panic=-1" > /tmp/qs.out 2>&1
echo "KVMQ smp rc $?"
grep -v "host doesn't support\|MCG_CAP" /tmp/qs.out | sed 's/^/KVMQ smp err /'
grep -a "LINUXQ\|smp: Brought up\|TIMER: vector" /tmp/smp.out | sed 's/^/KVMQ smp out /'
echo KVMQ-END
'''

# /init of the Linux guest: busybox, run by the guest's Linux
LINUX_INIT = r'''#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t devtmpfs dev /dev
echo "LINUXQ $(/bin/busybox uname -sr) | $(/bin/busybox grep -m1 'model name' /proc/cpuinfo)" > /dev/ttyS0
/bin/busybox reboot -f
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


def fetch_kernel():
    """vmlinuz-virt out of Alpine's linux-virt package (an apk is gzip'd tars back to back)"""
    if os.path.exists(f"{KERNEL}/vmlinuz-virt"):
        return
    import gzip, io, tarfile, urllib.request
    url = f"{ALPINE}/main/x86_64"
    index = tarfile.open(fileobj=io.BytesIO(urllib.request.urlopen(f"{url}/APKINDEX.tar.gz", timeout=60).read()))
    text = index.extractfile("APKINDEX").read().decode()
    rec = next(r for r in text.split("\n\n") if "\nP:linux-virt\n" in "\n" + r + "\n")
    ver = next(l[2:] for l in rec.split("\n") if l.startswith("V:"))
    apk = urllib.request.urlopen(f"{url}/linux-virt-{ver}.apk", timeout=120).read()
    tar = tarfile.open(fileobj=io.BytesIO(gzip.decompress(apk)), ignore_zeros=True)
    os.makedirs(KERNEL + ".tmp", exist_ok=True)
    open(f"{KERNEL}.tmp/vmlinuz-virt", "wb").write(tar.extractfile("boot/vmlinuz-virt").read())
    os.rename(KERNEL + ".tmp", KERNEL)


def linux_initrd(path):
    d = f"{B}/linitrd"
    os.makedirs(f"{d}/bin")
    os.makedirs(f"{d}/proc")
    os.makedirs(f"{d}/dev")
    shutil.copy("build/initramfs/bin/busybox", f"{d}/bin/busybox")
    open(f"{d}/init", "w").write(LINUX_INIT)
    os.chmod(f"{d}/init", 0o755)
    subprocess.run(f"cd {d} && find . -print0 | cpio --null -o -H newc --owner=0:0 > {os.path.abspath(path)} 2>/dev/null",
                   shell=True, check=True)
    shutil.rmtree(d)


def main():
    fetch_qemu()
    fetch_kernel()
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
    # QEMU, its libraries, and the firmware it loads for an isapc with -kernel
    os.makedirs(f"{root}/alp/usr/bin")
    os.makedirs(f"{root}/alp/usr/share/qemu")
    shutil.copytree(f"{QROOT}/lib", f"{root}/alp/lib", symlinks=True, copy_function=os.link)
    shutil.copytree(f"{QROOT}/usr/lib", f"{root}/alp/usr/lib", symlinks=True, copy_function=os.link)
    os.link(f"{QROOT}/usr/bin/qemu-system-x86_64", f"{root}/alp/usr/bin/qemu-system-x86_64")
    for f in ("bios.bin", "bios-256k.bin", "linuxboot.bin", "linuxboot_dma.bin", "kvmvapic.bin"):
        os.link(f"{QROOT}/usr/share/qemu/{f}", f"{root}/alp/usr/share/qemu/{f}")
    shutil.copy(f"{B}/fw.bin", f"{root}/fw.bin")
    os.makedirs(f"{root}/lnx")
    os.link(f"{KERNEL}/vmlinuz-virt", f"{root}/lnx/vmlinuz")
    linux_initrd(f"{root}/lnx/initrd.cpio")
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
        end = time.time() + 480
        while time.time() < end and p.poll() is None:
            t = open(log, "rb").read().decode("utf-8", "replace") if os.path.exists(log) else ""
            if "KVMQ-END" in t:
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
    lrc = next((l.split()[3] for l in lines if l.startswith("KVMQ linux rc ")), "")
    lout = next((l for l in lines if l.startswith("KVMQ linux out LINUXQ Linux ")), "")
    ok = [("the VM booted and ran the test", "KVMQ start" in out, ""),
          ("QEMU printed the guest's line", f"KVMQ out {MARK}" in out, ""),
          ("QEMU exited with the guest's code", rc == "33", rc),
          ("Linux ran its /init in QEMU", "model name" in lout, lout),
          ("QEMU ended with the guest's reboot", lrc == "0", lrc),
          ("Linux on pc with the APICs in KVM ran its /init",
           any(l.startswith("KVMQ smp out LINUXQ Linux ") for l in lines), ""),
          ("it brought up its second CPU", any("Brought up 1 node, 2 CPUs" in l for l in lines), ""),
          ("its timer came through the IOAPIC", any("TIMER: vector" in l for l in lines), ""),
          ("QEMU ended with that guest's reboot",
           next((l.split()[3] for l in lines if l.startswith("KVMQ smp rc ")), "") == "0", "")]
    for name, good, why in ok:
        print(("PASS: " if good else "FAIL: ") + name + ("" if good else f"  ({why})"))
    print(f"[kvmqemu] {sum(1 for _, g, _ in ok if g)}/{len(ok)} in {time.time() - t0:.0f}s")
    return 0 if all(g for _, g, _ in ok) else 1


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(124))
    sys.exit(main())
