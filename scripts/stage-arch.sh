#!/bin/sh
# Stage Arch Linux's pacman + its runtime closure under a self-contained /opt/arch
# prefix in the rootfs, so `pacman -U`/`-S` runs on-device, offline (x86_64 only).
#
#   stage-arch.sh <cache-dir> <prefix-rootfs> <target-pkg>
#
# Arch's current toolchain links against glibc >= 2.42 (e.g. libgpg-error imports
# __inet_pton_chk@GLIBC_2.42), newer than the host glibc Ranix ships for the
# apt/dpkg path. So the whole Arch userland is run against ARCH'S OWN glibc 2.43:
# every package (including glibc) is extracted under /opt/arch, and pacman is
# launched through Arch's ld-linux with --library-path /opt/arch/usr/lib. This is
# exactly how a real Arch install resolves these libraries and keeps the Arch
# closure isolated from the host-glibc Debian binaries in the same initramfs.
#
# Builds a local file:// repo (myrepo.db) holding the target package so the test
# can also exercise `pacman -Sy && pacman -S`, and seeds /opt/arch/etc/pacman.conf
# (SigLevel=Never for the offline local path) + an empty local db.
set -eu

CACHE="$1"; ROOT="$2"; TARGET="$3"
PKGDIR="$CACHE/arch-x86_64"
PREFIX="$ROOT/opt/arch"

# Fetch pacman's runtime closure (+ libseccomp, an undeclared libalpm ELF NEED)
# and the target package. glibc is fetched explicitly below: fetch-arch.sh skips
# it from the closure (it's the host-glibc-satisfied libc soname there), but the
# Arch userland is run against Arch's OWN glibc, so we need that package here.
sh "$(dirname "$0")/fetch-arch.sh" "$CACHE" pacman libseccomp "$TARGET" >/dev/null
sh "$(dirname "$0")/fetch-arch.sh" "$CACHE" --pkg glibc >/dev/null

zstdcat() {  # $1 in .zst  $2 out .tar  — host has libzstd.so.1 but no zstd CLI
	python3 "$(dirname "$0")/zstdcat.py" "$1" "$2"
}

rm -rf "$PREFIX"; mkdir -p "$PREFIX"
# Extract every fetched package into the prefix. Keep only what pacman needs to
# RUN — usr/bin, usr/lib (the ELF graph + glibc), and a few data dirs. Drop the
# bulk that would bloat the initramfs and starve the boot-time extractor: man
# pages, locales, docs, includes, systemd units, zsh/bash completions, ~25 MB of
# dead weight that makes a 276 MB initrd. Package metadata is dropped too.
for z in "$PKGDIR"/*.pkg.tar.zst; do
	zstdcat "$z" "$z.tar"
	tar xf "$z.tar" -C "$PREFIX" \
	    --exclude='.PKGINFO' --exclude='.BUILDINFO' --exclude='.MTREE' --exclude='.INSTALL' \
	    --exclude='usr/share/man' --exclude='usr/share/doc' --exclude='usr/share/info' \
	    --exclude='usr/share/locale' --exclude='usr/share/licenses' --exclude='usr/share/gtk-doc' \
	    --exclude='usr/share/bash-completion' --exclude='usr/share/zsh' \
	    --exclude='usr/share/i18n' --exclude='usr/include' --exclude='usr/lib/systemd' \
	    --exclude='usr/lib/sysusers.d' --exclude='usr/lib/tmpfiles.d' \
	    --exclude='usr/lib/cmake' --exclude='usr/lib/pkgconfig' --exclude='usr/share/man' \
	    --exclude='*.a' --exclude='usr/lib/audit' --exclude='usr/lib/gawk' \
	    --exclude='etc/systemd' 2>/dev/null || true
	rm -f "$z.tar"
done

# Drop the dependency packages' command-line tools (sqlite3, openssl, gpg, ...) —
# pacman is launched via /opt/arch/usr/bin/pacman directly, so only the pacman
# binaries are needed in usr/bin. This sheds ~25 MB the boot extractor would copy.
find "$PREFIX/usr/bin" -maxdepth 1 -type f ! -name 'pacman*' -delete 2>/dev/null || true

# pacman wrapper: exec the real binary through Arch's ld.so so every Arch lib
# (glibc included) resolves under /opt/arch/usr/lib, independent of the host
# glibc the Debian binaries use.
mkdir -p "$ROOT/usr/bin"
cat > "$ROOT/usr/bin/pacman" <<'EOF'
#!/bin/sh
P=/opt/arch
exec "$P/usr/lib/ld-linux-x86-64.so.2" --library-path "$P/usr/lib" \
     "$P/usr/bin/pacman" "$@"
EOF
chmod +x "$ROOT/usr/bin/pacman"

# pacman.conf: offline-friendly. SigLevel Never (no Arch keyring on the local
# path); a single local file repo `myrepo` for the -S milestone.
mkdir -p "$PREFIX/etc" "$PREFIX/etc/pacman.d"
cat > "$PREFIX/etc/pacman.conf" <<'EOF'
[options]
Architecture = x86_64
HoldPkg =
SigLevel = Never
LocalFileSigLevel = Never
[myrepo]
Server = file:///srv/pacmanrepo
EOF

# Empty local db so `pacman -U` has a place to register installs.
mkdir -p "$PREFIX/var/lib/pacman/local" "$PREFIX/var/cache/pacman/pkg"

# Local file repo for `pacman -S`: a repo db (myrepo.db) over the target package.
# `repo-add` isn't on the host, so synthesize the db tar (one <pkg>/desc +
# <pkg>/files entry) the same way repo-add would, from the package's .PKGINFO.
REPO="$ROOT/srv/pacmanrepo"
rm -rf "$REPO"; mkdir -p "$REPO"
tgt=$(ls "$PKGDIR/$TARGET"-*.pkg.tar.zst 2>/dev/null | head -1)
if [ -n "$tgt" ]; then
	cp "$tgt" "$REPO/"
	python3 "$(dirname "$0")/arch-repo-add.py" "$REPO" "$REPO/$(basename "$tgt")"
fi

# Stash the target package separately so the -U milestone installs from a path
# that is NOT the repo (mirrors apt staging: target only in the repo for -S).
mkdir -p "$ROOT/var/cache/pacman"
[ -n "$tgt" ] && cp "$tgt" "$ROOT/var/cache/pacman/$TARGET.pkg.tar.zst"
