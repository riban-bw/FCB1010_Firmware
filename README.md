# FCB1010 stomp-box firmware

Replacement firmware for the Behringer FCB1010 MIDI foot controller.

| Control | Behaviour |
|---|---|
| Footswitches 1 to 10 | Stomp switches. Each has a mode (see below): stomp, stomp with momentary, or momentary. The LED follows the state |
| Expression pedal A | Continuous CC 7 (volume), 0 to 127 |
| Expression pedal B | Continuous CC 11 (expression), 0 to 127 |
| UP / DOWN | Program change +1 / -1 (wraps 127 to 0), sent when the key is released. Held: after 400 ms the number auto-repeats at 4 Hz on the display only, and one program change is sent on release |
| MIDI IN | A CC on channel 1 with a stomp's controller number sets that stomp: value 64 to 127 is on (LED lit), 0 to 63 is off |
| UP + DOWN held 1 s | Configuration mode: MIDI channel, pedal modes, expression pedal calibration (see below) |

Everything uses one MIDI channel, 1 by default and changeable in configuration mode. Controller numbers, wrap behaviour, default pedal mode and noise filtering are set in `src/config.h`.

Default stomp CCs are 20 to 29 for switches 1 to 10. Edit `STOMP_CC_LIST` to change them.

## Pedal modes

Each footswitch has one of three modes, set in configuration mode (default: stomp with momentary, `DEFAULT_PEDAL_MODE`).

| Mode | Display digit | Behaviour |
|---|---|---|
| Stomp | 1 | Simple toggle between 127 and 0 |
| Stomp with momentary | 2 | Toggle, but a hold of more than 1 s undoes it on release (below) |
| Momentary | 3 | 127 while pressed, 0 on release |

## Momentary behaviour on a long press (stomp with momentary mode)

A footswitch press always acts immediately: it toggles the stomp and sends the new CC value. What happens on release depends on how long the switch was held.

- **Released within 1 second:** the new state stays latched, like a normal stomp switch.
- **Released after more than 1 second:** the stomp goes back to the state it had before the press, and the matching CC value is sent again.

For example, with a stomp ON, pressing it sends OFF. If you let go within a second it stays OFF. If you hold it longer and then let go, it sends ON again.

- The time is `MOMENTARY_HOLD_MS` in `src/config.h` (default 1000). The key scanner checks each switch every 12 ms and starts timing once the press has been debounced, so the real threshold is about 1.03 s.
- UP and DOWN are not affected.
- Plain stomp mode ignores the hold. Momentary mode always goes off on release.
- If a CC arrives over MIDI IN while the switch is held, release still goes back to the state before the press.
- If MIDI OUT is too full to send the revert at the moment of release, it is sent as soon as there is room.
- A switch that was already down when the pedal powered up does nothing special when released.

## Remote control over MIDI IN

Send a control change on the same channel (default 1) to set a stomp from another device or a DAW. For example, CC 20 value 127 turns switch 1 on and lights its LED, and CC 20 value 0 turns it off.

- The pedal only updates its own state and LED. It sends nothing back, so joining MIDI OUT to MIDI IN cannot cause a feedback loop.
- The next footswitch press toggles from the remote state. If CC 20 turned switch 1 on, pressing switch 1 sends CC 20 value 0.
- It handles running status, ignores MIDI clock and other real-time bytes, ignores SysEx, and ignores other channels and other controller numbers.
- Pedal positions and program number are not set by MIDI IN.
- The remote state is not remembered at power-off. All stomps start off.

## Important: this replaces the stock firmware completely

The stock presets, banks, direct-select modes and the Behringer SysEx editor do not work with this firmware. Put the original chip back to restore stock behaviour.

## Build

Needs SDCC 4.x (`apt install sdcc`), `make`, and `gcc` for the host tests.

```
make              # build/fcb1010_32k.bin and build/fcb1010_64k_mirrored.bin
make test         # 867 logic checks on this computer
test/sim_smoke.sh # runs the real image in the uCsim 8051 simulator (apt install sdcc-ucsim)
```

The code is about 3.7 KB, so it fits any chip from 8 KB up. Unused space is filled with 0xFF.

## Flashing to an EEPROM

I could not confirm from public sources exactly which chip is fitted in your unit. Check it before you buy anything.

1. **Identify the stock chip.** Open the case (16 screws on the underside). The program memory is the 28-pin socketed chip, often under a label or hot glue. Read the part number. A 27C256-type (32 KB) is common, but go by what is printed on yours.
2. **Back up the original.** Read the stock chip with your programmer and save the file. Put a fresh label on it and keep it. Do this first.
3. **Choose a replacement that fits the socket.** A parallel EEPROM such as the AT28C256 has a different pinout from a 27C256 on pins 1 and 27. Pin 27 is /WE on the EEPROM but address line A14 on the EPROM, so a plain swap can cause spurious write pulses and corrupt the code. Use a 28C256-to-27C256 adapter board (for example [this PCBWay design](https://www.pcbway.com/project/shareproject/28C256_to_27C256.html), which has jumpers for both pinouts), or a 27C256-pinout part such as an AT27C256R. I haven't tested either in an FCB1010.
4. **Program the chip.** With a programmer such as a TL866 and the open-source `minipro` tool, the command looks like this (the device name must match your chip, see `minipro -l`):

   ```
   minipro -p "AT28C256" -w build/fcb1010_32k.bin
   minipro -p "AT28C256" -m build/fcb1010_32k.bin   # verify
   ```

   If your chip is 64 KB (27C512 type), write `build/fcb1010_64k_mirrored.bin`. That file is the same image twice, so it runs whether the top address line is high or low.
5. **Install it with the pedal unplugged from power.** Match pin 1 to the notch on the socket, as the old chip was.
6. **Power on.** The display should show `0`. Press UP and the display should show `1`.

## Configuration mode

Hold UP and DOWN together for 1 second. The CONFIG LED lights. Footswitches and pedals send no MIDI while you are in configuration mode, and incoming MIDI is ignored.

A short press of UP + DOWN together moves to the next page. UP and DOWN on their own change the open value. Holding UP or DOWN auto-repeats (not on the calibration page). Hold UP + DOWN for 1 second again to save and leave; the display goes back to the program number.

| Page | Panel LED | Display | Controls |
|---|---|---|---|
| 1. MIDI channel | MIDI CHAN | Channel 1 to 16 | UP / DOWN change it (wraps) |
| 2. Pedal modes | MIDI FUNC | Footswitch number then mode digit, for example `32` is footswitch 3 in mode 2 | Press a footswitch to select it (its LED lights); UP / DOWN cycle its mode |
| 3. Calibration | EXP 1 and EXP 2 | Step: `1` heel, `2` toe, `0` done, `88` error | Pedals at heel, UP; pedals at toe, UP; DOWN restarts |

If a pedal moved less than 40 counts between the two calibration steps, the display shows `88` and the process restarts at step 1. Leaving configuration mode keeps whatever calibration was completed; a half-done one is discarded.

On leaving, a stomp that was on and whose mode you changed is turned off (the CC 0 is sent), so it does not carry a stale state into its new mode. A footswitch that was being held when configuration mode opened is released first.

Settings are stored in the 24C16 EEPROM: calibration at address 0x7F0 and channel plus pedal modes at 0x7F8, away from the low addresses where the stock firmware keeps presets. A failed save is not reported: the new settings apply until power-off. The old power-on calibration (holding DOWN while powering on) has been removed; use page 3.

## What has and has not been tested

Tested:
- The controller logic: stomp toggling, all three pedal modes, program change on release with auto-repeat, the UP + DOWN chord, every configuration page and its saved bytes, pedal scaling, hysteresis, calibration flow, a full MIDI buffer, bad input, and MIDI IN parsing including running status, split messages, real-time bytes, SysEx, other channels and the 63/64 threshold, plus the momentary long-press behaviour: your three cases, every switch, two switches at once, duplicate or unmatched hold and release events, a full MIDI queue at press and at release, calibration, and remote changes during a hold (`make test`).
- The real 8051 image in a simulator: key scan and debounce, toggle on and off, program change, MIDI UART output with the correct bytes, the UP + DOWN chord opening and closing configuration mode (read back from RAM, including the stack depth of the save path), and the long-press behaviour at 0.84 s (stays latched) and 1.2 s (reverts) of simulated time, both with DOWN auto-repeating (`test/sim_smoke.sh`).

Not tested, because it needs real hardware:
- **Everything on an actual FCB1010.** The hardware map (latch addresses, LED and key wiring, ADC and EEPROM pins) comes from jspyve's public-domain reverse-engineering library at github.com/jspyve/FCB1010-lib, not from Behringer documents or from my own measurements. If the key polarity or an LED is wrong on your unit, the switch `KEY_ACTIVE_LEVEL` in `src/config.h` or the LED tables in `src/hw.c` are where to look.
- The pedal ADC reading and the EEPROM calibration and settings save and load. The settings record has host-level round-trip tests only. The simulator does not model them. The code is adapted from the same library.
- **MIDI IN on the real image.** The simulator's UART input would not deliver bytes to the firmware, so I could not feed it real MIDI. The parser is tested on the host, and the simulator confirms the receive interrupt queues and drains bytes without disturbing key handling or the stack, but the step from a received byte to a lit LED has not been run end to end on the 8051 image. The FCB1010's MIDI IN feeds the same UART, per the reference code, which receives SysEx through it.
- **The 1 second threshold on real hardware.** It is counted from the 2 ms scan timer, which assumes a 12 MHz crystal. The simulator measures it correctly in simulated time, but a different crystal speed or a slower scan would shift it.
- The 31250 baud MIDI rate on real silicon. It is computed for a 12 MHz crystal; the simulator confirms the bytes but not the timing.

To lower the risk, program a spare chip first and keep the original in a safe place. A wrong LED or key map does no electrical harm, but it will look wrong.

## Memory

The 8032 has only 256 bytes of internal RAM and this firmware uses almost all of the directly addressable part. To make room for configuration mode the MIDI OUT queue is 16 bytes, the MIDI IN queue 8 and the key event queue 8. The simulator shows the stack peaking about 18 bytes below the end of RAM (during configuration save), so there is not much slack. If you add features, check `build/fcb1010.mem` for the stack headroom and run `test/sim_smoke.sh`, which fails if the stack gets too close to the end.

## Layout

```
src/ctl.c    controller behaviour (portable, unit tested)
src/hw.c     FCB1010 hardware: LED and key scan, MIDI UART, ADC, I2C EEPROM
src/main.c   start-up and main loop
src/config.h settings
test/        host tests and simulator script
```
