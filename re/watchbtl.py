"""Logs changes in the card battle manager's per-side state while a fight runs.

    python watchbtl.py [seconds]
"""
import struct
import sys
import time
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import memscan as m

BATTLE_PTR = 0x87A818   # steam

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60
pid = m.find_pid()
h = m.k32.OpenProcess(0x410, False, pid)
base, _ = m.module_range(pid)

last = None
t0 = time.time()
while time.time() - t0 < secs:
    raw = m.read(h, base + BATTLE_PTR, 8)
    btl = struct.unpack("<Q", raw)[0] if raw else 0
    d = m.read(h, btl, 0x220) if btl else None
    if not d:
        cur = None
    else:
        u = lambda o: struct.unpack_from("<i", d, o)[0]
        cur = (u(0xB8), u(0xBC), u(0xC0), u(0xC4), u(0x1A8), u(0x1AC), u(0x1B0), u(0x1B4),
               u(0x1B8), u(0x1BC), u(0x20C), struct.unpack_from("<I", d, 0x208)[0],
               d[0xD0:0xD4].hex(), d[0xEC:0xF0].hex())
    if cur != last:
        if cur is None:
            print("%7.2f  no battle" % (time.time() - t0))
        else:
            print("%7.2f  state %2d/%2d  result %2d/%2d  cards %d/%d  total %2d/%2d  x %d/%d  mode %2d  flags %08X  card %s / %s"
                  % ((time.time() - t0,) + cur))
        sys.stdout.flush()
        last = cur
    time.sleep(0.002)
