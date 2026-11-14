# Build the FCB1010 replacement firmware with SDCC (MCS-51) and run host tests.
#   make           build ROM images in build/, and update the emulator page to match
#                  (skipped with a note if clang is not installed)
#   make test      compile and run the logic tests on this computer
#   make emulator  compile ctl.c and keys.c to WebAssembly, embed it in emulator/index.html and
#                  run the emulator smoke test
#   make check     every test suite (host, emulator, simulator) without bumping the build
#   make BUILD_BUMP=0  build without bumping the build number (for test runs)
#   make clean

SDCC     ?= sdcc
MAKEBIN  ?= makebin
HOSTCC   ?= gcc
WASMCC   ?= clang

OUT      := build
ROM_SIZE ?= 32768

# Small memory model: Everything lives in the 8032's 256 bytes of internal RAM.
SDCCFLAGS := -mmcs51 --model-small --std-sdcc99 --iram-size 256 --code-size $(ROM_SIZE) -Isrc

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

all: $(OUT)/fcb1010_32k.bin $(OUT)/fcb1010_64k_mirrored.bin $(EMU_PAGE)

.PHONY: emu-skipped
emu-skipped:
	@echo "note: $(WASMCC) not found, emulator/index.html not updated (see make emulator)"

$(OUT):
	mkdir -p $(OUT)

# main.rel must be linked first so the interrupt vector table lands at address 0.
$(OUT)/%.rel: src/%.c src/*.h | $(OUT)
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

# 32KB image padded with 0xFF, for a 27C256 / 28C256 sized chip.
# makebin only emits the used bytes, so pad explicitly.
$(OUT)/fcb1010_32k.bin: $(OUT)/fcb1010.ihx
	$(MAKEBIN) -p $< $(OUT)/fcb1010_raw.bin
	python3 -I tools/check_size.py $(OUT)/fcb1010_raw.bin $(ROM_SIZE) README.md
	python3 -I tools/readme_version.py src/version.h src/version_build.inc README.md
	head -c $(ROM_SIZE) /dev/zero | tr '\000' '\377' > $@
	dd if=$(OUT)/fcb1010_raw.bin of=$@ conv=notrunc 2>/dev/null

# The same image twice, for a 64KB chip (27C512 type) sitting in a socket
# where the top address line may be high or low.
$(OUT)/fcb1010_64k_mirrored.bin: $(OUT)/fcb1010_32k.bin
	cat $< $< > $@

test: $(OUT)/test_ctl $(OUT)/test_keys
	$(OUT)/test_ctl
	$(OUT)/test_keys

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
