#!/usr/bin/env python3
"""Download the SingleStepTests/8088 v2 suite (about 730 MB of gzipped JSON).

usage: fetch_sst.py [OUTDIR]     (default: build/sst; the tests end up in OUTDIR/v2)
"""
import json
import os
import sys
import urllib.request
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "sst")
TREE = "https://api.github.com/repos/SingleStepTests/8088/git/trees/main?recursive=1"
BASE = "https://raw.githubusercontent.com/SingleStepTests/8088/main/"

with urllib.request.urlopen(TREE, timeout=60) as r:
    tree = json.load(r)["tree"]
paths = [e["path"] for e in tree if e["path"].startswith("v2/") and e["type"] == "blob"]


def fetch(path):
    dst = os.path.join(outdir, path)
    if os.path.exists(dst) and os.path.getsize(dst) > 0:
        return
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    err = None
    for _ in range(4):
        try:
            with urllib.request.urlopen(BASE + path, timeout=120) as r:
                data = r.read()
            with open(dst + ".part", "wb") as f:
                f.write(data)
            os.replace(dst + ".part", dst)
            return
        except OSError as e:
            err = e
    print("FAILED", path, err, flush=True)


with ThreadPoolExecutor(8) as ex:
    list(ex.map(fetch, paths))
print("%d files in %s" % (len(paths), os.path.join(outdir, "v2")))
