# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
# Real Casper, read-only checks: nothing here may touch sata0, sata0p0 or sata0p1.
sleep 12
{
echo ==KUTU son
uname -a; cat /proc/uptime
dmesg | grep -E "xhci|block:|gpt:|ahci|nvme" | cut -c1-100
cat /proc/partitions
fjord disk list
for d in sata0 sata0p0 sata0p1 sata0p2; do echo "$d ro=$(cat /sys/block/$d/ro) id=$(cat /sys/block/$d/id | tr '\n' ' ')"; done
sig=$(dd if=/dev/sata0p0 bs=1 skip=3 count=8 2>/dev/null); echo "p0 oem=[$sig]"
echo ==KUTU-SON
} > /tmp/diag 2>&1
# blocks 256 to 511 of the /home disk are set aside as the black box, see arac/casper/ev/kur-home.sh
dd if=/tmp/diag of=/dev/sata0p2 bs=4096 seek=256 conv=fsync 2>/dev/null; echo KUTU yazildi rc=$?
