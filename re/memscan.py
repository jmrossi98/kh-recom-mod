"""Tiny value scanner for the running Re:CoM process.

    python memscan.py new 15            first scan: every address holding 15
    python memscan.py next 14           keep candidates that now hold 14
    python memscan.py changed           keep candidates whose value changed
    python memscan.py same              keep candidates whose value did not change
    python memscan.py list              print candidates (address relative to exe base)
    python memscan.py peek 0x87A5B0 64  hex dump 64 bytes at exe base + offset

Use --size 1|2|4 (default 4) and --float for float scans. Only memory inside
the game module is scanned unless --heap is given, since LuaBackend addresses
are relative to the exe base.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import struct
import sys

EXE = "KINGDOM HEARTS Re_Chain of Memories.exe"
STATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "memscan_state.json")

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
MEM_COMMIT = 0x1000
PAGE_GUARD = 0x100
PAGE_NOACCESS = 0x01
WRITABLE = 0x04 | 0x08 | 0x40 | 0x80
TH32CS_SNAPPROCESS = 0x2
TH32CS_SNAPMODULE = 0x8 | 0x10

k32 = ctypes.WinDLL("kernel32", use_last_error=True)


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wt.DWORD),
                ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
                ("pcPriClassBase", wt.LONG), ("dwFlags", wt.DWORD), ("szExeFile", ctypes.c_wchar * 260)]


class MODULEENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("GlblcntUsage", wt.DWORD), ("ProccntUsage", wt.DWORD),
                ("modBaseAddr", ctypes.c_void_p), ("modBaseSize", wt.DWORD), ("hModule", wt.HMODULE),
                ("szModule", ctypes.c_wchar * 256), ("szExePath", ctypes.c_wchar * 260)]


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wt.DWORD), ("PartitionId", wt.WORD),
                ("RegionSize", ctypes.c_size_t), ("State", wt.DWORD), ("Protect", wt.DWORD),
                ("Type", wt.DWORD)]


k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.Process32FirstW.argtypes = k32.Process32NextW.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32)]
k32.Module32FirstW.argtypes = [wt.HANDLE, ctypes.POINTER(MODULEENTRY32)]
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t


def find_pid():
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32()
    pe.dwSize = ctypes.sizeof(pe)
    ok = k32.Process32FirstW(snap, ctypes.byref(pe))
    while ok:
        if pe.szExeFile.lower() == EXE.lower():
            k32.CloseHandle(snap)
            return pe.th32ProcessID
        ok = k32.Process32NextW(snap, ctypes.byref(pe))
    k32.CloseHandle(snap)
    sys.exit("game is not running")


def module_range(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid)
    me = MODULEENTRY32()
    me.dwSize = ctypes.sizeof(me)
    if not k32.Module32FirstW(snap, ctypes.byref(me)):
        sys.exit("could not read module list (error %d)" % ctypes.get_last_error())
    k32.CloseHandle(snap)
    return me.modBaseAddr, me.modBaseSize


def read(h, addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, addr, buf, size, ctypes.byref(got)):
        return None
    return buf.raw[:got.value]


def regions(h, lo, hi):
    addr = lo
    mbi = MBI()
    while addr < hi and k32.VirtualQueryEx(h, addr, ctypes.byref(mbi), ctypes.sizeof(mbi)):
        base = mbi.BaseAddress or 0
        end = base + mbi.RegionSize
        if (mbi.State == MEM_COMMIT and mbi.Protect & WRITABLE
                and not mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)):
            yield max(base, lo), min(end, hi)
        addr = end


def fmt_for(size, is_float):
    if is_float:
        return "<f"
    return {1: "<B", 2: "<H", 4: "<I"}[size]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["new", "next", "changed", "same", "list", "peek"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--size", type=int, default=4, choices=[1, 2, 4])
    ap.add_argument("--float", action="store_true")
    ap.add_argument("--heap", action="store_true", help="scan the whole address space, not just the exe")
    a = ap.parse_args()

    pid = find_pid()
    h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    if not h:
        sys.exit("OpenProcess failed (error %d)" % ctypes.get_last_error())
    base, modsize = module_range(pid)

    if a.cmd == "peek":
        off = int(a.args[0], 0)
        n = int(a.args[1], 0) if len(a.args) > 1 else 64
        data = read(h, base + off, n) or b""
        for i in range(0, len(data), 16):
            row = data[i:i + 16]
            print("%08X  %s" % (off + i, " ".join("%02X" % b for b in row)))
        return

    if a.cmd == "new":
        size = 4 if a.float else a.size
        fmt = fmt_for(size, a.float)
        want = struct.pack(fmt, float(a.args[0]) if a.float else int(a.args[0], 0))
        lo, hi = (0x10000, 0x7FFFFFFFFFFF) if a.heap else (base, base + modsize)
        hits = []
        for start, end in regions(h, lo, hi):
            data = read(h, start, end - start)
            if not data:
                continue
            i = data.find(want)
            while i != -1:
                if i % size == 0:
                    hits.append(start + i - base)
                i = data.find(want, i + 1)
        vals = {str(x): a.args[0] for x in hits}
        json.dump({"size": size, "float": a.float, "vals": vals}, open(STATE, "w"))
        print("%d candidates" % len(hits))
        return

    st = json.load(open(STATE))
    fmt = fmt_for(st["size"], st["float"])
    keep = {}
    for off, old in st["vals"].items():
        raw = read(h, base + int(off), st["size"])
        if not raw or len(raw) < st["size"]:
            continue
        cur = struct.unpack(fmt, raw)[0]
        old = float(old) if st["float"] else int(str(old), 0)
        if a.cmd == "next":
            target = float(a.args[0]) if st["float"] else int(a.args[0], 0)
            ok = cur == target
        elif a.cmd == "changed":
            ok = cur != old
        elif a.cmd == "same":
            ok = cur == old
        else:
            ok = True
        if ok:
            keep[off] = cur
    if a.cmd != "list":
        st["vals"] = {k: str(v) for k, v in keep.items()}
        json.dump(st, open(STATE, "w"))
    print("%d candidates" % len(keep))
    for off, cur in list(keep.items())[:40]:
        print("  exe+0x%X = %s" % (int(off), cur))


if __name__ == "__main__":
    main()
