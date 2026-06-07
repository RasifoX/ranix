#!/usr/bin/env python3
# Minimal host-side `repo-add`: build <repodir>/myrepo.db (a gzipped tar of
# <pkg>/desc records) over one or more .pkg.tar.zst files, so `pacman -Sy` can
# read a local file:// repo on Ranix. The host has no pacman/repo-add; we derive
# every field libalpm needs from the package's .PKGINFO + the file itself.
#
#   arch-repo-add.py <repodir> <pkg.tar.zst> [pkg.tar.zst...]
import sys, os, io, gzip, tarfile, hashlib, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))


def unzst(path):
    out = path + ".tar"
    subprocess.check_call([sys.executable, os.path.join(HERE, "zstdcat.py"), path, out])
    data = open(out, "rb").read()
    os.unlink(out)
    return data


def pkginfo(tar_bytes):
    t = tarfile.open(fileobj=io.BytesIO(tar_bytes))
    info = {}
    isize = 0
    for m in t.getmembers():
        base = m.name[2:] if m.name.startswith("./") else m.name
        if base == ".PKGINFO":
            for line in t.extractfile(m).read().decode().splitlines():
                if "=" in line and not line.startswith("#"):
                    k, v = line.split("=", 1)
                    info.setdefault(k.strip(), []).append(v.strip())
        elif m.isreg() and base not in (".BUILDINFO", ".MTREE"):
            isize += m.size
    info["_isize"] = [str(isize)]
    return info


def desc(repodir, pkgpath):
    raw = open(pkgpath, "rb").read()
    info = pkginfo(unzst(pkgpath))
    name = info["pkgname"][0]
    ver = info["pkgver"][0]
    fields = [
        ("FILENAME", [os.path.basename(pkgpath)]),
        ("NAME", info["pkgname"]),
        ("BASE", info.get("pkgbase", info["pkgname"])),
        ("VERSION", info["pkgver"]),
        ("DESC", info.get("pkgdesc", [""])),
        ("CSIZE", [str(len(raw))]),
        ("ISIZE", info["_isize"]),
        ("MD5SUM", [hashlib.md5(raw).hexdigest()]),
        ("SHA256SUM", [hashlib.sha256(raw).hexdigest()]),
        ("URL", info.get("url", [""])),
        ("ARCH", info.get("arch", ["x86_64"])),
        ("BUILDDATE", info.get("builddate", ["0"])),
        ("PACKAGER", info.get("packager", ["Ranix"])),
        ("PROVIDES", info.get("provides", [])),
        ("DEPENDS", info.get("depend", [])),
    ]
    body = ""
    for tag, vals in fields:
        vals = [v for v in vals if v != ""] if tag in ("PROVIDES", "DEPENDS") else vals
        if not vals:
            continue
        body += "%%%s%%\n%s\n\n" % (tag, "\n".join(vals))
    return "%s-%s" % (name, ver), body


def main():
    repodir = sys.argv[1]
    entries = [desc(repodir, p) for p in sys.argv[2:]]
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as tar:
        for dirname, body in entries:
            data = body.encode()
            ti = tarfile.TarInfo(dirname + "/desc")
            ti.size = len(data)
            tar.addfile(ti, io.BytesIO(data))
    with gzip.open(os.path.join(repodir, "myrepo.db"), "wb") as f:
        f.write(buf.getvalue())


if __name__ == "__main__":
    main()
