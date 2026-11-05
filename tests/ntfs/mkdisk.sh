#!/bin/bash
# Lay a Windows-shaped GPT disk around an NTFS volume image. usage: mkdisk.sh VOL.img DISK.img
# EFI, MSR, Basic data (the volume), Recovery, as a factory Windows install has them.
export PATH=/usr/sbin:/sbin:$PATH
# SS=4096 lays it out for a 4 KB logical block disk.
V=$1; D=$2; SS=${SS:-512}; K=$((512 / (SS / 512) / (512 / (SS / 512)) * (SS / 512)))
VS=$(( $(stat -c %s "$V") / SS ))
rm -f "$D"; truncate -s $(( (VS + (2048 + 204800 + 32768 + 2048 + 4096) / K) * SS )) "$D"
L=$(sudo -n losetup -b $SS -f --show "$D")
sudo -n sfdisk -q $L <<S
label: gpt
start=$((2048 / K)), size=$((204800 / K)), type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="EFI system partition"
size=$((32768 / K)), type=E3C9E316-0B5C-4DB8-817D-F92DF00215AE, name="Microsoft reserved partition"
size=$VS, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name="Basic data partition"
size=$((2048 / K)), type=DE94BBA4-06D1-4D40-A16A-BFD50179D6AC, name="Windows RE"
S
O=$(sudo -n sfdisk -d $L | awk -F'[=,]' '/Basic data/{print $2+0}')
sudo -n losetup -d $L
dd if="$V" of="$D" bs=1M oflag=seek_bytes seek=$((O*SS)) conv=notrunc,sparse status=none
echo "volume at sector $O"
