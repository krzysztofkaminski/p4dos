#!/usr/bin/env python3
"""bin2c.py <in.bin> <out.c> <symbol> [size]: emit a binary as a const uint8_t array."""
import sys

data = open(sys.argv[1], "rb").read()
sym = sys.argv[3]
size = int(sys.argv[4], 0) if len(sys.argv) > 4 else len(data)
assert len(data) == size, "%s: expected %d bytes, got %d" % (sys.argv[1], size, len(data))
with open(sys.argv[2], "w") as f:
    f.write("/* Generated from %s by tools/bin2c.py -- do not edit. */\n" % sys.argv[1])
    f.write("#include <stdint.h>\n\nconst uint8_t %s[%d] = {\n" % (sym, size))
    for i in range(0, size, 16):
        f.write("    " + ", ".join("0x%02x" % b for b in data[i:i + 16]) + ",\n")
    f.write("};\n")
