# One ROM plugin: switching FCB1010 firmware images

**Draft.** This plugin compiles and links against One ROM's plugin API, and its frame decoder and the 8051 sending code are tested on the host and in a simulator. It has never run on a One ROM or in an FCB1010.

With a [One ROM](https://onerom.org) in the FCB1010's program socket holding several firmware images, this plugin lets the running FCB1010 firmware switch to another image, for example the stock Behringer firmware, without any extra wiring. Power-cycling the pedal returns to One ROM's boot image (this replacement firmware).

## Why not RBCP

One ROM's host-control plugin implements RBCP, which signals by reading ROM addresses. An 8051 cannot drive it. The 8051 runs from the same ROM it would be signalling to, so its code fetches fall between every data read. RBCP's knock and its command mode take every read in turn, so those fetches would be read as protocol bytes. This plugin looks only at reads in one reserved 256-byte page that the 8051 never runs code from, so its code fetches are ignored.

## Protocol

The reserved page is the top 256 bytes the chip can address: 0x7F00-0x7FFF on a 32KB chip. A read of 0x7F00 + *n* sends byte *n*. A 16KB chip sees the same reads at 0x3F00-0x3FFF, and on a 27C512 the level of A15 does not matter.

A frame is 15 bytes, and no two consecutive bytes are equal. That lets the plugin drop a read captured twice.

| Bytes | Value | Meaning |
|---|---|---|
| 0-6 | `FCB1010` (0x46 0x43 0x42 0x31 0x30 0x31 0x30) | key |
| 7 | 0xC0 \| command | 1 = switch |
| 8 | slot (0-63) | the flash slot to serve |
| 9 | ~slot | check |
| 10-13 | 0x50 \| X[15:12], 0x60 \| X[11:8], 0x70 \| X[7:4], 0x80 \| X[3:0] | the address X of the 8051's spin loop |
| 14 | 0x90 \| check | low nibble of t ^ (t >> 4), where t = command ^ slot ^ X high byte ^ X low byte |

A byte that does not fit restarts the match.

## The switch, step by step

1. **The 8051 sends the request** (`fcb_onerom_switch(slot)` in `host_8051.c`). It disables interrupts and puts its registers back to their reset values. It then sends the frame three times and spins in this loop at X:

   ```
   X:   DJNZ R7, X     ; DF FE
        DJNZ R6, X     ; DE FC
        DJNZ R5, X     ; DD FA
        LJMP 0000      ; 02 00 00   (after about 1s: restart this firmware)
   ```

2. **The plugin loads the image.** It copies the flash slot into the RAM slot that is not being served.
3. **The plugin waits for the spin.** It waits until 64 captures in a row fall inside X..X+7, so the CPU is in the loop. It gives up after 300ms.
4. **The plugin patches the copy and switches.** In the copy, it saves the 9 bytes at X and replaces them with `00 00 00 00 00 00 02 00 00` (NOPs, then LJMP 0000). It then switches to the copy, which is atomic. The switch can land on any byte of the loop: an opcode fetched from the new image is a NOP, and a DJNZ offset fetched from it is 0, so either way the CPU reaches the LJMP. The new firmware then starts from its reset vector.
5. **The plugin restores the new image.** Once the CPU has fetched from addresses 0-2, it puts the 9 saved bytes back, so the new firmware runs unmodified.

If anything fails (no such slot, copy failed, the CPU never spun), the plugin logs it and does nothing. The 8051's loop then times out and restarts the FCB1010 firmware.

## Requirements

- **One ROM:** a Fire 28-pin One ROM, firmware 0.8.1 or later, with this plugin as its user plugin. It needs at least two RAM slots.
- **Images:** the images in One ROM's flash slots, with slot 0 as the boot image (this firmware). The slot numbers are those of One ROM's flash slots, counting ROM images only and not plugins.
- **Reserved page:** this firmware must have no code or tables at 0x3F00-0x3FFF or 0x7F00-0x7FFF. It is about 10.8KB, so it currently does not.
- **Where to call it:** `fcb_onerom_switch` must be called from the main loop, not from an interrupt.

## Building and testing

```
make ORA=<one-rom checkout>/firmware/ora   # build/plugin_user.bin (needs arm-none-eabi-gcc)
make test                                  # needs sdcc, s51 (uCsim) and cc
```

`make test` runs these checks:
- It assembles `host_8051.c` with SDCC and checks that the spin loop's bytes are the ones the patch assumes.
- It runs the code in uCsim and reads back the frame it built.
- It runs `test/test_frame.c`, which decodes that frame, every slot with spin addresses across the 32KB range, duplicated captures and junk. It also checks that a single changed bit in any byte rejects a frame, and that 20 million random in-page bytes make no frame.

The plugin was compile-checked with clang for the Cortex-M33 against One ROM's headers and linker script, and with GCC's warnings. It was not built with arm-none-eabi-gcc.

## Not verified

- **Speed:** whether the plugin keeps up with the bus. An 8051 at 12MHz makes up to two million reads a second, and the ring holds 64 captures, about 32µs. The main loop costs one AND and compare per capture outside the page.
- **Chip enable:** capture with /CE tied low and only /OE (the 8051's /PSEN) toggling, and the `check_control_pins` filter on such captures. Set `FCB_CHECK_CONTROL_PINS=0` if in-page reads are being dropped.
- **Pulse filter:** One ROM's `addr_mon_min_cs_act_ns` filter against /PSEN pulses of about 200ns.
- **Monitor mode:** whether `ORA_MONITOR_MODE_CONTROL` is needed for `set_active_ram_slot`. host-control uses it.
- **Slot loading:** loading slot 0 into a RAM slot other than 0, and copying into RAM slot 0 when switching back from slot 1.
- **Restart after the switch:** the stock firmware's start-up from a soft jump to 0000. It sees reset values in the registers, but not in its RAM or in the FCB1010's LED and relay latches.

## Integrating with the FCB1010 firmware (not done)

- Add `host_8051.c` to the firmware build.
- Make `tools/check_size.py` refuse code at or above 0x3F00.
- Call `fcb_onerom_switch(slot)` from the main loop on request: either a footswitch held at power-on (switch N selects slot N) or a config-page entry. Clear the display first, and send nothing more on MIDI.
