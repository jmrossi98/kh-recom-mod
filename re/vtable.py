"""Dumps a vtable from the exe on disk.

    python vtable.py 0x529CB8 [count]      rva of the first slot
"""
import struct
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
IMAGE_BASE = 0x140000000

data = open(os.path.join(HERE, "recom.exe"), "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
opt = struct.unpack_from("<H", data, pe + 20)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + opt + 40 * i
    name = data[o:o + 8].rstrip(b"\0").decode()
    vsize, va, rsize, raw = struct.unpack_from("<IIII", data, o + 8)
    secs.append((name, va, vsize, raw, rsize))


def off(rva):
    for name, va, vsize, raw, rsize in secs:
        if va <= rva < va + max(vsize, rsize):
            return raw + rva - va, name
    return None, None


def in_text(rva):
    return off(rva)[1] == ".text"


if __name__ == "__main__":
    rva = int(sys.argv[1], 0)
    count = int(sys.argv[2], 0) if len(sys.argv) > 2 else 64
    for i in range(count):
        o, _ = off(rva + i * 8)
        ptr = struct.unpack_from("<Q", data, o)[0]
        target = ptr - IMAGE_BASE
        if not (0 < target < 0x1000000 and in_text(target)):
            break
        print("slot %2d (+0x%03X)  %X" % (i, i * 8, ptr))
