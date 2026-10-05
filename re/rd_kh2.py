"""Dump values at a virtual address of a PE file: rd_kh2.py <exe> <va> [count] [f|i|h|b]"""
import struct, sys
d = open(sys.argv[1], "rb").read()
pe = struct.unpack_from("<I", d, 0x3C)[0]
n = struct.unpack_from("<H", d, pe + 6)[0]
opt = struct.unpack_from("<H", d, pe + 20)[0]
base = struct.unpack_from("<Q", d, pe + 48)[0]
secs = [struct.unpack_from("<IIII", d, pe + 24 + opt + i * 40 + 8) for i in range(n)]
def off(va):
    r = va - base
    for vs, a, rs, raw in secs:
        if a <= r < a + max(vs, rs):
            return raw + r - a
va = int(sys.argv[2], 16)
cnt = int(sys.argv[3]) if len(sys.argv) > 3 else 1
kind = sys.argv[4] if len(sys.argv) > 4 else "f"
size = {"f": 4, "i": 4, "h": 2, "b": 1}[kind]
vals = struct.unpack_from("<%d%s" % (cnt, {"f": "f", "i": "i", "h": "h", "b": "B"}[kind]), d, off(va))
print("%x:" % va, " ".join(("%g" % v) if kind == "f" else str(v) for v in vals))
