#!/bin/bash
# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
# /test.sh of tests/swapboot.sh: three runs in a 256 MB machine.
ok=0
grep -E "MemTotal|MemFree" /proc/meminfo | sed 's/^/SB-mem: /'

echo 1G > /sys/block/zram0/disksize && mkswap /dev/zram0 >/dev/null && swapon -p 100 /dev/zram0
r=$(timeout 300 /bin/test_swap hogz 2>&1); echo "$r" | sed 's/^/SB-zram: /'
[ "$(echo "$r" | tail -1)" = OK ] && ok=$((ok+1))
cat /sys/block/zram0/mm_stat | sed 's/^/SB-mm_stat: /'
swapoff /dev/zram0; echo 0 > /sys/block/zram0/disksize

mkswap /dev/nvme0n1 >/dev/null && swapon /dev/nvme0n1
cat /proc/swaps | sed 's/^/SB-swaps: /'
r=$(timeout 400 /bin/test_swap hog 2>&1); echo "$r" | sed 's/^/SB-disk: /'
[ "$(echo "$r" | tail -1)" = OK ] && ok=$((ok+1))
swapoff /dev/nvme0n1

echo 32M > /sys/block/zram0/disksize && mkswap /dev/zram0 >/dev/null && swapon /dev/zram0
r=$(timeout 300 /bin/test_swap oom 2>&1); echo "$r" | sed 's/^/SB-oom: /'
[ "$(echo "$r" | tail -1)" = OK ] && ok=$((ok+1))
free | sed 's/^/SB-free: /'
swapoff /dev/zram0
echo "SB-RESULT: $ok/3"
sync
poweroff -f
