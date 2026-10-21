#!/usr/bin/env python3
"""Boot FreeDOS on the emulator core compiled for RISC-V, inside qemu-system-riscv32.

usage: rv32_boot.py [--qemu PATH] [--elf build/rv32/p4dos.elf] [--disk build/p4dos-hd.img]
Drives the serial console: waits for the DOS prompt, runs a few commands and checks
their output. Exit status 0 on success.
"""
import argparse
import os
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CSI = re.compile(r"\x1b\[([0-9;?]*)([A-Za-z])")


class Screen:
    """Just enough of a VT100 to follow what core/term.c sends."""

    def __init__(self):
        self.rows = [[" "] * 80 for _ in range(25)]
        self.r = self.c = 0
        self.pending = ""

    def feed(self, data):
        s = self.pending + data.decode("utf-8", "replace")
        self.pending = ""
        i = 0
        while i < len(s):
            ch = s[i]
            if ch == "\x1b":
                m = CSI.match(s, i)
                if not m:
                    if len(s) - i < 12:
                        self.pending = s[i:]
                        return
                    i += 1
                    continue
                args, final = m.group(1), m.group(2)
                if final == "H":
                    a = (args.split(";") + ["1", "1"])[:2]
                    self.r, self.c = int(a[0] or 1) - 1, int(a[1] or 1) - 1
                elif final == "J":
                    self.rows = [[" "] * 80 for _ in range(25)]
                i = m.end()
                continue
            if ch == "\n":
                if self.r >= 24:
                    self.rows = self.rows[1:] + [[" "] * 80]
                else:
                    self.r += 1
            elif ch == "\r":
                self.c = 0
            elif ch >= " " and self.r < 25 and self.c < 80:
                self.rows[self.r][self.c] = ch
                self.c += 1
            i += 1

    def text(self):
        return "\n".join("".join(r).rstrip() for r in self.rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=os.environ.get("QEMU_RV32", "qemu-system-riscv32"))
    ap.add_argument("--elf", default=os.path.join(ROOT, "build", "rv32", "p4dos.elf"))
    ap.add_argument("--disk", default=os.path.join(ROOT, "build", "p4dos-hd.img"))
    ap.add_argument("-v", action="store_true")
    a = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        disk = os.path.join(tmp, "hd.img")
        shutil.copy(a.disk, disk)
        cmd = [a.qemu, "-M", "virt", "-m", "256M", "-bios", "none", "-nographic",
               "-monitor", "none", "-serial", "stdio", "-kernel", a.elf,
               "-device", "loader,file=%s,addr=0x84000000" % disk]
        p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, bufsize=0)
        scr = Screen()

        def expect(text, timeout):
            end = time.time() + timeout
            while True:
                shown = scr.text()
                if text in shown:
                    return shown
                left = end - time.time()
                if left <= 0 or p.poll() is not None:
                    sys.stderr.write("FAILED waiting for %r; screen:\n%s\n" % (text, shown))
                    p.kill()
                    sys.exit(1)
                if select.select([p.stdout], [], [], min(left, 0.5))[0]:
                    scr.feed(os.read(p.stdout.fileno(), 65536))

        def send(text):
            for ch in text:
                os.write(p.stdin.fileno(), ch.encode())
                time.sleep(0.03)

        try:
            t0 = time.time()
            out = expect("C:\\>", 120)
            print("FreeDOS prompt on RV32 after %.1f s" % (time.time() - t0))
            if a.v:
                print(out)
            send("cls\r")
            send("mem\r")
            out = expect("Largest executable", 60)
            for line in out.splitlines():
                if "Conventional" in line or "Total Expanded" in line:
                    print(line.strip())
            send("cls\r")
            send("echo RV32-OK>t.txt\r")
            send("type t.txt\r")
            expect("\nRV32-OK", 30)
            print("file write/read on the RAM disk: ok")
        finally:
            p.kill()
    print("PASS")


if __name__ == "__main__":
    main()
