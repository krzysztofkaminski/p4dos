<a href="../README.md"><img src="img/icon.svg" width="72" align="right" alt="P4DOS"></a>

# P4DOS installation guide

How to build P4DOS and put it on a Guition JC8012P4A1C-I-W-Y board. Using it afterwards is
described in the [user manual](user-manual.md); the [README](../README.md) explains how it
works.

1. [What you need](#what-you-need)
2. [Install the tools](#install-the-tools)
3. [Try it on the PC first](#try-it-on-the-pc-first)
4. [Find out the chip revision](#find-out-the-chip-revision)
5. [Back up the board](#back-up-the-board)
6. [Build the firmware](#build-the-firmware)
7. [Flash the board](#flash-the-board)
8. [First start](#first-start)
9. [Updating the firmware](#updating-the-firmware)
10. [Firmware options](#firmware-options)
11. [Troubleshooting](#troubleshooting)
12. [Back to the factory firmware](#back-to-the-factory-firmware)

## What you need

- **The board:** Guition JC8012P4A1C-I-W-Y (ESP32-P4, 10.1" 800x1280 panel). Other ESP32-P4
  boards are not supported: the panel, its initialisation and the pins are this board's.
  Its hardware is described in [guition.md](../guition.md).
- **A USB-C data cable** from the PC to the board's **upper** USB-C port. It powers the
  board, flashes it and carries the serial console.
- **A USB keyboard** for the **middle** USB-C port, with a USB-C (male) to USB-A (female)
  OTG adapter. A hub or a Logitech Unifying receiver works too. A USB mouse is optional.
- **Optional, a TF (microSD) card** formatted FAT16 or FAT32 with a partition table, as
  cards are sold (not exFAT). It becomes drive D:.
- **A Linux PC.** These steps were written on Debian. The build needs bash, GCC, Python 3,
  NASM, mtools and ESP-IDF 5.5.

## Install the tools

On Debian or Ubuntu:

```sh
sudo apt install git gcc make python3 python3-venv nasm mtools \
    wget flex bison gperf cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
sudo apt install libsdl2-dev        # optional: the SDL window of the PC build
sudo usermod -aG dialout "$USER"    # access to /dev/ttyACM0; log out and in again
```

ESP-IDF, Espressif's framework for the chip; the firmware is built with version 5.5.4:

```sh
git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32p4
export IDF_PATH=~/esp/esp-idf        # also put this line in ~/.bashrc
```

`port/guition/build.sh` loads the ESP-IDF environment by itself; it only needs `IDF_PATH`.
If the repository has its own copy at `toolchain/esp-idf-5.5.4` (tools in
`toolchain/espressif`), `build.sh` uses that one instead.

The esptool commands in this guide are written as `python -m esptool`. Run them in a shell
where ESP-IDF's environment is loaded (`. "$IDF_PATH/export.sh"`), or install esptool on
its own with `pip install esptool`.

## Try it on the PC first

The same emulator runs on Linux, without a board:

```sh
make                       # build/p4dos
make image                 # build/p4dos-hd.img, the FreeDOS disk (downloads FreeDOS 1.3 packages)
cp build/p4dos-hd.img my.img
build/p4dos -c my.img      # FreeDOS in this terminal; Ctrl-] quits
make sdl && build/p4dos-sdl --sdl -c my.img --mouse    # a 1280x800 window, as on the panel
```

`build/p4dos --help` lists the options (floppy and hard disk images, processor, memory).

## Find out the chip revision

ESP32-P4 chips come in two lines that need different builds: revision v1.x (the first
boards) and v3.x (later boards). A build for one line does not start on the other.

Connect the upper USB-C port to the PC and run:

```sh
python -m esptool --chip esp32p4 -p /dev/ttyACM0 flash_id
```

The line that names the chip shows the revision, for example `ESP32-P4 (revision v1.3)`.
Opening the port can reset the board; that does no harm.

| Revision | Build | Glass |
|---|---|---|
| below v3.0 (v1.3 seen) | `port/guition/build.sh` | old generation |
| v3.x (v3.2 seen) | `P4DOS_REV3=1 port/guition/build.sh` | new generation |

The two boards seen so far had these glass generations; if a board has the other one, see
[Troubleshooting](#troubleshooting).

## Back up the board

Save the whole flash before writing anything. It is the only way back to the factory
firmware:

```sh
python -m esptool --chip esp32p4 -p /dev/ttyACM0 -b 460800 read_flash 0 0x1000000 factory-backup.bin
```

This reads 16 MB and takes a few minutes. Keep the file.

## Build the firmware

```sh
port/guition/build.sh                 # chip revision below v3.0
P4DOS_REV3=1 port/guition/build.sh    # chip revision v3.x
```

The first run builds the FreeDOS disk image if it is missing (`make image`, which downloads
the FreeDOS 1.3 packages) and lets ESP-IDF fetch the components `espressif/usb_host_hid`
and `espressif/esp_codec_dev`. The result is `port/guition/build/p4dos.bin`
(`port/guition/build-rev3/` for v3.x), next to the bootloader and the partition table.

To put your own files on C: from the start, build the image with them before the
firmware: `python3 tools/mkimage.py --add path/to/GAMES` copies a file or a directory to
C:\ (a directory keeps its name, here C:\GAMES; the option can be repeated).

## Flash the board

Close any terminal that has the port open (picocom, screen, `tools/hostdisk.py`), then:

```sh
port/guition/build.sh -p /dev/ttyACM0 flash                 # chip revision below v3.0
P4DOS_REV3=1 port/guition/build.sh -p /dev/ttyACM0 flash    # chip revision v3.x
```

This writes the bootloader, the partition table, the firmware and the FreeDOS image of
drive C:. **Whatever is on C: is replaced.** To update a board that is already in use, see
[Updating the firmware](#updating-the-firmware).

## First start

The board restarts after flashing. On the panel:

1. the BIOS banner: version, processor, memory, the Sound Blaster compatible card, the
   hard disks;
2. FreeDOS starting: HimemX, expanded memory, `Welcome to FreeDOS (on an emulated 80386DX
   with 80387)`;
3. the prompt `C:\>`, about 4 seconds after power-on.

Plug the keyboard into the middle port at any time. To see the same screen on the PC:

```sh
picocom /dev/ttyACM0       # Ctrl-A Ctrl-X quits picocom
```

The [user manual](user-manual.md) takes it from here.

## Updating the firmware

To install a new firmware and keep drive C: as it is, write only the firmware:

```sh
python -m esptool --chip esp32p4 -p /dev/ttyACM0 write_flash 0x10000 port/guition/build/p4dos.bin
```

(`port/guition/build-rev3/p4dos.bin` for v3.x.) `build.sh ... flash` would write the C:
image as well.

Drive C: can be saved and restored as a whole:

```sh
python -m esptool --chip esp32p4 -p /dev/ttyACM0 read_flash 0x200000 0xE00000 c-backup.img
python -m esptool --chip esp32p4 -p /dev/ttyACM0 write_flash 0x200000 c-backup.img
```

The saved file is a hard disk image that mtools can read and change on the PC, for example
`mdir -i c-backup.img@@32256 ::` or `mcopy -i c-backup.img@@32256 GAME.EXE ::`.

## Firmware options

```sh
port/guition/build.sh menuconfig                 # P4DOS_REV3=1 in front for v3.x
```

The menu "P4DOS" has these options. After changing them, build and write the firmware
again.

| Option | Default | What it does |
|---|---|---|
| Turn the picture upside down | off | for a board used the other way up |
| Display: the "new panel" generation of the glass | off; on in the v3.x build | the panel's initialisation and timing |
| Processor | 80386DX | or 80186, or 8086, for the few programs that only work on those |
| 80387 numeric coprocessor | on | the floating-point coprocessor of the 80386 machine |
| Memory of the 80386 machine | 16 MB | 1 to 24 MB |
| Expanded memory (EMS 4.0) | 4096 KB (8192 KB on the 8086 and 80186) | 0 to 16384 KB, 0 turns EMS off |
| XMS memory of the 80186 or 8086 machine | 4096 KB | only for those two processors |
| Sound through the ES8311 codec | on | Sound Blaster compatible card and PC speaker |
| Codec output level at full volume | 85 | how loud "100 %" is |
| DOS console on the USB-Serial-JTAG port | on | the screen and keyboard on a terminal on the PC |
| Report the emulation speed on the log | off | for benchmarking |
| Use the TF card slot | on | |
| TF card contents | the card is drive D: | or "disk image files": floppy and hard disk images on the card |

## Troubleshooting

- **Nothing on the panel after flashing.** A firmware built for the other revision line
  does not start: the bootloader refuses it (`picocom /dev/ttyACM0` shows its log). Check
  the revision as above, build with or without `P4DOS_REV3=1`, and flash again.
- **A light screen with horizontal lines, or no usable picture.** The board has the other
  generation of the glass. Change "Display: the new panel generation of the glass" in
  menuconfig, build, and write the firmware again.
- **The picture is upside down.** Change "Turn the picture upside down" in menuconfig.
- **esptool: the port is busy, or permission denied.** Close every program that has the
  port open. For "permission denied", add yourself to the group `dialout` (see above) and
  log in again.
- **esptool cannot connect.** Put the chip into download mode by hand, the usual way for
  ESP32 chips: hold the BOOT button, press and release RESET, release BOOT, then flash
  again. Press RESET afterwards to start the firmware.
- **The board restarts when a terminal opens the port.** This can happen with the upper
  port and does no harm.
- **The keyboard does nothing.** It belongs on the middle port, through an OTG adapter;
  the upper port is for the PC. The firmware log on the upper port shows a line when a USB
  device is connected.

For problems when using P4DOS (drive D:, sound, programs), see the
[user manual](user-manual.md#troubleshooting).

## Back to the factory firmware

```sh
python -m esptool --chip esp32p4 -p /dev/ttyACM0 -b 460800 write_flash 0 factory-backup.bin
```
