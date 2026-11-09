# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
echo NT-START
cat /proc/partitions
fjord disk list
D=/dev/nvme0n1
for t in $D ${D}p0 ${D}p2; do test_diskguard $t | grep -E "FAIL|diskguard"; done
for t in nvme0n1 nvme0n1p2; do echo "$t ro=$(cat /sys/block/$t/ro) denied=$(cat /sys/block/$t/ro_denied) id=$(cat /sys/block/$t/id | tr '\n' ' ')"; done
dmesg | grep -E "blockguard|block:|gpt:|ntfs" | head -20
mkdir -p /mnt/w
fjord disk mount ${D}p2 /mnt/w; echo mount=$?
grep ntfs /proc/mounts
test_ntfsro /mnt/w | grep -vE "^PASS"
echo NT-HASH-START
time fjord disk hash /mnt/w
echo NT-HASH-END rc=$?
fjord disk unmount /mnt/w; echo unmount=$?
fjord disk mount ${D}p2 /mnt/w; echo mount2=$?
ls /mnt/w | head -3
cd /; fjord disk unmount /mnt/w
dmesg | grep -iE "panic|fault|ntfs:" | tail -5
echo denied=$(cat /sys/block/nvme0n1p2/ro_denied)
echo NT-END
sync; poweroff -f
