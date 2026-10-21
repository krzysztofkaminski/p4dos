"""Reader for MOO files, the binary format of the SingleStepTests suites for the 80386."""
import gzip
import struct

RG32 = ["cr0", "cr3", "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "cs", "ds", "es",
        "fs", "gs", "ss", "eip", "eflags", "dr6", "dr7"]


def chunks(data, pos=0, end=None):
    end = len(data) if end is None else end
    while pos + 8 <= end:
        kind = data[pos:pos + 4].decode("ascii")
        (size,) = struct.unpack_from("<I", data, pos + 4)
        yield kind, pos + 8, pos + 8 + size
        pos += 8 + size


def regs32(data, pos):
    (mask,) = struct.unpack_from("<I", data, pos)
    out = {}
    pos += 4
    for i, name in enumerate(RG32):
        if mask >> i & 1:
            (out[name],) = struct.unpack_from("<I", data, pos)
            pos += 4
    return out


def state(data, pos, end):
    st = {"regs": {}, "ram": [], "masks": {}}
    for kind, a, b in chunks(data, pos, end):
        if kind == "RG32":
            st["regs"] = regs32(data, a)
        elif kind == "RM32":
            st["masks"] = regs32(data, a)
        elif kind == "RAM ":
            (n,) = struct.unpack_from("<I", data, a)
            st["ram"] = [struct.unpack_from("<IB", data, a + 4 + 5 * i) for i in range(n)]
    return st


def load(path):
    """Returns (file-wide register masks, list of tests)."""
    with gzip.open(path) as f:
        data = f.read()
    masks, tests = {}, []
    for kind, a, b in chunks(data):
        if kind == "RM32":
            masks = regs32(data, a)
        elif kind == "TEST":
            t = {"idx": struct.unpack_from("<I", data, a)[0], "exception": None}
            for k, c, d in chunks(data, a + 4, b):
                if k == "NAME":
                    (n,) = struct.unpack_from("<I", data, c)
                    t["name"] = data[c + 4:c + 4 + n].decode("ascii", "replace")
                elif k == "BYTS":
                    (n,) = struct.unpack_from("<I", data, c)
                    t["bytes"] = data[c + 4:c + 4 + n]
                elif k == "INIT":
                    t["initial"] = state(data, c, d)
                elif k == "FINA":
                    t["final"] = state(data, c, d)
                elif k == "EXCP":
                    t["exception"] = struct.unpack_from("<BI", data, c)
                elif k == "HASH":
                    t["hash"] = data[c:d].hex()
            tests.append(t)
    return masks, tests
