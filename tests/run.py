#!/usr/bin/env python3
"""Regression tests: boot FreeDOS in the host build of the emulator (headless, virtual
time) and drive it through scripted sessions.

usage: run.py [-k SUBSTRING] [--keep] [--shots DIR]
Needs build/p4dos (make), build/p4dos-hd.img and build/p4dos-floppy.img (make image),
nasm and mtools.
"""
import argparse
import os
import select
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path[:0] = [HERE, os.path.join(ROOT, "tools")]
import hostdisk  # noqa: E402
from rv32_boot import Screen  # noqa: E402

P4DOS = os.path.join(ROOT, "build", "p4dos")
HD = os.path.join(ROOT, "build", "p4dos-hd.img")
FLOPPY = os.path.join(ROOT, "build", "p4dos-floppy.img")
PART = 63 * 512  # offset of the FAT partition inside the hard disk image
ENV = dict(os.environ, MTOOLS_SKIP_CHECK="1")

TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


class Session:
    def __init__(self, tmp, shots):
        self.tmp = tmp
        self.shots = shots
        self.hd = os.path.join(tmp, "hd.img")
        shutil.copy(HD, self.hd)

    def add_program(self, name):
        """Assemble tests/dos/<name>.asm and put it in C:\\ of the working image."""
        com = os.path.join(self.tmp, name.upper() + ".COM")
        subprocess.check_call(["nasm", "-f", "bin", "-o", com,
                               os.path.join(HERE, "dos", name + ".asm")])
        subprocess.check_call(["mcopy", "-o", "-i", "%s@@%d" % (self.hd, PART), com, "::"],
                              env=ENV)

    def put_file(self, name, data):
        """Write a file into C:\\ of the working image."""
        path = os.path.join(self.tmp, name)
        with open(path, "wb") as f:
            f.write(data)
        subprocess.check_call(["mcopy", "-o", "-i", "%s@@%d" % (self.hd, PART), path, "::"],
                              env=ENV)

    def run(self, script, args=("-c", None), timeout=300):
        """Run a script; returns everything its `dump` commands printed."""
        argv = [P4DOS]
        for a in args:
            argv.append(self.hd if a is None else a)
        path = os.path.join(self.tmp, "script.txt")
        with open(path, "w") as f:
            f.write(script.replace("@SHOTS@", self.shots or self.tmp))
        p = subprocess.run(argv + ["--script", path], capture_output=True, text=True,
                           timeout=timeout)
        if p.returncode != 0:
            raise AssertionError("emulator script failed:\n" + p.stderr[-3000:])
        return p.stdout

    def mtype(self, path):
        return subprocess.check_output(["mtype", "-i", "%s@@%d" % (self.hd, PART), "::" + path],
                                       env=ENV)


def expect(out, *needles):
    for n in needles:
        if n not in out:
            raise AssertionError("expected %r in:\n%s" % (n, out))


@test
def boot_to_prompt(s):
    out = s.run("wait:30 C:\\>\ntype ver\\r\nsleep 600\ntype mem\\r\nsleep 1500\ndump\n")
    expect(out, "FreeCom version", "Conventional          640K", "Total Expanded (EMS)")


@test
def identifies_as_80386_vga(s):
    s.add_program("sysinfo")
    out = s.run("wait:30 C:\\>\ntype sysinfo\\r\nsleep 1500\ndump\n")
    expect(out, "CPU:    80386", "FPU:    present", "Memory: 640 KB",
           "Video:  VGA, analog color", "EMS:    4096 KB", "Mouse:  none",
           "Welcome to FreeDOS (on an emulated 80386DX with 80387).")


@test
def coprocessor_80387(s):
    s.add_program("fputest")
    out = s.run("wait:30 C:\\>\ntype fputest\\r\nsleep 3000\ndump\n")
    expect(out, "80387 test passed")


@test
def no_coprocessor_when_not_fitted(s):
    s.add_program("sysinfo")
    out = s.run("wait:30 C:\\>\ntype sysinfo\\r\nsleep 1500\ndump\n",
                args=("--fpu", "0", "-c", None))
    expect(out, "CPU:    80386", "FPU:    none", "Welcome to FreeDOS (on an emulated 80386DX).")


@test
def extended_memory_and_dos_high(s):
    out = s.run("wait:30 C:\\>\ntype mem\\r\nsleep 2500\ndump\n")
    expect(out, "Extended (XMS)     15,360K", "FreeDOS is resident in the high memory area")


@test
def dpmi_protected_mode_program(s):
    # A 32-bit program in ring 3 under CWSDPMI: protected mode, paging and its page
    # faults, interrupts reflected to real mode.
    s.add_program("dpmitest")
    out = s.run("wait:30 C:\\>\ntype cwsdpmi -p\\r\nsleep 2500\ntype dpmitest\\r\nsleep 6000\ndump\n")
    expect(out, "DPMITEST: 32-bit protected mode, 1024 KB of extended memory checked")


@test
def virtual_8086_mode_under_jemm386(s):
    # DOS itself in virtual-8086 mode, paging on, with JEMM386 providing EMS and trapping
    # what DOS does.
    s.put_file("FDCONFIG.SYS", b"!LASTDRIVE=Z\r\n!BUFFERS=20\r\n!FILES=40\r\nDOS=HIGH\r\n"
               b"DEVICE=C:\\FDOS\\BIN\\HIMEMX.EXE\r\nDEVICE=C:\\FDOS\\BIN\\JEMM386.EXE\r\n"
               b"SHELL=C:\\COMMAND.COM C:\\ /E:1024 /P=C:\\AUTOEXEC.BAT\r\n")
    s.add_program("sysinfo")
    out = s.run("wait:60 C:\\>\ntype sysinfo\\r\nsleep 2500\ntype mem\\r\nsleep 3000\ndump\n")
    expect(out, "CPU:    80386", "EMS:    ", "Total Expanded (EMS)")
    if "EMS:    4096 KB" in out:
        raise AssertionError("expanded memory is not JEMM386's:\n" + out)


@test
def cpu_80186_mode(s):
    s.add_program("sysinfo")
    out = s.run("wait:30 C:\\>\ntype sysinfo\\r\nsleep 1500\ndump\n",
                args=("--cpu", "186", "-c", None))
    expect(out, "CPU:    80186", "FPU:    none", "Memory: 640 KB", "EMS:    4096 KB",
           "Welcome to FreeDOS (on an emulated 80186).")


@test
def instructions_added_by_the_80186(s):
    s.add_program("t186")
    out = s.run("wait:30 C:\\>\ntype t186\\r\nsleep 1500\ndump\n",
                args=("--cpu", "186", "-c", None))
    expect(out, "80186 instruction test passed")


@test
def file_io_reaches_the_image(s):
    out = s.run("wait:30 C:\\>\n"
                "type md \\\\work\\r\nsleep 400\n"
                "type echo first line>\\\\work\\\\a.txt\\r\nsleep 400\n"
                "type echo second line>>\\\\work\\\\a.txt\\r\nsleep 400\n"
                "type copy \\\\work\\\\a.txt \\\\work\\\\b.txt\\r\nsleep 600\n"
                "type xcopy \\\\fdos\\\\bin\\\\*.com \\\\work\\\\bin\\\\ /q\\r\nsleep 8000\n"
                "type fc /b \\\\fdos\\\\bin\\\\debug.com \\\\work\\\\bin\\\\debug.com\\r\nsleep 2500\n"
                "type del \\\\work\\\\a.txt\\r\nsleep 400\n"
                "type dir \\\\work\\r\nsleep 800\ndump\n")
    expect(out, "B        TXT", "No differences")
    if "A        TXT" in out.split("dir \\work")[-1]:
        raise AssertionError("A.TXT was not deleted:\n" + out)
    if s.mtype("WORK/B.TXT") != b"first line\r\nsecond line\r\n":
        raise AssertionError("file contents on the image are wrong: %r" % s.mtype("WORK/B.TXT"))
    src = s.mtype("FDOS/BIN/XCOPY.EXE")
    if s.mtype("WORK/BIN/DEBUG.COM") != s.mtype("FDOS/BIN/DEBUG.COM") or not src:
        raise AssertionError("copied binary differs")
    subprocess.check_call(["mdir", "-i", "%s@@%d" % (s.hd, PART), "::WORK/BIN"], env=ENV,
                          stdout=subprocess.DEVNULL)


@test
def full_screen_editor(s):
    out = s.run("wait:30 C:\\>\ntype edit note.txt\\r\nwait:20 File\nsleep 500\n"
                "type Hello from the editor\nsleep 300\nshothalf @SHOTS@/edit.png\ndump\n"
                "type \\af\nsleep 400\ntype x\nsleep 600\ntype n\nwait:10 C:\\>\ndump\n")
    expect(out, "Edit", "Hello from the editor")


@test
def debug_runs_code(s):
    # Assemble and run a tiny program in DEBUG: prints '!' through DOS and stops at INT 3.
    out = s.run("wait:30 C:\\>\ntype debug\\r\nwait:10 -\n"
                "type a 100\\r\nsleep 300\n"
                "type mov ah,2\\r\nsleep 200\ntype mov dl,21\\r\nsleep 200\n"
                "type int 21\\r\nsleep 200\ntype int 3\\r\nsleep 200\ntype \\r\nsleep 300\n"
                "type g=100\\r\nsleep 800\ndump\ntype q\\r\nwait:10 C:\\>\n")
    expect(out, "!", "AX=02")


@test
def ems_pages_hold_data(s):
    s.add_program("emstest")
    out = s.run("wait:30 C:\\>\ntype emstest\\r\nsleep 2500\ndump\n")
    expect(out, "EMS test passed")


@test
def xms_blocks_hold_data(s):
    s.add_program("xmstest")
    out = s.run("wait:30 C:\\>\ntype xmstest\\r\nsleep 2500\ntype mem\\r\nsleep 1500\ndump\n")
    expect(out, "XMS test passed", "Extended (XMS)")
    free_kb = int(out.split("Largest executable program size")[1].split("K")[0])
    if free_kb < 560:
        raise AssertionError("only %d KB of conventional memory free" % free_kb)


@test
def xms_a20_locks_nest(s):
    # The probe Windows 3.1 makes before it starts in standard mode.
    s.add_program("a20test")
    out = s.run("wait:30 C:\\>\ntype a20test\\r\nsleep 2500\ndump\n")
    expect(out, "A20 test passed")


@test
def sound_blaster_plays_by_dma(s):
    import struct
    import wave
    s.add_program("sbtest")
    wav = os.path.join(s.tmp, "sb.wav")
    out = s.run("wait:30 C:\\>\nwav %s\ntype sbtest\\r\nsleep 2500\nwav\ndump\n" % wav)
    expect(out, "Sound Blaster test passed: DSP 2.01")
    with wave.open(wav) as w:
        rate = w.getframerate()
        data = struct.unpack("<%dh" % w.getnframes(), w.readframes(w.getnframes()))
    # The test tone: 0.25 s of a 441 Hz square wave.
    loud = [i for i, v in enumerate(data) if abs(v) > 3000]
    if not loud:
        raise AssertionError("the recording is silent")
    dur = (loud[-1] - loud[0]) / rate
    seg = data[loud[0]:loud[-1]]
    crossings = sum(1 for a, b in zip(seg, seg[1:]) if (a < 0) != (b < 0))
    freq = crossings / 2 / dur
    if not 0.23 < dur < 0.27 or not 430 < freq < 452:
        raise AssertionError("tone is %.3f s at %.0f Hz, expected 0.25 s at 441 Hz" % (dur, freq))


@test
def volume_shortcut(s):
    import struct
    import wave
    s.add_program("sbtest")
    wavs = [os.path.join(s.tmp, "v%d.wav" % i) for i in range(3)]
    # Play the test tone at the default volume, after Ctrl+Alt+Down twice, and after
    # Ctrl+Alt+Up four times. The arrow keys must not reach DOS (they would recall the
    # previous command line and run SBTEST once more).
    script = "wait:30 C:\\>\nwav %s\ntype sbtest\\r\nsleep 2500\nwav\n" % wavs[0]
    script += "hid 05\nhid 05 51\nhid 05\nhid 05 51\nhid 05\nhid 00\nsleep 500\n"
    script += "wav %s\ntype sbtest\\r\nsleep 2500\nwav\n" % wavs[1]
    script += "hid 05\n" + "hid 05 52\nhid 05\n" * 4 + "hid 00\nsleep 500\n"
    script += "wav %s\ntype sbtest\\r\nsleep 2500\nwav\ndump\n" % wavs[2]
    out = s.run(script)
    if out.count("Sound Blaster test passed") != 3:
        raise AssertionError("expected three test runs:\n" + out)
    peaks = []
    for w in wavs:
        with wave.open(w) as f:
            d = struct.unpack("<%dh" % f.getnframes(), f.readframes(f.getnframes()))
        peaks.append(max(abs(v) for v in d))
    # 60 % -> 40 % -> 80 %, with gain proportional to the square of the setting
    for got, want in ((peaks[1] / peaks[0], (40 / 60) ** 2), (peaks[2] / peaks[0], (80 / 60) ** 2)):
        if abs(got - want) > 0.05:
            raise AssertionError("volume ratio %.2f, expected %.2f (peaks %s)" % (got, want, peaks))


@test
def vga_planar_modex_and_timing(s):
    s.add_program("vgatest")
    # Drawn with the display refreshed as on the board, only where something changed, and
    # compared with the whole picture drawn afresh.
    out = s.run("wait:30 C:\\>\ntype vgatest\\r\nframes 25 1200\nshothalf @SHOTS@/vga12.png\n"
                "type x\nframes 25 600\nshothalf @SHOTS@/modex.png\n"
                "type x\nwait:20 Timer fired\nsleep 300\ndump\n")
    if out.count("frames: ok") != 2:
        raise AssertionError("incremental redraw differs from the picture:\n" + out)
    # 480 displayed lines. The frame is 527 lines with the retrace starting at line 490, so
    # a one-shot programmed for 100 lines after the retrace starts fires on line 63 of the
    # next frame: 63 displayed lines have gone by, give or take one.
    expect(out, "Displayed scan lines: 480")
    fired = int(out.split("Timer fired at line: ")[1].split()[0])
    if not 62 <= fired <= 64:
        raise AssertionError("timer interrupt landed after %d displayed lines, not 63" % fired)


@test
def graphics_modes(s):
    s.add_program("gfxtest")
    out = s.run("wait:30 C:\\>\ntype gfxtest\\r\nsleep 6000\nshothalf @SHOTS@/mode13.png\n"
                "type x\nsleep 12000\nshothalf @SHOTS@/mode04.png\n"
                "type x\nsleep 12000\nshothalf @SHOTS@/mode06.png\n"
                "type x\nsleep 25000\nshothalf @SHOTS@/mode11.png\n"
                "type x\nwait:20 Graphics test done\ndump\n")
    expect(out, "Graphics test done")


@test
def serial_mouse_with_ctmouse(s):
    s.add_program("sysinfo")
    s.add_program("mousetst")
    out = s.run("wait:30 C:\\>\ntype sysinfo\\r\nsleep 1500\ndump\n"
                "type mousetst\\r\nwait:10 Click the left\n"
                "mouse 40 16 0\nmouse 0 0 1\nsleep 500\nmouse 0 0 0\nsleep 500\ndump\n",
                args=("--mouse", "-c", None))
    expect(out, "Mouse:  present", "Click at X=")
    x = int(out.split("Click at X=")[1].split()[0])
    if x <= 320:
        raise AssertionError("pointer did not move right: X=%d" % x)


@test
def floppy_boot_and_format(s):
    a = os.path.join(s.tmp, "a.img")
    b = os.path.join(s.tmp, "b.img")
    shutil.copy(FLOPPY, a)
    with open(b, "wb") as f:
        f.truncate(1440 * 1024)
    out = s.run("wait:40 A:\\>\ndump\ntype format b: /u /v:TESTDISK\\r\nwait:20 Press ENTER\n"
                "type \\r\nsleep 1000\ndump\nwait:240 A:\\>\nsleep 300\n"
                "type copy a:\\\\autoexec.bat b:\\r\nsleep 1500\ntype dir b:\\r\nsleep 1000\ndump\n",
                args=("-a", a, "-b", b, "-c", None))
    expect(out, "FreeDOS rescue floppy", "AUTOEXEC BAT")
    listing = subprocess.check_output(["mdir", "-i", b, "::"], env=ENV, text=True)
    if "AUTOEXEC" not in listing.upper():
        raise AssertionError("formatted floppy is not readable by mtools:\n" + listing)


def make_card(path, mb):
    """A sparse image partitioned and formatted the way SD cards come: one FAT32 (LBA)
    partition starting at sector 8192."""
    import struct
    total = mb * 2048
    start = 8192
    with open(path, "wb") as f:
        f.truncate(total * 512)
        mbr = bytearray(512)
        mbr[446:462] = struct.pack("<BBBBBBBBII", 0, 0x82, 0x03, 0, 0x0C, 0xFE, 0xFF, 0xFF,
                                   start, total - start)
        mbr[510:512] = b"\x55\xAA"
        f.write(mbr)
    target = "%s@@%d" % (path, start * 512)
    subprocess.check_call(["mformat", "-i", target, "-F", "-T", str(total - start), "-h", "255",
                           "-s", "63", "-H", str(start), "-v", "SDCARD", "::"], env=ENV)
    return target


@test
def sd_card_is_drive_d(s):
    # 10 GB: beyond what CHS addressing reaches, so DOS has to use the LBA calls.
    card = os.path.join(s.tmp, "card.img")
    target = make_card(card, 10240)
    hello = os.path.join(s.tmp, "HELLO.TXT")
    with open(hello, "wb") as f:
        f.write(b"written on the PC\r\n")
    subprocess.check_call(["mmd", "-i", target, "::STUFF"], env=ENV)
    subprocess.check_call(["mcopy", "-i", target, hello, "::STUFF/HELLO.TXT"], env=ENV)
    out = s.run("wait:40 C:\\>\ntype type d:\\\\stuff\\\\hello.txt\\r\nsleep 1500\n"
                "type copy c:\\\\fdos\\\\bin\\\\edit.exe d:\\\\stuff\\r\nsleep 4000\n"
                "type echo from dos>d:\\\\dos.txt\\r\nsleep 1500\n"
                "type dir d:\\\\stuff\\r\nsleep 2500\ndump\n",
                args=("-c", None, "-d", card))
    expect(out, "written on the PC", "EDIT     EXE", "HELLO    TXT")
    if subprocess.check_output(["mtype", "-i", target, "::DOS.TXT"], env=ENV) != b"from dos\r\n":
        raise AssertionError("file written by DOS is not on the card image")
    if subprocess.check_output(["mtype", "-i", target, "::STUFF/EDIT.EXE"], env=ENV) != \
            s.mtype("FDOS/BIN/EDIT.EXE"):
        raise AssertionError("file copied to the card differs")


class Terminal:
    """The emulator in its terminal mode, by pipes and in real time, with a host disk."""

    def __init__(self, hd, image):
        self.p = subprocess.Popen([P4DOS, "-c", hd], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)
        self.link = hostdisk.Link(image, lambda b: os.write(self.p.stdin.fileno(), b))
        self.scr = Screen()

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if select.select([self.p.stdout], [], [], 0.02)[0]:
                self.scr.feed(self.link.feed(os.read(self.p.stdout.fileno(), 65536)))

    def at_prompt(self):
        return self.scr.text().rstrip().endswith("\nC:\\>")

    def wait(self, cond, what, timeout=60):
        end = time.time() + timeout
        while not cond():
            if time.time() > end or self.p.poll() is not None:
                raise AssertionError("no %s; screen:\n%s" % (what, self.scr.text()))
            self.pump(0.05)

    def command(self, line, timeout=60):
        for ch in line + "\r":
            os.write(self.p.stdin.fileno(), ch.encode())
            self.pump(0.03)
        self.pump(0.3)
        self.wait(self.at_prompt, "prompt after " + line, timeout)
        return self.scr.text()

    def close(self):
        self.p.stdin.close()
        try:
            self.p.wait(10)
        finally:
            self.p.kill()


@test
def host_disk_over_the_console(s):
    # tools/hostdisk.py serves a packed directory; with C: alone the host disk is D:.
    src, image = os.path.join(s.tmp, "src"), os.path.join(s.tmp, "host.img")
    big = os.urandom(300000)
    os.mkdir(src)
    with open(os.path.join(src, "HELLO.TXT"), "wb") as f:
        f.write(b"sent by the host\r\n")
    with open(os.path.join(src, "BIG.BIN"), "wb") as f:
        f.write(big)
    hostdisk.pack(src, image)
    with open(image, "r+b", buffering=0) as f:
        t = Terminal(s.hd, f)
        try:
            t.link.hello()
            t.wait(lambda: t.link.attached, "answer to the hello", 20)
            t.pump(1.0)  # the reboot that makes DOS see the drive
            t.wait(t.at_prompt, "prompt")
            expect(t.command("type D:\\HELLO.TXT"), "sent by the host")
            t.command("copy D:\\BIG.BIN C:\\")
            t.command("copy C:\\BIG.BIN D:\\BACK.BIN")
        finally:
            t.close()
    if s.mtype("BIG.BIN") != big:
        raise AssertionError("the file read from the host disk differs")
    back = subprocess.check_output(["mtype", "-i", "%s@@%d" % (image, PART), "::BACK.BIN"], env=ENV)
    if back != big:
        raise AssertionError("the file written to the host disk differs")


@test
def ctrl_alt_del_reboots(s):
    out = s.run("wait:30 C:\\>\ntype echo before\\r\nsleep 300\ncad\nsleep 500\n"
                "wait:30 C:\\>\ndump\n")
    expect(out, "FreeDOS kernel")
    if "before" in out:
        raise AssertionError("screen was not cleared by the reboot")


@test
def cpu_8086_mode_boots(s):
    out = s.run("wait:30 C:\\>\ndump\n", args=("--cpu", "8086", "-c", None))
    expect(out, "FreeDOS kernel", "Welcome to FreeDOS (on an emulated 8086).")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-k", default="")
    ap.add_argument("--shots", help="directory for screenshots (default: discarded)")
    a = ap.parse_args()
    for f in (P4DOS, HD, FLOPPY):
        if not os.path.exists(f):
            sys.exit("missing %s: run `make` and `make image` first" % f)
    if a.shots:
        os.makedirs(a.shots, exist_ok=True)

    failed = 0
    for fn in TESTS:
        if a.k not in fn.__name__:
            continue
        with tempfile.TemporaryDirectory() as tmp:
            try:
                fn(Session(tmp, a.shots))
                print("ok    " + fn.__name__)
            except (AssertionError, subprocess.SubprocessError) as e:
                failed += 1
                print("FAIL  " + fn.__name__)
                print("      " + str(e).replace("\n", "\n      "))
    print("%d failed" % failed if failed else "all passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
