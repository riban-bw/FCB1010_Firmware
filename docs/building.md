# Building

[Back to the README](../README.md)

How to build the firmware and tests, and the options that can be changed when building.

## Building and testing

Requires SDCC 4.x, `make` (and `gcc` for the host tests) (`apt install sdcc make gcc`).

```
make              # the ROM images in build/ (see below) and the editor page
make test         # the logic checks on this computer (controller, keys, SRAM probe)
make check        # every test suite, without bumping the build number
make editor     # update editor/index.html and run its checks (needs clang and Node)
test/sim_smoke.sh # runs the real image in the uCsim 8051 simulator (apt install sdcc-ucsim)
```

There is one ROM image per chip size, made by `tools/rom_image.py`: `build/fcb1010_27c128.bin` (16KB), `build/fcb1010_27c256.bin` (32KB, also for a 28C256) and `build/fcb1010_27c512.bin` (64KB, the 32KB image twice). Each is the code padded with 0xFF and repeated to fill the chip; the build fails if the code outgrows a chip. The code is linked for addresses below 32KB only, since how the FCB1010 wires the top address line of a 27C512 socket is not known.

If clang (with the wasm32 target) is installed, `make` also builds the browser editor in `editor/index.html`, so changes to `src/config.h` show up there too; otherwise it prints a note and leaves the page as it was.

The code is about 14.9KB (15229 bytes at the last build), so it needs a 16KB or larger chip. Each build checks the size against the 32KB ROM (the build fails if it does not fit).

Each build also updates the documentation: the code size above, the firmware version on configuration page 7 (in [Configuration](configuration.md)), and every value taken from `src/config.h` (the option table below and the numbers quoted in the README and these pages). A GitHub Actions workflow (`.github/workflows/build.yml`) builds and tests every push and offers the firmware images as a download; it fails if the documentation was not updated after a change to `src/config.h`.

## Firmware version

The version shows on configuration page 7 as major, minor and build. Major and minor are set by hand in `src/version.h`, in decimal (0 to 99), and shown in decimal; the build number is shown in hex. The build number in `src/version_build.inc` goes up by one each time `make` rebuilds the firmware image; test builds (`make check`, or `make BUILD_BUMP=0`) leave it alone.

## Compile-time configuration

These options are in `src/config.h`. Change them and rebuild. The current values are filled in by the build.

| Option | Current value | Meaning |
|---|---|---|
| `MIDI_CHANNEL` | 1 | MIDI channel (1 to 16) used until a channel is saved in configuration mode |
| `STOMP_CC_LIST` | 21, 22, 23, 24, 25, 26, 27, 28, 29, 30 | Controller number of each footswitch, 1 to 10 |
| `PEDAL_CC_LIST` | 7, 11 | Controller numbers of expression pedals A and B until changed on configuration page 2 (each bank has its own) |
| `PEDAL_PICKUP_A` | 0 | Expression pedal A's response until changed on configuration page 2 (each bank has its own): 0 immediate, 1 pickup |
| `PEDAL_PICKUP_B` | 0 | The same for pedal B |
| `PEDAL_PICKUP_WINDOW` | 2 | A pickup pedal picks up within this many CC steps of its controller's value (or on passing it) |
| `PEDAL_PICKUP_FLASH_MS` | 40 | Half-period (ms) of a waiting pedal's EXP LED flash at the pickup point; each CC step away adds 3ms. 2 to 128 |
| `PEDAL_PICKUP_SHOW_MS` | 2000 | Time (ms) the display shows a pickup pedal's distance after it stops moving |
| `DEFAULT_PEDAL_MODE` | 0 | Mode of every footswitch until one is saved: 0 stomp, 1 stomp with momentary, 2 momentary, 3 inverted momentary |
| `MOMENTARY_HOLD_MS` | 600 | Long press time (ms): stomp with momentary reverts after it, and the configuration save / cancel hold |
| `CONFIG_HOLD_MS` | 1000 | UP + DOWN hold (ms) that opens configuration mode |
| `PROGRAM_WRAP` | 0 | 1: program change wraps between 127 and 0 (and the bank between the last and 1); 0: both stop at the ends |
| `BANK_COUNT` | 30 | Number of footswitch banks, 1 to 30 (each takes 55 bytes of the EEPROM, eleven 5-byte records; 36 would fit below the shared records, and the room of banks 31 to 36 is kept spare for later settings) |
| `NOTE_BANK_1` | 3 | A bank (1 to `BANK_COUNT`, 0 for none) whose footswitches default to momentary notes until it is saved, footswitch 1 playing the first note and the others `NOTE_BANK_SCALE` above it |
| `NOTE_BANK_1_FIRST` | 33 | The first note of that bank (33 is A1, with middle C, C4, at 60) |
| `NOTE_BANK_2` | 4 | A second such bank |
| `NOTE_BANK_2_FIRST` | 57 | Its first note |
| `NOTE_BANK_SCALE` | 0, 3, 5, 7, 10, 12, 15, 17, 19, 22 | The note banks' notes above their first, in semitones, footswitch 1 to 10: a minor pentatonic scale, the top row an octave above the bottom row. The first note plus each must be at most 127 |
| `DISPLAY_PROGRAM_OFFSET` | 0 | Added to the displayed program number: 0 shows 0 to 127, 1 shows 1 to 128 |
| `PROGRAM_REPEAT_DELAY_MS` | 400 | UP / DOWN hold (ms) before auto-repeat starts |
| `PROGRAM_REPEAT_HZ` | 4 | Auto-repeat rate (steps per second) |
| `PROGRAM_REPEAT_FAST_AFTER_MS` | 2700 | Hold time (ms) after which the repeat speeds up |
| `PROGRAM_REPEAT_FAST_HZ` | 25 | Fast auto-repeat rate (steps per second) |
| `PEDAL_HYST` | 2 | ADC counts a pedal must move before a new value is sent (noise filter) |
| `PEDAL_END_SNAP` | 2 | Readings this close (ADC counts) to the calibrated heel or toe send exactly 0 or 127 |
| `CAL_MIN_SPAN` | 40 | Smallest heel-to-toe movement (ADC counts) accepted by calibration |
| `CHANNEL_DIGIT_FLASH_MS` | 250 | Flash half-period (ms) of the `0` after switch 10 on the channel page |
| `CHANNEL_DIGIT_TIMEOUT_MS` | 5000 | How long (ms) the channel page waits for a second digit after switch 10 |
| `CONFIG_CHANGED_FLASH_MS` | 250 | CONFIG LED flash half-period (ms) while a change is unsaved |
| `MIDI_RX_FLASH_MS` | 60 | How long (ms) the MIDI CHAN LED lights for each received byte |
| `MIDI_TX_FLASH_MS` | 60 | How long (ms) the MIDI FUNCTION LED lights for each message sent |
| `BANK_PC_LABEL_MS` | 1000 | How long (ms) configuration page 5 shows `Pr`, then the program, in turn. A multiple of 50, up to 6350 |
| `SCROLL_FRAME_MS` | 200 | Time (ms) each frame of the "riban" scroll shows; power-on is ten frames (3 seconds at 300); smaller scrolls faster. A multiple of 50, at least 100 |
| `BOOT_LAMP_MS` | 1000 | Time (ms) from power-on that every LED is lit, before the normal LEDs show while the scroll finishes. A multiple of 50 |
| `VERSION_SHOW_MS` | 1000 | Time (ms) each version part shows on page 7. A multiple of 50; 10 x `SCROLL_FRAME_MS` + 4 x `VERSION_SHOW_MS` must be at most 12750 |
| `DEBOUNCE_SAMPLES` | 3 | Consecutive key scans (12ms apart) that must agree before a press or release counts |
| `KEY_ACTIVE_LEVEL` | 1 | Input level of a pressed key. Change it if every key reads inverted |
| `STARTUP_TICKS` | 100 | 2ms ticks to settle at power-on; key events in this time are discarded |
| `MIDI_TX_QUEUE` | 128 | MIDI OUT queue size in bytes (a power of two) |
| `MIDI_RX_QUEUE` | 128 | MIDI IN queue size in bytes (a power of two) |
| `KEY_EVENT_QUEUE` | 16 | Key event queue size (a power of two) |

`STOMP_COUNT` and `TICK_MS` describe the hardware and should not be changed. The queues are in the external SRAM (see [Memory](technical.md#memory)); each can be up to 256 bytes.

## Source layout

```
src/ctl.c      controller behaviour (portable, unit tested)
src/hw.c       FCB1010 hardware: LED and key scan, 7-segment display, MIDI UART, ADC, I2C EEPROM
src/keys.h     key debounce and hold detection, shared by the firmware, tests and editor
src/main.c     start-up and main loop
src/config.h   compile-time options (see Compile-time configuration)
src/version.h  firmware major and minor version
test/          host tests, editor smoke test and simulator script
editor/      browser editor (runs ctl.c compiled to WebAssembly)
tools/         build hooks: size check, build number, documentation updates
probe/         SRAM probe image (see [SRAM probe](sram-probe.md))
docs/          this documentation
```
