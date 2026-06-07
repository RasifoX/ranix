#!/usr/bin/env python3
# Decompress a single zstd stream: zstdcat.py <in.zst> <out>. The host has
# libzstd.so.1 (a glibc dependency) but no `zstd` CLI, and GNU tar's --zstd
# shells out to that missing binary; so drive libzstd's streaming API via ctypes
# to unpack Arch's .pkg.tar.zst / repo .db archives at build time.
import ctypes, sys

z = ctypes.CDLL("libzstd.so.1")


class Buf(ctypes.Structure):
    _fields_ = [("p", ctypes.c_void_p), ("size", ctypes.c_size_t), ("pos", ctypes.c_size_t)]


z.ZSTD_createDStream.restype = ctypes.c_void_p
z.ZSTD_isError.restype = ctypes.c_uint
z.ZSTD_isError.argtypes = [ctypes.c_size_t]
z.ZSTD_decompressStream.argtypes = [ctypes.c_void_p, ctypes.POINTER(Buf), ctypes.POINTER(Buf)]
z.ZSTD_decompressStream.restype = ctypes.c_size_t


def main(src_path, dst_path):
    data = open(src_path, "rb").read()
    src = ctypes.create_string_buffer(data, len(data))
    ds = z.ZSTD_createDStream()
    z.ZSTD_initDStream(ctypes.c_void_p(ds))
    cap = 1 << 20
    obuf = ctypes.create_string_buffer(cap)
    inb = Buf(ctypes.cast(src, ctypes.c_void_p), len(data), 0)
    with open(dst_path, "wb") as out:
        while inb.pos < inb.size:
            ob = Buf(ctypes.cast(obuf, ctypes.c_void_p), cap, 0)
            r = z.ZSTD_decompressStream(ctypes.c_void_p(ds), ctypes.byref(ob), ctypes.byref(inb))
            if z.ZSTD_isError(r):
                sys.stderr.write("zstd: decode error\n")
                sys.exit(1)
            out.write(obuf.raw[: ob.pos])
            if r == 0:
                break


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
