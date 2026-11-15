# riban FCB1010 stomp-box firmware

The Behringer FCB1010 is a sturdy MIDI foot controller, but its stock firmware is built around presets and banks, which makes it awkward to use as a simple row of effect switches. This replacement firmware turns it into a straightforward MIDI stomp-box: each of the ten footswitches is an on/off switch with its own LED, UP and DOWN step through programs, and the two expression pedals send continuous controllers. Everything is set up on the pedal itself, with no computer or editor needed.

It suits anyone who wants to switch effects in a DAW, a plugin host or a MIDI-capable amp or synth the way they would on a pedalboard: press to turn on, press again to turn off, and see at a glance what is on.

## Features

- 10 footswitches, each sending its own MIDI controller (CC), note or program change, with a choice of four switch behaviours
- Program change from UP and DOWN, with fast scrolling when held
- 2 expression pedals
- 2 relay outputs (the SWITCH 1 and SWITCH 2 jacks), triggered by any footswitch or pedal, to switch amp channels or other gear
- An extra on/off controller from tapping UP and DOWN together
- Follows incoming MIDI, so its LEDs stay in step with your DAW or synth
- Selectable MIDI channel
- Simple configuration on the pedal, saved in its memory

[Try the emulator in your browser](https://riban-bw.github.io/FCB1010_Firmware/emulator/). It runs this firmware's own control code, so you can try the footswitches, pedals and configuration pages before flashing a chip. Its on-screen hints explain each control as you use it.

## Using the pedal

| Control | What it does |
|---|---|
| Footswitches 1 to 10 | Turn an effect on or off. The LED shows whether it is on. Each switch can instead act while held (see Footswitch modes) |
| UP / DOWN | Next / previous program. The display shows the program number, and the program change is sent when you let go. Hold to scroll |
| Expression pedal A | Volume (CC 7), 0 to 127 |
| Expression pedal B | Expression (CC 11), 0 to 127 |
| UP + DOWN tapped together | An extra on/off switch (CC 20). The DIRECT SELECT LED shows its state |
| UP + DOWN held 1s | Opens configuration mode (see Configuration) |

Everything uses one MIDI channel, channel 1 unless you change it in configuration mode. Footswitches 1 to 10 send CC 21 to 30, with value 127 for on and 0 for off. You can give any footswitch a different message (see [Footswitch messages](#footswitch-messages)).

### Footswitch modes

Each footswitch has one of four modes, chosen in configuration mode (default: stomp).

| Mode | Display digit | Behaviour |
|---|---|---|
| Stomp | 1 | Press to turn on, press again to turn off |
| Stomp with momentary | 2 | As stomp, but a hold of more than 0.6s undoes it on release (see below) |
| Momentary | 3 | On while pressed, off when released |
| Inverted momentary | 4 | Off while pressed, on when released. Its LED is lit while it rests, from power-on onwards |

In stomp with momentary mode a press always acts immediately: it toggles the effect. What happens when you let go depends on how long you held it:

- **Released within 0.6 seconds:** the effect stays as it is now, like a normal stomp.
- **Released after more than 0.6 seconds:** the effect goes back to how it was before the press.

For example, with an effect on, pressing its switch turns it off. If you let go within 0.6 seconds it stays off. If you hold it longer and then let go, it comes back on. This lets you drop an effect out (or bring one in) for just as long as you hold the switch.

### Footswitch messages

Each footswitch sends one of three kinds of message, set in the footswitch editor (see [Configuration](#configuration)):

| Type | Display | On | Off |
|---|---|---|---|
| CC | `CC` | The controller number with the on value | The controller number with the off value |
| Note | `nt` | Note on: the note number, with the on value as velocity | Note off: the note number, with the off value as velocity |
| Prog | `Pr` | Program change: the first program | Program change: the second program |

Every type works in any of the footswitch modes. A program change switch has two programs and toggles between them: for example, in stomp mode one press sends program 5 and lights the LED, and the next sends program 6 and turns it off. If you give it the same program twice, it sends that program on every press and nothing when you let go, whatever its mode, and its LED lights only while you hold it. Whenever a footswitch sends a program, the display shows it and UP and DOWN step on from it.

### Changing program

UP and DOWN step the program number by one, from 0 to 127 (stopping at 0 and 127; a press that cannot change the number sends nothing). The program change is sent when you let go of the switch. Hold the pedal to auto-repeat, initially slowly then rapidly, and only the program you stop on is sent.

### The relay outputs (SWITCH 1 and SWITCH 2)

Each of the two relay jacks can follow any mix of footswitches, the UP + DOWN switch and the expression pedals. The relay closes when any of the things it follows is on, and opens when they are all off. For a pedal, "on" means pushed past a threshold you set. The SWITCH 1 and SWITCH 2 LEDs show the relay states. Nothing is set up until you choose, so out of the box the relays stay open.

### Keeping in step with your DAW

The pedal listens on its MIDI IN socket. When your DAW or synth sends one of the pedal's controllers on the pedal's channel, the matching LED changes to match: the switch's on value turns it on and its off value turns it off, and any other value counts as on from 64 to 127 and off from 0 to 63. A note switch follows its note the same way: a note on turns it on and a note off turns it off. Program changes are ignored. For example, CC 21 value 127 sets stomp 1 on and lights its LED, and CC 21 value 0 turns it off. The next press of that footswitch then carries on from there.

The pedal never sends these back out, so it is safe to connect MIDI OUT and MIDI IN to the same device. The MIDI CHAN LED flickers whenever MIDI arrives.

### Power-on

At power-on every LED lights and "riban" scrolls across the display. Then the display shows the program number and the pedal is ready. Pressing any switch skips this, and the press works as usual. Stomp states are OFF, except switches in inverted momentary mode, which start ON.

## Configuration

Hold UP and DOWN together for 1 second. The CONFIG LED lights. It flashes while any setting differs from what is saved in the pedal's memory, and stops flashing if you revert every change. If it flashes as soon as you open configuration mode, the last save did not reach the persistent memory. Footswitches and pedals send no MIDI while you are in configuration mode, and incoming MIDI is ignored.

Tap UP or DOWN to move to the next or previous page. To leave, hold UP (ENTER) for 0.6 seconds, or UP + DOWN together for 1 second, to save, or hold DOWN (ESCAPE) for 0.6 seconds to cancel every change. Configuration mode always opens on page 1, and each page is marked by a panel LED.

| Page | Panel LED | Display | Controls |
|---|---|---|---|
| 1. Pedal modes | MIDI FUNCTION | Mode of the selected footswitch (1 to 4 - see [Footswitch modes](#footswitch-modes) above) | Press a footswitch to select it (its LED lights) and see its mode; press it again to change its mode. Hold a footswitch for 0.6 seconds to open its editor (see below). |
| 2. MIDI channel | MIDI CHANNEL | Channel 1 to 16 | Footswitches 1 to 10 pick channels 1 to 10. Switch 10 flashes `0` for 5 seconds; press 1 to 6 for 11 to 16, or 10 again for 10. After 5 seconds, or on a page change, the channel stays as 10. |
| 3. Heel calibration | EXP 1 and EXP 2 | `Lo`, then `0` once the reading is taken | Put both pedals at the heel and press any footswitch. |
| 4. Toe calibration | EXP 1 and EXP 2 | `Hi`, then `0` once the reading is taken, or `Er` | Put both pedals at the toe and press any footswitch. `Er` means a pedal hardly moved between heel and toe; press a footswitch to try again. Calibrating with the heel and toe swapped reverses the pedal. |
| 5. SWITCH 1 relay | SWITCH 1 | Blank, or the trigger point of the pedal last moved | Press footswitches to choose which ones the relay follows (their LEDs light). Tap UP + DOWN to follow the UP + DOWN switch (DIRECT SELECT lights). Move a pedal to the point where the relay should close; move it to the heel to stop following it (EXP 1 / EXP 2 light while a pedal is followed). |
| 6. SWITCH 2 relay | SWITCH 2 | As page 5 | As page 5, for the SWITCH 2 relay. |
| 7. Lamp test and version | All | "riban", then the firmware version | Every LED lights while "riban" scrolls, then the firmware version shows in three parts: major with a trailing dot (`00.`), minor with dots both sides (`.01.`), and build with a leading dot (`.0A`), then it repeats. Each of the ten steps (six scroll frames, the three version parts and a blank) blinks its own footswitch LED, 1 to 10 in turn. Hold a footswitch to show its step, for example 8 for the minor version; let go to carry on. |

#### Footswitch editor

On page 1, hold a footswitch to edit its message. That footswitch's LED stays lit so you can see which one you are editing. Tap UP and DOWN to move between its settings. A panel LED shows which one you are on:

| Setting | Panel LED | Display | How to set it |
|---|---|---|---|
| Message type | SELECT | `CC`, `nt` (note) or `Pr` (program change) | Footswitch 1 for CC, 2 for note, 3 for program change. Or move either expression pedal: the bottom third is CC, the middle third note, the top third program change. |
| Number | NUMBER | 0 to 127 | The controller number, note number, or a program change's first program (sent when the switch turns on). Type it with the footswitches (1 to 9 are digits 1 to 9, 10 is 0), for example 6 then 10 for 60, or move either expression pedal. |
| On value | VALUE 1 | 0 to 127 | The value a CC sends when the switch turns on, or the note on velocity. For a program change, its second program (sent when it turns off; the same as the first for a single program). Set it the same way. |
| Off value | VALUE 2 | 0 to 127 | The value a CC sends when the switch turns off, or the note off velocity. Set it the same way. A program change has no off value, so UP stops at its second program. |

A number is complete once no more digits would fit (13 and up, or three digits), so the next digit you press starts a new number. Hold UP (ENTER) for 0.6 seconds to keep your changes and go back to page 1, or hold DOWN (ESCAPE) for 0.6 seconds to put the old values back. Like other settings, the message is only stored when you save and leave configuration mode. If the switch was on when you changed its message, the old message's "off" is sent and the switch turns off.

When you save, each footswitch whose mode or message you changed is put in its new mode's resting state, so it does not carry a stale state into its new mode: on for inverted momentary, off for the other modes. If that changes the switch, its new value is sent, so the receiving device stays in step. If a save fails it is not reported: the new settings apply until power-off.

## Installing

### BEWARE: This replaces the stock firmware completely

The stock presets, banks, direct-select modes and the Behringer SysEx editor do not work with this firmware. Put the original chip back to restore stock behaviour. The settings are kept away from where the stock firmware keeps its presets, but they may still be corrupted, so be prepared to set them up again.

### Flashing to an EPROM or EEPROM

The program memory is a 28-pin chip in a socket. The EPROM used in my FCB1010 is an HT27C512-70, which is 64KB. Some units have the smaller 27C256 (32KB) or 27C128 (16KB). The firmware is the same for each; only the image file and the replacement chip differ.

| Chip in the socket | Size | Image to write | Replacement chips |
|---|---|---|---|
| 27C512 (for example HT27C512) | 64KB | `build/fcb1010_27c512.bin` | A 27C512-pinout part: W27C512 (electrically erasable, so it can be reprogrammed in the programmer), or a one-time programmable AT27C512R |
| 27C256 | 32KB | `build/fcb1010_27c256.bin` | A 27C256-pinout part such as an AT27C256R, or an AT28C256 EEPROM on an adapter (see step 3) |
| 27C128 | 16KB | `build/fcb1010_27c128.bin` | A 27C128-pinout part, or a larger chip of the socket's pinout with its image |

The 27C512 image is the 32KB firmware twice, so it runs whether the socket drives the chip's top address line (pin 1, A15) high or low. All three images are built by `make` and offered by the GitHub build.

1. **Identify the stock chip.** Open the case (16 screws on the underside). The program memory is the 28-pin socketed chip, often under a label or hot glue. Read the part number and find it in the table above; go by what is printed on yours. I recommend taking a photo of the original physical installation to have a record of what it looked like before you started to make changes.
2. **Back up the original.** Read the stock chip with your programmer and save the file. Put a fresh label on it and keep it. This may allow you to flash the stock firmware to your new, target (E)EPROM to validate the flashing process. It is unlikely that the original chip is rewritable.
3. **Choose a replacement with the same pinout as the socket.** The 27C512 and 27C256 differ on pin 1 (A15 on the 27C512, Vpp on the 27C256), so use a part of the same family as the stock chip. A parallel EEPROM such as the AT28C256 differs from both on pins 1 and 27: pin 27 is /WE on the EEPROM but address line A14 on the EPROMs, so a plain swap can cause spurious write pulses and corrupt the code. If you want an AT28C256 in a 27C256 socket, use a 28C256-to-27C256 adapter board (for example [this PCBWay design](https://www.pcbway.com/project/shareproject/28C256_to_27C256.html), which has jumpers for both pinouts). I haven't tested any of these in an FCB1010.
4. **Program the chip.** With a programmer such as a TL866 and the open-source `minipro` tool, the commands look like this (the device name must match your chip exactly; list the names with `minipro -l | grep -i 27c512`):

   ```
   minipro -p "W27C512@DIP28" -w build/fcb1010_27c512.bin   # 27C512 socket
   minipro -p "W27C512@DIP28" -m build/fcb1010_27c512.bin   # verify
   minipro -p "AT27C256R@DIP28" -w build/fcb1010_27c256.bin # 27C256 socket
   ```

   `minipro` erases an electrically erasable chip such as the W27C512 before writing it. A used UV-erasable EPROM (one with a window) must be erased under UV light first.
5. **Install it with the pedal unplugged from power.** Match pin 1 to the notch on the socket, the same as the old chip was.
6. **Power on.** "riban" should scroll across the display, then it should show `0`. Press UP and release it, and the display should show `1`.

To lower the risk, program a spare chip first and keep the original in a safe place. The firmware has not yet run on a real FCB1010 (see What has and has not been tested).

## Technical reference

The rest of this document is for building, changing or checking the firmware.

### MIDI implementation

| Message | Sent | Received |
|---|---|---|
| Control change, footswitches | CC 21 to 30 (`STOMP_CC_LIST`), 127 on, 0 off by default; any controller number, on value and off value per switch | A CC on channel 1 with a CC switch's controller number sets that switch: its on value is on, its off value off, and any other value 64 to 127 on, 0 to 63 off |
| Note on / off, footswitches | Note switches: note on (0x9n) with the on value as velocity when on, note off (0x8n) with the off value as velocity when off | A note on with velocity 1 to 127 sets a note switch with that note on; a note off, or a note on with velocity 0, sets it off |
| Control change, UP + DOWN switch | `CHORD_CC`, 127 / 0 | CC 20 sets the UP + DOWN toggle and DIRECT SELECT LED the same way |
| Control change, pedals | CC 7 and CC 11 (`PEDAL_CC_LIST`), 0 to 127 | Ignored |
| Program change | 0 to 127, on release of UP / DOWN; a program change footswitch sends its first program when it turns on and its second when it turns off (with both the same, that program on every press) | Ignored |

- Everything is on one channel: `MIDI_CHANNEL` until a channel is saved in configuration mode.
- Received messages only set the pedal's own state and LEDs; nothing is echoed to MIDI OUT. The next footswitch press toggles from the received state: if CC 21 turned switch 1 on, pressing switch 1 sends CC 21 value 0.
- The receiver handles running status and split messages, ignores real-time bytes (clock, active sensing), SysEx, other channels and other controller numbers. Received messages are ignored in configuration mode.
- Pedal positions and the program number are not set by MIDI IN, and no state is remembered at power-off.
- A pedal sends a new value only after its reading moves `PEDAL_HYST` counts, and readings near the calibrated ends snap to 0 and 127 (`PEDAL_END_SNAP`).
- If MIDI OUT is full, a footswitch press is ignored rather than changing a state the receiver never hears about. A stomp with momentary revert that cannot be sent at release is sent as soon as there is room.

### Timing details

- **Long press.** The threshold is `MOMENTARY_HOLD_MS` in `src/config.h` (currently 600). The key scanner checks each switch every 12ms and starts timing once the press has been debounced (`DEBOUNCE_SAMPLES`), so the real threshold is about 0.64s. The same hold saves (UP) or cancels (DOWN) in configuration mode. If a CC arrives over MIDI IN during the hold, the release still goes back to the state before the press. A switch already down at power-on does nothing when released. UP and DOWN are not affected in run mode.
- **Program repeat.** Held: after 400ms the number auto-repeats at 4Hz on the display only, speeding up to about 25Hz once the key has been held for `PROGRAM_REPEAT_FAST_AFTER_MS` (2.7s), and one program change is sent on release.
- **Configuration.** UP + DOWN held 1s opens configuration mode (`CONFIG_HOLD_MS`); held 1s again in configuration mode it saves and leaves. A short UP + DOWN is the CC 20 toggle in run mode and does nothing in configuration mode except on the relay pages.
- **Page 7 and power-on.** Each "riban" frame shows for half of `VERSION_SHOW_MS`, and each version part shows in hex, one part per second, followed by a blank of the same length. During each step its footswitch LED is dark for about the first half of a scroll frame. Power-on shows the scroll only, once.
- All timing comes from a 2ms timer tick and assumes the FCB1010's 12MHz crystal, as does the 31250 baud MIDI rate.

### Building

Requires SDCC 4.x, `make` (and `gcc` for the host tests) (`apt install sdcc make gcc`).

```
make              # the ROM images in build/ (see below) and the emulator page
make test         # 6187 logic checks on this computer
make check        # every test suite, without bumping the build number
make emulator     # update emulator/index.html and run its checks (needs clang and Node)
test/sim_smoke.sh # runs the real image in the uCsim 8051 simulator (apt install sdcc-ucsim)
```

There is one ROM image per chip size, made by `tools/rom_image.py`: `build/fcb1010_27c128.bin` (16KB), `build/fcb1010_27c256.bin` (32KB, also for a 28C256) and `build/fcb1010_27c512.bin` (64KB, the 32KB image twice). Each is the code padded with 0xFF and repeated to fill the chip; the build fails if the code outgrows a chip. The code is linked for addresses below 32KB only, since how the FCB1010 wires the top address line of a 27C512 socket is not known.

If clang (with the wasm32 target) is installed, `make` also builds the browser emulator in `emulator/index.html`, so changes to `src/config.h` show up there too; otherwise it prints a note and leaves the page as it was.

The code is about 10.8KB (11063 bytes at the last build), so it needs a 16KB or larger chip. Each build checks the size against the 32KB ROM (the build fails if it does not fit).

Each build also updates this README: the code size above, the firmware version on configuration page 7, and every value taken from `src/config.h` (the option table below and the numbers quoted elsewhere). A GitHub Actions workflow (`.github/workflows/build.yml`) builds and tests every push and offers the firmware images as a download; it fails if the README was not updated after a change to `src/config.h`.

### Firmware version

The version shows on configuration page 7 as major, minor and build. Major and minor are set by hand in `src/version.h`. The build number in `src/version_build.inc` goes up by one each time `make` rebuilds the firmware image; test builds (`make check`, or `make BUILD_BUMP=0`) leave it alone.

### Compile-time configuration

These options are in `src/config.h`. Change them and rebuild. The current values are filled in by the build.

| Option | Current value | Meaning |
|---|---|---|
| `MIDI_CHANNEL` | 1 | MIDI channel (1 to 16) used until a channel is saved in configuration mode |
| `STOMP_CC_LIST` | 21, 22, 23, 24, 25, 26, 27, 28, 29, 30 | Controller number of each footswitch, 1 to 10 |
| `CHORD_CC` | 20 | Controller toggled by tapping UP + DOWN together. Keep it out of `STOMP_CC_LIST` |
| `PEDAL_CC_LIST` | 7, 11 | Controller numbers of expression pedals A and B |
| `DEFAULT_PEDAL_MODE` | 0 | Mode of every footswitch until one is saved: 0 stomp, 1 stomp with momentary, 2 momentary, 3 inverted momentary |
| `MOMENTARY_HOLD_MS` | 600 | Long press time (ms): stomp with momentary reverts after it, and the configuration save / cancel hold |
| `CONFIG_HOLD_MS` | 1000 | UP + DOWN hold (ms) that opens configuration mode |
| `PROGRAM_WRAP` | 0 | 1: program change wraps between 127 and 0; 0: it stops at the ends |
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
| `VERSION_SHOW_MS` | 1000 | Time (ms) each version part shows on page 7; each "riban" frame shows for half of it. A multiple of 200, at most 3600 |
| `DEBOUNCE_SAMPLES` | 3 | Consecutive key scans (12ms apart) that must agree before a press or release counts |
| `KEY_ACTIVE_LEVEL` | 1 | Input level of a pressed key. Change it if every key reads inverted |
| `STARTUP_TICKS` | 100 | 2ms ticks to settle at power-on; key events in this time are discarded |
| `MIDI_TX_QUEUE` | 16 | MIDI OUT queue size in bytes (a power of two) |
| `MIDI_RX_QUEUE` | 8 | MIDI IN queue size in bytes (a power of two) |
| `KEY_EVENT_QUEUE` | 8 | Key event queue size (a power of two) |

`STOMP_COUNT` and `TICK_MS` describe the hardware and should not be changed. The queue sizes are cut to fit the 8032's RAM (see Memory).

### Settings storage

Settings are stored in the 24C16 EEPROM (2KB, addresses 0x000-0x7FF) as fourteen 6-byte records at the top of the chip (0x790-0x7FF), away from the low addresses where the stock firmware keeps presets. Each record is 8-byte aligned, and its last two bytes (0x796-0x797, 0x79E-0x79F and so on) are unused.

| Address | Record | Byte 0 | Byte 1 | Byte 2 | Byte 3 | Byte 4 | Byte 5 |
|---|---|---|---|---|---|---|---|
| 0x790 + 8 × (n - 1) | Footswitch n (1-10: 0x790, 0x798 ... 0x7D8) | magic 0xD0 + (n - 1) | message type | number (0-127) | on value (0-127) | off value (0-127) | checksum; salts 0x96, 0x69, 0x55, 0xAA, 0x33, 0xCC, 0x0F, 0xF0, 0x5F, 0xF5 for switches 1-10 |
| 0x7E0 | SWITCH 1 relay | magic 0xC3 | followed footswitches 1-8 (bit 0 = switch 1) | bits 0-1: footswitches 9-10; bit 2: follows the UP + DOWN toggle | EXP 1 trigger value (1-127, 0 = off) | EXP 2 trigger value (1-127, 0 = off) | checksum, salt 0x3C |
| 0x7E8 | SWITCH 2 relay | magic 0xC4 | as SWITCH 1 | as SWITCH 1 | as SWITCH 1 | as SWITCH 1 | checksum, salt 0xC3 |
| 0x7F0 | Pedal calibration | magic 0xC1 | EXP 1 heel reading | EXP 1 toe reading | EXP 2 heel reading | EXP 2 toe reading | checksum, salt 0x5A |
| 0x7F8 | Channel and modes | magic 0xC2 | MIDI channel (1-16) | modes of footswitches 1-4 | modes of footswitches 5-8 | bits 0-3: modes of footswitches 9-10 | checksum, salt 0xA5 |

- **Checksum:** the XOR of bytes 0-4 and the record's salt.
- **Pedal modes:** 2 bits per footswitch, the lowest-numbered switch in the lowest bits (switch 1 in bits 0-1 of byte 2, switch 2 in bits 2-3, and so on). The values are 0 stomp, 1 stomp with momentary, 2 momentary and 3 inverted momentary.
- **Calibration:** the readings are the raw 8-bit ADC values.
- **Footswitch messages:** the type is 0 CC, 1 note, 2 program change; type 3 is unused and loads as CC with the same values. Values over 127 lose bit 7. A program change sends the number when on and the on value when off (both the same: a single program, sent on every press); it ignores the off value. (Earlier development builds kept two switches per record at 0x7B8-0x7DF with magics 0xC5-0xC9; those no longer match and load as the defaults.)
- **Missing or invalid records:** a record with the wrong magic or checksum loads as the defaults:
  - footswitch messages are CC `STOMP_CC_LIST` with on value 127 and off value 0;
  - relays follow nothing and have no triggers;
  - calibration is heel 0, toe 255;
  - the channel and pedal modes come from `src/config.h`.
- **Out-of-range values:** a stored channel outside 1-16 loads as the default channel, and a trigger value over 127 loads as off.
- **Saving:** a save writes each record in one page write (the six bytes are within one 16-byte page) and reads it back in one sequential read. Loading reads a record the same way.
- **CONFIG LED:** it compares the settings being edited with what is read back from these records.

### Hardware notes

The hardware map (latch addresses, LED and key wiring, ADC and EEPROM pins) comes from jspyve's public-domain reverse-engineering library at github.com/jspyve/FCB1010-lib, not from Behringer documents or from my own measurements. If the key polarity or an LED is wrong on your unit, `KEY_ACTIVE_LEVEL` in `src/config.h` or the LED tables in `src/hw.c` are where to look. The relay outputs are driven from bits 0 and 1 of the key column latch, as in jspyve's library; whether a set bit closes or opens a relay, and which bit is which jack, is unverified. The letters on the display (`Er`, `Lo`, `Hi`, the "riban" scroll and the hex digits) are drawn from segment tables in `src/hw.c` that have not been checked on a real display.

### What has and has not been tested

Tested:
- The controller logic: stomp toggling, all four pedal modes, CC, note and program change footswitch messages and the footswitch editor (typed and pedal values, accept, revert, saved records), program change on release with auto-repeat, the UP + DOWN chord, every configuration page and its saved bytes, pedal scaling, hysteresis, calibration flow, a full MIDI buffer, bad input, and MIDI IN parsing including running status, split messages, real-time bytes, SysEx, other channels and the 63/64 threshold, plus the momentary long-press behaviour: every switch, two switches at once, duplicate or unmatched hold and release events, a full MIDI queue at press and at release, calibration, and incoming MIDI CC changes during a hold (`make test`).
- The browser emulator, which runs the same control code, with a scripted smoke test (`make emulator`).
- The real 8051 image in a simulator: key scan and debounce, toggle on and off, program change, MIDI UART output with the correct bytes, the UP + DOWN chord opening and closing configuration mode (read back from RAM, including the stack depth of the save path), the long-press behaviour just under (stays latched) and just over (reverts) the long-press time, the power-on and page 7 LEDs and display, and the stack under receive load (`test/sim_smoke.sh`).

Not tested, because it needs real hardware:
- **Everything on an actual FCB1010** (see Hardware notes).
- The pedal ADC reading and the EEPROM calibration and settings save and load. The settings records have host-level round-trip tests only, and the simulator does not model them. The code is adapted from the same library.
- **MIDI IN on the real image.** The simulator's UART input would not deliver bytes to the firmware. The parser is tested on the host, and the simulator confirms the receive interrupt queues and drains bytes without disturbing key handling or the stack, but the step from a received byte to a lit LED has not been run end to end on the 8051 image. The FCB1010's MIDI IN feeds the same UART, per the reference code, which receives SysEx through it.
- **The 0.6 second long-press threshold on real hardware.** It is counted from the 2ms scan timer, which assumes a 12MHz crystal. The simulator measures it correctly in simulated time, but a different crystal speed or a slower scan would shift it.
- The 31250 baud MIDI rate on real silicon. It is computed for a 12MHz crystal; the simulator confirms the bytes but not the timing.

A wrong LED or key map does no electrical harm, but it will look wrong.

### Memory

The 8032 has only 256 bytes of internal RAM and this firmware uses almost all of the directly addressable part. To make room for configuration mode the MIDI OUT queue is 16 bytes, the MIDI IN queue 8 and the key event queue 8. The simulator shows the stack peaking 22 bytes below the end of RAM (0xE9, during configuration save, with both interrupts nested on top). If you add features, check `build/fcb1010.mem` for the stack headroom and run `test/sim_smoke.sh`, which fails if the stack gets too close to the end.

### Source layout

```
src/ctl.c      controller behaviour (portable, unit tested)
src/hw.c       FCB1010 hardware: LED and key scan, 7-segment display, MIDI UART, ADC, I2C EEPROM
src/keys.h     key debounce and hold detection, shared by the firmware, tests and emulator
src/main.c     start-up and main loop
src/config.h   compile-time options (see Compile-time configuration)
src/version.h  firmware major and minor version
test/          host tests, emulator smoke test and simulator script
emulator/      browser emulator (runs ctl.c compiled to WebAssembly)
tools/         build hooks: size check, build number, README updates
```
