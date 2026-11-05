echo NT-START
cat /proc/partitions
dmesg | grep -E "xhci|gpt:|block:" | head -20
fjord disk list
D=$(ls /dev/usb0 2>/dev/null)
mkdir -p /mnt/w
fjord disk mount /dev/usb0p2 /mnt/w; echo mount=$?
echo NT-HASH-START
time fjord disk hash /mnt/w
echo NT-HASH-END rc=$?
fjord disk unmount /mnt/w
echo "denied=$(cat /sys/block/usb0p2/ro_denied)"
time dd if=/dev/usb0 of=/dev/null bs=128k count=2000
cat /sys/block/usb0/stat
echo NT-END
poweroff -f
