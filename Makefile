# Build the FCB1010 replacement firmware with SDCC (MCS-51) and run host tests.
#   make           build ROM images in build/
#   make test      compile and run the logic tests on this computer
#   make emulator  compile ctl.c and keys.c to WebAssembly and embed it in emulator/index.html
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

.PHONY: all test emulator clean

all: $(OUT)/fcb1010_32k.bin $(OUT)/fcb1010_64k_mirrored.bin

$(OUT):
	mkdir -p $(OUT)

# main.rel must be linked first so the interrupt vector table lands at address 0.
$(OUT)/%.rel: src/%.c src/*.h | $(OUT)
	$(SDCC) $(SDCCFLAGS) -c $< -o $@

$(OUT)/fcb1010.ihx: $(OBJS)
	$(SDCC) $(SDCCFLAGS) $(OBJS) -o $@

# 32KB image padded with 0xFF, for a 27C256 / 28C256 sized chip.
# makebin only emits the used bytes, so pad explicitly.
$(OUT)/fcb1010_32k.bin: $(OUT)/fcb1010.ihx
	$(MAKEBIN) -p $< $(OUT)/fcb1010_raw.bin
	head -c $(ROM_SIZE) /dev/zero | tr '\000' '\377' > $@
	dd if=$(OUT)/fcb1010_raw.bin of=$@ conv=notrunc 2>/dev/null

# The same image twice, for a 64KB chip (27C512 type) sitting in a socket
# where the top address line may be high or low.
$(OUT)/fcb1010_64k_mirrored.bin: $(OUT)/fcb1010_32k.bin
	cat $< $< > $@

test: $(OUT)/test_ctl $(OUT)/test_keys
	$(OUT)/test_ctl
	$(OUT)/test_keys

$(OUT)/test_ctl: test/test_ctl.c src/ctl.c src/*.h | $(OUT)
	$(HOSTCC) -std=c99 -Wall -Wextra -Isrc test/test_ctl.c -o $@

$(OUT)/test_keys: test/test_keys.c src/keys.c src/*.h | $(OUT)
	$(HOSTCC) -std=c99 -Wall -Wextra -Isrc test/test_keys.c -o $@

# The browser emulator runs the firmware's own ctl.c and keys.c, compiled to WebAssembly with
# emulator/emu.c standing in for hw.c and main.c. Needs clang with the wasm32 target and lld.
WASMFLAGS := --target=wasm32 -O2 -std=c99 -Wall -Wextra -ffreestanding -nostdlib -DCTL_INSPECT -Isrc \
             -Wl,--no-entry -Wl,--strip-all

$(OUT)/fcb1010_emu.wasm: emulator/emu.c src/ctl.c src/keys.c src/*.h | $(OUT)
	$(WASMCC) $(WASMFLAGS) emulator/emu.c src/ctl.c src/keys.c -o $@

emulator: $(OUT)/fcb1010_emu.wasm
	python3 -I emulator/embed_wasm.py $< emulator/index.html
	node test/emu_smoke.mjs emulator/index.html

clean:
	rm -rf $(OUT)
