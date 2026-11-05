# the disk part of tests/ranix_test.sh on its own, with every disk allowed
echo NT-START
c() { [ "$2" = "$3" ] && echo "PASS: $1" || echo "FAIL: $1 want [$2] got [$3]"; }
dmesg | grep -E "ext2|block:" | head -5
c gpt-ext4 ranix-gpt-ok "$(cat /mnt/hello.txt 2>/dev/null)"
c ext4-write ranix-w64 "$(echo -n ranix-w64 > /mnt/wtest.txt 2>/dev/null; cat /mnt/wtest.txt 2>/dev/null)"
c blkread "EFI PART" "$(dd if=/dev/sata0 bs=1 skip=512 count=8 2>/dev/null)"
c usb-read RANIX-USB-DISK-OK "$(dd if=/dev/usb0 bs=512 skip=2 count=1 2>/dev/null | dd bs=1 count=17 2>/dev/null)"
printf RANIXUSBWRITE | dd of=/dev/usb0 bs=512 seek=4000 count=1 2>/dev/null
c usb-write RANIXUSBWRITE "$(dd if=/dev/usb0 bs=512 skip=4000 count=1 2>/dev/null | dd bs=1 count=13 2>/dev/null)"
c nvme-read RANIX-NVME-DISK-OK "$(dd if=/dev/nvme0n1 bs=512 skip=12000 count=1 2>/dev/null | dd bs=1 count=18 2>/dev/null)"
printf RANIXNVMEWRITE | dd of=/dev/nvme0n1 bs=512 seek=13000 count=1 2>/dev/null
c nvme-write RANIXNVMEWRITE "$(dd if=/dev/nvme0n1 bs=512 skip=13000 count=1 2>/dev/null | dd bs=1 count=14 2>/dev/null)"
mkdir -p /mnt2
mount -t ext2 /dev/nvme0n1 /mnt2 2>/dev/null
c ext2-multi ranix-nvme-ext2-ok "$(cat /mnt2/marker.txt 2>/dev/null)"
timeout 40 /bin/test_ext2rw /mnt 2>&1 | tail -3
c ext2rw-1k OK "$(timeout 40 /bin/test_ext2rw /mnt 2>&1 | tail -1)"
c ext2rw-4k OK "$(timeout 40 /bin/test_ext2rw /mnt2 2>&1 | tail -1)"
timeout 60 /bin/test_ext2hard /mnt/hard1 nofill 2>&1 | tail -2; mkdir -p /mnt/hard1; timeout 60 /bin/test_ext2hard /mnt/hard1 nofill 2>&1 | tail -2; mkdir -p /mnt2/hard; timeout 60 /bin/test_ext2hard /mnt2/hard nofill 2>&1 | head -3; df /mnt2 2>&1 | tail -1; rm -rf /mnt2/hard
c ext2hard OK "$(mkdir -p /mnt2/hard; timeout 60 /bin/test_ext2hard /mnt2/hard nofill 2>&1 | tail -1; rm -rf /mnt2/hard)"
umount /mnt2
c mountoffset OK "$(timeout 10 /bin/test_mountoffset /dev/nvme0n1 /mnt3 8388608 ranix-offset-ok 2>&1 | tail -1)"
c gpt-part OK "$(timeout 10 /bin/test_gpt_part 2>/dev/null | tail -1)"
c devperm OK "$(timeout 8 /bin/test_devperm 2>/dev/null)"
c ro-state 0 "$(cat /sys/block/sata0/ro)"
echo NT-END
poweroff -f
