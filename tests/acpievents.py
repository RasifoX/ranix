#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
"""ACPI events in QEMU: the power button, polled and through the SCI.

Three boots of build/ranix.elf with build/initramfs and a /test.sh of our own.
After the guest's test_powerbtn says READY, the QEMU monitor presses the power
button (system_powerdown). Checked:
  P  default (PM1 polled): KEY_POWER on /dev/input/event0, SIGPWR to init,
     /sys/class/power_supply empty (QEMU has no battery), /sys/kernel/lid unknown
  S  ranix.sci=1 on SeaBIOS (legacy mode, SCI_EN 0): no SCI, no silent
     ACPI_ENABLE; the button is polled and still works
  A  ranix.sci=1 ranix.acpi_enable=1: handed over aloud, the SCI in level mode
     (ELCR), the press through the interrupt, no storm

Usage: python3 tests/acpievents.py   (after make ARCH=x86_64 iso; QEMU_CMD as for make test)
"""
import os, re, shlex, shutil, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
B = "build/acpievents"
QEMU = shlex.split(os.environ.get("QEMU_CMD", "qemu-system-x86_64"))
results = []

GUEST = r'''#!/bin/bash
echo "AE-MODE $(tr ' ' '\n' < /proc/cmdline | sed -n 's/^ae.mode=//p')"
/bin/test_powerbtn AE-PB    # straight to the console: a pipe would hold READY back
sleep 1
grep '^power acpi' /proc/hwdiag | sed 's/^/AE-HW /'
echo AE-END
poweroff -f
'''


def check(name, ok, why=""):
    results.append(bool(ok))
    print(("PASS: " if ok else "FAIL: ") + name + ("" if ok else "  (" + str(why)[:300] + ")"), flush=True)


def boot(mode, cmdline):
    iso = f"{B}/iso"
    shutil.rmtree(iso, ignore_errors=True)
    os.makedirs(f"{iso}/boot/grub")
    shutil.copy("build/ranix.elf", f"{iso}/boot/ranix.elf")
    os.link(f"{B}/initramfs.cpio", f"{iso}/boot/initramfs.cpio")
    open(f"{iso}/boot/grub/grub.cfg", "w").write(
        f'set timeout=0\nset default=0\nmenuentry "Ranix" {{\n  multiboot2 /boot/ranix.elf {cmdline} ae.mode={mode}\n'
        f'  module2 /boot/initramfs.cpio\n}}\n')
    subprocess.run(["grub-mkrescue", "-o", f"{B}/ae.iso", iso], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    shutil.rmtree(iso)
    log, mon = f"{B}/serial-{mode}.log", f"{B}/mon-{mode}.sock"
    for f in (log, mon):
        if os.path.exists(f):
            os.unlink(f)
    p = subprocess.Popen(QEMU + ["-smp", "2", "-m", "2G", "-display", "none", "-no-reboot", "-serial", f"file:{log}",
                                 "-boot", "d", "-cdrom", f"{B}/ae.iso", "-monitor", f"unix:{mon},server,nowait"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if os.path.exists(mon):
            break
        time.sleep(0.1)
    s = socket.socket(socket.AF_UNIX)
    s.connect(mon)
    s.settimeout(20)

    def cmd(c):
        if c:
            s.sendall(c.encode() + b"\n")
        buf = b""
        while not buf.endswith(b"(qemu) "):
            buf += s.recv(4096)

    def serial():
        try:
            return open(log, "rb").read().decode("utf-8", "replace").replace("\r", "")
        except OSError:
            return ""

    def wait(word, t):
        end = time.time() + t
        while time.time() < end and p.poll() is None:
            if word in serial():
                return True
            time.sleep(0.1)
        return word in serial()

    cmd(None)
    ready = wait("AE-PB READY", 120)
    if ready:
        time.sleep(0.5)
        cmd("system_powerdown")
    wait("AE-END", 60)
    try:
        p.wait(30)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
    s.close()
    return ready, serial()


def line(text, key):
    m = re.findall(r"^" + re.escape(key) + r" ?(.*)$", text, re.M)
    return m[-1] if m else ""


def main():
    shutil.rmtree(B, ignore_errors=True)
    os.makedirs(B)
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
    for mode, cl in (("P", ""), ("S", "ranix.sci=1"), ("A", "ranix.sci=1 ranix.acpi_enable=1")):
        ready, out = boot(mode, cl)
        check(f"{mode}: guest ready", ready, out[-400:])
        check(f"{mode}: KEY_POWER on event0, no supplies made up, lid unknown", line(out, "AE-PB") == "OK" and
              out.find("AE-PB READY") < out.find("acpi: power button"),
              [l for l in out.split("\n") if l.startswith("AE-PB")])
        check(f"{mode}: SIGPWR reached init", "init: SIGPWR (power button)" in out, out[-300:])
        hw = line(out, "AE-HW")
        kv = dict(x.split("=") for x in hw.split()[2:] if "=" in x) if hw else {}
        check(f"{mode}: one press counted", kv.get("presses") == "1", hw)
        if mode == "A":
            check("A: through the SCI, level, handed over aloud", kv.get("sci") == "1" and int(kv.get("scis", "0")) >= 1 and
                  kv.get("elcr_level") == "1" and kv.get("handed_over") == "1" and
                  "acpi: handing the firmware's events to the OS" in out and "acpi: SCI on irq 9 (level)" in out, hw)
            check("A: no storm, nothing foreign", int(kv.get("foreign", "99")) < 5 and int(kv.get("scis", "0")) < 50 and
                  kv.get("storms") == "0", hw)
        elif mode == "S":
            # SeaBIOS leaves the firmware in legacy mode: ranix.sci=1 alone must not hand it over
            check("S: legacy firmware is not handed over silently", kv.get("sci") == "0" and kv.get("handed_over") == "0" and
                  "acpi: firmware in legacy mode (SCI_EN 0)" in out and "handing the firmware" not in out, hw)
        else:
            check("P: polled, no SCI", kv.get("sci") == "0", hw)
    for f in os.listdir(B):
        if f.endswith((".iso", ".cpio", ".sock")):
            os.unlink(os.path.join(B, f))
    n = sum(results)
    print(f"[acpievents] {n}/{len(results)}")
    return 0 if results and n == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
