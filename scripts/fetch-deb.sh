#!/bin/sh
# Fetch a Debian package + its runtime-library closure as .deb files into a
# cache dir, for shipping into the initramfs (offline dpkg/apt on Ranix).
#
#   fetch-deb.sh <cache-dir> <suite> <arch> <pkg> [pkg...]
#
# Resolves Depends/Pre-Depends transitively from the suite's Packages index,
# skips libc6/libc-bin (Ranix ships the host glibc), takes the first alternative
# of any "a | b" dep, and downloads each .deb from deb.debian.org. Deterministic
# and re-run safe: an already-present, non-empty .deb is left untouched. Prints
# nothing on success beyond per-package lines; exits non-zero on download failure.
set -eu

CACHE="$1"; SUITE="$2"; ARCH="$3"; shift 3
MIRROR="http://deb.debian.org/debian"
IDX="$CACHE/Packages-$SUITE-$ARCH.txt"
DEBDIR="$CACHE/deb-$ARCH"
mkdir -p "$DEBDIR"

if [ ! -s "$IDX" ]; then
	wget -q "$MIRROR/dists/$SUITE/main/binary-$ARCH/Packages.xz" -O "$IDX.xz" || {
		rm -f "$IDX.xz"; echo "ERROR: Packages index download failed" >&2; exit 1; }
	xz -dc "$IDX.xz" > "$IDX" || { rm -f "$IDX"; echo "ERROR: Packages decompress failed" >&2; exit 1; }
fi

# pkg<TAB>filename<TAB>dep,dep,...  (version constraints + alternatives stripped)
DB="$CACHE/pkgdb-$SUITE-$ARCH.tsv"
if [ ! -s "$DB" ] || [ "$IDX" -nt "$DB" ]; then
	awk 'BEGIN{RS="";FS="\n"}
	{
		pkg="";fn="";deps="";
		for(i=1;i<=NF;i++){
			if($i ~ /^Package: /)pkg=substr($i,10);
			else if($i ~ /^Filename: /)fn=substr($i,11);
			else if($i ~ /^Depends: /)deps=deps" "substr($i,10);
			else if($i ~ /^Pre-Depends: /)deps=deps" "substr($i,14);
		}
		if(pkg!=""){FN[pkg]=fn;DEP[pkg]=deps}
	}
	END{
		for(p in FN){
			d=DEP[p];
			gsub(/\([^)]*\)/,"",d);   # drop version constraints
			gsub(/ \| [^,]*/,"",d);   # keep first alternative
			n=split(d,a,",");out="";
			for(i=1;i<=n;i++){x=a[i];gsub(/^[ \t]+|[ \t]+$/,"",x);if(x!="")out=out (out==""?"":",") x}
			print p "\t" FN[p] "\t" out
		}
	}' "$IDX" > "$DB"
fi

# Breadth-first closure. SKIP lists deps Ranix already provides or that are
# pure config-time pseudo-packages irrelevant to running the tool.
SKIP=" libc6 libc-bin "
seen=""
queue="$*"
while [ -n "$queue" ]; do
	p=${queue%% *}; [ "$p" = "$queue" ] && queue="" || queue=${queue#* }
	case " $seen " in *" $p "*) continue;; esac
	case "$SKIP" in *" $p "*) continue;; esac
	seen="$seen $p"
	line=$(grep -m1 "^$p	" "$DB" || true)
	[ -z "$line" ] && { echo "WARN: package not found: $p" >&2; continue; }
	fn=$(printf '%s' "$line" | cut -f2)
	deps=$(printf '%s' "$line" | cut -f3 | tr ',' ' ')
	out="$DEBDIR/$(basename "$fn")"
	if [ ! -s "$out" ]; then
		wget -q "$MIRROR/$fn" -O "$out" || { rm -f "$out"; echo "ERROR: download failed: $p" >&2; exit 1; }
	fi
	echo "$p $(basename "$out")"
	queue="$queue $deps"
done
