#!/usr/bin/env python3
"""Build the P4DOS disk images from the official FreeDOS 1.3 packages.

  build/p4dos-hd.img      hard disk image (MBR + one FAT partition) with FreeDOS installed;
                         flashed into the board's "dosdisk" partition and used as C:
  build/p4dos-floppy.img  1.44 MB bootable rescue floppy

Needs nasm and mtools. Packages are downloaded once into build/mkimage/.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = "https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/"

# Packages whose BIN/ files go to C:\FDOS\BIN.
PACKAGES = ["kernel", "freecom", "attrib", "chkdsk", "choice", "comp", "ctmouse", "debug",
            "deltree", "devload", "diskcopy", "edit", "edlin", "fc", "fdisk", "find", "format",
            "keyb", "keyb_lay", "label", "mem", "mode", "more", "move", "nansi", "replace",
            "share", "sort", "tree", "xcopy",
            # for the 80386: extended memory manager, EMM386, DPMI host and DOS extender
            "himemx", "jemm", "util/cwsdpmi", "util/dos32a"]
SKIP = {"JLOAD.EXE", "AHCICD.DLL", "MOVEXBDA.EXE", "UMBM.EXE", "CPUID.EXE", "JEMMDBG.EXE",
        "CPUSTAT.EXE", "XMSSTAT.EXE", "JDBGBRK.EXE", "JEMMEXL.EXE", "HIMEMX2.EXE",
        "KERNL386.SYS", "KERNL86N.SYS", "KERNL86.SYS", "COMMAND.COM", "DEBXXVDD.DLL",
        "EDLIN32.EXE", "_CHOICE.EXE", "_LABEL.EXE", "_MORE.EXE", "_MOVE.EXE", "DEBUGX.COM"}
FLOPPY_FILES = ["FDISK.EXE", "FDISK.INI", "FORMAT.EXE", "SYS.COM", "EDIT.EXE", "ATTRIB.COM",
                "CHKDSK.EXE", "XCOPY.EXE", "DELTREE.COM", "MEM.EXE", "MORE.EXE", "DEBUG.COM",
                "LABEL.EXE", "MODE.COM"]

HEADS, SPT = 16, 63


def run(*cmd, **kw):
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    subprocess.check_call(list(cmd), env=env, **kw)


def fetch(pkgdir):
    os.makedirs(pkgdir, exist_ok=True)
    for p in PACKAGES:
        dst = os.path.join(pkgdir, os.path.basename(p) + ".zip")
        if not os.path.exists(dst):
            print("fetching", p)
            url = REPO + (p if "/" in p else "base/" + p) + ".zip"
            with urllib.request.urlopen(url, timeout=120) as r:
                data = r.read()
            with open(dst + ".part", "wb") as f:
                f.write(data)
            os.replace(dst + ".part", dst)


def extract(pkgdir, work):
    """Returns {NAME: bytes} of every top-level BIN/ file, plus the kernel source tree."""
    files = {}
    for p in PACKAGES:
        with zipfile.ZipFile(os.path.join(pkgdir, os.path.basename(p) + ".zip")) as z:
            for n in z.namelist():
                parts = n.split("/")
                if len(parts) == 2 and parts[0].upper() == "BIN" and parts[1]:
                    files[parts[1].upper()] = z.read(n)
                if p == "kernel" and n.upper() == "SOURCE/KERNEL/SOURCES.ZIP":
                    src = os.path.join(work, "kernel-src")
                    os.makedirs(src, exist_ok=True)
                    with open(os.path.join(work, "kernel-src.zip"), "wb") as f:
                        f.write(z.read(n))
                    with zipfile.ZipFile(os.path.join(work, "kernel-src.zip")) as kz:
                        for m in kz.namelist():
                            if m.lower().startswith("boot/") and m.lower().endswith(".asm"):
                                with open(os.path.join(src, os.path.basename(m)), "wb") as f:
                                    f.write(kz.read(m))
    return files


def patch_himemx(data):
    """XMS function 6, local disable A20, while other locks remain: answer "done" as
    Microsoft's HIMEM does, not error 94h (A20 still enabled). FreeDOS holds a lock of its
    own whenever it runs from the HMA, and the DOS extender of Windows 3.1 takes the error
    for a broken driver ("Cannot start Windows in standard mode")."""
    at, code = 0x186, bytes.fromhex("ff0e120075dcebcb")  # dec [a20_locks] / jnz / jmp
    if data[at:at + len(code)] != code or data[0x153:0x159] != bytes.fromhex("b80100b300c3"):
        sys.exit("HIMEMX.EXE is not the 3.38 this patch was made for")
    return data[:at + 5] + b"\xc7" + data[at + 6:]  # jnz error_A20_still_enabled -> xms_success


def nasm(src, out, *defs, cwd=None):
    run("nasm", "-f", "bin", "-o", out, *defs, src, cwd=cwd)
    return open(out, "rb").read()


def install_boot_sector(img, offset, bootcode, drive):
    """What SYS does: keep the BPB the format wrote, replace the boot code around it."""
    with open(img, "r+b") as f:
        f.seek(offset)
        old = f.read(512)
        new = bytearray(bootcode)
        new[11:62] = old[11:62]
        new[3:11] = b"FRDOS5.1"
        new[0x24] = drive
        f.seek(offset)
        f.write(new)


def fat_type(img, offset):
    with open(img, "rb") as f:
        f.seek(offset + 0x36)
        return f.read(5).decode("ascii")


def mcopy(target, name, data, work):
    tmp = os.path.join(work, "tmpfile")
    with open(tmp, "wb") as f:
        f.write(data)
    run("mcopy", "-o", "-i", target, tmp, "::" + name)


def crlf(path):
    data = open(path, "rb").read()
    return data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")


def build_hd(out, mb, files, boots, mbr, drivers, work, extra):
    total = mb * 2048
    cyls = total // (HEADS * SPT)
    if cyls > 1024:
        sys.exit("hard disk image too large for CHS addressing (max 504 MB)")
    start = SPT
    psize = cyls * HEADS * SPT - start
    # Unused space is left as FFh, the erased state of NOR flash: on the board, writing a
    # file into fresh clusters then needs no block erase.
    with open(out, "wb") as f:
        f.write(b"\xff" * (total * 512))
    target = "%s@@%d" % (out, start * 512)
    run("mformat", "-i", target, "-T", str(psize), "-h", str(HEADS), "-s", str(SPT),
        "-H", str(start), "-v", "FREEDOS", "::")
    ft = fat_type(out, start * 512)
    install_boot_sector(out, start * 512, boots[ft], 0x80)

    end = start + psize - 1
    ec, eh, es = end // (HEADS * SPT), (end // SPT) % HEADS, end % SPT + 1
    ptype = 0x01 if ft == "FAT12" else (0x04 if psize < 65536 else 0x06)
    entry = struct.pack("<BBBBBBBBII", 0x80, 1, 1, 0, ptype, eh, es | ((ec >> 2) & 0xC0),
                        ec & 0xFF, start, psize)
    sector0 = bytearray(512)
    sector0[:len(mbr)] = mbr
    sector0[446:462] = entry
    sector0[510:512] = b"\x55\xAA"
    with open(out, "r+b") as f:
        f.write(sector0)

    mcopy(target, "KERNEL.SYS", files["KERNL86.SYS"], work)
    mcopy(target, "COMMAND.COM", files["COMMAND.COM"], work)
    mcopy(target, "FDCONFIG.SYS", crlf(os.path.join(ROOT, "dosfiles", "FDCONFIG.SYS")), work)
    mcopy(target, "AUTOEXEC.BAT", crlf(os.path.join(ROOT, "dosfiles", "AUTOEXEC.BAT")), work)
    run("mmd", "-i", target, "::FDOS", "::FDOS/BIN", "::TEMP")
    for name in sorted(files):
        if name not in SKIP:
            mcopy(target, "FDOS/BIN/" + name, files[name], work)
    for name, data in drivers.items():
        mcopy(target, "FDOS/BIN/" + name, data, work)
    for path in extra:
        run("mcopy", "-o", "-s", "-i", target, path, "::")
    print("%s: %d MB, %s, C/H/S %d/%d/%d" % (out, mb, ft, cyls, HEADS, SPT))


def build_floppy(out, files, boots, drivers, work):
    with open(out, "wb") as f:
        f.truncate(1440 * 1024)
    run("mformat", "-i", out, "-f", "1440", "-v", "RESCUE", "::")
    install_boot_sector(out, 0, boots[fat_type(out, 0)], 0x00)
    mcopy(out, "KERNEL.SYS", files["KERNL86.SYS"], work)
    mcopy(out, "COMMAND.COM", files["COMMAND.COM"], work)
    mcopy(out, "FDCONFIG.SYS", crlf(os.path.join(ROOT, "dosfiles", "FLOPPY.SYS")), work)
    mcopy(out, "AUTOEXEC.BAT", crlf(os.path.join(ROOT, "dosfiles", "FLOPPY.BAT")), work)
    for name in FLOPPY_FILES:
        mcopy(out, name, files[name], work)
    for name, data in drivers.items():
        mcopy(out, name, data, work)
    print("%s: 1.44 MB rescue floppy" % out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build", default=os.path.join(ROOT, "build"))
    ap.add_argument("--hd-mb", type=int, default=14,
                    help="hard disk image size; 14 matches the board's flash partition")
    ap.add_argument("--add", action="append", default=[],
                    help="extra file or directory to copy to C:\\ (repeatable)")
    a = ap.parse_args()

    for tool in ("nasm", "mformat", "mcopy", "mmd"):
        if not shutil.which(tool):
            sys.exit("missing tool: " + tool)
    work = os.path.join(a.build, "mkimage")
    fetch(os.path.join(work, "pkg"))
    files = extract(os.path.join(work, "pkg"), work)
    files["HIMEMX.EXE"] = patch_himemx(files["HIMEMX.EXE"])

    src = os.path.join(work, "kernel-src")
    boots = {
        "FAT12": nasm("boot.asm", os.path.join(work, "fat12.bin"), "-DISFAT12", cwd=src),
        "FAT16": nasm("boot.asm", os.path.join(work, "fat16.bin"), "-DISFAT16", cwd=src),
    }
    dosfiles = os.path.join(ROOT, "dosfiles")
    mbr = nasm("mbr.asm", os.path.join(work, "mbr.bin"), cwd=dosfiles)
    drivers = {
        "EMM.SYS": nasm("emm.asm", os.path.join(work, "emm.sys"), cwd=dosfiles),
        "XMS.SYS": nasm("xms.asm", os.path.join(work, "xms.sys"), cwd=dosfiles),
        "SPEED.COM": nasm("speed.asm", os.path.join(work, "speed.com"), cwd=dosfiles),
        "WELCOME.COM": nasm("welcome.asm", os.path.join(work, "welcome.com"), cwd=dosfiles),
    }

    build_hd(os.path.join(a.build, "p4dos-hd.img"), a.hd_mb, files, boots, mbr, drivers, work,
             a.add)
    build_floppy(os.path.join(a.build, "p4dos-floppy.img"), files, boots, drivers, work)


if __name__ == "__main__":
    main()
