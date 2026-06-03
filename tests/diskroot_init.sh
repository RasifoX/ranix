#!/bin/sh
# Runs as the disk root's /test.sh after init pivots into it. That this script
# runs at all (instead of the initramfs's 307-test suite) already proves the
# pivot happened — /test.sh differs between ramfs and disk. /etc/diskroot-id is
# seeded only onto the disk, so it's a positive "we are on the disk" signal.
# tests/diskroot.sh boots twice and checks the FRESH -> SURVIVED transition: a
# write to the absolute path /var can only survive a reboot if / is the disk.
[ -f /etc/diskroot-id ] && echo "DISKROOT:ONDISK:$(cat /etc/diskroot-id)"
if [ -f /var/diskroot_marker ]; then
	echo "DISKROOT:SURVIVED:$(cat /var/diskroot_marker)"
else
	mkdir -p /var
	echo RANIX_DISKROOT_OK > /var/diskroot_marker
	sync
	echo "DISKROOT:FRESH"
fi
