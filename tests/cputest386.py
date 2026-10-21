#!/usr/bin/env python3
"""Run the SingleStepTests/80386 real-mode suite against the CPU core (80386 model).

usage: cputest386.py <dir with *.MOO.gz> [-n TESTS_PER_FILE] [-v] [NAME ...]
"""
import argparse
import ctypes
import multiprocessing
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import moo  # noqa: E402

LIB = None
RAM = None
REVOKED = set()
UMASK = {}
# EFLAGS bits the 80386 has; the rest reads as ones or zeros on the test machine.
FLAG_BITS = 0x00037FD5


def build(outdir):
    so = os.path.join(outdir, "libcputest386.so")
    subprocess.check_call(["gcc", "-O2", "-shared", "-fPIC", "-o", so,
                           os.path.join(HERE, "..", "core", "cpu.c"),
                           os.path.join(HERE, "cputest386_glue.c"), "-lm"])
    return so


def init(so, revoked, umask):
    global LIB, RAM
    LIB = ctypes.CDLL(so)
    LIB.t386_init.restype = ctypes.POINTER(ctypes.c_uint8)
    RAM = LIB.t386_init()
    REVOKED.update(revoked)
    UMASK.update(umask)


def load_umask(path):
    """Flags each opcode leaves defined, from the suite's opcode table (80386.csv)."""
    import csv
    out = {}
    for r in csv.DictReader(open(path)):
        if r["f_umask"]:
            out[r["op"] + ("." + r["ex"] if r["ex"] else "")] = int(r["f_umask"], 16)
    return out


def run_file(args):
    path, limit = args
    name = os.path.basename(path).split(".MOO")[0]
    fmasks, tests = moo.load(path)
    if limit:
        tests = tests[:limit]
    regs = (ctypes.c_uint32 * 20)()
    fails = []
    for t in tests:
        if t.get("hash") in REVOKED:
            continue
        ini, fin = t["initial"], t["final"]
        for i, r in enumerate(moo.RG32):
            regs[i] = ini["regs"][r]
        touched = []
        for a, v in ini["ram"]:
            RAM[a] = v
            touched.append(a)
        halted = LIB.t386_run(regs)
        exp = dict(ini["regs"])
        exp.update(fin["regs"])
        masks = dict(fmasks)
        masks.update(fin["masks"])
        base = name
        while base[:2] in ("66", "67") and len(base.split(".")[0]) > 2:
            base = base[2:]
        fmask = masks.get("eflags", 0xFFFFFFFF) & FLAG_BITS
        fmask &= UMASK.get(base, 0xFFFF) | 0xFFFF0000
        bad = []
        if not halted:
            bad.append("no HLT")
        for i, r in enumerate(moo.RG32):
            got, want = regs[i], exp[r]
            m = masks.get(r, 0xFFFFFFFF)
            if r == "eflags":
                m = fmask
            elif r in ("cs", "ds", "es", "fs", "gs", "ss"):
                m &= 0xFFFF
            elif r == "cr0":
                m &= 0x8000000F
            elif r in ("dr6", "dr7"):
                continue
            if (got ^ want) & m:
                bad.append("%s=%08X want %08X" % (r, got, want))
        # An exception pushes FLAGS, including the bits this opcode leaves undefined.
        fl = t["exception"][1] if t["exception"] else None
        for a, v in fin["ram"]:
            got = RAM[a]
            if got != v:
                if fl is not None and a - fl in (0, 1) and \
                        (got ^ v) & (fmask >> (8 * (a - fl))) & 0xFF == 0:
                    pass
                else:
                    bad.append("[%06X]=%02X want %02X" % (a, got, v))
            touched.append(a)
        for a in touched:
            RAM[a] = 0
        if bad:
            fails.append((t["idx"], t["name"], t["bytes"].hex(), bad, ini["regs"], t["exception"]))
    return name, len(tests), fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("-n", type=int, default=0)
    ap.add_argument("-v", action="store_true")
    ap.add_argument("-j", type=int, default=os.cpu_count())
    ap.add_argument("--build-dir", default=os.path.join(HERE, "..", "build"))
    ap.add_argument("--revoked", help="file with the hashes of tests known to be wrong")
    ap.add_argument("--csv", help="80386.csv of the suite: which flags each opcode defines")
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()

    os.makedirs(a.build_dir, exist_ok=True)
    so = build(a.build_dir)
    revoked = set()
    rv = a.revoked or os.path.join(a.dir, "revocation_list.txt")
    if os.path.exists(rv):
        revoked = set(open(rv).read().split())
    csvp = a.csv or os.path.join(a.dir, "80386.csv")
    umask = load_umask(csvp) if os.path.exists(csvp) else {}
    files = sorted(f for f in os.listdir(a.dir) if f.endswith(".MOO.gz"))
    if a.names:
        files = [f for f in files if f.split(".MOO")[0] in a.names]
    jobs = [(os.path.join(a.dir, f), a.n) for f in files]

    total = failed = 0
    bad_files = []
    with multiprocessing.Pool(a.j, initializer=init, initargs=(so, revoked, umask)) as pool:
        for name, n, fails in pool.imap(run_file, jobs):
            total += n
            failed += len(fails)
            if fails:
                bad_files.append(name)
                print("%-12s %5d/%d failed" % (name, len(fails), n))
                for idx, nm, by, bad, ini, exc in fails[:4 if a.v else 1]:
                    print("    #%d %-36s [%s] %s%s" % (idx, nm, by, "; ".join(bad[:6]),
                                                      " (exception %d)" % exc[0] if exc else ""))
                    if a.v:
                        print("        " + " ".join("%s=%X" % (r, ini[r]) for r in moo.RG32[2:18]))
    print("\n%d tests, %d failed, %d/%d files with failures" %
          (total, failed, len(bad_files), len(files)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
