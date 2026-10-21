# P4DOS -- host tools, tests and images. The board firmware itself is built with
# ESP-IDF (see port/guition/build.sh).

CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra
# For the interpreter on RISC-V: a tuning under which GCC loads and stores unaligned words
# in one instruction, and the layout of core/cpu.c kept in source order (see its run loop).
CPU_TUNE := -mtune=thead-c906 -fno-reorder-blocks
BUILD   := build
CORE    := core/cpu.c core/pc.c core/bios.c core/biosvid.c core/disk.c core/hostdisk.c core/kbd.c \
           core/uart.c core/ems.c core/xms.c core/sb.c core/audio.c core/video.c core/render.c core/term.c \
           core/rom.c core/vgapal.c
HOSTSRC := port/host/main.c
SST     ?= $(BUILD)/sst/v2
SST386  ?= $(BUILD)/sst386

all: $(BUILD)/p4dos

CORE_DEPS := $(wildcard core/*.h)

$(BUILD)/p4dos: $(CORE) $(HOSTSRC) port/host/nosdl.c $(wildcard core/*.h port/host/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Icore -Iport/host -o $@ $(CORE) $(HOSTSRC) port/host/nosdl.c -lm

sdl: $(BUILD)/p4dos-sdl
$(BUILD)/p4dos-sdl: $(CORE) $(HOSTSRC) port/host/sdl.c $(wildcard core/*.h port/host/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Icore -Iport/host $$(sdl2-config --cflags) -o $@ \
	    $(CORE) $(HOSTSRC) port/host/sdl.c $$(sdl2-config --libs) -lm

# The BIOS ROM is checked in as core/rom.c; rebuild it after editing bios/bios.asm.
rom:
	@mkdir -p $(BUILD)
	cd bios && nasm -f bin -o ../$(BUILD)/bios.bin -l ../$(BUILD)/bios.lst bios.asm
	python3 tools/bin2c.py $(BUILD)/bios.bin core/rom.c p4dos_rom 65536

fonts:
	python3 tools/mkfont.py /usr/share/seabios/vgabios-stdvga.bin bios
	python3 tools/bin2c.py bios/vgapal.bin core/vgapal.c p4dos_vgapal 768

# FreeDOS disk images (downloads the FreeDOS 1.3 packages on first use).
image:
	python3 tools/mkimage.py

# CPU conformance against SingleStepTests/8088 (about 730 MB, downloaded on first use).
# CPUTEST_ARGS="-n 500" runs a sample of each opcode.
cputest:
	@test -f $(SST)/metadata.json || python3 tests/fetch_sst.py $(dir $(SST:/=))
	python3 tests/cputest.py $(SST) $(CPUTEST_ARGS)

# The 80386 against SingleStepTests/80386, real mode (about 600 MB), and against
# test386.asm, which covers protected mode, paging, virtual-8086 mode and task switches.
cputest386:
	@test -f $(SST386)/80386.csv || python3 tests/fetch_sst386.py $(SST386)
	python3 tests/cputest386.py $(SST386) $(CPUTEST_ARGS)

test386:
	python3 tests/test386.py
	python3 tests/test386.py --undef

# The 80387's arithmetic against the x87 unit of this machine (x86 hosts only): every
# operation in every rounding mode and precision, results and exception flags bit for bit.
# Then the coprocessor's fast path against that arithmetic: every escape opcode from random
# states, run as an instruction and by the general code.
fputest:
	@mkdir -p $(BUILD)
	$(CC) -O1 -Wall -Wno-unused-function -o $(BUILD)/fx80_test tests/fx80_test.c
	$(BUILD)/fx80_test 2000000
	$(CC) -O2 -Wall -Wno-unused-function -Icore -o $(BUILD)/fpu_fast_test tests/fpu_fast_test.c -lm
	$(BUILD)/fpu_fast_test 20000000

# FreeDOS on the RISC-V build inside qemu-system-riscv32.
test-rv32: $(BUILD)/rv32/p4dos.elf
	python3 tests/rv32_boot.py --qemu $(QEMU_RV32)

test: $(BUILD)/p4dos
	python3 tests/run.py

clean:
	rm -rf $(BUILD)/p4dos $(BUILD)/p4dos-sdl $(BUILD)/rv32

.PHONY: all sdl rom fonts image cputest cputest386 test386 fputest test test-rv32 clean

# --- bare-metal RV32 build for qemu-system-riscv32 -M virt ----------------------------
# Uses the Espressif RISC-V GCC (the compiler that builds the board firmware): the project's
# own in toolchain/ if there, else one in ~/.espressif; failing both, any riscv32/riscv64
# bare-metal GCC with rv32imac newlib multilibs.
RV32_CC ?= $(lastword riscv64-unknown-elf-gcc \
              $(sort $(wildcard $(HOME)/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-gcc)) \
              $(sort $(wildcard $(CURDIR)/toolchain/espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-gcc)))
RV32_FLAGS := -march=rv32imac_zicsr_zifencei -mabi=ilp32 -mno-strict-align -O2 -g -Wall -Wextra -ffunction-sections \
              -fdata-sections -Icore $(CPU_TUNE)
QEMU_RV32 ?= qemu-system-riscv32

rv32: $(BUILD)/rv32/p4dos.elf
$(BUILD)/rv32/p4dos.elf: $(CORE) port/rv32virt/main.c port/rv32virt/start.S port/rv32virt/link.ld $(wildcard core/*.h)
	@mkdir -p $(BUILD)/rv32
	$(RV32_CC) $(RV32_FLAGS) -nostartfiles -Wl,--gc-sections -Wl,--no-warn-rwx-segments -T port/rv32virt/link.ld \
	    -o $@ port/rv32virt/start.S port/rv32virt/main.c $(CORE) -lm

run-rv32: $(BUILD)/rv32/p4dos.elf
	$(QEMU_RV32) -M virt -m 256M -bios none -nographic -kernel $< \
	    -device loader,file=$(BUILD)/p4dos-hd.img,addr=0x84000000

.PHONY: rv32 run-rv32

# CPU core benchmark on RV32: RISC-V instructions per x86 instruction (exact, via minstret).
bench-rv32: $(BUILD)/rv32/bench.elf
	@timeout 300 $(QEMU_RV32) -M virt -m 64M -bios none -nographic -monitor none \
	    -icount shift=0 -kernel $<
$(BUILD)/rv32/bench.elf: core/cpu.c $(CORE_DEPS) tests/rv32_bench.c port/rv32virt/start.S port/rv32virt/link.ld
	@mkdir -p $(BUILD)/rv32
	$(RV32_CC) $(RV32_FLAGS) -nostartfiles -Wl,--gc-sections -Wl,--no-warn-rwx-segments \
	    -T port/rv32virt/link.ld -o $@ port/rv32virt/start.S tests/rv32_bench.c core/cpu.c -lm

.PHONY: bench-rv32
