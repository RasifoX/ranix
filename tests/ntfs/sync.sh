#!/bin/sh
# Push the Pi tree to ~/ranix-nt on Casper. Sources only: build outputs stay put.
A=/home/rasi/marshall-vekil/ranix-inceleme/arac
rsync -a --chmod=Dgo+rx,Fgo+r --exclude build --exclude .cache --exclude .git --exclude 't/' --include 'userspace/' --include 'userspace/*.c' --include 'userspace/*.h' --exclude 'userspace/*' -e "ssh -i $A/casper/anahtar -o UserKnownHostsFile=$A/casper/known_hosts" $A/../src/ rasi@192.168.1.109:ranix-nt/
