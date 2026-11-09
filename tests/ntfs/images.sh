#!/bin/bash
# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
# The image set. One at a time: each runs ntfs-3g for minutes.
cd ~/ranix-nt
I="bash tests/ntfs/mkimg.sh"
$I t/w4k.img 1000 small
$I t/c512.img 1000 small -c 512
$I t/s4k.img 1000 small -s 4096 -c 4096
$I t/c64k.img 1400 small -c 65536
$I t/big.img 12000 big
echo ALLDONE
