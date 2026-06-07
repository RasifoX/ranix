#!/bin/sh
# Fetch an Arch Linux package + its runtime closure as .pkg.tar.zst files into a
# cache dir, for shipping into the initramfs (offline pacman on Ranix, x86_64).
#
#   fetch-arch.sh <cache-dir> <pkg> [pkg...]
#
# Resolves the runtime SHARED-LIBRARY closure from the core+extra repo db files:
# starting from the named package(s) it follows only soname `depend`s
# (`libfoo.so=N-64`), mapping each through %PROVIDES% to the providing package.
# This is the true ELF load closure (what `pacman` and libalpm dlopen/NEED) — it
# deliberately skips pacman's high-level package depends (bash, coreutils, gnupg,
# systemd, ...) that are runtime tools, not loadable libraries Ranix must ship.
# The repo db files are tiny gzip tarballs of per-package `desc` records; we
# parse them in Python (the host has no pacman). Deterministic + re-run safe: an
# already-present, non-empty package is left untouched. glibc is skipped — Ranix
# ships the host glibc, which satisfies the libc soname.
#
# `--pkg <name>` fetches exactly the named package (no closure walk, no SKIP) —
# used to pull glibc, which the closure mode deliberately omits.
set -eu

CACHE="$1"; shift
SINGLE=
if [ "${1:-}" = "--pkg" ]; then SINGLE="$2"; shift 2; fi
PKGS="$*"
MIRROR="${ARCH_MIRROR:-https://mirror.rackspace.com/archlinux}"
PKGDIR="$CACHE/arch-x86_64"
mkdir -p "$PKGDIR"

# Fetch + validate the two repo db files (gzipped tar of desc records). Retry a
# few times: package mirrors occasionally hand back a truncated db.
for repo in core extra; do
	db="$CACHE/arch-$repo.db"
	ok=0
	for try in 1 2 3 4 5; do
		if [ -s "$db" ] && python3 - "$db" >/dev/null 2>&1 <<-'PY'
		import sys,gzip,tarfile,io
		tarfile.open(fileobj=io.BytesIO(gzip.decompress(open(sys.argv[1],"rb").read()))).getmembers()
		PY
		then ok=1; break; fi
		wget -q "$MIRROR/$repo/os/x86_64/$repo.db" -O "$db.tmp" && mv "$db.tmp" "$db" || rm -f "$db.tmp"
	done
	[ "$ok" = 1 ] || { echo "ERROR: $repo.db unavailable/corrupt" >&2; exit 1; }
done

# Resolve the closure in Python: build name->filename, name->depends, and
# soname-provides (`libfoo.so=N-64`) -> providing package, then BFS the deps.
RESOLVED=$(python3 - "$CACHE/arch-core.db" "$CACHE/arch-extra.db" "$PKGS" "$SINGLE" <<'PY'
import sys,gzip,tarfile,io,re
dbs=sys.argv[1:3]; roots=sys.argv[3].split(); single=sys.argv[4] if len(sys.argv)>4 else ""
FN={}; DEP={}; PROV={}
def field(txt,name):
    m=re.search(r'%'+name+r'%\n(.*?)(?:\n\n|\Z)',txt,re.S)
    return m.group(1).split('\n') if m else []
for db in dbs:
    t=tarfile.open(fileobj=io.BytesIO(gzip.decompress(open(db,'rb').read())))
    for m in t.getmembers():
        if not m.name.endswith('/desc'): continue
        txt=t.extractfile(m).read().decode()
        name=field(txt,'NAME')[0]
        FN[name]=field(txt,'FILENAME')[0]
        DEP[name]=[d for d in field(txt,'DEPENDS') if d]
        for p in field(txt,'PROVIDES'):
            if not p: continue
            # `libfoo.so=16-64` -> key the exact major (libfoo.so=16); a plain
            # name (`libfoo`) stays as-is. Keying on the major keeps distinct
            # sonames (libxml2.so=2 in -legacy vs libxml2.so=16) separate.
            PROV[p.rsplit('-',1)[0] if p.endswith('-64') else p]=name
def norm(d):
    # depend `libfoo.so=16-64` -> `libfoo.so=16` to match the PROVIDES key.
    return d.rsplit('-',1)[0] if d.endswith('-64') else d
def sobase(d):
    return re.split(r'[<>=:]',d,1)[0]
def resolve(d):
    k=norm(d)
    if k in PROV: return PROV[k]
    b=sobase(d)
    if b in FN: return b
    if b in PROV: return PROV[b]
    return None
# Pull the full runtime closure, but stop at the high-level *tools* pacman lists
# as depends (these are programs it forks, not libraries it loads — Ranix already
# provides a shell/coreutils, and a fresh gnupg/systemd would fight the running
# system). glibc is skipped: Ranix ships its own (see below). Everything else —
# all soname providers and the small support libs (lz4, libseccomp, gcc-libs,
# gnutls, ...) — is shipped so the pacman ELF graph loads cleanly.
SKIP={'glibc','bash','sh','coreutils','gawk','gettext','grep','sed','gnupg',
      'systemd','ca-certificates','ca-certificates-mozilla','ca-certificates-utils',
      'pacman-mirrorlist','filesystem','iana-etc','tzdata','libmakepkg-dropins',
      'gnulib-l10n'}
if single:
    # Single-package mode: emit exactly this package (used for glibc, which the
    # closure walk skips). No expansion, no SKIP.
    if single in FN: print(single+'\t'+FN[single])
    sys.exit(0)
seen=set(); order=[]; q=list(roots)
while q:
    d=q.pop(0)
    p=resolve(d)
    if p is None or p in seen: continue
    seen.add(p)
    if p in SKIP: continue
    order.append(p)
    q.extend(DEP.get(p,[]))
for p in order:
    print(p+'\t'+FN[p])
PY
)
[ -n "$RESOLVED" ] || { echo "ERROR: closure resolution produced nothing" >&2; exit 1; }

# Download each package (retry per repo a few times: package mirrors occasionally
# 503/truncate). A failure marker survives the while-subshell so the caller sees
# a non-zero exit (a piped `while` runs in a subshell whose `exit` is local).
FAIL="$PKGDIR/.fetchfail"; rm -f "$FAIL"
printf '%s\n' "$RESOLVED" | while IFS="	" read -r name fn; do
	[ -n "$fn" ] || continue
	out="$PKGDIR/$fn"
	if [ ! -s "$out" ]; then
		got=0
		for try in 1 2 3; do
			for repo in core extra; do
				if wget -q "$MIRROR/$repo/os/x86_64/$fn" -O "$out.tmp" && [ -s "$out.tmp" ]; then
					mv "$out.tmp" "$out"; got=1; break
				fi
				rm -f "$out.tmp"
			done
			[ "$got" = 1 ] && break
		done
		[ "$got" = 1 ] || { echo "ERROR: download failed: $fn" >&2; echo "$fn" >> "$FAIL"; continue; }
	fi
	echo "$name $fn"
done
[ -f "$FAIL" ] && { rm -f "$FAIL"; exit 1; } || true
