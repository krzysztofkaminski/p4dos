<a href="../README.md"><img src="img/icon.svg" width="72" align="right" alt="P4DOS"></a>

# P4DOS user manual

P4DOS makes the Guition JC8012P4A1C board an IBM-compatible PC with an 80386DX processor.
The PC runs FreeDOS 1.3. The panel is the monitor, a USB keyboard is the keyboard, and a TF
card is a second hard disk. This manual is about using it. Installing it is described in
the [installation guide](installation.md); the [README](../README.md) explains how it
works.

1. [The machine](#the-machine)
2. [Connections](#connections)
3. [Starting and switching off](#starting-and-switching-off)
4. [Keyboard and mouse](#keyboard-and-mouse)
5. [Drives](#drives)
6. [The TF card](#the-tf-card)
7. [Moving files between the PC and the board](#moving-files-between-the-pc-and-the-board)
8. [The serial console](#the-serial-console)
9. [Memory](#memory)
10. [Sound](#sound)
11. [Display](#display)
12. [Programs](#programs)
13. [Configuration files](#configuration-files)
14. [Limits](#limits)
15. [Troubleshooting](#troubleshooting)

## The machine

| | |
|---|---|
| Processor | 80386DX with 80387 coprocessor. An 80186 or 8086 can be chosen when building the firmware |
| Speed | about 7 to 9 million instructions per second for 16-bit programs (about a 386DX-33), 4 to 6 for 32-bit games |
| Memory | 16 MB: 640 KB conventional (618 KB free at the prompt), 15 MB extended (XMS), and 4 MB of expanded memory (EMS 4.0) |
| Video | VGA: text modes, CGA, EGA and VGA graphics modes, "Mode X" |
| Sound | Sound Blaster 2.0 compatible (port 220h, IRQ 7, DMA 1) and PC speaker |
| Drives | C: 14 MB in the board's flash; D: the TF card |
| Input | USB keyboard; USB mouse as a serial mouse on COM1 |
| Clock | the board's RTC chip when it has a backup cell |
| Software | FreeDOS 1.3 with HIMEMX, EMS, CTMOUSE, CWSDPMI, DOS/32A and JEMM386 |

## Connections

The board has three USB-C ports. Any of them powers it.

| Port | Use |
|---|---|
| Upper | to the PC: power, flashing, the serial console, files over USB |
| Middle | keyboard and mouse, through a USB-C (male) to USB-A (female) OTG adapter; a hub or a Logitech Unifying receiver works too |
| Lower | not used |

## Starting and switching off

Power the board. The panel shows the BIOS banner, then FreeDOS starts and greets you with
the name of the emulated processor. The prompt `C:\>` appears about 4 seconds after
power-on. Type `?` for a list of FreeCOM's built-in commands; the other commands are in
`C:\FDOS\BIN` (see [Programs](#programs)).

- **Restart:** Ctrl+Alt+Del. This works also when a program has hung.
- **Switch off:** wait a second after the last write to a disk, then remove power. Writes
  to C: are kept in a cache and go to flash when DOS is idle, at least every half second.
  Do not switch off while a program is writing.

## Keyboard and mouse

Keyboards and mice can be plugged in and out at any time. The keyboard LEDs follow Num
Lock, Caps Lock and Scroll Lock. The layout is US English; FreeDOS's `KEYB` is in
`C:\FDOS\BIN` for others (not tried with P4DOS).

Keys handled by P4DOS itself, which DOS never sees:

| Keys | What they do |
|---|---|
| Ctrl+Alt+Up | louder, in ten steps; each step beeps |
| Ctrl+Alt+Down | quieter |

The volume is remembered across power cycles and starts at 60 %.

A USB mouse is presented to DOS as a Microsoft serial mouse on COM1. `CTMOUSE`, loaded at
start-up, makes it available to programs (INT 33h). The serial mouse is always there, so
a mouse plugged in later works without reloading the driver. The mouse has worked in the
PC build; on the board it has not been tried yet.

## Drives

| Drive | What it is |
|---|---|
| C: | 14 MB in the board's flash. FreeDOS is installed here |
| D: | the TF card, when one is inserted |
| next letter | the host disk, while `tools/hostdisk.py` serves one from the PC (E: with a TF card, D: without) |
| A:, B: | only in the firmware's "disk image files" card mode (see below) |

C: survives power-off and firmware updates that write only the firmware. Writing the
firmware with `build.sh ... flash` replaces C: with a fresh FreeDOS installation (see the
[installation guide](installation.md#updating-the-firmware)).

## The TF card

An ordinary card, FAT16 or FAT32 with a partition table as cards are sold, is drive D:.
DOS uses it directly, so files written on a PC can be read on the board and the other way
round. exFAT cards do not work: format them as FAT32.

- **Insert the card before power-on**, or press Ctrl+Alt+Del after inserting or changing
  it: DOS looks for drives only when it starts.
- **Do not pull the card while DOS is writing to it.**
- Long file names appear in their short form, for example `LONGFI~1.TXT`.
- On some boards the card writes only on a slower 1-bit connection. P4DOS switches to it
  by itself at the first failed write, and the card then works at about a quarter of the
  speed. If a card refuses writes even then, D: becomes read-only and DOS reports "write
  protected". Switching the board off and on, or another card, starts afresh.

**Disk image files** (a firmware option, see the installation guide): instead of being D:
itself, the card holds image files in a directory `P4DOS`: `A.IMG` and `B.IMG` are
floppy drives A: and B: (a bootable floppy in A: starts before C:), and `HD.IMG` is the
hard disk D:. `build/p4dos-floppy.img` from the build is a rescue floppy with FDISK,
FORMAT and SYS.

## Moving files between the PC and the board

**On the TF card.** Copy files to the card on the PC, insert it into the board, restart
DOS (Ctrl+Alt+Del), and copy from D: with `COPY` or `XCOPY`.

**Over the USB cable,** with the board connected to the PC on the upper port and nothing
else using the port. `tools/hostdisk.py` (needs pyserial and mtools on the PC) serves a
disk to DOS through the serial console:

```sh
python3 tools/hostdisk.py ~/dosfiles          # a directory, read-only
python3 tools/hostdisk.py --ro disk.img       # a hard disk image, read-only
python3 tools/hostdisk.py disk.img            # a hard disk image, writable
```

DOS restarts and has the disk as one more drive (E: with a TF card, D: without). The
terminal shows the DOS screen and types into it; Ctrl-] ends. A directory is packed into a
temporary image and served read-only. An image must have a partition table, like
`build/p4dos-hd.img`; files DOS writes to it are in the image afterwards and can be taken
out with mtools: `mcopy -i disk.img@@32256 ::FILE.TXT .`

**Into the C: image.** C: can be read out of the board, changed with mtools on the PC and
written back; the commands are in the installation guide under
[Updating the firmware](installation.md#updating-the-firmware). Before the first
installation, `tools/mkimage.py --add` puts files onto C: from the start.

## The serial console

The text screen is also sent to the PC on the upper USB-C port, and keys typed there reach
DOS as if they came from the keyboard. The machine can be used without the panel or a USB
keyboard this way.

```sh
picocom /dev/ttyACM0          # or: screen /dev/ttyACM0
```

On Windows the port appears as a COM port; the baud rate setting does not matter. Use a
terminal that understands ANSI escape sequences.

- Cursor keys, function keys, Home, End, Page Up/Down, Insert and Delete work as on the
  keyboard.
- Alt+key: press Escape, then the key (or use a terminal that sends Alt that way).
- Ctrl-L repaints the screen.
- In graphics modes the terminal shows a line naming the mode instead of the picture.
- Only one program can use the port at a time. Close the terminal before flashing or
  starting `tools/hostdisk.py`.
- Opening the port can restart the board.

The port also carries the firmware's log: warnings and errors, and a line when a USB
device is connected or removed or the volume changes.

## Memory

| Kind | Size | Provided by |
|---|---|---|
| Conventional | 640 KB, 618 KB free at the prompt (DOS is loaded high) | |
| Extended (XMS) | 15,360 KB | `HIMEMX.EXE` |
| Expanded (EMS 4.0) | 4096 KB, page frame at E000h | `EMM.SYS` |

**DPMI programs** (DJGPP and similar) find `CWSDPMI.EXE` in `C:\FDOS\BIN` and start it
themselves; `CWSDPMI -p` keeps it loaded. **DOS/4GW-style programs** can be started with
`DOS32A PROGRAM.EXE`.

**Programs that want EMM386** can have `JEMM386.EXE` instead of `EMM.SYS`. It runs DOS in
virtual-8086 mode, which is slower. In `C:\FDCONFIG.SYS`, replace the line
`DEVICE=C:\FDOS\BIN\EMM.SYS` by `DEVICE=C:\FDOS\BIN\JEMM386.EXE` (never load both), then
restart.

## Sound

The sound card is compatible with a Sound Blaster 2.0. The environment variable
`BLASTER=A220 I7 D1 T3` is set at start-up. In a program's sound setup, choose "Sound
Blaster" with port 220h, IRQ 7 and DMA channel 1.

- 8-bit digital sound plays (speech, effects, digitised music). FM music (AdLib, OPL2) is
  not emulated, so music written for it stays silent.
- The PC speaker plays as well.
- Volume: Ctrl+Alt+Up and Ctrl+Alt+Down (see [Keyboard and mouse](#keyboard-and-mouse)).

Sound goes to the board's speaker connector. It plays correctly in the PC build, but sound
from a speaker on a real board has not been confirmed yet.

## Display

The 10.1" panel is used as a 1280x800 landscape screen. Text and 200-line modes are
enlarged by whole numbers, so pixels stay sharp; 480-line modes use the nearest line:

| Mode | Picture | Enlarged | On the panel |
|---|---|---|---|
| 80x25 text | 640x400 | 2 x 2 | full screen |
| 40x25 text | 320x400 | 4 x 2 | full screen |
| 320x200 (CGA, mode 13h) | 320x200 | 4 x 4 | full screen |
| 640x200 | 640x200 | 2 x 4 | full screen |
| 640x480 (modes 11h, 12h) | 640x480 | 2 x 5/3 | full screen |
| 640x350 (mode 10h) | 640x350 | 2 x 2 | 1280x700, centred |
| 320x240 ("Mode X") | 320x240 | 3 x 3 | 960x720, centred |

43- and 50-line text modes and loadable text fonts are not available.

## Programs

FreeDOS's commands on C: (in `C:\FDOS\BIN`): ATTRIB, CHKDSK, CHOICE, COMP, CTMOUSE, DEBUG,
DELTREE, DEVLOAD, DISKCOPY, EDIT, EDLIN, FC, FDISK, FIND, FORMAT, KEYB, LABEL, MEM, MODE,
MORE, MOVE, REPLACE, SHARE, SORT, SYS, TREE, XCOPY; the ANSI driver NANSI.SYS; the
memory managers and DOS extenders named above; and from P4DOS:

- `SPEED`: runs four instruction mixes for a second each and prints how many thousand
  instructions per second the machine manages.
- `WELCOME`: the greeting, naming the emulated processor.

**Games.** Copy the game to C: or the TF card and run its setup program for the sound
card. 16-bit games of the 386 era run at about the speed of a 386DX-33. 32-bit games that
need a 486 or a Pentium are too slow or do not start (no 486 instructions).

**Windows 3.1x** installs with its own Setup from a directory holding the files of its
installation disks, and runs in standard mode (`WIN`). Two things do not work: 386
enhanced mode (`WIN /3`), and starting a DOS program from inside Windows, which crashes;
leave Windows first.

## Configuration files

`C:\FDCONFIG.SYS` (FreeDOS's CONFIG.SYS):

| Line | Meaning |
|---|---|
| `IDLEHALT=1` | DOS halts the processor while it waits, which lets the board rest at the prompt |
| `DOS=HIGH` | DOS in the high memory area, for more free conventional memory |
| `DEVICE=C:\FDOS\BIN\HIMEMX.EXE` | extended memory (XMS) |
| `DEVICE=C:\FDOS\BIN\XMS.SYS` | XMS on the 8086 and 80186 machines; stays out of the way when HIMEMX is loaded |
| `DEVICE=C:\FDOS\BIN\EMM.SYS` | expanded memory (EMS) |
| `SHELL=C:\COMMAND.COM ...` | the command interpreter |

`C:\AUTOEXEC.BAT` sets the search path, the `BLASTER` variable and the prompt, loads
`CTMOUSE` and runs `WELCOME`.

Edit them with `EDIT C:\FDCONFIG.SYS` and restart with Ctrl+Alt+Del. If the machine no
longer starts after a change, press F5 when the line `FreeDOS kernel` appears: FreeDOS then
skips both files, and they can be repaired with `EDIT`.

## Limits

Not emulated: 486 and later instructions, FM music (AdLib), compressed (ADPCM) Sound
Blaster playback, 43/50-line text and loadable text fonts, the debug registers'
breakpoints, and the floppy and hard disk controller hardware (disks are reached through
the BIOS, which nearly all DOS programs use).

There is no network, and the board's touch screen, microphone and camera are not used.

## Troubleshooting

- **A program hangs.** Press Ctrl+Alt+Del.
- **D: is missing.** The card was inserted after DOS started: press Ctrl+Alt+Del. If it is
  still missing, check that the card is FAT16 or FAT32 (not exFAT) with a partition table.
- **D: reports "write protected".** The card refused writes in every mode. Switch the
  board off and on; if it happens again, try another card.
- **No keyboard.** The keyboard belongs on the middle port, through an OTG adapter.
- **A game has no sound or hangs at its sound test.** Set it to Sound Blaster (not Pro or
  16) at port 220h, IRQ 7, DMA 1. Music for AdLib is not emulated.
- **A program complains about too little memory.** `MEM` shows what is free. Programs
  that need EMS find it through `EMM.SYS`; programs that need XMS or DPMI through HIMEMX
  and CWSDPMI.
- **The board does not start or shows no picture after an update.** See the installation
  guide's [troubleshooting](installation.md#troubleshooting).
