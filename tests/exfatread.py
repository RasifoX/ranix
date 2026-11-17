#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
"""Reading a fragmented exFAT file (kernel/fs/exfat.c).

The host makes an exFAT image with mkfs.exfat and, through a loop mount, two files
written in turns, 4 KB at a time, so each is a FAT chain rather than one contiguous
run. A KVM guest mounts it, reads one file in 4 KB pieces and prints its md5 and
the disk reads that took: the bytes must match, and the chain is walked once, not
from its start for every read (as the FAT driver once did).

Usage: sudo python3 tests/exfatread.py   (root for the loop mount; after make ARCH=x86_64 iso)"""
import hashlib, os, re, shlex, shutil, signal, subprocess, sys, time

B = "build/exfatread"
QEMU = shlex.split(os.environ.get("QEMU_CMD", "qemu-system-x86_64 -enable-kvm -cpu host"))
PIECES = 256            # 1 MB per file, 4 KB clusters
GUEST = r'''#!/bin/bash
for d in /dev/vblk0 /dev/vda /dev/vdb /dev/sda /dev/sdb; do [ -b $d ] && mount -t exfat $d /mnt 2>/dev/null && break; done
grep -q " /mnt " /proc/mounts || { echo "EXREAD mount yok: $(ls /dev | tr '\n' ' ')"; echo EXREAD-END; exit; }
echo "EXREAD liste $(ls /mnt | tr '\n' ' ')"
reads() { n=0; for b in /sys/block/*/stat; do n=$((n + $(cut -d' ' -f1 $b))); done; echo $n; }
r0=$(reads); dd if=/mnt/a.bin of=/tmp/a bs=4096 2>/dev/null; r1=$(reads)
echo "EXREAD reads $((r1 - r0))"
# the test root has busybox without its applet links
echo "EXREAD md5 $(busybox md5sum /tmp/a | cut -d' ' -f1)"
umount /mnt; echo EXREAD-END
'''


def main():
    if os.geteuid() != 0:
        print("[exfatread] skip: needs root for the loop mount")
        return 0
    os.environ["PATH"] += ":/usr/sbin:/sbin"
    shutil.rmtree(B, ignore_errors=True)
    os.makedirs(B)
    img, mnt = f"{B}/ex.img", f"{B}/mnt"
    subprocess.run(["truncate", "-s", "64M", img], check=True)
    subprocess.run(["mkfs.exfat", "-c", "4K", img], check=True, capture_output=True)
    os.makedirs(mnt)
    subprocess.run(["mount", "-o", "loop", img, mnt], check=True)
    a = bytearray()
    try:
        with open(f"{mnt}/a.bin", "wb") as fa, open(f"{mnt}/b.bin", "wb") as fb:
            for i in range(PIECES):
                pa = hashlib.sha256(b"a%d" % i).digest() * 128      # 4 KB
                fa.write(pa); fa.flush(); os.fsync(fa.fileno())
                fb.write(b"b" * 4096); fb.flush(); os.fsync(fb.fileno())
                a += pa
    finally:
        subprocess.run(["umount", mnt], check=True)
    want = hashlib.md5(a).hexdigest()

    root = f"{B}/root"
    shutil.copytree("build/initramfs", root, symlinks=True, copy_function=os.link)
    for f in ("test.sh", "reboot-on-exit"):
        if os.path.lexists(f"{root}/{f}"):
            os.unlink(f"{root}/{f}")
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
    subprocess.run(["grub-mkrescue", "-o", f"{B}/ex.iso", iso], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    shutil.rmtree(iso)

    log = f"{B}/serial.log"
    p = subprocess.Popen(QEMU + ["-smp", "2", "-m", "1G", "-display", "none", "-no-reboot", "-serial", f"file:{log}",
                                 "-boot", "d", "-cdrom", f"{B}/ex.iso", "-vga", "none",
                                 "-drive", f"file={img},format=raw,if=virtio"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    try:
        end = time.time() + 180
        while time.time() < end and p.poll() is None:
            if os.path.exists(log) and "EXREAD-END" in open(log, "rb").read().decode("utf-8", "replace"):
                time.sleep(1)
                break
            time.sleep(1)
    finally:
        if p.poll() is None:
            os.killpg(p.pid, 9)
            p.wait()
    out = open(log, "rb").read().decode("utf-8", "replace").replace("\r", "") if os.path.exists(log) else ""
    for l in out.split("\n"):
        if l.startswith("EXREAD"):
            print(l)

    got = (re.findall(r"^EXREAD md5 (\w+)$", out, re.M) or [""])[-1]
    reads = int((re.findall(r"^EXREAD reads (\d+)$", out, re.M) or ["-1"])[-1])
    ok = [("the fragmented file reads back whole", got == want, got),
          # 2048 data sectors and ~256 FAT steps; from the chain's start each time ~33000
          ("a sequential read walks the chain once", 0 <= reads <= 4 * PIECES * 8, reads)]
    for name, good, why in ok:
        print(("PASS: " if good else "FAIL: ") + name + ("" if good else f"  ({why})"))
    print(f"[exfatread] {sum(1 for _, g, _ in ok if g)}/{len(ok)}")
    return 0 if all(g for _, g, _ in ok) else 1


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(124))
    sys.exit(main())
