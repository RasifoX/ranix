#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
"""Read the Ranix persistent kernel log after a reboot. Needs root.

Ranix mirrors its log into 1 MiB of RAM at physical 0x20000000 (see
docs/watchdog_pstore.md). Boot Linux with memmap=1M$0x20000000 so that RAM stays
untouched and /dev/mem may read it. Without that, /proc/kcore is tried, but Linux
may already have reused the memory and only the CRCs can tell.

    ranix-lastlog.py                 newest boot, from RAM
    ranix-lastlog.py --all           both slots
    ranix-lastlog.py --file ram.bin  a raw dump of the region
"""
import argparse
import struct
import sys
import zlib

BASE = 0x20000000
REGION = 1 << 20
SLOT = REGION // 2
HDR = 4096
HDR_FMT = "<8sIIQQQII64sI"
HDR_LEN = struct.calcsize(HDR_FMT)
CRC_OFF = HDR_LEN - 4
STATES = {
    1: "no orderly end (hang, watchdog or power loss)",
    2: "clean reboot",
    3: "clean poweroff",
    4: "panic",
}


def read_devmem(base):
    with open("/dev/mem", "rb", buffering=0) as f:
        f.seek(base)
        out = bytearray()
        while len(out) < REGION:
            chunk = f.read(REGION - len(out))
            if not chunk:
                break
            out += chunk
    return bytes(out)


def read_kcore(base):
    """Find the direct-map segment holding the physical range in /proc/kcore."""
    with open("/proc/kcore", "rb") as f:
        ehdr = f.read(64)
        phoff = struct.unpack_from("<Q", ehdr, 32)[0]
        phentsize, phnum = struct.unpack_from("<HH", ehdr, 54)
        f.seek(phoff)
        table = f.read(phentsize * phnum)
        for i in range(phnum):
            ptype, _, off, _, paddr, filesz, _, _ = struct.unpack_from(
                "<IIQQQQQQ", table, i * phentsize)
            if ptype == 1 and paddr <= base and base + REGION <= paddr + filesz:
                f.seek(off + base - paddr)
                return f.read(REGION)
    raise OSError("region not found in /proc/kcore")


def parse_slot(raw):
    magic, ver, dsize, seq, total, beat, state, dcrc, reason, hcrc = struct.unpack_from(
        HDR_FMT, raw)
    if magic != b"RANIXLOG" or dsize != SLOT - HDR:
        return None
    data = raw[HDR:HDR + dsize]
    if total <= dsize:
        text = data[:total]
        data_ok = zlib.crc32(text) == dcrc
    else:
        head = total % dsize
        text = data[head:] + data[:head]
        data_ok = None
    return {
        "version": ver,
        "seq": seq,
        "total": total,
        "beat_ticks": beat,
        "state": state,
        "reason": reason.split(b"\0")[0].decode("utf-8", "replace"),
        "hdr_ok": zlib.crc32(raw[:CRC_OFF]) == hcrc,
        "data_ok": data_ok,
        "text": text,
    }


def show(s):
    data = {True: "intact", False: "CRC MISMATCH", None: "wrapped, not checkable"}[s["data_ok"]]
    print("=== Ranix boot #%d: %s" % (s["seq"], STATES.get(s["state"], "unknown")))
    if s["reason"]:
        print("    reason: " + s["reason"])
    print("    %d bytes logged, data %s, header %s, last pet tick %d" % (
        s["total"], data, "ok" if s["hdr_ok"] else "CRC MISMATCH", s["beat_ticks"]))
    sys.stdout.write(s["text"].decode("utf-8", "replace").replace("\r", ""))
    if not s["text"].endswith(b"\n"):
        print()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--file", help="raw dump of the region instead of live RAM")
    ap.add_argument("--base", type=lambda v: int(v, 0), default=BASE)
    ap.add_argument("--offset", type=lambda v: int(v, 0), default=0,
                    help="byte offset of the region inside --file")
    ap.add_argument("--all", action="store_true", help="print both slots, oldest first")
    args = ap.parse_args()

    if args.file:
        with open(args.file, "rb") as f:
            f.seek(args.offset)
            raw = f.read(REGION)
    else:
        try:
            raw = read_devmem(args.base)
        except OSError as e:
            print("/dev/mem: %s; trying /proc/kcore, which may show reused memory" % e,
                  file=sys.stderr)
            raw = read_kcore(args.base)

    slots = [s for s in (parse_slot(raw[:SLOT]), parse_slot(raw[SLOT:SLOT * 2])) if s]
    if not slots:
        print("no Ranix log found: RAM did not survive, or Ranix never ran", file=sys.stderr)
        return 1
    slots.sort(key=lambda s: s["seq"])
    for s in slots if args.all else slots[-1:]:
        show(s)
    return 0


if __name__ == "__main__":
    sys.exit(main())
