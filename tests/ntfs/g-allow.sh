echo NT-START
c() { [ "$2" = "$3" ] && echo "PASS: $1" || echo "FAIL: $1 want [$2] got [$3]"; }
r() { cat /sys/block/$1/ro; }
fjord disk list
P=nvme0n1p2; D=nvme0n1
c listed-partition-writable 0 "$(r $P)"
c disk-stays-protected 1 "$(r $D)"
c other-partition-protected 1 "$(r ${D}p1)"
printf ALLOWED | dd of=/dev/$P bs=512 seek=2000 count=1 2>/dev/null
c write-to-listed ALLOWED "$(dd if=/dev/$P bs=512 skip=2000 count=1 2>/dev/null | dd bs=1 count=7 2>/dev/null)"
printf DENIED | dd of=/dev/${D}p1 bs=512 seek=10 count=1 2>/dev/null
c other-partition-unchanged 0 "$(dd if=/dev/${D}p1 bs=512 skip=10 count=1 2>/dev/null | dd bs=1 count=6 2>/dev/null | tr -d '\0' | wc -c)"
fjord disk protect /dev/$P
c protect 1 "$(r $P)"
fjord disk allow /dev/$P serial:wrong 2>/dev/null; c wrong-identity 4 "$?"
ID=$(cat /sys/block/$P/id | grep partuuid)
fjord disk allow /dev/$P $ID; c allow 0 "$?"
c allowed-again 0 "$(r $P)"
DI=$(cat /sys/block/$D/id | grep serial)
fjord disk allow /dev/$D $DI; c allow-disk 0 "$?"
c cascades 0 "$(r ${D}p1)"
fjord disk protect /dev/$D
c protect-cascades 1 "$(r ${D}p1)"
echo NT-END
poweroff -f
