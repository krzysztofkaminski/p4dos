#!/usr/bin/env python3
"""Run the SingleStepTests/8088 v2 suite against the CPU core (8086 mode).

usage: cputest.py <dir with *.json.gz and metadata.json> [-n TESTS_PER_FILE] [-v] [OPCODE ...]
"""
import argparse
import ctypes
import gzip
import json
import multiprocessing
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REGS = ["ax", "bx", "cx", "dx", "cs", "ss", "ds", "es", "sp", "bp", "si", "di", "ip", "flags"]
LIB = None
RAM = None


def build(outdir):
    so = os.path.join(outdir, "libcputest.so")
    subprocess.check_call(["gcc", "-O2", "-shared", "-fPIC", "-o", so,
                           os.path.join(HERE, "..", "core", "cpu.c"),
                           os.path.join(HERE, "cputest_glue.c"), "-lm"])
    return so


def init(so):
    global LIB, RAM
    LIB = ctypes.CDLL(so)
    LIB.t_init.restype = ctypes.POINTER(ctypes.c_uint8)
    RAM = LIB.t_init(0)


def run_file(args):
    path, meta, limit, verbose = args
    name = os.path.basename(path).split(".json")[0]
    op = name.split(".")[0]
    m = meta.get(op, {})
    if "reg" in m:
        m = m["reg"].get(name.split(".")[1], {}) if "." in name else {}
    mask = m.get("flags-mask", 0xFFFF)
    status = m.get("status", "?")
    with gzip.open(path) as f:
        tests = json.load(f)
    if limit:
        tests = tests[:limit]
    regs = (ctypes.c_uint16 * 14)()
    fails = []
    for t in tests:
        ini, fin = t["initial"], t["final"]
        for i, r in enumerate(REGS):
            regs[i] = ini["regs"][r]
        touched = []
        for a, v in ini["ram"]:
            RAM[a] = v
            touched.append(a)
        LIB.t_run(regs)
        exp = dict(ini["regs"])
        exp.update(fin["regs"])
        bad = []
        for i, r in enumerate(REGS):
            got, want = regs[i], exp[r]
            if r == "flags":
                got &= mask
                want &= mask
            if got != want:
                bad.append("%s=%04X want %04X" % (r, regs[i], exp[r]))
        # An exception pushes FLAGS, including the bits this opcode leaves undefined.
        ss, sp = ini["regs"]["ss"], ini["regs"]["sp"]
        fl_lo = ((ss << 4) + ((sp - 2) & 0xFFFF)) & 0xFFFFF
        fl_hi = ((ss << 4) + ((sp - 1) & 0xFFFF)) & 0xFFFFF
        for a, v in fin["ram"]:
            got = RAM[a]
            if got != v:
                if a == fl_lo and (got ^ v) & mask & 0xFF == 0:
                    continue
                if a == fl_hi and (got ^ v) & (mask >> 8) == 0:
                    continue
                bad.append("[%05X]=%02X want %02X" % (a, got, v))
            touched.append(a)
        for a in touched:
            RAM[a] = 0
        if bad:
            fails.append((t["name"], " ".join("%02X" % b for b in t["bytes"]), bad, ini["regs"]))
    return name, status, len(tests), fails, verbose


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("-n", type=int, default=0)
    ap.add_argument("-v", action="store_true")
    ap.add_argument("-j", type=int, default=os.cpu_count())
    ap.add_argument("--build-dir", default=os.path.join(HERE, "..", "build"))
    ap.add_argument("ops", nargs="*")
    a = ap.parse_args()

    os.makedirs(a.build_dir, exist_ok=True)
    so = build(a.build_dir)
    meta = json.load(open(os.path.join(a.dir, "metadata.json")))["opcodes"]
    files = sorted(f for f in os.listdir(a.dir) if f.endswith(".json.gz"))
    if a.ops:
        files = [f for f in files if f.split(".")[0] in a.ops or f.split(".json")[0] in a.ops]
    jobs = [(os.path.join(a.dir, f), meta, a.n, a.v) for f in files]

    total = failed = 0
    bad_files = []
    with multiprocessing.Pool(a.j, initializer=init, initargs=(so,)) as pool:
        for name, status, n, fails, verbose in pool.imap(run_file, jobs):
            total += n
            failed += len(fails)
            if fails:
                bad_files.append((name, status, n, len(fails)))
                print("%-6s %-12s %5d/%d failed" % (name, status, len(fails), n))
                for nm, by, bad, ini in fails[:4 if verbose else 1]:
                    print("    %-40s [%s] %s" % (nm, by, "; ".join(bad[:6])))
                    if verbose:
                        print("        " + " ".join("%s=%04X" % (r, ini[r]) for r in REGS))
    print("\n%d tests, %d failed, %d/%d files with failures" %
          (total, failed, len(bad_files), len(files)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
