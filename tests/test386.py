#!/usr/bin/env python3
"""Run test386.asm (https://github.com/barotto/test386.asm) on the CPU core: a ROM that
exercises an 80386 in real and protected mode, with paging, privilege levels, virtual-8086
mode and task switches, and prints the results of some 45,000 arithmetic operations to be
compared with those of a real processor.

usage: test386.py [--dir DIR] [--undef]
Downloads the sources to DIR (default build/test386) on first use; needs nasm and gcc.
--undef also checks what the 80386 does where the manuals say "undefined".
"""
import argparse
import io
import os
import re
import subprocess
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
URL = "https://github.com/barotto/test386.asm/archive/refs/heads/master.zip"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(ROOT, "build", "test386"))
    ap.add_argument("--undef", action="store_true")
    a = ap.parse_args()

    src = os.path.join(a.dir, "test386.asm-master")
    if not os.path.isdir(src):
        os.makedirs(a.dir, exist_ok=True)
        with urllib.request.urlopen(URL, timeout=120) as r:
            zipfile.ZipFile(io.BytesIO(r.read())).extractall(a.dir)

    # Our configuration: diagnostic codes to port 190h, text to port E9h, the larger ROM
    # that includes the task switching tests.
    cfg_path = os.path.join(src, "src", "configuration.asm")
    cfg = open(cfg_path).read()
    for name, value in (("POST_PORT", "0x190"), ("OUT_PORT", "0xE9"), ("ROM128", "1"),
                        ("TEST_UNDEF", "1" if a.undef else "0"), ("CPU_FAMILY", "3")):
        cfg = re.sub(r"(?m)^%s equ .*$" % name, "%s equ %s" % (name, value), cfg)
    work = os.path.join(a.dir, "undef" if a.undef else "plain")
    os.makedirs(os.path.join(work, "src"), exist_ok=True)
    subprocess.check_call(["cp", "-r", os.path.join(src, "src"), work])
    with open(os.path.join(work, "src", "configuration.asm"), "w") as f:
        f.write(cfg)
    rom = os.path.join(work, "test386.bin")
    subprocess.run(["nasm", "-i./src/", "-f", "bin", "src/test386.asm", "-l", "test386.lst",
                    "-o", rom], cwd=work, check=True, stderr=subprocess.DEVNULL)
    host = os.path.join(a.dir, "test386_host")
    subprocess.check_call(["gcc", "-O2", "-o", host, os.path.join(HERE, "test386_host.c"),
                           os.path.join(ROOT, "core", "cpu.c"), "-lm"])
    out = os.path.join(work, "ee.txt")
    p = subprocess.run([host, rom, out], capture_output=True, text=True)
    print(p.stdout.strip())
    if p.returncode != 0:
        print("FAILED: the ROM stopped before POST code FF (see %s/test386.lst)" % work)
        return 1
    want = open(os.path.join(src, "test386-EE-reference.txt"), newline="").read().replace("\r", "")
    got = open(out, newline="").read().replace("\r", "")
    if got != want:
        gl, wl = got.split("\n"), want.split("\n")
        bad = [i for i in range(min(len(gl), len(wl))) if gl[i] != wl[i]]
        print("FAILED: %d of %d result lines differ from the reference; first:" %
              (len(bad) + abs(len(gl) - len(wl)), len(wl)))
        for i in bad[:5]:
            print("  got  " + gl[i] + "\n  want " + wl[i])
        return 1
    print("passed: POST FF, %d result lines match the reference" % (len(want.split("\n")) - 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
