#!/usr/bin/env python3
"""Serve a disk image to P4DOS over its serial console. DOS gets one more hard disk, with
the drive letter after those of its own disks, next to the screen and keyboard that the
console carries already.

usage: hostdisk.py [-p PORT] [--ro] IMAGE
       hostdisk.py [-p PORT] DIRECTORY

IMAGE is a hard disk image with a partition table. A DIRECTORY is packed into a temporary
image and served read-only: its contents are the root of the drive (needs mtools).
DOS reboots when serving starts, to see the drive. The terminal shows the DOS screen and
types into it; Ctrl-] ends.

The frames are described in core/hostdisk.c. Needs pyserial.
"""
import argparse
import os
import random
import select
import struct
import subprocess
import sys
import tempfile
import time
import zlib

MAGIC = b"\x1b_HD"
SPT = 63


def frame(kind, seq, payload=b""):
    body = kind + bytes([seq]) + struct.pack("<H", len(payload)) + payload
    return MAGIC + body + struct.pack("<I", zlib.crc32(body))


class Link:
    """The host's end of the console line. feed() takes what arrives, answers the disk
    requests in it and returns the rest: the terminal mirror."""

    def __init__(self, image, write, readonly=False):
        self.image = image
        self.write = write
        self.readonly = readonly
        self.sectors = os.fstat(image.fileno()).st_size // 512
        self.session = random.getrandbits(32)
        self.attached = False
        self.sectors_read = self.sectors_written = 0
        self.buf = b""

    def hello(self):
        self.write(frame(b"H", 0, struct.pack("<IIB", self.session, self.sectors, self.readonly)))

    def bye(self):
        self.write(frame(b"B", 0, struct.pack("<I", self.session)))

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(MAGIC)
            if i < 0:
                # What ends the buffer may be the beginning of a frame.
                keep = next((k for k in (3, 2, 1) if self.buf.endswith(MAGIC[:k])), 0)
                i = len(self.buf) - keep
            out.append(self.buf[:i])
            self.buf = self.buf[i:]
            if len(self.buf) < 8:
                break
            n = struct.unpack_from("<H", self.buf, 6)[0]
            if len(self.buf) < 12 + n:
                break
            body = self.buf[4:8 + n]
            if zlib.crc32(body) != struct.unpack_from("<I", self.buf, 8 + n)[0]:
                self.buf = self.buf[4:]
                continue
            self.buf = self.buf[12 + n:]
            self.request(body[:1], body[1], body[4:])
        return b"".join(out)

    def request(self, kind, seq, p):
        if kind == b"A":
            self.attached = True
        elif kind == b"R" and len(p) == 5:
            lba, n = struct.unpack("<IB", p)
            self.image.seek(lba * 512)
            self.write(frame(b"D", seq, self.image.read(n * 512).ljust(n * 512, b"\0")))
            self.sectors_read += n
        elif kind == b"W" and len(p) >= 5:
            lba, n = struct.unpack_from("<IB", p)
            if self.readonly or len(p) != 5 + n * 512 or lba + n > self.sectors:
                self.write(frame(b"E", seq))
                return
            self.image.seek(lba * 512)
            self.image.write(p[5:])
            self.write(frame(b"K", seq))
            self.sectors_written += n
        elif kind == b"F":
            self.write(frame(b"K", seq))


def pack(directory, path):
    """Make a hard disk image with one FAT partition holding the contents of a directory."""
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    need = 0
    for d, _, files in os.walk(directory):
        # room for the largest clusters FAT16 has, and for the directory itself
        need += 32768 + sum(-(-os.path.getsize(os.path.join(d, f)) // 32768) * 32768 for f in files)
    mb = max(32, (need >> 20) + 4)
    fat32 = mb > 2000
    heads = 16 if mb <= 504 else 255
    cyls = -(-mb * 2048 // (heads * SPT))
    psize = cyls * heads * SPT - SPT
    with open(path, "wb") as f:
        f.truncate((SPT + psize) * 512)
    target = "%s@@%d" % (path, SPT * 512)
    subprocess.check_call(["mformat", "-i", target, "-T", str(psize), "-h", str(heads),
                           "-s", str(SPT), "-H", str(SPT), "-v", "HOSTDISK"]
                          + (["-F"] if fat32 else []) + ["::"], env=env)
    end = SPT + psize - 1
    ec, eh, es = min(end // (heads * SPT), 1023), (end // SPT) % heads, end % SPT + 1
    ptype = 0x0C if fat32 else 0x06 if cyls <= 1024 else 0x0E
    sector0 = bytearray(512)
    sector0[446:462] = struct.pack("<BBBBBBBBII", 0, 1, 1, 0, ptype, eh, es | ((ec >> 2) & 0xC0),
                                   ec & 0xFF, SPT, psize)
    sector0[510:512] = b"\x55\xAA"
    with open(path, "r+b") as f:
        f.write(sector0)
    entries = [os.path.join(directory, e) for e in sorted(os.listdir(directory))]
    if entries:
        subprocess.check_call(["mcopy", "-s", "-m", "-i", target] + entries + ["::"], env=env)


def serve(ser, link):
    import termios
    import tty

    fd = sys.stdin.fileno()
    saved = termios.tcgetattr(fd) if os.isatty(fd) else None
    if saved:
        tty.setraw(fd)
    next_hello = 0
    try:
        while True:
            if not link.attached and time.time() >= next_hello:
                link.hello()
                next_hello = time.time() + 0.2
            data = ser.read(max(1, ser.in_waiting))
            if data:
                os.write(sys.stdout.fileno(), link.feed(data))
            if saved and select.select([fd], [], [], 0)[0]:
                keys = os.read(fd, 256)
                if b"\x1d" in keys:
                    break
                ser.write(keys)
    except KeyboardInterrupt:
        pass
    finally:
        link.bye()
        ser.flush()
        if saved:
            termios.tcsetattr(fd, termios.TCSADRAIN, saved)
    print("\r\nhost disk: %d KB read, %d KB written%s" % (
        link.sectors_read // 2, link.sectors_written // 2,
        "" if link.attached else "; never attached: is this firmware too old?"))


def main():
    ap = argparse.ArgumentParser(description="Serve a disk image to P4DOS over its serial console.")
    ap.add_argument("-p", "--port", default="/dev/ttyACM0")
    ap.add_argument("--ro", action="store_true", help="serve the image read-only")
    ap.add_argument("source", help="hard disk image, or a directory to pack into one")
    a = ap.parse_args()
    import serial

    with tempfile.TemporaryDirectory() as tmp:
        path, readonly = a.source, a.ro
        if os.path.isdir(a.source):
            path, readonly = os.path.join(tmp, "disk.img"), True
            pack(a.source, path)
        with open(path, "rb" if readonly else "r+b", buffering=0) as image:
            ser = serial.Serial(a.port, 115200, timeout=0.02)
            serve(ser, Link(image, ser.write, readonly))


if __name__ == "__main__":
    main()
