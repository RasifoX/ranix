# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
echo NT-START
cat /proc/partitions
fjord disk list
S=${SRC:-/dev/nvme0n1p2}
mkdir -p /mnt/w /mnt/d
fjord disk mount $S /mnt/w; echo mount=$?
mount -t ext2 /dev/sata0 /mnt/d; echo dest=$?
fjord disk copy /mnt/w /mnt/d/kopya 2>&1 | tail -1; echo COPY-RC
fjord disk copy /mnt/w /mnt/d/kopya --resume 2>&1 | tail -1; echo RESUME-RC $?
fjord disk verify /mnt/w /mnt/d/kopya > /tmp/v 2>/tmp/e; echo VERIFY-RC $?; tail -1 /tmp/e; grep -c . /tmp/v; grep DIFFERENT /tmp/v | head
grep -vE "^(LINK|LONGNAME|STREAMS|UNREADABLE|LONGPATH) " /mnt/d/kopya/.fjord-disk-report.txt | cut -c1-110 | head -30
umount /mnt/d; echo umount=$?
fjord disk unmount /mnt/w
echo "srcdenied=$(cat /sys/block/nvme0n1p2/ro_denied 2>/dev/null) disk=$(cat /sys/block/nvme0n1/ro_denied 2>/dev/null)"
dmesg | grep -iE "panic|oops" | head -3
echo NT-END
poweroff -f
