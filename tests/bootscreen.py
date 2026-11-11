#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
"""What the screen shows while Ranix boots and shuts down, in QEMU.

Four boots of build/ranix.elf with build/initramfs and a /test.sh of our own;
pictures come from the QEMU monitor (screendump), keys from sendkey.

  Q  quiet          the boot screen (orca, bar, no text); serial has no info
                    lines but dmesg has them all; Esc shows the log; "poweroff"
                    brings the shutdown screen back; a <3> line breaks through
  V  classic        (asks GRUB for 1600x1200) the top text row is the first kernel message
                    (lines from before the console existed are not lost)
  D  quiet, font 2  "done" with no desktop: the text console comes back after
                    15 s; ranix.fontscale=2 gives 16x32 cells
  L  quiet          "log" written to the switch shows the log at once

Usage: python3 tests/bootscreen.py   (after make ARCH=x86_64 iso; QEMU_CMD as for make test)
Prints PASS/FAIL lines and "[bootscreen] N/M"; exit 0 only when all pass.
"""
import os, re, shlex, shutil, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
B = "build/bootscreen"
QEMU = shlex.split(os.environ.get("QEMU_CMD", "qemu-system-x86_64"))
results = []

# the boot screen's colours (kernel/tty/splash.c) and the console's text
BG, BODY, WHITE, FILL, TRACK, TEXT = 0x0B121A, 0x2E4862, 0xE8EEF4, 0x9CC2E2, 0x1C2A38, 0xC8D4E0
CON_FG, CON_BG = 0xFFFFFF, 0x000000


def check(name, ok, why=""):
    results.append(bool(ok))
    print(("PASS: " if ok else "FAIL: ") + name + ("" if ok else "  (" + str(why)[:300] + ")"), flush=True)


GUEST = r'''#!/bin/bash
S=/proc/sys/kernel/ranix_splash
M=$(tr ' ' '\n' < /proc/cmdline | sed -n 's/^bs.mode=//p')
st() { cat $S 2>/dev/null || echo none; }
waitst() { for i in $(seq 1 $2); do case "$(st)" in "$1"*) return 0;; esac; sleep 0.1; done; return 1; }
ready() { echo "BS-READY $1"; sleep 3; }
echo "BS-MODE $M state=$(st)"
echo "BS-KMSG $(dmesg | grep -c '] console: ') $(dmesg | wc -l)"
dmesg | grep '] console: ' | sed 's/^/BS-CON /'
case $M in
Q) ready splash
   waitst log 300; echo "BS-ESC $(st)"; sleep 0.5; ready log
   echo poweroff > $S; echo "BS-DOWN $(st)"; ready down
   echo '<3>bootscreen: a deliberate error' > /dev/kmsg
   waitst log 100; echo "BS-ERR $(st)"; sleep 0.5; ready err ;;
V) ready classic ;;
D) echo done > $S; sleep 17; echo "BS-DONE $(st)"; ready done ;;
L) echo "BS-BEFORE $(st)"; echo log > $S; echo "BS-LOG $(st)"; sleep 0.5; ready log ;;
R) echo "BS-RESCUE $(st)"; ready rescue ;;
H) sleep 8; echo "BS-BOOTSTALL $(st)"; ready bootstall
   echo poweroff > $S; echo "BS-DOWN $(st)"; sleep 8; echo "BS-DOWNSTALL $(st)"; ready downstall ;;
esac
echo BS-END
poweroff -f
'''


def font():
    """Spleen 8x16 from the kernel's own table: codepoint -> 16 row bytes."""
    s = open("kernel/tty/font_spleen.h").read()
    cps = [int(x, 0) for x in re.findall(r"0x[0-9A-Fa-f]+|\d+", re.search(r"spleen_cps\[\d+\]\s*=\s*\{(.*?)\};", s, re.S).group(1))]
    body = re.search(r"spleen_glyphs\[\d+\]\[16\]\s*=\s*\{(.*?)\n\};", s, re.S).group(1)
    rows = re.findall(r"\{([^}]*)\}", body)
    return {cp: [int(x, 16) for x in r.split(",")] for cp, r in zip(cps, rows)}


def ppm(path):
    d = open(path, "rb").read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", d)
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, d[m.end():]


def pix(img, x, y):
    w, h, d = img
    i = (y * w + x) * 3
    return (d[i] << 16) | (d[i + 1] << 8) | d[i + 2]


def count(img, colour):
    w, h, d = img
    c = bytes([(colour >> 16) & 255, (colour >> 8) & 255, colour & 255])
    return sum(1 for i in range(0, len(d), 3) if d[i:i + 3] == c)


def find_row(img, glyphs, text):
    """The text row (0-based) whose start is exactly text in the console font, or -1."""
    w, h, _ = img
    for r in range(h // 16):
        bad = 0
        for i, ch in enumerate(text):
            g = glyphs.get(ord(ch), glyphs[0x20])
            for y in range(16):
                for x in range(8):
                    if (pix(img, i * 8 + x, r * 16 + y) == CON_FG) != bool(g[y] & (0x80 >> x)):
                        bad += 1
            if bad:
                break
        if not bad:
            return r
    return -1


def row_text(img, glyphs, text, scale=1):
    """Pixels of the top text row that differ from text drawn in the console font."""
    bad = 0
    for i, ch in enumerate(text):
        g = glyphs.get(ord(ch), glyphs[0x20])
        for y in range(16 * scale):
            for x in range(8 * scale):
                on = g[y // scale] & (0x80 >> (x // scale))
                if (pix(img, i * 8 * scale + x, y) == CON_FG) != bool(on):
                    bad += 1
    return bad


class Vm:
    def __init__(self, mode, cmdline, gfx=""):
        self.mode = mode
        iso = f"{B}/iso"
        shutil.rmtree(iso, ignore_errors=True)
        os.makedirs(f"{iso}/boot/grub")
        shutil.copy("build/ranix.elf", f"{iso}/boot/ranix.elf")
        os.link(f"{B}/initramfs.cpio", f"{iso}/boot/initramfs.cpio")
        g = f"  set gfxpayload={gfx}\n" if gfx else ""
        open(f"{iso}/boot/grub/grub.cfg", "w").write(
            f'set timeout=0\nset default=0\nmenuentry "Ranix" {{\n{g}  multiboot2 /boot/ranix.elf {cmdline} bs.mode={mode}\n'
            f'  module2 /boot/initramfs.cpio\n}}\n')
        subprocess.run(["grub-mkrescue", "-o", f"{B}/bs.iso", iso], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        shutil.rmtree(iso)
        self.log = f"{B}/serial-{mode}.log"
        self.mon = f"{B}/mon-{mode}.sock"
        for f in (self.log, self.mon):
            if os.path.exists(f):
                os.unlink(f)
        self.p = subprocess.Popen(QEMU + ["-smp", "2", "-m", "2G", "-display", "none", "-no-reboot",
                                          "-serial", f"file:{self.log}", "-boot", "d", "-cdrom", f"{B}/bs.iso",
                                          "-monitor", f"unix:{self.mon},server,nowait"],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if os.path.exists(self.mon):
                break
            time.sleep(0.1)
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(self.mon)
        self.s.settimeout(20)
        self.cmd(None)

    def cmd(self, c):
        if c:
            self.s.sendall(c.encode() + b"\n")
        buf = b""
        while not buf.endswith(b"(qemu) "):
            buf += self.s.recv(4096)
        return buf

    def serial(self):
        try:
            return open(self.log, "rb").read().decode("utf-8", "replace").replace("\r", "")
        except OSError:
            return ""

    def wait(self, word, t=120):
        end = time.time() + t
        while time.time() < end:
            if word in self.serial():
                return True
            if self.p.poll() is not None:
                return word in self.serial()
            time.sleep(0.1)
        return False

    def line(self, key):
        m = re.findall(r"^" + re.escape(key) + r"(?: (.*))?$", self.serial(), re.M)   # BS-DOWN is not BS-DOWNSTALL
        return m[-1] if m else None

    def shot(self, name):
        path = os.path.abspath(f"{B}/{self.mode}-{name}.ppm")
        self.cmd(f"screendump {path}")
        for _ in range(50):
            if os.path.exists(path) and os.path.getsize(path) > 100:
                break
            time.sleep(0.1)
        time.sleep(0.2)
        return ppm(path)

    def end(self):
        self.wait("BS-END", 30)
        try:
            self.p.wait(30)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
        self.s.close()


def splash_like(img):
    w, h, _ = img
    return pix(img, 2, 2) == BG and pix(img, w - 3, h - 3) == BG


def boot_q():
    v = Vm("Q", "quiet")
    ok = v.wait("BS-READY splash")
    check("Q: quiet boot reached the test", ok, v.serial()[-300:])
    if not ok:
        return v.end()
    a = v.shot("splash")
    before = "\n".join(l for l in v.serial().split("BS-READY splash")[0].split("\n") if not l.startswith("BS-"))
    check("Q: boot screen from the first frame (field in the corners)", splash_like(a), hex(pix(a, 2, 2)))
    check("Q: the orca is drawn", count(a, BODY) > 5000 and count(a, WHITE) > 1000, (count(a, BODY), count(a, WHITE)))
    check("Q: progress bar", count(a, FILL) > 50 and count(a, TRACK) > 0, (count(a, FILL), count(a, TRACK)))
    check("Q: no console text on the screen", count(a, CON_FG) == 0, count(a, CON_FG))
    check("Q: serial shows no info lines", "] console: " not in before and "boot: framebuffer" not in before, before[:300])
    k = (v.line("BS-KMSG") or "0 0").split()
    check("Q: the log keeps every line (dmesg)", k[0] == "1" and int(k[1]) > 30, k)
    check("Q: switch reads boot", (v.line("BS-MODE") or "").startswith("Q state=boot"), v.line("BS-MODE"))
    v.cmd("sendkey esc")
    ok = v.wait("BS-READY log", 60)
    check("Q: Esc shows the log", ok and (v.line("BS-ESC") or "").startswith("log"), v.line("BS-ESC"))
    b = v.shot("log")
    q_glyphs = font()
    k = find_row(b, q_glyphs, "-- konsol --")
    check("Q: the reveal keeps what programs wrote (console under the log)",
          k >= 0 and find_row(b, q_glyphs, "BS-MODE Q") > k, k)
    check("Q: log on the screen (text, no field)", count(b, CON_FG) > 2000 and count(b, BG) == 0,
          (count(b, CON_FG), count(b, BG)))
    ok = v.wait("BS-READY down", 30)
    c = v.shot("down")
    check("Q: shutdown screen", ok and (v.line("BS-DOWN") or "").startswith("down") and splash_like(c) and
          count(c, BODY) > 5000 and count(c, TEXT) > 50 and count(c, CON_FG) == 0,
          (v.line("BS-DOWN"), hex(pix(c, 2, 2)), count(c, TEXT), count(c, CON_FG)))
    ok = v.wait("BS-READY err", 30)
    d = v.shot("err")
    check("Q: an error line breaks through", ok and (v.line("BS-ERR") or "").startswith("log") and
          count(d, CON_FG) > 2000 and count(d, BG) == 0, (v.line("BS-ERR"), count(d, CON_FG)))
    v.end()


def boot_v(glyphs):
    v = Vm("V", "", "1600x1200x32")
    ok = v.wait("BS-READY classic")
    check("V: classic boot reached the test", ok, v.serial()[-300:])
    if not ok:
        return v.end()
    a = v.shot("classic")
    s = v.serial()
    m = re.search(r"\] console: (\d+)x(\d+) framebuffer, (\d+)x(\d+) cells, font 8x16", s)
    check("V: console line, cells from the framebuffer size", m and int(m.group(3)) == int(m.group(1)) // 8 and
          int(m.group(4)) == int(m.group(2)) // 16, re.findall(r"console: .*", s)[:1])
    check("V: switch reads off", (v.line("BS-MODE") or "") == "V state=off 0", v.line("BS-MODE"))
    lines = [l for l in s.split("\n") if l.strip()]
    first = lines[0][:200] if lines else ""
    shown = s.split("BS-READY classic")[0].count("\n") + 1
    check("V: whole boot fits the screen (test premise)", shown < 75, shown)
    bad = row_text(a, glyphs, first)
    check("V: top row is the first kernel message", first and bad == 0, f"{bad} pixels differ from {first!r}")
    check("V: no boot screen", count(a, BG) == 0 and count(a, BODY) == 0, count(a, BG))
    v.end()


def boot_d():
    v = Vm("D", "quiet ranix.fontscale=2")
    ok = v.wait("BS-READY done", 90)
    check("D: done reached", ok, v.serial()[-300:])
    if not ok:
        return v.end()
    a = v.shot("done")
    check("D: no desktop after done: the text console", (v.line("BS-DONE") or "").startswith("off 100") and
          count(a, BG) == 0 and count(a, CON_FG) > 500, (v.line("BS-DONE"), count(a, BG), count(a, CON_FG)))
    con = v.line("BS-CON") or ""
    check("D: ranix.fontscale=2 gives 16x32 cells", con.endswith("font 16x32"), con)
    v.end()


def boot_r():
    v = Vm("R", "quiet fjord.rescue")
    ok = v.wait("BS-READY rescue", 90)
    st = v.line("BS-RESCUE") or ""
    a = v.shot("rescue") if ok else None
    check("R: fjord.rescue: no boot screen over the shell", ok and st.startswith("off") and count(a, BG) == 0 and
          count(a, CON_FG) > 500, (st, a and count(a, BG)))
    v.end()


def boot_h():
    v = Vm("H", "quiet ranix.splash_stall=5")
    ok = v.wait("BS-READY bootstall", 90)
    check("H: a boot that stalls shows the log", ok and (v.line("BS-BOOTSTALL") or "").startswith("log"),
          v.line("BS-BOOTSTALL"))
    ok = v.wait("BS-READY downstall", 60)
    a = v.shot("downstall") if ok else None
    check("H: a shutdown that hangs shows the log", ok and (v.line("BS-DOWN") or "").startswith("down") and
          (v.line("BS-DOWNSTALL") or "").startswith("log") and count(a, BG) == 0 and count(a, CON_FG) > 2000,
          (v.line("BS-DOWN"), v.line("BS-DOWNSTALL")))
    v.end()


def boot_x():
    v = Vm("X", "quiet panic_test")
    ok = v.wait("[ end of panic ]", 90)
    time.sleep(2)
    a = v.shot("panic") if v.p.poll() is None else None
    check("X: a panic under quiet is on the screen, not the boot screen",
          ok and a is not None and count(a, BG) == 0 and count(a, CON_FG) > 1000, (ok, a and count(a, BG)))
    v.p.kill()
    v.p.wait()
    v.s.close()


def boot_l():
    v = Vm("L", "quiet")
    ok = v.wait("BS-READY log", 90)
    check("L: log word shows the log", ok and (v.line("BS-BEFORE") or "").startswith("boot") and
          (v.line("BS-LOG") or "").startswith("log"), (v.line("BS-BEFORE"), v.line("BS-LOG")))
    if ok:
        a = v.shot("log")
        check("L: log on the screen", count(a, CON_FG) > 2000 and count(a, BG) == 0, count(a, CON_FG))
    v.end()


def main():
    shutil.rmtree(B, ignore_errors=True)
    os.makedirs(B)
    root = f"{B}/root"
    shutil.copytree("build/initramfs", root, symlinks=True, copy_function=os.link)   # links: no second copy
    if os.path.lexists(f"{root}/test.sh"):
        os.unlink(f"{root}/test.sh")                 # never write through a link into build/initramfs
    open(f"{root}/test.sh", "w").write(GUEST)
    os.chmod(f"{root}/test.sh", 0o755)
    for f in ("reboot-on-exit",):
        if os.path.lexists(f"{root}/{f}"):
            os.unlink(f"{root}/{f}")
    subprocess.run(f"cd {root} && find . -print0 | cpio --null -o -H newc --owner=0:0 > ../initramfs.cpio 2>/dev/null",
                   shell=True, check=True)
    shutil.rmtree(root)
    glyphs = font()
    # X (panic_test) is not in the default set: in QEMU the ud2 ends the VM before the
    # panic screen prints, in the old kernel too (pre-existing, its own fix)
    only = sys.argv[1] if len(sys.argv) > 1 else "QVDLRH"
    if "Q" in only: boot_q()
    if "V" in only: boot_v(glyphs)
    if "D" in only: boot_d()
    if "L" in only: boot_l()
    if "R" in only: boot_r()
    if "H" in only: boot_h()
    if "X" in only: boot_x()
    for f in os.listdir(B):
        if f.endswith((".iso", ".cpio", ".sock")):
            os.unlink(os.path.join(B, f))
    n = sum(results)
    print(f"[bootscreen] {n}/{len(results)}")
    return 0 if results and n == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
