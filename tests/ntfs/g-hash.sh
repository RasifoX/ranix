echo NT-START
cat /proc/partitions
fjord disk list
mkdir -p /mnt/w
D=$(ls /dev/ | grep -E '^(nvme0n1|usb0|sata0)p2$' | head -1)
fjord disk mount /dev/$D /mnt/w; echo mount=$?
echo NT-HASH-START
fjord disk hash /mnt/w
echo NT-HASH-END rc=$?
fjord disk unmount /mnt/w
echo "denied=$(cat /sys/block/$D/ro_denied)"
dmesg | grep -ciE "panic|oops"
echo NT-END
poweroff -f
