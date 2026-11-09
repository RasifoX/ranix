#!/bin/bash
# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
# Build one NTFS image on Debian. usage: mkimg.sh OUT.img SIZE_MB PROFILE [mkntfs flags]
# PROFILE small|big. Loop files only, under ~/ranix-nt/t, never a real disk.
set -e
export PATH=/usr/sbin:/sbin:$PATH
O=$1; S=$2; P=$3; shift 3
T=$(dirname "$(readlink -f "$0")")
M=$(mktemp -d ~/ranix-nt/t/mnt.XXXX)
rm -f "$O"; truncate -s ${S}M "$O"
mkntfs -F -q -L "Windows Yerel" "$@" "$O"
sudo -n lowntfs-3g -o streams_interface=xattr,big_writes "$O" "$M"
sudo -n python3 "$T/gen-tree.py" "$M" "$P"
sudo -n umount "$M"; sleep 1
# names past 255 UTF-8 bytes: only libntfs itself takes them, a mount refuses
W=$(mktemp -d ~/ranix-nt/t/wrk.XXXX)
echo "turkish 255, longer than a VFS name" > $W/a; echo "second long one" > $W/b; echo "third long" > $W/c
ntfscp -q -f "$O" $W/a "long/$(python3 -c "print('ş'*251+'.txt')")"
ntfscp -q -f "$O" $W/b "long/$(python3 -c "print('ğ'*200+'-b.txt')")"
ntfscp -q -f "$O" $W/c "long/$(python3 -c "print('ğ'*200+'-c.txt')")"
rm -rf $W
# the reference view: ntfs-3g read-only, every file read through it
sudo -n ntfs-3g -o ro,streams_interface=none "$O" "$M"
sudo -n python3 "$T/ref.py" "$M" | LC_ALL=C sort -k3 > "$O.ref"
sudo -n umount "$M"; rmdir "$M"
wc -l "$O.ref"
