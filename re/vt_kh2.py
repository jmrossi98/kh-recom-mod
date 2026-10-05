"""Print the entries of a vtable in a PE file: vt_kh2.py <exe> <va> [count]"""
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
for i in range(int(sys.argv[3]) if len(sys.argv) > 3 else 10):
    print(i, "%x" % struct.unpack_from("<Q", d, off(va) + i * 8)[0])
