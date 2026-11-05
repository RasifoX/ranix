#!/usr/bin/env python3
"""Populate an ntfs-3g mount with a Windows-like tree. Usage: gen-tree.py MOUNT PROFILE
Debian only: the images are made by mkntfs and ntfs-3g, the driver never sees them
before they are complete. Deterministic: the same seed gives the same bytes."""
import hashlib, os, random, struct, sys

root, profile = sys.argv[1], sys.argv[2]
big = profile == "big"
mini = profile == "mini"
rng = random.Random(0x5EED + len(profile))


def data(n, kind=0):
    """n bytes: kind 0 random, 1 compressible text, 2 runs and zero gaps."""
    if kind == 1:
        line = b"Lorem ipsum dolor sit amet, consectetur adipiscing elit 0123456789\n"
        return (line * (n // len(line) + 1))[:n]
    if kind == 2:
        out = bytearray()
        while len(out) < n:
            out += bytes([rng.randrange(256)]) * rng.randrange(1, 20000)
            out += bytes(rng.randrange(0, 70000))
        return bytes(out[:n])
    return rng.randbytes(n)


def put(path, blob, sync=False):
    p = os.path.join(root, path)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(blob)


def sa(path, name, value):
    os.setxattr(os.path.join(root, path), name, value)


# ---- Windows skeleton
for d in ["Windows/System32/config", "Windows/WinSxS", "Program Files/Common Files",
          "Program Files (x86)", "ProgramData/Microsoft", "Users/Public/Documents",
          "Users/rasi/AppData/Local/Temp", "Users/rasi/Masaüstü", "Users/rasi/Belgeler"]:
    os.makedirs(os.path.join(root, d), exist_ok=True)
put("Windows/System32/ntoskrnl.exe", data(1234567))
put("Windows/System32/config/SOFTWARE", data(777777, 2))
put("pagefile.sys", bytes(4096))
put("hiberfil.sys", b"wake" + bytes(12284))
put("Users/rasi/NTUSER.DAT", data(262144))
put("bootmgr", data(413738))

# ---- Turkish names and other UTF-16 corners
for name in ["şğüöçıİ.txt", "Öğrenci Çalışması.docx", "ÇĞİÖŞÜ ıiIİ.md", "İstanbul’da kış.jpg",
             "日本語ファイル.txt", "emoji-😀-🚀.txt", "Ελληνικά.txt", "кириллица.txt"]:
    put("Users/rasi/Belgeler/" + name, name.encode() * 50)
put("Users/rasi/Masaüstü/Ödev/Dönem Sonu Raporu/Sonuç.txt", b"Son\xc3\xa7\n")
put("Users/rasi/Belgeler/A.txt", b"upper")
put("Users/rasi/Belgeler/LongFileNameWithShortAlias-0123456789.txt", b"8.3 alias")

# ---- long names and long paths
n255 = "a" * 251 + ".txt"
put("long/" + n255, b"ascii 255")
p = "long/deep"
for i in range(40):                   # a path of about 2 KB
    p += "/" + ("dizin%02d-" % i) + "x" * 40
put(p + "/leaf.bin", data(5000))
os.makedirs(os.path.join(root, "long/verydeep"))
here = os.getcwd()
os.chdir(os.path.join(root, "long/verydeep"))
for i in range(90):                   # about 4.5 KB in UTF-8, over PATH_MAX
    n = ("şğ%02d" % i) * 8
    os.mkdir(n)
    os.chdir(n)
with open("en-dip.txt", "wb") as f:
    f.write(b"past PATH_MAX\n")
os.chdir(here)

# ---- sizes around every storage form
for sz in [0, 1, 400, 700, 1023, 1024, 4095, 4096, 4097, 65535, 65536, 65537, 1 << 20]:
    put("sizes/f%d.bin" % sz, data(sz))

# ---- many directories, and a directory big enough for several index levels
for i in range(20 if mini else 300):
    put("many/d%03d/sub%d/f.txt" % (i, i % 7), b"x" * (i % 900))
for i in range(400 if mini else 6000):
    put("Windows/WinSxS/amd64_microsoft-windows-component-%05d_31bf3856ad364e35.manifest" % i,
        b"<assembly id=%d/>" % i)

# ---- sparse: ntfs-3g keeps holes for seeks past the end
def sparse(path, parts, size):
    fn = os.path.join(root, path)
    os.makedirs(os.path.dirname(fn), exist_ok=True)
    with open(fn, "wb") as f:
        for off, blob in parts:
            f.seek(off)
            f.write(blob)
        f.truncate(size)

sparse("sparse/hole-middle.bin", [(0, data(70000)), (5 << 20, data(90000))], 8 << 20)
sparse("sparse/tail-hole.bin", [(0, b"head")], 3 << 20)
sparse("sparse/all-hole.bin", [], 64 << 20)
sparse("sparse/many-holes.bin", [(i * (1 << 17), data(4096)) for i in range(400)], 64 << 20)
if big:
    sparse("sparse/virtual-20g.bin", [(19 << 30, data(1 << 20))], 20 << 30)

# ---- compressed: a directory with the attribute passes it to new files
cdir = os.path.join(root, "packed")
os.makedirs(cdir)
sa("packed", "system.ntfs_attrib_be", struct.pack(">I", 0x800))
put("packed/text.txt", data(2000000, 1))
put("packed/random.bin", data(300000))                  # stays stored, units full
put("packed/mixed.bin", data(1500000, 2))
put("packed/small.txt", data(100, 1))
put("packed/edge-65536.txt", data(65536, 1))
put("packed/edge-65537.txt", data(65537, 1))
fn = os.path.join(cdir, "sparse-and-packed.bin")
with open(fn, "wb") as f:
    f.write(data(200000, 1))
    f.seek(3 << 20)
    f.write(data(200000, 1))
os.makedirs(os.path.join(cdir, "sub"))
put("packed/sub/nested.txt", data(500000, 1))
if big:
    with open(os.path.join(cdir, "big-text.bin"), "wb") as f:
        for i in range(600):
            f.write(data(1 << 20, 2))

# ---- big and fragmented, enough for an $ATTRIBUTE_LIST
if big:
    with open(os.path.join(root, "big-random.bin"), "wb") as f:
        for i in range(2200):
            f.write(rng.randbytes(1 << 20))
fa = os.path.join(root, "frag-a.bin")
fb = os.path.join(root, "frag-b.bin")
with open(fa, "wb") as a, open(fb, "wb") as b:
    for i in range(40000 if big else 1500 if mini else 12000):
        a.write(rng.randbytes(4096)); a.flush()
        b.write(rng.randbytes(4096)); b.flush()
# free space cut into single clusters: the next big file gets thousands of runs,
# too many for one record, and they spread over attribute-list extents
os.makedirs(os.path.join(root, "frag"))
sv = os.statvfs(root)
pieces = min(3000 if mini else 24000, sv.f_bavail * sv.f_frsize // (3 * max(sv.f_frsize, 4096)))
for i in range(pieces):
    with open(os.path.join(root, "frag/p%05d" % i), "wb") as f:
        f.write(b"p" * 4096)
for i in range(0, pieces, 2):
    os.unlink(os.path.join(root, "frag/p%05d" % i))
with open(os.path.join(root, "frag/shattered.bin"), "wb") as f:
    for i in range(pieces // 8):
        f.write(rng.randbytes(16384))
os.makedirs(os.path.join(root, "packed-frag"))
sa("packed-frag", "system.ntfs_attrib_be", struct.pack(">I", 0x800))
with open(os.path.join(root, "packed-frag/shattered.txt"), "wb") as f:
    for i in range(pieces // 20):
        f.write(data(16384, 2))
# many hard links grow the base record past its size as well
put("links/orig.txt", b"linked content\n")
for i in range(300):
    os.link(os.path.join(root, "links/orig.txt"), os.path.join(root, "links/hl-%03d-%s.txt" % (i, "n" * 60)))
# attribute lists also come from many named streams
put("streams/ads.txt", b"main stream\n")
for i in range(40):
    os.setxattr(os.path.join(root, "streams/ads.txt"), "user.stream%02d" % i, b"z" * 300)

put("tiny/A.txt", b"tiny a\n")
put("tiny/b.txt", b"tiny b\n")
put("tiny/c.txt", b"tiny c\n")
put("streams/small.txt", b"small list\n")
for i in range(12):
    os.setxattr(os.path.join(root, "streams/small.txt"), "user.s%02d" % i, b"q" * 180)

# ---- reparse points through ntfs-3g's raw interface
def reparse(path, tag, body, isdir=False):
    fn = os.path.join(root, path)
    os.makedirs(os.path.dirname(fn), exist_ok=True)
    if isdir:
        os.makedirs(fn)
    elif not os.path.exists(fn):
        open(fn, "wb").close()
    os.setxattr(fn, "system.ntfs_reparse_data", struct.pack("<IHH", tag, len(body), 0) + body)


def link_body(target, symlink):
    t = target.encode("utf-16-le")
    sub = ("\\??\\" + target).encode("utf-16-le")
    head = struct.pack("<HHHH", len(t), len(sub), 0, len(t))
    if symlink:
        head += struct.pack("<I", 0)
    return head + t + sub

reparse("reparse/sym-abs", 0xA000000C, link_body("C:\\Windows\\System32", True))
reparse("reparse/sym-escape", 0xA000000C, link_body("..\\..\\..\\etc\\passwd", True))
reparse("reparse/junction", 0xA0000003, link_body("C:\\Users\\rasi", False), True)
reparse("reparse/docs-and-settings", 0xA0000003, link_body("C:\\Users", False), True)
put("reparse/wof.bin", bytes(5000))
reparse("reparse/wof.bin", 0x80000017, struct.pack("<IIII", 1, 1, 1, 2))
put("reparse/cloud-hydrated.bin", b"hydrated data\n" * 100)
reparse("reparse/cloud-hydrated.bin", 0x9000101A, b"\x00" * 16)
reparse("reparse/unknown-tag.bin", 0x8000ABCD, b"\x01" * 32)
reparse("reparse/lx-symlink", 0xA000001D, struct.pack("<I", 2) + b"/mnt/c/Users")
print("tree done")
