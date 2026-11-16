#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
"""The boot before this one, read back through /sys/fs/pstore.

One QEMU run, two boots: the first writes a mark into the kernel log (/dev/kmsg)
and reboots; QEMU keeps the RAM over a reset, so the second finds the first's log
in the pstore region and reads it from /sys/fs/pstore/console-ramoops-0, with
ranix-previous saying how that boot ended (a clean reboot). Telemetry reads the
same files after a crash.

Usage: python3 tests/pstoreprev.py   (after make ARCH=x86_64 iso; QEMU_CMD as for make test)"""
import os, re, shlex, shutil, signal, subprocess, sys, time

B = "build/pstoreprev"
QEMU = shlex.split(os.environ.get("QEMU_CMD", "qemu-system-x86_64 -enable-kvm -cpu host"))
MARK = "PSTPREV-IZ-7319"
GUEST = r'''#!/bin/bash
P=/sys/fs/pstore
if [ -e $P/ranix-previous ]; then
    echo "PSTPREV ikinci"
    echo "PSTPREV info $(cat $P/ranix-previous)"
    echo "PSTPREV iz $(grep -c %(mark)s $P/console-ramoops-0)"
    echo "PSTPREV boyut $(cat $P/console-ramoops-0 | wc -c) $(stat -c %%s $P/console-ramoops-0)"
    echo "PSTPREV liste $(ls $P | tr '\n' ' ')"
    echo PSTPREV-SON
else
    echo "PSTPREV ilk $(ls /sys/fs 2>&1 | tr '\n' ' ')"
    [ -n "$(ls $P 2>/dev/null)" ] && echo "PSTPREV ilk-bos-degil"
    echo "%(mark)s" > /dev/kmsg
    sleep 1
    reboot -f
fi
'''


def main():
    shutil.rmtree(B, ignore_errors=True)
    os.makedirs(B)
    root = f"{B}/root"
    shutil.copytree("build/initramfs", root, symlinks=True, copy_function=os.link)
    for f in ("test.sh", "reboot-on-exit"):
        if os.path.lexists(f"{root}/{f}"):
            os.unlink(f"{root}/{f}")
    open(f"{root}/test.sh", "w").write(GUEST % {"mark": MARK})
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
    subprocess.run(["grub-mkrescue", "-o", f"{B}/p.iso", iso], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    shutil.rmtree(iso)

    log = f"{B}/serial.log"
    # no -no-reboot: the first boot's reboot is a reset, and the RAM stays
    p = subprocess.Popen(QEMU + ["-smp", "2", "-m", "1G", "-display", "none", "-serial", f"file:{log}",
                                 "-boot", "d", "-cdrom", f"{B}/p.iso", "-vga", "none"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    try:
        end = time.time() + 240
        while time.time() < end and p.poll() is None:
            t = open(log, "rb").read().decode("utf-8", "replace") if os.path.exists(log) else ""
            if "PSTPREV-SON" in t or t.count("PSTPREV ilk") >= 3:
                time.sleep(1)
                break
            time.sleep(1)
    finally:
        if p.poll() is None:
            os.killpg(p.pid, 9)
            p.wait()
    out = open(log, "rb").read().decode("utf-8", "replace").replace("\r", "") if os.path.exists(log) else ""
    for l in out.split("\n"):
        if l.startswith("PSTPREV"):
            print(l)

    info = (re.findall(r"^PSTPREV info (.*)$", out, re.M) or [""])[-1]
    iz = (re.findall(r"^PSTPREV iz (\d+)$", out, re.M) or ["0"])[-1]
    boy = (re.findall(r"^PSTPREV boyut (\d+) (\d+)$", out, re.M) or [("0", "-1")])[-1]
    ok = [("the first boot had no previous log", "PSTPREV ilk" in out and "ilk-bos-degil" not in out, ""),
          ("the second boot sees the first", "PSTPREV ikinci" in out, ""),
          ("its log holds the mark", int(iz) >= 1, iz),
          ("it ended in a clean reboot", "clean reboot" in info, info),
          ("its data is intact", "intact" in info, info),
          ("stat size = bytes read", int(boy[0]) > 0 and boy[0] == boy[1], boy)]
    for name, good, why in ok:
        print(("PASS: " if good else "FAIL: ") + name + ("" if good else f"  ({why})"))
    print(f"[pstoreprev] {sum(1 for _, g, _ in ok if g)}/{len(ok)}")
    return 0 if all(g for _, g, _ in ok) else 1


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(124))
    sys.exit(main())
