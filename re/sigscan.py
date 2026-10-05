"""Resolve a table of named byte signatures against a PE file.

usage: sigscan.py <exe> <signatures.txt> [name filter]

Each line of the table is
    Class | decl | resolver | arg=<hex> | post=... | AA BB ?? CC ...
Resolvers:
    FindSignature          the match itself
    FetchFunctionFromCall  match+arg is a call/jmp, take its target
    FetchRelativePointer   match+arg is a rip-relative int32
    FetchAbsolutePointer   match+arg is an int32 RVA
Prints the virtual address of everything it can resolve.
"""
import re
import struct
import sys


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
    out = []
    off = pe + 24 + opt
    for i in range(nsec):
        name = data[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, off + 8)
        out.append((name, va, vsize, raw, rsize))
        off += 40
    return base, out


def main():
    data = open(sys.argv[1], "rb").read()
    base, secs = sections(data)
    text = next(s for s in secs if s[0] == ".text")
    _, tva, _, traw, trsize = text
    code = data[traw:traw + trsize]
    want = sys.argv[3].lower() if len(sys.argv) > 3 else ""

    def rva(off):
        return off + tva

    def read_i32(r):
        return struct.unpack_from("<i", code, r - tva)[0]

    for line in open(sys.argv[2], encoding="utf-8", errors="replace"):
        if line.startswith("#") or "|" not in line:
            continue
        parts = [p.strip() for p in line.rstrip("\n").split("|")]
        if len(parts) < 6:
            continue
        cls, decl, resolver, arg, post, sig = parts[0], parts[1], parts[2], parts[3], parts[4], parts[-1]
        name = cls + " :: " + decl[:90]
        if want and want not in name.lower():
            continue
        toks = sig.split()
        if len(toks) < 6 or any(not re.fullmatch(r"[0-9A-Fa-f]{2}|\?\?", t) for t in toks):
            continue
        pat = b"".join(b"." if t == "??" else re.escape(bytes([int(t, 16)])) for t in toks)
        hits = [m.start() for m in re.finditer(pat, code, re.S)]
        if len(hits) != 1:
            print("%-18s %s  (%d matches)" % ("-", name, len(hits)))
            continue
        at = rva(hits[0])
        a = int(arg.split("=")[1], 16) if "=" in arg and arg.split("=")[1] else 0
        if resolver == "FindSignature":
            target = at
        elif resolver == "FetchFunctionFromCall":
            target = at + a + 5 + read_i32(at + a + 1)
        elif resolver == "FetchRelativePointer":
            target = at + a + 4 + read_i32(at + a)
        elif resolver == "FetchAbsolutePointer":
            target = read_i32(at + a)
        else:
            continue
        print("%016x   %s   [match %x %s %s]" % (base + target, name, base + at, resolver, post))


main()
