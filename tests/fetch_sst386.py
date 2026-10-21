#!/usr/bin/env python3
"""Download the SingleStepTests/80386 real-mode suite (about 600 MB of gzipped MOO files),
with the suite's opcode table and its list of revoked tests.

usage: fetch_sst386.py [OUTDIR]     (default: build/sst386)
"""
import json
import os
import sys
import urllib.request
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "sst386")
API = "https://api.github.com/repos/SingleStepTests/80386/contents/v1_ex_real_mode"
BASE = "https://raw.githubusercontent.com/SingleStepTests/80386/main/"

os.makedirs(outdir, exist_ok=True)
with urllib.request.urlopen(API, timeout=60) as r:
    files = [(e["download_url"], e["name"], e["size"]) for e in json.load(r)]
files += [(BASE + n, n, 0) for n in ("80386.csv", "revocation_list.txt")]


def fetch(item):
    url, name, size = item
    dst = os.path.join(outdir, name)
    if size and os.path.exists(dst) and os.path.getsize(dst) == size:
        return
    err = None
    for _ in range(4):
        try:
            with urllib.request.urlopen(url, timeout=120) as r:
                data = r.read()
            with open(dst + ".part", "wb") as f:
                f.write(data)
            os.replace(dst + ".part", dst)
            return
        except OSError as e:
            err = e
    print("FAILED", name, err, flush=True)


with ThreadPoolExecutor(8) as ex:
    list(ex.map(fetch, files))
print("%d files in %s" % (len(files), outdir))
