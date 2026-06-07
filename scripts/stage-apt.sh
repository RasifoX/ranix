#!/bin/sh
# Stage apt + its dependency closure into the Debian rootfs and build a tiny
# local file:// repo so `apt-get update && apt-get install -y tree` runs
# on-device, offline and deterministic.
#
#   stage-apt.sh <cache-dir> <suite> <deb-dir> <rootfs> <target> <pkg> [pkg...]
#
# <target> is the install package (`tree`): it is placed only in the local repo,
# never pre-extracted, so apt's solver+cache+dpkg invocation actually install
# it. The remaining packages (apt, libapt-pkg, gpgv, the http/https/file
# methods, the archive keyring, ...) are extracted into the rootfs at their real
# multiarch paths. Also seeds /etc/apt (sources.list -> file:///srv/repo, an
# apt.conf making the install root the live root) and the apt state/cache dirs.
set -eu

CACHE="$1"; SUITE="$2"; DEBDIR="$3"; ROOTFS="$4"; TARGET="$5"; shift 5
PKGS="$TARGET $*"

sh "$(dirname "$0")/fetch-deb.sh" "$CACHE" "$SUITE" amd64 $PKGS >/dev/null

# Extract apt + closure into the rootfs (skip the target package — apt installs
# it — and the hello/libc-provided artifacts handled by the dpkg staging).
for d in "$DEBDIR"/*.deb; do
	b=$(basename "$d")
	case "$b" in
		hello_*) continue;;
		"$TARGET"_*) continue;;
	esac
	dpkg-deb -x "$d" "$ROOTFS" 2>/dev/null || true
done

# Local file repo: flat layout (pool/ + Packages + Release) holding the target
# package. `dpkg-scanpackages` builds the index; `[trusted=yes]` in sources.list
# lets apt accept it without a signed Release.
REPO="$ROOTFS/srv/repo"
rm -rf "$REPO"; mkdir -p "$REPO/pool"
cp "$DEBDIR/$TARGET"_*.deb "$REPO/pool/"
( cd "$REPO" && dpkg-scanpackages pool /dev/null > Packages 2>/dev/null \
    && gzip -kf Packages \
    && apt-ftparchive release . > Release 2>/dev/null )

# apt config + state. The install root is the live root (/), so apt unpacks tree
# straight into /usr/bin like dpkg-install does. The status db is the same one
# the dpkg staging seeded with libc6 installed.
mkdir -p "$ROOTFS/etc/apt/apt.conf.d" "$ROOTFS/etc/apt/preferences.d" \
         "$ROOTFS/etc/apt/sources.list.d" \
         "$ROOTFS/var/lib/apt/lists/partial" \
         "$ROOTFS/var/cache/apt/archives/partial" \
         "$ROOTFS/var/log/apt"
printf 'deb [trusted=yes] file:///srv/repo ./\n' > "$ROOTFS/etc/apt/sources.list"
cat > "$ROOTFS/etc/apt/apt.conf.d/99ranix" <<'EOF'
APT::Install-Recommends "false";
APT::Install-Suggests "false";
Acquire::Languages "none";
Acquire::Check-Valid-Until "false";
Dpkg::Use-Pty "false";
EOF
