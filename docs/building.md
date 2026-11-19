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

There is one ROM image per chip size, made by `tools/rom_image.py`: `build/fcb1010_27c128.bin` (16KB, only while the code fits), `build/fcb1010_27c256.bin` (32KB, also for a 28C256) and `build/fcb1010_27c512.bin` (64KB, the 32KB image twice). Each is the code padded with 0xFF and repeated to fill the chip. An image the code does not fit is not made (a note says so): the code fits 16KB for now, so the 27C128 image is made. The code is linked for addresses below 32KB only: the socket carries address lines A0 to A14, so the pedal can address 32KB of program memory.

If clang (with the wasm32 target) is installed, `make` also builds the browser editor in `editor/index.html`, so changes to `src/config.h` show up there too; otherwise it prints a note and leaves the page as it was.

The code is about 15.9KB (16231 bytes at the last build), so it needs a 16KB or larger chip. Each build checks the size against the 32KB ROM (the build fails if it does not fit, and prints a note once less than 2KB is left, so the code can be optimised for size in time).

Each build also updates the documentation: the code size above, the firmware version on configuration page 9 (in [Configuration](configuration.md)), and every value taken from `src/config.h` (the option table below and the numbers quoted in the README and these pages). A GitHub Actions workflow (`.github/workflows/build.yml`) builds and tests every push and offers the firmware images as a download; it fails if the documentation was not updated after a change to `src/config.h`.

## Firmware version

The version shows on configuration page 9 as major, minor and build. Major and minor are set by hand in `src/version.h`, in decimal (0 to 199), and shown in decimal (from 100 with the display's leftmost "1"); the build number is shown in hex. The build number in `src/version_build.inc` goes up by one each time `make` rebuilds the firmware image; test builds (`make check`, or `make BUILD_BUMP=0`) leave it alone.

## Compile-time configuration

These options are in `src/config.h`. Change them and rebuild. The current values are filled in by the build.

| Option | Current value | Meaning |
|---|---|---|
| `MIDI_CHANNEL` | 1 | MIDI channel (1 to 16) used until a channel is saved in configuration mode |
| `STOMP_CC_LIST` | 21, 22, 23, 24, 25, 26, 27, 28, 29, 30 | Controller number of each footswitch, 1 to 10 |
| `PEDAL_CC_LIST` | 7, 11 | Controller numbers of expression pedals A and B until changed on configuration pages 3 and 4 (each bank has its own) |
| `PEDAL_PICKUP_A` | 0 | Expression pedal A's response until changed on configuration page 3 (each bank has its own): 0 immediate, 1 pickup |
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
| `CAL_ERROR_SHOW_MS` | 2000 | How long (ms) `Er` shows after a refused calibration, then the pedal's value. A multiple of `CHANNEL_DIGIT_FLASH_MS`, up to 255 of them |
| `CHANNEL_DIGIT_FLASH_MS` | 250 | Flash half-period (ms) of the right-hand dot while a typed number can take another digit (also `Lo` / `Hi` in calibration and the save prompt's bank number) |
| `CONFIG_SHOW_MS` | 800 | How long (ms) configuration page 1 shows each of the selected footswitch's settings, and the misc page each name and value. A multiple of 50 |
| `CONFIG_SWITCH_SHOW_MS` | 1600 | How long (ms) configuration page 1 shows the selected footswitch's number before its settings. A multiple of 50 |
| `CONFIG_CHANGED_FLASH_MS` | 250 | CONFIG LED flash half-period (ms) while a change is unsaved |
| `MIDI_RX_FLASH_MS` | 60 | How long (ms) the MIDI CHAN LED lights for each received byte |
| `MIDI_TX_FLASH_MS` | 60 | How long (ms) the MIDI FUNCTION LED lights for each message sent |
| `BANK_PC_LABEL_MS` | 1000 | How long (ms) configuration page 7 shows `Pr`, then the program, in turn. A multiple of 50, up to 6350 |
| `SCROLL_FRAME_MS` | 200 | Time (ms) each frame of the "riban" scroll shows; power-on is ten frames (3 seconds at 300); smaller scrolls faster. A multiple of 50, at least 100 |
| `BOOT_LAMP_MS` | 1000 | Time (ms) from power-on that every LED is lit, before the normal LEDs show while the scroll finishes. A multiple of 50 |
| `DEFAULT_METRONOME` | 0 | The visual metronome (configuration page 8) until one is saved: 1 on |
| `DEFAULT_RUNNING_STATUS` | 0 | Running status on MIDI OUT (configuration page 8) until one is saved: 1 on |
| `LED_DIM_US` | 20 | How long the dim LEDs (configuration mode) are lit in each 2ms LED slot, in µs (0 to 1000): smaller is dimmer; 20 is 1% of the time |
| `LED_BRIGHTNESS` | 127 | The LED brightness (configuration page 9, pedal B) until one is saved: 0 to 127 (full) |
| `LED_MIN_US` | 40 | How long every LED is lit in each 2ms slot at the lowest brightness (0), in µs (1 to 1000) |
| `PARAM_FLASH_MS` | 100 | The footswitch editor: the setting's panel LED flickers, on and off this long each |
| `DEFAULT_ACTIVE_SENSING` | 0 | Active sensing on MIDI OUT (configuration page 8) until one is saved: 1 on |
| `SYSEX_ID` | 0x7D | The SysEx manufacturer ID of the identity reply and the settings messages, one byte (0x01 to 0x7D). 0x7D is the MIDI ID for non-commercial use: the project has no ID of its own, and 0x7C is only for MIDI 2.0 endpoint discovery (see [The SysEx ID](midi.md#the-sysex-id)) |
| `ACTIVE_SENSE_MS` | 250 | The interval between active sensing bytes, 2 to 510ms (the MIDI specification asks for at most 300ms) |
| `LAMP_TEST_MS` | 1000 | Time (ms) every LED is lit on entering configuration page 9, before the chase. A multiple of 50 |
| `LAMP_CHASE_MS` | 100 | Time (ms) each panel LED is lit in page 9's chase along the row and back. A multiple of 50 |
| `VERSION_SHOW_MS` | 1000 | Time (ms) each version part shows on page 9. A multiple of 50; 10 x `SCROLL_FRAME_MS` + 4 x `VERSION_SHOW_MS` must be at most 12750 |
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
tools/         build hooks: size check, build number, documentation updates, ctl_ram.py
               (writes build/ctl_ram.c, ctl.c's directly addressed variables)
probe/         SRAM probe image (see [SRAM probe](sram-probe.md))
docs/          this documentation
```
