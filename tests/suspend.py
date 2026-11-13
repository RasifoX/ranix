#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
"""Suspend in QEMU: s2idle and S3 (ozel-SUSP1/TASARIM.md 7).

Short boots of build/ranix.elf with build/initramfs and a /test.sh of our own;
the guest half is userspace/test_suspend.c. One boot per mode:
  B  the files: state, mem_sleep "[s2idle]", root only, deep/disk refused
  R  s2idle woken by the RTC alarm, three times: MONOTONIC does not count the
     sleep, BOOTTIME and REALTIME do (and REALTIME agrees with the host), the vDSO
     agrees with the system call, the vDSO never goes back on the other CPU,
     that CPU works after, a 3 s nap across the sleep takes 3 s of MONOTONIC
  P  s2idle woken by the power button (monitor system_powerdown): wake=power, and
     the press that woke is not a shutdown request (no SIGPWR to init)
  K  s2idle woken by a key (monitor sendkey)
  D  S3 on PIIX4 (-global PIIX4_PM.disable_s3=0, ranix.s3=1): "[s2idle] deep",
     QEMU reports the guest suspended, woken by the RTC alarm, the clocks as in R
  W  S3 on PIIX4 woken by the monitor's system_wakeup
  Q  S3 on q35 (-global ICH9-LPC.disable_s3=0) woken by system_wakeup
  T  KT13's early-wake tests across an s2idle (test_sleeptail, test_vvarepoch):
     2.5 ms sleeps before, across and after; none early, the one across
     included; MONOTONIC does not jump, BOOTTIME takes the sleep; every vvar
     period continues the one before to 1 ns except across the suspend
  U  the same across S3 (PIIX4)
S3 boots have no VGA and no NIC: a device Ranix cannot restore blocks deep.

Usage: python3 tests/suspend.py [MODES]   (after make ARCH=x86_64 iso; QEMU_CMD as
for make test, with KVM: QEMU_CMD="qemu-system-x86_64 -enable-kvm -cpu host")
"""
import os, re, shlex, shutil, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
B = "build/suspend"
QEMU = shlex.split(os.environ.get("QEMU_CMD", "qemu-system-x86_64"))
results = []
SECS = 4                       # the RTC alarm

GUEST = r'''#!/bin/bash
mode=$(tr ' ' '\n' < /proc/cmdline | sed -n 's/^sus.mode=//p')
echo "SUS-MODE $mode"
case $mode in
B) /bin/test_suspend files ;;
R) for i in 1 2 3; do /bin/test_suspend sleep freeze %(secs)d; done ;;
P|K) /bin/test_suspend sleep freeze 0 ;;
D) /bin/test_suspend files deep; echo deep > /sys/power/mem_sleep; /bin/test_suspend sleep mem %(secs)d ;;
W|Q) echo deep > /sys/power/mem_sleep; /bin/test_suspend sleep mem 0 ;;
T) /bin/test_suspend tail freeze %(secs)d ;;
U) echo deep > /sys/power/mem_sleep; /bin/test_suspend tail mem %(secs)d ;;
A) echo deep > /sys/power/mem_sleep; echo mem > /sys/power/state; echo "SUS-A rc=$? fail=$(cat /sys/power/suspend_stats/fail) step=$(cat /sys/power/suspend_stats/last_failed_step)" ;;
esac
grep '^power ' /proc/hwdiag | sed 's/^/SUS-HW /'
echo SUS-END
poweroff -f
''' % {"secs": SECS}

MODES = {
    #     extra QEMU args                                     cmdline       action when SUS-READY
    "B": ([],                                                  "",           None),
    "R": ([],                                                  "",           None),
    "P": ([],                                                  "",           "system_powerdown"),
    "K": ([],                                                  "",           "sendkey a"),
    "D": (["-global", "PIIX4_PM.disable_s3=0", "-vga", "none", "-nic", "none"], "ranix.s3=1", None),
    "W": (["-global", "PIIX4_PM.disable_s3=0", "-vga", "none", "-nic", "none"], "ranix.s3=1", "system_wakeup"),
    "T": ([],                                                  "",           None),
    "U": (["-global", "PIIX4_PM.disable_s3=0", "-vga", "none", "-nic", "none"], "ranix.s3=1", None),
    # q35 has its AHCI built in, and Ranix's AHCI driver has no resume: deep is refused, by name
    "A": (["-M", "q35", "-global", "ICH9-LPC.disable_s3=0", "-vga", "none", "-nic", "none"], "ranix.s3=1", None),
    # the S3 path itself on q35, the unused AHCI let through (ranix.s3=force, tests only)
    "Q": (["-M", "q35", "-global", "ICH9-LPC.disable_s3=0", "-vga", "none", "-nic", "none"], "ranix.s3=force", "system_wakeup"),
}


def check(name, ok, why=""):
    results.append(bool(ok))
    print(("PASS: " if ok else "FAIL: ") + name + ("" if ok else "  (" + str(why)[:300] + ")"), flush=True)


def boot(mode):
    extra, cmdline, action = MODES[mode]
    iso = f"{B}/iso"
    shutil.rmtree(iso, ignore_errors=True)
    os.makedirs(f"{iso}/boot/grub")
    shutil.copy("build/ranix.elf", f"{iso}/boot/ranix.elf")
    os.link(f"{B}/initramfs.cpio", f"{iso}/boot/initramfs.cpio")
    open(f"{iso}/boot/grub/grub.cfg", "w").write(
        f'set timeout=0\nset default=0\nmenuentry "Ranix" {{\n  multiboot2 /boot/ranix.elf {cmdline} sus.mode={mode}\n'
        f'  module2 /boot/initramfs.cpio\n}}\n')
    subprocess.run(["grub-mkrescue", "-o", f"{B}/sus.iso", iso], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    shutil.rmtree(iso)
    log, mon = f"{B}/serial-{mode}.log", f"{B}/mon-{mode}.sock"
    for f in (log, mon):
        if os.path.exists(f):
            os.unlink(f)
    p = subprocess.Popen(QEMU + ["-smp", "2", "-m", "2G", "-display", "none", "-no-reboot", "-serial", f"file:{log}",
                                 "-boot", "d", "-cdrom", f"{B}/sus.iso", "-rtc", "base=utc",
                                 "-monitor", f"unix:{mon},server,nowait"] + extra,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
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
        return buf.decode("utf-8", "replace")

    def serial():
        try:
            return open(log, "rb").read().decode("utf-8", "replace").replace("\r", "")
        except OSError:
            return ""

    def wait(word, t, count=1):
        end = time.time() + t
        while time.time() < end and p.poll() is None:
            if serial().count(word) >= count:
                return True
            time.sleep(0.1)
        return serial().count(word) >= count

    info = {"suspended": False, "ready_host": []}
    cmd(None)
    cycles = 3 if mode == "R" else 0 if mode in ("B", "A") else 1
    for c in range(1, cycles + 1):
        if not wait("SUS-READY", 150, c):
            break
        info["ready_host"].append(time.time())
        # while asleep: does QEMU itself see the guest suspended (S3)?
        t_end = time.time() + SECS + 3
        while time.time() < t_end and serial().count("SUS-RESULT") + serial().count("SUS-TAIL") < c:
            if "suspended" in cmd("info status"):
                info["suspended"] = True
                if action == "system_wakeup":
                    time.sleep(1)
                    cmd(action)
                    break
            time.sleep(0.3)
        if action and action != "system_wakeup":
            time.sleep(1.5)                 # well inside the sleep
            cmd(action)
        if mode in ("T", "U"):
            wait("SUS-TAIL", 60, c)
        elif not wait("SUS-RESULT", 60, c) and action is None and mode in ("D",):
            cmd("system_wakeup")            # the alarm did not wake S3: say so, but finish the boot
            info["rtc_s3_failed"] = True
            wait("SUS-RESULT", 30, c)
        info.setdefault("result_host", []).append(time.time())
    wait("SUS-END", 90)
    try:
        p.wait(30)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, 9)
        p.wait()
    s.close()
    return serial(), info


def fields(line):
    return dict(x.split("=", 1) for x in line.split()[1:] if "=" in x)


def judge_sleep(mode, out, info, n, kind, wake, host_secs=None):
    res = [fields(l) for l in re.findall(r"^SUS-RESULT .*$", out, re.M)]
    naps = [fields(l) for l in re.findall(r"^SUS-NAP .*$", out, re.M)]
    check(f"{mode}: {n} sleep(s) came back", len(res) == n, out[-500:])
    for i, r in enumerate(res):
        tag = f"{mode}{i + 1}"
        mono, boot_, real = int(r.get("mono_ms", -1)), int(r.get("boot_ms", -1)), int(r.get("real_ms", -1))
        check(f"{tag}: no error, {kind}, wake={wake}", r.get("err") == "0" and f"kind={kind}" in r.get("last", "")
              and f"wake={wake}" in r.get("last", ""), r)
        check(f"{tag}: success counted", r.get("success") == str(i + 1), r)
        if wake == "rtc":
            check(f"{tag}: BOOTTIME and REALTIME count the sleep ({boot_} / {real} ms)",
                  SECS * 1000 - 1100 <= boot_ <= SECS * 1000 + 3000 and SECS * 1000 - 1100 <= real <= SECS * 1000 + 3000, r)
        else:
            check(f"{tag}: BOOTTIME and REALTIME count the sleep ({boot_} / {real} ms)", boot_ >= 1000 and real >= 1000, r)
        check(f"{tag}: MONOTONIC does not ({mono} ms, BOOTTIME {boot_})", 0 <= mono < 1500 and boot_ - mono >= 800, r)
        check(f"{tag}: vDSO = system call (mono {r.get('dv_mono_us')} boot {r.get('dv_boot_us')} real {r.get('dv_real_us')} us)",
              all(int(r.get(k, 99999)) <= 2000 for k in ("dv_mono_us", "dv_boot_us", "dv_real_us")), r)
        check(f"{tag}: the vDSO never went back on the other CPU", r.get("back") == "0", r)
        check(f"{tag}: the other CPU runs after the resume", int(r.get("after", 0)) > 1000, r)
    for i, nap in enumerate(naps):
        nm, nb = int(nap.get("mono_ms", -1)), int(nap.get("boot_ms", -1))
        check(f"{mode}: a 3 s nap across the sleep: 3 s of MONOTONIC ({nm} ms), more of BOOTTIME ({nb} ms)",
              2900 <= nm <= 3800 and nb - nm >= 800, nap)
    # REALTIME after the sleep agrees with the host's clock
    m = re.findall(r"SUS-READY real=(\d+)", out)
    if m and info["ready_host"] and res:
        guest0, host0 = int(m[0]), info["ready_host"][0]
        check(f"{mode}: the guest clock started near the host's", abs(guest0 - host0) < 3, (guest0, host0))
    for i, r in enumerate(res):
        if i < len(info.get("result_host", [])) and "real_end" in r:
            d = int(r["real_end"]) - info["result_host"][i]
            check(f"{mode}{i + 1}: REALTIME after the sleep agrees with the host ({d:+.1f} s)", abs(d) < 3, r)


def main():
    want = sys.argv[1] if len(sys.argv) > 1 else "BRPKDWAQTU"
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
    for mode in want:
        out, info = boot(mode)
        check(f"{mode}: no panic, no fault", not re.search(r"panic|BUG:|page fault in kernel|triple", out, re.I), out[-400:])
        hw = (re.findall(r"^SUS-HW power sleep (.*)$", out, re.M) or [""])[-1]
        kv = dict(x.split("=") for x in hw.split() if "=" in x)
        if mode == "B":
            check("B: files as Linux has them, root only, deep/disk refused", "SUS-FILES OK" in out,
                  [l for l in out.split("\n") if l.startswith("SUS-FILES")])
            check("B: deep off without ranix.s3=1, and said why", kv.get("deep_ok") == "0" and
                  ("ranix.s3=1" in out or "no \\_S3_" in out), hw)
        elif mode == "R":
            judge_sleep("R", out, info, 3, "s2idle", "rtc")
            check("R: the wake alarm is spent after it fired", "PM: resume from s2idle" in out, out[-300:])
        elif mode in ("P", "K"):
            judge_sleep(mode, out, info, 1, "s2idle", "power" if mode == "P" else "key")
            if mode == "P":
                check("P: the press that woke is not a shutdown request", "init: SIGPWR" not in out and
                      "acpi: power button" not in out, out[-400:])
        elif mode in ("T", "U"):
            t = (re.findall(r"^SUS-TAIL .*$", out, re.M) or [""])[-1]
            r = fields(t)
            g = lambda k: int(r.get(k, -1))
            check(f"{mode}: the sleep happened", r.get("err") == "0" and g("n") > 500, t)
            check(f"{mode}: no 2.5 ms sleep ended early, the one across the suspend included", g("early") == 0, t)
            check(f"{mode}: no more than 1 in 10 over 300 us late", 0 <= g("late") <= g("n") // 10, t)
            check(f"{mode}: MONOTONIC did not jump across the suspend (longest sleep {g('mono_gap_ms')} ms)",
                  0 <= g("mono_gap_ms") < 200, t)
            check(f"{mode}: BOOTTIME took the sleep ({g('boot_gap_ms')} ms)", g("boot_gap_ms") >= SECS * 1000 - 1100, t)
            check(f"{mode}: every vvar period continues the one before ({g('vvar_bad')} bad of {g('vvar_n')})",
                  g("vvar_bad") == 0 and g("vvar_n") > 100 and g("vvar_skip") >= 1, t)
            if mode == "U":
                check("U: QEMU saw the guest suspended (S3)", info["suspended"], info)
        elif mode == "A":
            check("A: q35's AHCI blocks deep, by name, without sleeping", "PM: ahci has no resume, no S3" in out and
                  re.search(r"SUS-A rc=[1-9]\d* fail=1 step=suspend_devices", out) and not info["suspended"],
                  [l for l in out.split("\n") if l.startswith(("SUS-A", "PM:"))])
        else:
            check(f"{mode}: QEMU saw the guest suspended (S3)", info["suspended"], info)
            if mode == "D":
                check("D: deep offered with ranix.s3=1", "SUS-FILES OK" in out,
                      [l for l in out.split("\n") if l.startswith("SUS-FILES")])
                check("D: the RTC alarm woke S3", not info.get("rtc_s3_failed"), info)
            judge_sleep(mode, out, info, 1, "deep", "rtc" if mode == "D" else "other")
            check(f"{mode}: no CPU lost in S3", kv.get("ap_lost") == "0", hw)
    for f in os.listdir(B):
        if f.endswith((".iso", ".cpio", ".sock")):
            os.unlink(os.path.join(B, f))
    n = sum(results)
    print(f"[suspend] {n}/{len(results)}")
    return 0 if results and n == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
