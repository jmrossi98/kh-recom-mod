"""Snapshot / diff helper for the exe's writable data and the player battle block.

    python snap.py take NAME      save a snapshot
    python snap.py diff A B       list bytes that differ between two snapshots
"""
import pickle
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import memscan as m

HERE = os.path.dirname(os.path.abspath(__file__))
PLAYER_PTR = 0x87B390   # steam
PLAYER_SPAN = 0x4000


def take(name):
    pid = m.find_pid()
    h = m.k32.OpenProcess(0x410, False, pid)
    base, size = m.module_range(pid)
    snap = {"exe": {}, "player": None}
    for s, e in m.regions(h, base, base + size):
        snap["exe"][s - base] = m.read(h, s, e - s)
    bp = struct.unpack("<Q", m.read(h, base + PLAYER_PTR, 8))[0]
    snap["player_addr"] = bp
    # read page by page, the block can run into unmapped memory
    data = b""
    for off in range(0, PLAYER_SPAN, 0x1000):
        end = ((bp + off) | 0xFFF) + 1
        chunk = m.read(h, bp + len(data), end - (bp + len(data))) if bp else None
        if not chunk:
            break
        data += chunk
    snap["player"] = data
    pickle.dump(snap, open(os.path.join(HERE, "snap_%s.pkl" % name), "wb"))
    print("saved %s: %d exe bytes, player block at 0x%X" % (name, sum(len(v) for v in snap["exe"].values()), bp))


def diff(a, b):
    A = pickle.load(open(os.path.join(HERE, "snap_%s.pkl" % a), "rb"))
    B = pickle.load(open(os.path.join(HERE, "snap_%s.pkl" % b), "rb"))
    if A["player_addr"] != B["player_addr"]:
        print("player block moved, skipping it")
    else:
        pa, pb = A["player"], B["player"]
        for i in range(min(len(pa), len(pb))):
            if pa[i] != pb[i]:
                print("player+0x%04X  %3d -> %3d" % (i, pa[i], pb[i]))
    for off, da in A["exe"].items():
        db = B["exe"].get(off)
        if not db:
            continue
        for i in range(min(len(da), len(db))):
            if da[i] != db[i]:
                print("exe+0x%06X  %3d -> %3d" % (off + i, da[i], db[i]))


if __name__ == "__main__":
    if sys.argv[1] == "take":
        take(sys.argv[2])
    else:
        diff(sys.argv[2], sys.argv[3])
