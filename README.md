# FCB1010 stomp-box firmware

The Behringer FCB1010 MIDI foot controller's stock firmware does not work well as a simple stomp-box. This is firmware implements behaviour that one may expect from a stomp-box.

[Try the emulator in your browser](https://riban-bw.github.io/FCB1010_Firmware/emulator/). It runs this firmware's own control code, so you can try the footswitches, pedals and configuration pages before flashing a chip.

| Control | Behaviour |
|---|---|
| Footswitches 1 to 10 | Stomp switches. Each has a mode (see below): stomp, stomp with momentary, momentary, or inverted momentary. The LED follows the state |
| Expression pedal A | Continuous CC 7 (volume), 0 to 127 |
| Expression pedal B | Continuous CC 11 (expression), 0 to 127 |
| UP / DOWN | Program change +1 / -1, stopping at 0 and 127 (no wrap; `PROGRAM_WRAP` in `src/config.h`), sent when the key is released. At 0 or 127 a press that cannot change the number sends nothing. Held: after 400 ms the number auto-repeats at 4 Hz on the display only, speeding up to about 25 Hz once the key has been held for `PROGRAM_REPEAT_FAST_AFTER_MS` (2.7 s), and one program change is sent on release |
| MIDI IN | A CC on channel 1 with a stomp's controller number sets that stomp: value 64 to 127 is on (LED lit), 0 to 63 is off. CC 20 sets the UP + DOWN toggle and DIRECT SELECT LED the same way |
| UP + DOWN tapped together | Toggles CC 20 (127 / 0). The DIRECT SELECT LED shows its state. Set by `CHORD_CC` in `src/config.h`; keep it different from the stomp CCs |
| UP + DOWN held 1 s | Configuration mode: pedal modes, MIDI channel, expression pedal calibration, relay triggers (see below). Held 1 s again in configuration mode: save and leave |

Everything uses one MIDI channel, 1 by default and changeable in configuration mode. Controller numbers, wrap behaviour, default pedal mode and noise filtering are set in `src/config.h`.

Default stomp CCs are 21 to 30 for switches 1 to 10. Edit `STOMP_CC_LIST` to change them.

## Pedal modes

Each footswitch has one of four modes, set in configuration mode (default: stomp, `DEFAULT_PEDAL_MODE`).

| Mode | Display digit | Behaviour |
|---|---|---|
| Stomp | 1 | Simple toggle between 127 and 0 |
| Stomp with momentary | 2 | Toggle, but a hold of more than 0.6 s undoes it on release (see below) |
| Momentary | 3 | 127 while pressed, 0 on release |
| Inverted momentary | 4 | 0 while pressed, 127 on release. The LED follows the value sent, so it is lit at rest: from power-on, and straight after saving it in this mode |

## Momentary behaviour on a long press (stomp with momentary mode)

A footswitch press always acts immediately: it toggles the stomp and sends the new CC value. What happens on release depends on how long the switch was held.

- **Released within 0.6 seconds:** the new state stays latched, like a normal stomp switch.
- **Released after more than 0.6 seconds:** the stomp goes back to the state it had before the press, and the matching CC value is sent again.

For example, with a stomp ON, pressing it sends OFF. If you let go within 0.6 seconds it stays OFF. If you hold it longer and then let go, it sends ON again.

- The time is `MOMENTARY_HOLD_MS` in `src/config.h` (currently 600). The key scanner checks each switch every 12 ms and starts timing once the press has been debounced, so the real threshold is about 0.64 s.
- UP and DOWN are not affected.
- Plain stomp mode ignores the hold. Momentary mode always goes off on release, and inverted momentary always goes back on.
- If a CC arrives over MIDI IN while the switch is held, release still goes back to the state before the press.
- If MIDI OUT is too full to send the revert at the moment of release, it is sent as soon as there is room.
- A switch that was already down when the pedal powered up does nothing special when released.

## Relay switches (SWITCH 1 and SWITCH 2)

Each relay can follow any of footswitches 1 to 10, the CC 20 toggle (UP + DOWN), and either expression pedal, chosen on configuration pages 5 and 6. The triggers are combined with a logical OR: the relay closes when any of its triggers is on and opens when they are all off. A footswitch trigger is on while that switch's state is on (its LED lit), the CC 20 trigger while CC 20 is on, and a pedal trigger while the pedal's CC value is at or above the value set for it (0 means no pedal trigger). There are no triggers until you set some. In run mode the SWITCH 1 / SWITCH 2 LEDs show the relays; configuration mode leaves the relays as they were. The settings are saved with the others, at 0x7E0 and 0x7E8 in the EEPROM.

The relay outputs are driven from bits 0 and 1 of the key column latch, as in jspyve's library. This is unverified on a real unit, including whether a set bit closes or opens the relay.

## Setting state with MIDI

In run mode the MIDI CHAN LED flashes briefly (`MIDI_RX_FLASH_MS`) for each received MIDI byte, real-time bytes such as clock and active sensing excepted.

Send a control change on the same channel (default 1) to set a stomp state, e.g. to sync with a DAW. For example, CC 21 value 127 sets stomp 1 on and lights its LED, and CC 21 value 0 turns it off. CC 20 (the UP + DOWN toggle) works the same way and sets the DIRECT SELECT LED; the next UP + DOWN tap toggles from that state.

- The pedal only updates its own state and LED. It sends nothing back, so joining MIDI OUT to MIDI IN cannot cause a feedback loop.
- The next footswitch press toggles from this state. If CC 21 turned switch 1 on, pressing switch 1 sends CC 21 value 0.
- It handles running status, ignores MIDI clock and other real-time bytes, ignores SysEx, and ignores other channels and other controller numbers.
- Pedal positions and program number are not set by MIDI IN.
- States are not remembered at power-off. Stomps start off, except inverted momentary switches, which start on (their resting state).

## Important: this replaces the stock firmware completely

The stock presets, banks, direct-select modes and the Behringer SysEx editor do not work with this firmware. Put the original chip back to restore stock behaviour. Although effort is made to avoid overwriting the stock firmware's configuration, presets and configuration may be corrupted and need reasserting.

## Build

Requires SDCC 4.x, `make` (and `gcc` for the host tests) (`apt install sdcc make gcc`).

```
make              # build/fcb1010_32k.bin and build/fcb1010_64k_mirrored.bin, and the emulator page
make test         # 1117 logic checks on this computer
make check        # every test suite, without bumping the build number
make emulator     # update emulator/index.html and run its checks (needs clang and Node)
test/sim_smoke.sh # runs the real image in the uCsim 8051 simulator (apt install sdcc-ucsim)
```

Each `make` that rebuilds the firmware image increments the build number in `src/version_build.inc`; test builds (`make check`, or `make BUILD_BUMP=0`) leave it alone. If clang (with the wasm32 target) is installed, `make` also builds the browser emulator in `emulator/index.html`, so changes to `src/config.h` show up there too; otherwise it prints a note and leaves the page as it was.

The code is about 9.0 KB (9168 bytes at the last build), so it needs a 16 KB or larger chip; the 32 KB image suits a 27C256 / 28C256. Unused space is filled with 0xFF. Each build checks the size against the 32 KB ROM (the build fails if it does not fit) and updates the figure in this paragraph.

## Flashing to an EEPROM

The EPROM used in the FCB1010 is HT27C512-70 (at least it is in my device).

1. **Identify the stock chip.** Open the case (16 screws on the underside). The program memory is the 28-pin socketed chip, often under a label or hot glue. Read the part number. A 27C256-type (32 KB) is common, but go by what is printed on yours. I recommend taking a photo of the original physcial installation to have a record of what it looked like before you started to make changes.
2. **Back up the original.** Read the stock chip with your programmer and save the file. Put a fresh label on it and keep it. Do this first.
3. **Choose a replacement that fits the socket.** A parallel EEPROM such as the AT28C256 has a different pinout from a 27C256 on pins 1 and 27. Pin 27 is /WE on the EEPROM but address line A14 on the EPROM, so a plain swap can cause spurious write pulses and corrupt the code. Use a 28C256-to-27C256 adapter board (for example [this PCBWay design](https://www.pcbway.com/project/shareproject/28C256_to_27C256.html), which has jumpers for both pinouts), or a 27C256-pinout part such as an AT27C256R. I haven't tested either in an FCB1010.
4. **Program the chip.** With a programmer such as a TL866 and the open-source `minipro` tool, the command looks like this (the device name must match your chip, see `minipro -l`):

   ```
   minipro -p "AT28C256" -w build/fcb1010_32k.bin
   minipro -p "AT28C256" -m build/fcb1010_32k.bin   # verify
   ```

   If your chip is 64 KB (27C512 type), write `build/fcb1010_64k_mirrored.bin`. That file is the same image twice, so it runs whether the top address line is high or low.
5. **Install it with the pedal unplugged from power.** Match pin 1 to the notch on the socket, the same as the old chip was.
6. **Power on.** The display should show `0`. Press UP and the display should show `1`.

## Configuration mode

Hold UP and DOWN together for 1 second. The CONFIG LED lights, and flashes while any setting differs from what is saved in the pedal's memory (EEPROM). If you put every change back (for example cycle a mode all the way round), it stops flashing. If it flashes as soon as you enter configuration mode, the last save did not reach the memory. Footswitches and pedals send no MIDI while you are in configuration mode, and incoming MIDI is ignored.

UP and DOWN on their own move to the next or previous page. Hold UP (ENTER) for 0.6 seconds, or UP + DOWN together for 1 second, to save and leave, or hold DOWN (ESCAPE) for 0.6 seconds to cancel every change and leave. (The UP and DOWN holds use the same time as the stomp with momentary long press, `MOMENTARY_HOLD_MS` in `src/config.h`.) Either way the display goes back to the program number. Configuration mode opens on page 1. Tapping UP + DOWN does nothing in configuration mode, except on pages 5 and 6 (see below).

| Page | Panel LED | Display | Controls |
|---|---|---|---|
| 1. Pedal modes | MIDI FUNCTION | Mode digit of the selected footswitch (1 to 4); the footswitch's own LED shows which one is selected | Press a footswitch to select it and see its mode; press it again to change its mode. |
| 2. MIDI channel | MIDI CHANNEL | Channel 1 to 16 | Footswitches 1 to 10 pick channels 1 to 10. Switch 10 flashes `0` for 5 seconds; press 1 to 6 for 11 to 16 or 10 again for 10. After 5 seconds, or on a page change, the channel stays as 10. |
| 3. Heel calibration | EXP 1 and EXP 2 | `Lo`, then `0` once the reading is taken | Put both pedals at the heel and press any footswitch. |
| 4. Toe calibration | EXP 1 and EXP 2 | `Hi`, then `0` once the reading is taken, or `Er` | Put both pedals at the toe and press any footswitch. `Er` indicates there was insufficient difference between low and high readings. You may set low and high opposite to give an inverted control value. |
| 5. SWITCH 1 relay | SWITCH 1 | Blank or the trigger value of the pedal last moved | Press footswitches 1 to 10 to choose which ones the relay follows (their LEDs light). Tap UP + DOWN to follow CC 20 (DIRECT SELECT lights). Move a pedal to set the CC value at which the relay turns on; move it to the heel (0) to turn that trigger off (EXP 1 / EXP 2 light while a pedal trigger is set). Multiple triggers may be selected. |
| 6. SWITCH 2 relay | SWITCH 2 | As page 5 | As page 5, for the SWITCH 2 relay. |
| 7. Lamp test and version | All | All segments then firmware version | Every LED blinks 3 times, then every LED stays lit (the CONFIG LED keeps flashing if there are unsaved changes) while the firmware version shows in hex, one part per second, repeating: major with a trailing dot (`00.`), minor with leading and trailing dots (`.01.`), and build with a leading dot (`.07`). The display blanks briefly before repeating. Press any footswitch to run it again. Major and minor are set in `src/version.h`; the build number (`src/version_build.inc`) goes up by one each time `make` rebuilds the firmware image, and the build fills the version shown here with the one it built. The blink and display times are in `src/config.h` |

If a pedal moved less than 40 counts between the heel and toe readings, page 4 shows `Er` and nothing changes; press a footswitch again to retake the toe reading. Page 4 on its own checks the toe against the current heel. A heel reading with no toe reading is discarded.

On saving, a stomp that was on and whose mode you changed is turned off (the CC value 0 is sent), so it does not carry a stale state into its new mode. A footswitch changed to inverted momentary is set on instead and its LED lights (its resting state), but no CC is sent for it. The DIRECT SELECT LED is dark while you are in configuration mode and shows the CC 20 state again when you leave. A footswitch that was being held when configuration mode opened is released first.

Settings are stored in the 24C16 EEPROM: relay triggers at 0x7E0 and 0x7E8, calibration at address 0x7F0 and channel plus pedal modes at 0x7F8, away from the low addresses where the stock firmware keeps presets. A failed save is not reported: the new settings apply until power-off.

## What has and has not been tested

Tested:
- The controller logic: stomp toggling, all four pedal modes, program change on release with auto-repeat, the UP + DOWN chord, every configuration page and its saved bytes, pedal scaling, hysteresis, calibration flow, a full MIDI buffer, bad input, and MIDI IN parsing including running status, split messages, real-time bytes, SysEx, other channels and the 63/64 threshold, plus the momentary long-press behaviour: your three cases, every switch, two switches at once, duplicate or unmatched hold and release events, a full MIDI queue at press and at release, calibration, and incoming MIDI CC changes during a hold (`make test`).
- The real 8051 image in a simulator: key scan and debounce, toggle on and off, program change, MIDI UART output with the correct bytes, the UP + DOWN chord opening and closing configuration mode (read back from RAM, including the stack depth of the save path), and the long-press behaviour just under (stays latched) and just over (reverts) the long-press time, both with DOWN auto-repeating (`test/sim_smoke.sh`).

Not tested, because it needs real hardware:
- **Everything on an actual FCB1010.** The hardware map (latch addresses, LED and key wiring, ADC and EEPROM pins) comes from jspyve's public-domain reverse-engineering library at github.com/jspyve/FCB1010-lib, not from Behringer documents or from my own measurements. If the key polarity or an LED is wrong on your unit, the switch `KEY_ACTIVE_LEVEL` in `src/config.h` or the LED tables in `src/hw.c` are where to look.
- The pedal ADC reading and the EEPROM calibration and settings save and load. The settings record has host-level round-trip tests only. The simulator does not model them. The code is adapted from the same library.
- **MIDI IN on the real image.** The simulator's UART input would not deliver bytes to the firmware, so I could not feed it real MIDI. The parser is tested on the host, and the simulator confirms the receive interrupt queues and drains bytes without disturbing key handling or the stack, but the step from a received byte to a lit LED has not been run end to end on the 8051 image. The FCB1010's MIDI IN feeds the same UART, per the reference code, which receives SysEx through it.
- **The 0.6 second long-press threshold on real hardware.** It is counted from the 2 ms scan timer, which assumes a 12 MHz crystal. The simulator measures it correctly in simulated time, but a different crystal speed or a slower scan would shift it.
- The 31250 baud MIDI rate on real silicon. It is computed for a 12 MHz crystal; the simulator confirms the bytes but not the timing.

To lower the risk, program a spare chip first and keep the original in a safe place. A wrong LED or key map does no electrical harm, but it will look wrong.

## Memory

The 8032 has only 256 bytes of internal RAM and this firmware uses almost all of the directly addressable part. To make room for configuration mode the MIDI OUT queue is 16 bytes, the MIDI IN queue 8 and the key event queue 8. The simulator shows the stack peaking about 41 bytes below the end of RAM (during configuration save). If you add features, check `build/fcb1010.mem` for the stack headroom and run `test/sim_smoke.sh`, which fails if the stack gets too close to the end.

## Layout

```
src/ctl.c    controller behaviour (portable, unit tested)
src/hw.c     FCB1010 hardware: LED and key scan, MIDI UART, ADC, I2C EEPROM
src/main.c   start-up and main loop
src/config.h settings
test/        host tests and simulator script
```
