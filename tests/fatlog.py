#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
"""The boot log on a FAT partition, written in place (kernel/fs/fat.c fat_write).

The USB image for a machine Ranix may not come up on carries a FAT partition with
a preallocated RANIX-LOG.TXT that Windows can read. Here: mkfs.fat makes such an
image on the host, a KVM guest mounts it, writes into the file (inside it, across a
sector and a cluster edge, and past its end, which must stop at the size), and the
host checks with fsck.fat and mtype that the filesystem is clean, the size is the
same and the bytes are there.

Usage: python3 tests/fatlog.py   (after make ARCH=x86_64 iso; QEMU_CMD as for make test)"""
import os, re, shlex, shutil, signal, subprocess, sys, time

B = "build/fatlog"
QEMU = shlex.split(os.environ.get("QEMU_CMD", "qemu-system-x86_64 -enable-kvm -cpu host"))
SIZE = 1 << 20          # the log file, preallocated
GUEST = r'''#!/bin/bash
for d in /dev/vda /dev/vdb /dev/sda /dev/sdb; do [ -b $d ] && mount -t vfat $d /mnt 2>/dev/null && break; done
grep " /mnt " /proc/mounts | sed 's/^/FATLOG mount: /'; ls -l /mnt | sed 's/^/FATLOG ls: /'
grep -q " /mnt " /proc/mounts || { echo "FATLOG mount yok: $(ls /dev | tr '\n' ' ')"; echo FATLOG-SON; exit; }
f=/mnt/RANIX-LOG.TXT
printf 'RANIX-BASLA\n' | dd of=$f conv=notrunc 2>/tmp/e1; echo "FATLOG yaz1 rc=$? $(tail -1 /tmp/e1)"
# across a sector (510) and a cluster (4096 with -s 8) edge
printf 'KESIT-OK' | dd of=$f bs=1 seek=4092 conv=notrunc 2>/dev/null; echo "FATLOG yaz2 rc=$?"
# past the end: only up to the size goes in
head -c 100 /dev/zero | tr '\0' 'Z' | dd of=$f bs=1 seek=%(end)d conv=notrunc 2>/dev/null; echo "FATLOG yaz3 rc=$?"
echo "FATLOG boyut $(stat -c %%s $f)"
echo "FATLOG liste $(ls /mnt | tr '\n' ' ')"
# reading the whole file in small pieces walks its cluster chain once, not once per read
# every disk's read count (/sys/block names it vblk0, not after /dev/vda)
reads() { n=0; for b in /sys/block/*/stat; do n=$((n + $(cut -d' ' -f1 $b))); done; echo $n; }
r0=$(reads); dd if=$f of=/dev/null bs=4096 2>/dev/null; r1=$(reads)
echo "FATLOG okuma $((r1 - r0))"
sync; umount /mnt; echo "FATLOG umount rc=$?"
echo FATLOG-SON
'''


def main():
    os.environ["PATH"] += ":/usr/sbin:/sbin"   # mkfs.fat, fsck.fat on Debian
    shutil.rmtree(B, ignore_errors=True)
    os.makedirs(B)
    img = f"{B}/fat.img"
    # FAT32 wants 65525+ clusters: 300 MB of 4 KB ones, sparse (an ESP is this size or more)
    subprocess.run(["truncate", "-s", "300M", img], check=True)
    subprocess.run(["mkfs.fat", "-F", "32", "-s", "8", "-n", "RANIXLOG", img], check=True, capture_output=True)
    pad = f"{B}/log.txt"
    open(pad, "wb").write(b" " * SIZE)
    open(f"{B}/readme.txt", "w").write("Ranix boot log partition\n")
    for src, dst in ((pad, "::RANIX-LOG.TXT"), (f"{B}/readme.txt", "::README.TXT")):
        subprocess.run(["mcopy", "-i", img, src, dst], check=True)

    root = f"{B}/root"
    shutil.copytree("build/initramfs", root, symlinks=True, copy_function=os.link)
    for f in ("test.sh", "reboot-on-exit"):
        if os.path.lexists(f"{root}/{f}"):
            os.unlink(f"{root}/{f}")
    open(f"{root}/test.sh", "w").write(GUEST % {"end": SIZE - 10})
    os.chmod(f"{root}/test.sh", 0o755)
    subprocess.run(f"cd {root} && find . -print0 | cpio --null -o -H newc --owner=0:0 > ../initramfs.cpio 2>/dev/null",
                   shell=True, check=True)
    shutil.rmtree(root)
    iso = f"{B}/iso"
    os.makedirs(f"{iso}/boot/grub")
    shutil.copy("build/ranix.elf", f"{iso}/boot/ranix.elf")
    os.link(f"{B}/initramfs.cpio", f"{iso}/boot/initramfs.cpio")
    open(f"{iso}/boot/grub/grub.cfg", "w").write(
        'set timeout=0\nset default=0\nmenuentry "Ranix" {\n  multiboot2 /boot/ranix.elf ranix.disk.rw=all\n'
        '  module2 /boot/initramfs.cpio\n}\n')
    subprocess.run(["grub-mkrescue", "-o", f"{B}/fat.iso", iso], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    shutil.rmtree(iso)

    log = f"{B}/serial.log"
    p = subprocess.Popen(QEMU + ["-smp", "2", "-m", "1G", "-display", "none", "-no-reboot", "-serial", f"file:{log}",
                                 "-boot", "d", "-cdrom", f"{B}/fat.iso", "-vga", "none",
                                 "-drive", f"file={img},format=raw,if=virtio"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    try:
        end = time.time() + 180
        while time.time() < end and p.poll() is None:
            if os.path.exists(log) and "FATLOG-SON" in open(log, "rb").read().decode("utf-8", "replace"):
                time.sleep(1)
                break
            time.sleep(1)
    finally:
        if p.poll() is None:
            os.killpg(p.pid, 9)
            p.wait()
    out = open(log, "rb").read().decode("utf-8", "replace").replace("\r", "") if os.path.exists(log) else ""
    for l in out.split("\n"):
        if l.startswith("FATLOG"):
            print(l)

    ok = []
    fsck = subprocess.run(["fsck.fat", "-n", img], capture_output=True, text=True)
    ok.append(("fsck.fat clean", fsck.returncode == 0, fsck.stdout.strip().splitlines()[-1:] if fsck.stdout else fsck.stderr))
    data = subprocess.run(["mtype", "-i", img, "::RANIX-LOG.TXT"], capture_output=True).stdout
    ok.append(("size unchanged", len(data) == SIZE, len(data)))
    ok.append(("the first write is there", data.startswith(b"RANIX-BASLA\n"), data[:16]))
    ok.append(("across the cluster edge", data[4092:4100] == b"KESIT-OK", data[4088:4104]))
    ok.append(("past the end stopped at the size", data[SIZE - 10:] == b"Z" * 10, data[SIZE - 12:]))
    names = (re.findall(r"^FATLOG liste (.*)$", out, re.M) or [""])[-1].split()
    ok.append(("readdir lists the 13-character name", "RANIX-LOG.TXT" in names, names))
    reads = int((re.findall(r"^FATLOG okuma (\d+)$", out, re.M) or ["-1"])[-1])
    # 1 MB of 4 KB clusters: 2048 data sectors and ~256 FAT steps; walking the chain
    # from the start on every 4 KB read is ~33000 (the USB log hung on wc, 11-16)
    ok.append(("a sequential read walks the chain once", 0 <= reads <= 4 * (SIZE // 512), reads))
    readme = subprocess.run(["mtype", "-i", img, "::README.TXT"], capture_output=True).stdout
    ok.append(("the other file untouched", readme == b"Ranix boot log partition\n", readme))
    for name, good, why in ok:
        print(("PASS: " if good else "FAIL: ") + name + ("" if good else f"  ({why})"))
    print(f"[fatlog] {sum(1 for _, g, _ in ok if g)}/{len(ok)}")
    return 0 if all(g for _, g, _ in ok) else 1


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(124))
    sys.exit(main())
