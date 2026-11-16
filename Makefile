# Build the FCB1010 replacement firmware with SDCC (MCS-51) and run host tests.
#   make           build ROM images in build/, and update the emulator page to match
#                  (skipped with a note if clang is not installed)
#   make test      compile and run the logic tests on this computer
#   make emulator  compile ctl.c and keys.c to WebAssembly, embed it in emulator/index.html and
#                  run the emulator smoke test
#   make check     every test suite (host, emulator, simulator) without bumping the build
#   make BUILD_BUMP=0  build without bumping the build number (for test runs)
#   make probe     the SRAM probe test images (build/probe_27c*.bin, see probe/sram_probe.c)
#   make clean

SDCC     ?= sdcc
MAKEBIN  ?= makebin
HOSTCC   ?= gcc
WASMCC   ?= clang

OUT      := build
ROM_SIZE ?= 32768

# Small memory model: variables default to the 8032's 256 bytes of internal RAM; the
# queues and large tables are XDATA, in the board's 32KB SRAM at 0x8000-0xFFFF (IC14's
# chip select is A15 inverted; below 0x8000 writes reach the display / key latches).
XRAMFLAGS := --xram-loc 0x8000 --xram-size 0x8000
SDCCFLAGS := -mmcs51 --model-small --std-sdcc99 --iram-size 256 --code-size $(ROM_SIZE) $(XRAMFLAGS) -Isrc

OBJS := $(OUT)/main.rel $(OUT)/hw.rel $(OUT)/ctl.rel

.PHONY: all test emulator check clean FORCE

# BUILD_BUMP=0 (test runs) links without bumping the build number and leaves a flag, so
# the next normal make relinks and bumps even if nothing else changed: an image built
# for testing never keeps the previous build's number.
BUILD_BUMP ?= 1
UNBUMPED   := $(OUT)/unbumped.flag
ifeq ($(BUILD_BUMP),1)
RELINK := $(if $(wildcard $(UNBUMPED)),FORCE)
endif

# The emulator page embeds its own compiled copy of ctl.c and config.h, so a plain make
# updates it too (after the firmware, so it shows the same build number). Without clang
# it is skipped with a note.
ifneq ($(shell command -v $(WASMCC) 2>/dev/null),)
EMU_PAGE := $(OUT)/emulator.stamp
else
EMU_PAGE := emu-skipped
endif

# One ROM image per chip size (tools/rom_image.py): 27C128 (16KB), 27C256 (32KB) and
# 27C512 (64KB), each the code padded with 0xFF and repeated to fill the chip.
ROMS := $(OUT)/fcb1010_27c128.bin $(OUT)/fcb1010_27c256.bin $(OUT)/fcb1010_27c512.bin

all: $(ROMS) $(EMU_PAGE)

.PHONY: emu-skipped
emu-skipped:
	@echo "note: $(WASMCC) not found, emulator/index.html not updated (see make emulator)"

$(OUT):
	mkdir -p $(OUT)

# main.rel must be linked first so the interrupt vector table lands at address 0.
$(OUT)/%.rel: src/%.c src/*.h src/eeprom.inc | $(OUT)
	$(SDCC) $(SDCCFLAGS) -c $< -o $@

# Each link of the image is a new build: tools/bump_build.py adds one to the build
# number in src/version_build.inc, then version.c is compiled with it and linked last.
# No other rule depends on that file, so a make with nothing changed does not relink
# or bump it.
$(OUT)/fcb1010.ihx: $(OBJS) src/version.c src/version.h src/platform.h $(RELINK)
ifeq ($(BUILD_BUMP),1)
	python3 -I tools/bump_build.py src/version_build.inc
	rm -f $(UNBUMPED)
else
	@echo "test build: build number not bumped"
	touch $(UNBUMPED)
endif
	$(SDCC) $(SDCCFLAGS) -c src/version.c -o $(OUT)/version.rel
	$(SDCC) $(SDCCFLAGS) $(OBJS) $(OUT)/version.rel -o $@

# The linked code as a binary (makebin only emits the used bytes), then the size check
# and the documentation hooks.
$(OUT)/fcb1010_raw.bin: $(OUT)/fcb1010.ihx
	$(MAKEBIN) -p $< $@
	python3 -I tools/check_size.py $@ $(ROM_SIZE) docs/building.md
	python3 -I tools/readme_version.py src/version.h src/version_build.inc docs/configuration.md
	python3 -I tools/readme_config.py src/config.h README.md docs/*.md

$(OUT)/fcb1010_27c128.bin: $(OUT)/fcb1010_raw.bin tools/rom_image.py
	python3 -I tools/rom_image.py $< $(ROM_SIZE) 16384 $@

$(OUT)/fcb1010_27c256.bin: $(OUT)/fcb1010_raw.bin tools/rom_image.py
	python3 -I tools/rom_image.py $< $(ROM_SIZE) 32768 $@

$(OUT)/fcb1010_27c512.bin: $(OUT)/fcb1010_raw.bin tools/rom_image.py
	python3 -I tools/rom_image.py $< $(ROM_SIZE) 65536 $@

# SRAM probe (probe/sram_probe.c): a separate test image that looks for RAM in the external
# data space and shows what it finds. It links hw.c only; no build number, no documentation hooks.
PROBE_ROMS := $(OUT)/probe_27c128.bin $(OUT)/probe_27c256.bin $(OUT)/probe_27c512.bin

.PHONY: probe
probe: $(PROBE_ROMS)

$(OUT)/sram_probe.rel: probe/sram_probe.c src/*.h | $(OUT)
	$(SDCC) $(SDCCFLAGS) -c $< -o $@

# The probe overwrites the SRAM it tests, so its copy of hw.c keeps everything in internal
# RAM (NO_XDATA) with the old small queues.
PROBE_HW_FLAGS := -DNO_XDATA -DMIDI_TX_QUEUE=16 -DMIDI_RX_QUEUE=8 -DKEY_EVENT_QUEUE=8

$(OUT)/hw_probe.rel: src/hw.c src/*.h src/eeprom.inc | $(OUT)
	$(SDCC) $(SDCCFLAGS) $(PROBE_HW_FLAGS) -c $< -o $@

# sram_probe.rel first so the interrupt vector table lands at address 0.
$(OUT)/probe.ihx: $(OUT)/sram_probe.rel $(OUT)/hw_probe.rel
	$(SDCC) $(SDCCFLAGS) $^ -o $@

$(OUT)/probe_raw.bin: $(OUT)/probe.ihx
	$(MAKEBIN) -p $< $@

$(OUT)/probe_27c128.bin: $(OUT)/probe_raw.bin tools/rom_image.py
	python3 -I tools/rom_image.py $< $(ROM_SIZE) 16384 $@

$(OUT)/probe_27c256.bin: $(OUT)/probe_raw.bin tools/rom_image.py
	python3 -I tools/rom_image.py $< $(ROM_SIZE) 32768 $@

$(OUT)/probe_27c512.bin: $(OUT)/probe_raw.bin tools/rom_image.py
	python3 -I tools/rom_image.py $< $(ROM_SIZE) 65536 $@

test: $(OUT)/test_ctl $(OUT)/test_keys $(OUT)/test_probe $(OUT)/test_eeprom
	$(OUT)/test_ctl
	$(OUT)/test_keys
	$(OUT)/test_probe
	$(OUT)/test_eeprom

$(OUT)/test_probe: test/test_probe.c probe/sram_probe.c | $(OUT)
	$(HOSTCC) -std=c99 -Wall -Wextra -Wno-unused-function test/test_probe.c -o $@

$(OUT)/test_eeprom: test/test_eeprom.c src/eeprom.inc src/*.h | $(OUT)
	$(HOSTCC) -std=c99 -Wall -Wextra -Isrc test/test_eeprom.c -o $@

$(OUT)/test_ctl: test/test_ctl.c src/ctl.c src/version.c src/version_build.inc src/*.h | $(OUT)
	$(HOSTCC) -std=c99 -Wall -Wextra -Isrc test/test_ctl.c -o $@

$(OUT)/test_keys: test/test_keys.c src/keys.c src/*.h | $(OUT)
	$(HOSTCC) -std=c99 -Wall -Wextra -Isrc test/test_keys.c -o $@

# The browser emulator runs the firmware's own ctl.c and keys.c, compiled to WebAssembly with
# emulator/emu.c standing in for hw.c and main.c. Needs clang with the wasm32 target and lld.
WASMFLAGS := --target=wasm32 -O2 -std=c99 -Wall -Wextra -ffreestanding -nostdlib -DCTL_INSPECT -Isrc \
             -Wl,--no-entry -Wl,--strip-all

$(OUT)/fcb1010_emu.wasm: emulator/emu.c src/ctl.c src/keys.c src/version.c src/version_build.inc src/*.h | $(OUT)
	$(WASMCC) $(WASMFLAGS) emulator/emu.c src/ctl.c src/keys.c src/version.c -o $@

# Embed the WebAssembly in the page whenever it was rebuilt (the stamp records when).
$(OUT)/emulator.stamp: $(OUT)/fcb1010_emu.wasm
	python3 -I emulator/embed_wasm.py $< emulator/index.html
	touch $@

emulator: $(OUT)/emulator.stamp
	node test/emu_smoke.mjs emulator/index.html

# All test suites, building without bumping the build number. The simulator scenarios
# need s51 (sdcc-ucsim) on PATH; without it they are skipped with a note.
check:
	$(MAKE) BUILD_BUMP=0 all
	$(MAKE) test
	$(MAKE) BUILD_BUMP=0 emulator
	@if command -v s51 >/dev/null 2>&1; then test/sim_smoke.sh; \
	 else echo "note: s51 not found, simulator scenarios skipped (install sdcc-ucsim)"; fi

clean:
	rm -rf $(OUT)
