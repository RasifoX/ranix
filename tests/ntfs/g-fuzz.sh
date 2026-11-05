echo NT-START
mkdir -p /mnt/f
test_ntfsfuzz /dev/nvme0n1 /mnt/f /hot.txt ${FZSEED:-1} ${FZN:-1000}
echo fuzz-rc=$?
dmesg | grep -ciE "panic|oops|general protection"
echo NT-END
poweroff -f
