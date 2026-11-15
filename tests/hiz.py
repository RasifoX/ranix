#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
"""The speed baseline in KVM: boot build/ranix.elf with /test.sh running /bin/bench
(userspace/bench.c) and print its HIZ lines. The same bench binary, static, runs
on Linux for the reference (FLOWING-PLAN stage 0).

Usage: python3 tests/hiz.py   (after make ARCH=x86_64 iso; QEMU_CMD as for make test)
HIZ_PROF=NAME: run only that measure under /dev/prof (the tick sampler) and print
where the kernel spent its ticks, by function and by caller."""
import base64, bisect, collections, os, shlex, shutil, struct, subprocess, sys, time

B = "build/hiz"
QEMU = shlex.split(os.environ.get("QEMU_CMD", "qemu-system-x86_64 -enable-kvm -cpu host"))
GUEST = "#!/bin/bash\n/bin/bench\necho HIZ-SON\n"
PROF = os.environ.get("HIZ_PROF")
GUEST_PROF = """#!/bin/bash
echo 1 > /dev/prof; /bin/bench %s; echo 0 > /dev/prof
python3 -c 'import base64; d = open("/dev/prof", "rb").read(); print("PROF-BASLA"); [print(base64.b64encode(d[i:i + 3072]).decode()) for i in range(0, len(d), 3072)]; print("PROF-SON")'
echo HIZ-SON
"""


def symbols():
    out = subprocess.run(["nm", "-n", "build/ranix.elf"], capture_output=True, text=True).stdout
    syms = [(int(a, 16), n) for a, t, n in (l.split()[:3] for l in out.splitlines() if len(l.split()) >= 3) if t in "tTwW"]
    return [a for a, _ in syms], [n for _, n in syms]


def report(out):
    if "PROF-BASLA" not in out or "PROF-SON" not in out:
        print("PROF yok (no /dev/prof: it needs a framebuffer)")
        return
    b64 = out.split("PROF-BASLA", 1)[1].split("PROF-SON", 1)[0]
    d = b"".join(base64.b64decode(l.strip()) for l in b64.split("\n") if l.strip())
    addrs, names = symbols()

    def name(a):
        i = bisect.bisect_right(addrs, a) - 1
        return names[i] if i >= 0 and a >= 0xffff000000000000 else ("user" if a < 0x800000000000 else hex(a))
    fn, pair = collections.Counter(), collections.Counter()
    n = 0
    for i in range(0, len(d) - 23, 24):
        rip, caller, pid, cpu = struct.unpack_from("<QQII", d, i)
        if not rip:
            continue
        n += 1
        fn[name(rip)] += 1
        pair[(name(rip), name(caller) if caller else "-")] += 1
    print(f"PROF {n} ornek")
    for f, c in fn.most_common(25):
        print(f"PROF {100 * c / n:5.1f}% {f}")
    for (f, c2), c in pair.most_common(25):
        print(f"PROFC {100 * c / n:5.1f}% {f} <- {c2}")


def main():
    shutil.rmtree(B, ignore_errors=True)
    os.makedirs(B)
    root = f"{B}/root"
    shutil.copytree("build/initramfs", root, symlinks=True, copy_function=os.link)
    for f in ("test.sh", "reboot-on-exit"):
        if os.path.lexists(f"{root}/{f}"):
            os.unlink(f"{root}/{f}")
    open(f"{root}/test.sh", "w").write(GUEST_PROF % PROF if PROF else GUEST)
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
    subprocess.run(["grub-mkrescue", "-o", f"{B}/hiz.iso", iso], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    shutil.rmtree(iso)
    log = f"{B}/serial.log"
    p = subprocess.Popen(QEMU + ["-smp", "2", "-m", "2G", "-display", "none", "-no-reboot", "-serial", f"file:{log}",
                                 "-boot", "d", "-cdrom", f"{B}/hiz.iso"] + ([] if PROF else ["-vga", "none"]),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    out = ""
    try:
        end = time.time() + 900
        while time.time() < end and p.poll() is None:
            out = open(log, "rb").read().decode("utf-8", "replace") if os.path.exists(log) else ""
            if "HIZ-SON" in out:
                break
            time.sleep(1)
    finally:
        if p.poll() is None:
            os.killpg(p.pid, 9)
            p.wait()
    out = open(log, "rb").read().decode("utf-8", "replace") if os.path.exists(log) else ""   # the guest may exit between reads
    lines = [l.strip() for l in out.replace("\r", "").split("\n") if l.startswith("HIZ ")]
    print("\n".join(lines))
    if PROF:
        report(out.replace("\r", ""))
    return 0 if any(l.startswith("HIZ bitti") for l in lines) else 1


if __name__ == "__main__":
    sys.exit(main())
