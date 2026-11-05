#!/usr/bin/env python3
"""Force $MFT itself into many extents so record 0 needs an $ATTRIBUTE_LIST: fill the volume
with one-cluster files, free every other one, then make enough new records that the MFT must
grow into the gaps. Usage: gen-mftfrag.py MOUNT"""
import os, sys

root = sys.argv[1]
os.makedirs(root + "/fill")
n = 0
try:
    while True:
        with open("%s/fill/f%06d" % (root, n), "wb") as f:
            f.write(b"F" * 4096)
            f.flush()
        n += 1
except OSError:
    pass
print("filled", n)
for i in range(0, n, 2):
    os.unlink("%s/fill/f%06d" % (root, i))
os.makedirs(root + "/after")
made = 0
try:
    for i in range(n // 3):
        with open("%s/after/n%06d" % (root, i), "wb") as f:
            f.write(b"after %d\n" % i)
        made += 1
except OSError:
    pass
print("made", made)
