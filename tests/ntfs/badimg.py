#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
"""Hand-made bad volumes. Reads a clean NTFS image, writes one patch file per case: each is
a list of (offset, bytes) against the image, so nothing but the test VM's disk changes.
Usage: badimg.py IMAGE OUTDIR. Patch format: u64 off, u32 len, data, repeated."""
import os, struct, sys

img, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
f = open(img, "rb")


def rd(off, n):
    f.seek(off)
    return bytearray(f.read(n))


boot = rd(0, 512)
bps = struct.unpack_from("<H", boot, 11)[0]
spc = boot[13]
cs = bps * spc
mft_lcn = struct.unpack_from("<Q", boot, 48)[0]
rs_c = struct.unpack_from("<b", boot, 64)[0]
RS = 1 << -rs_c if rs_c < 0 else rs_c * cs


def runs(attr):
    p = struct.unpack_from("<H", attr, 32)[0]
    out, lcn = [], 0
    while attr[p]:
        h = attr[p]
        ln, on = h & 15, h >> 4
        p += 1
        length = int.from_bytes(attr[p:p + ln], "little")
        p += ln
        if on:
            lcn += int.from_bytes(attr[p:p + on], "little", signed=True)
            out.append((lcn, length))
        else:
            out.append((None, length))
        p += on
    return out


def unfix(b):
    uo, uc = struct.unpack_from("<HH", b, 4)
    st = len(b) // (uc - 1)
    for i in range(1, uc):
        b[i * st - 2:i * st] = b[uo + 2 * i:uo + 2 * i + 2]
    return b


def refix(b):
    uo, uc = struct.unpack_from("<HH", b, 4)
    st = len(b) // (uc - 1)
    for i in range(1, uc):
        b[uo + 2 * i:uo + 2 * i + 2] = b[i * st - 2:i * st]
        b[i * st - 2:i * st] = b[uo:uo + 2]
    return b


rec0 = unfix(rd(mft_lcn * cs, RS))


def attrs(r):
    p = struct.unpack_from("<H", r, 20)[0]
    while True:
        t = struct.unpack_from("<I", r, p)[0]
        if t == 0xFFFFFFFF:
            return
        ln = struct.unpack_from("<I", r, p + 4)[0]
        yield t, p, ln
        p += ln


mft_runs = []
for t, p, ln in attrs(rec0):
    if t == 0x80:
        mft_runs = runs(rec0[p:p + ln])


def vcn_to_off(rs, vcn):
    for lcn, n in rs:
        if vcn < n:
            return (lcn + vcn) * cs
        vcn -= n


def rec_off(no):
    b = no * RS
    return vcn_to_off(mft_runs, b // cs) + b % cs


def rec(no):
    return unfix(rd(rec_off(no), RS))


def name_of(r):
    best = None
    for t, p, ln in attrs(r):
        if t == 0x30:
            vo = struct.unpack_from("<H", r, p + 20)[0] + p
            parent = struct.unpack_from("<Q", r, vo)[0] & ((1 << 48) - 1)
            n = r[vo + 64]
            if r[vo + 65] != 2:
                best = (parent, r[vo + 66:vo + 66 + 2 * n].decode("utf-16-le", "replace"))
    return best


index = {}
total = 0
for t, p, ln in attrs(rec0):
    if t == 0x80:
        total = struct.unpack_from("<Q", rec0, p + 48)[0] // RS
for no in range(total):
    try:
        r = rec(no)
    except Exception:
        continue
    if r[:4] != b"FILE" or not struct.unpack_from("<H", r, 22)[0] & 1:
        continue
    if struct.unpack_from("<Q", r, 32)[0]:
        continue
    n = name_of(r)
    if n:
        index[no] = n


def path(no):
    parts = []
    while no != 5:
        if no not in index:
            return None
        p, nm = index[no]
        parts.append(nm)
        no = p
    return "/" + "/".join(reversed(parts))


byp = {path(n): n for n in index}


def emit(case, patches):
    with open(os.path.join(out, case + ".pat"), "wb") as o:
        for off, data in patches:
            o.write(struct.pack("<QI", off, len(data)) + bytes(data))


def rewrite(no, edit):
    r = rec(no)
    edit(r)
    return [(rec_off(no), refix(r))]


def attr_at(r, typ):
    for t, p, ln in attrs(r):
        if t == typ:
            return p, ln


def data_edit(fn, path_):
    """A case changing the unnamed $DATA header of a file."""
    no = byp[path_]

    def e(r):
        p, ln = attr_at(r, 0x80)
        fn(r, p, ln)
    return rewrite(no, e)


def root_entry(no, name):
    """Offset inside record no of the index-root entry for name."""
    r = rec(no)
    p, ln = attr_at(r, 0x90)
    vo = struct.unpack_from("<H", r, p + 20)[0] + p
    e = vo + 16 + struct.unpack_from("<I", r, vo + 16)[0]
    while True:
        el, kl, fl = struct.unpack_from("<HHH", r, e + 8)
        if fl & 2:
            return None
        n = r[e + 16 + 64]
        if r[e + 16 + 66:e + 16 + 66 + 2 * n].decode("utf-16-le") == name:
            return e
        e += el


cases = {}

# ---- boot sector
def boot_case(name, fn):
    b = bytearray(boot)
    fn(b)
    cases[name] = [(0, b)]

boot_case("boot-bitlocker", lambda b: b.__setitem__(slice(3, 11), b"-FVE-FS-"))
boot_case("boot-no-sig", lambda b: b.__setitem__(510, 0))
boot_case("boot-cluster0", lambda b: b.__setitem__(13, 0))
boot_case("boot-cluster3", lambda b: b.__setitem__(13, 3))
boot_case("boot-bps1", lambda b: struct.pack_into("<H", b, 11, 1))
boot_case("boot-mft-past-end", lambda b: struct.pack_into("<Q", b, 48, 1 << 50))
boot_case("boot-sectors-huge", lambda b: struct.pack_into("<Q", b, 40, (1 << 62)))
boot_case("boot-rec-zero", lambda b: b.__setitem__(64, 0))
boot_case("boot-rec-huge", lambda b: b.__setitem__(64, 0x7F))

# ---- $MFT record
def mft_case(name, fn):
    r = rec(0)
    fn(r)
    cases[name] = [(rec_off(0), refix(r))]

def mft_data(fn):
    def e(r):
        p, ln = attr_at(r, 0x80)
        fn(r, p, ln)
    return e

mft_case("mft-size-huge", mft_data(lambda r, p, ln: struct.pack_into("<Q", r, p + 48, 1 << 61)))
mft_case("mft-lowest-nonzero", mft_data(lambda r, p, ln: struct.pack_into("<Q", r, p + 16, 7)))
mft_case("mft-attr-len0", mft_data(lambda r, p, ln: struct.pack_into("<I", r, p + 4, 0)))
mft_case("mft-attr-len-huge", mft_data(lambda r, p, ln: struct.pack_into("<I", r, p + 4, 0x7FFFFFF8)))
mft_case("mft-used-huge", lambda r: struct.pack_into("<I", r, 24, 0xFFFFFFF0))
mft_case("mft-first-attr-end", lambda r: struct.pack_into("<H", r, 20, RS))
cases["mft-torn"] = [(mft_lcn * cs + 510, b"\x12\x34")]

# ---- one file's data
F = "/Windows/System32/ntoskrnl.exe"
cases["run-lcn-past-end"] = data_edit(lambda r, p, ln: r.__setitem__(p + struct.unpack_from("<H", r, p + 32)[0], 0x81) or r.__setitem__(slice(p + struct.unpack_from("<H", r, p + 32)[0] + 1, p + struct.unpack_from("<H", r, p + 32)[0] + 10), b"\x01" + b"\xFF" * 7 + b"\x7F"), F)
cases["run-no-terminator"] = data_edit(lambda r, p, ln: r.__setitem__(slice(p + struct.unpack_from("<H", r, p + 32)[0], p + ln), b"\x11\x01\x01" * ((ln - struct.unpack_from("<H", r, p + 32)[0]) // 3 + 1)), F)
cases["run-offset-past-attr"] = data_edit(lambda r, p, ln: struct.pack_into("<H", r, p + 32, ln + 8), F)
cases["data-size-2e62"] = data_edit(lambda r, p, ln: struct.pack_into("<Q", r, p + 48, 1 << 62), F)
cases["data-size-2e63"] = data_edit(lambda r, p, ln: struct.pack_into("<Q", r, p + 48, 1 << 63), F)
cases["data-init-past-size"] = data_edit(lambda r, p, ln: struct.pack_into("<Q", r, p + 56, 1 << 40), F)
cases["data-compress-unit-31"] = data_edit(lambda r, p, ln: (r.__setitem__(p + 12, 1), r.__setitem__(p + 34, 31)), F)
cases["data-encrypted-flag"] = data_edit(lambda r, p, ln: r.__setitem__(p + 13, 0x40), F)
cases["torn-file-record"] = [(rec_off(byp[F]) + 510, b"\x55\xAA")]

# ---- one compressed file
P = "/packed/text.txt"
no = byp[P]
r = rec(no)
p, ln = attr_at(r, 0x80)
rl = [x for x in runs(r[p:p + ln]) if x[0] is not None]
cases["lznt1-garbage-chunk"] = [(rl[0][0] * cs, b"\xFF\xBF" + b"\xFF" * 64)]
cases["lznt1-bad-backref"] = [(rl[0][0] * cs, b"\x02\xB0\x01\xFF\xFF\x00\x00")]

# ---- directory entries
D = byp["/tiny"]
e = root_entry(D, "A.txt")
if e:
    o = rec_off(D)
    def edit(name):
        def fn(r):
            r[e + 16 + 64] = len(name)
            n = name.encode("utf-16-le")
            r[e + 16 + 66:e + 16 + 66 + len(n)] = n
        return fn
    cases["name-dotdot"] = rewrite(D, edit(".."))
    cases["name-slash"] = rewrite(D, edit("a/b"))
    cases["name-len-past-key"] = rewrite(D, lambda r: r.__setitem__(e + 16 + 64, 255))
    cases["entry-len-zero"] = rewrite(D, lambda r: struct.pack_into("<H", r, e + 8, 0))
    cases["entry-len-huge"] = rewrite(D, lambda r: struct.pack_into("<H", r, e + 8, 0xFFF8))
    cases["entry-key-huge"] = rewrite(D, lambda r: struct.pack_into("<H", r, e + 10, 0xFFF0))
    cases["entry-mref-past-end"] = rewrite(D, lambda r: struct.pack_into("<Q", r, e, (1 << 47)))
    cases["entry-stale-seq"] = rewrite(D, lambda r: struct.pack_into("<H", r, e + 6, 0x7777))
    # a directory that holds itself, and one that holds its parent's parent
    cases["dir-holds-itself"] = rewrite(D, lambda r: struct.pack_into("<Q", r, e, D | (struct.unpack_from("<H", r, 16)[0] << 48)))
    U = byp["/Users"]
    cases["dir-holds-ancestor"] = rewrite(D, lambda r: struct.pack_into("<Q", r, e, U | (rec(U)[16] | rec(U)[17] << 8) << 48))

# ---- index blocks
ws = rec(byp["/Windows/WinSxS"])
ip, il = attr_at(ws, 0xA0)
irun = [x for x in runs(ws[ip:ip + il]) if x[0] is not None][0]
ib = irun[0] * cs
cases["indx-zeroed"] = [(ib, bytes(4096))]
cases["indx-torn"] = [(ib + 1022, b"\x99\x99")]
cases["indx-offsets-huge"] = [(ib + 24, struct.pack("<II", 0xFFFFFFF0, 0xFFFFFFF0))]
bp, bl = attr_at(ws, 0xB0)
if ws[bp + 8] == 0:
    bvo = struct.unpack_from("<H", ws, bp + 20)[0] + bp
    cases["index-bitmap-all-set"] = rewrite(byp["/Windows/WinSxS"], lambda r: r.__setitem__(slice(bvo, bvo + 8), b"\xFF" * 8))

# ---- volume state
vr = rec(3)
vp, vl = attr_at(vr, 0x70)
vo3 = struct.unpack_from("<H", vr, vp + 20)[0] + vp
cases["vol-dirty"] = rewrite(3, lambda r: struct.pack_into("<H", r, vo3 + 10, 1))
if "/hiberfil.sys" in byp:
    hr = rec(byp["/hiberfil.sys"])
    hp, hl = attr_at(hr, 0x80)
    if hr[hp + 8]:
        hrun = [x for x in runs(hr[hp:hp + hl]) if x[0] is not None][0]
        cases["vol-hibernated"] = [(hrun[0] * cs, b"HIBR")]

# ---- attribute list, in its own cluster or in the record
for no in range(24, total):
    r = rec(no)
    if r[:4] != b"FILE" or not struct.unpack_from("<H", r, 22)[0] & 1 or struct.unpack_from("<Q", r, 32)[0]:
        continue
    if not any(t == 0x20 for t, _, _ in attrs(r)):
        continue
    pl, ll = attr_at(r, 0x20)
    if r[pl + 8]:
        at = [x for x in runs(r[pl:pl + ll]) if x[0] is not None][0][0] * cs
        cases["list-entry-len0"] = [(at + 4, b"\0\0")]
        cases["list-entry-huge"] = [(at + 4, b"\xF8\xFF")]
        cases["list-wrong-record"] = [(at + 16, struct.pack("<Q", 1))]
        print("attribute list cases on record", no)
        break

for k, v in cases.items():
    emit(k, v)
print(len(cases), "cases,", len(byp), "paths,", "record", RS, "cluster", cs)
