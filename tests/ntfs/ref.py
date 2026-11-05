#!/usr/bin/env python3
"""The reference listing of a tree: sha256, size and path of every regular file, walking
by directory descriptor so a path past PATH_MAX is no obstacle. Usage: ref.py DIR"""
import hashlib, os, stat, sys


def walk(dfd, prefix):
    for e in sorted(os.listdir(dfd), key=os.fsencode):
        st = os.stat(e, dir_fd=dfd, follow_symlinks=False)
        p = prefix + "/" + e
        if stat.S_ISDIR(st.st_mode):
            f = os.open(e, os.O_RDONLY | os.O_DIRECTORY, dir_fd=dfd)
            walk(f, p)
            os.close(f)
        elif stat.S_ISREG(st.st_mode):
            try:
                f = os.open(e, os.O_RDONLY, dir_fd=dfd)
            except OSError as x:
                print("OPENERR %d %s" % (x.errno, p))
                continue
            h = hashlib.sha256()
            try:
                while True:
                    b = os.read(f, 1 << 20)
                    if not b:
                        break
                    h.update(b)
                print("%s  %d  %s" % (h.hexdigest(), st.st_size, p))
            except OSError as x:
                print("READERR %d %s" % (x.errno, p))
            os.close(f)
        else:
            print("OTHER %o %s" % (st.st_mode, p))


sys.stdout.reconfigure(errors="surrogateescape")
walk(os.open(sys.argv[1], os.O_RDONLY | os.O_DIRECTORY), "")
