# Finding the SRAM (probe image)

[Back to the README](../README.md)

The board has a 32KB static RAM (IC14). The factory schematic puts it at 0x8000-0xFFFF, and the firmware uses it there (see [Memory](technical.md#memory)), but that has not been checked on a real board. `make probe` builds a separate test image that looks for it: `build/probe_27c512.bin` (and `probe_27c256.bin`, `probe_27c128.bin`), flashed like the firmware. It shares the display, LED, key and pedal code with the firmware, and nothing else.

At power-on it tests every 256-byte page of the 64KB external data space. It writes a pattern, writes the opposite pattern to a nearby address, then reads the first address back, so an empty bus (which tends to read back whatever was last written) does not pass as RAM. Below 0x8000 every write also reaches the display and key latches (the schematic decodes them from the low three address bits wherever A15 is 0), so the probe only ever writes at offsets that land on the display latches, never the key and relay latch or the latch resets; the display may flash an odd pattern for a moment while it runs. It then marks each RAM page with its own number to find mirrors (the same chip appearing at two addresses because an address line is not decoded).

It shows its results as a list of 16-bit values, one at a time. Each value cycles through its position in the list (decimal, no dots), its high byte with a trailing dot (`80.`), its low byte with a leading dot (`.00`), then a blank:

| Value | Meaning |
|---|---|
| 0 | Number of distinct RAM pages of 256 bytes: `00.` `.80` for 32KB, `00.` `.00` for none |
| 1, 2, 3 | First block of RAM: start address, end address, and the address it mirrors (its own start if it is not a mirror) |
| 4, 5, 6 ... | The next block, and so on (up to 8 blocks) |

The schematic puts the SRAM at 0x8000-0xFFFF (chip select from A15 through an inverter), so expect value 0 = 0080, then 8000, FFFF, 8000, and nothing more. The footswitch LED of the value is lit (switch 1 for value 0, up to switch 10 for value 9; SWITCH 1 also lights for values 10 to 19, SWITCH 2 for 20 and up). Move expression pedal A from heel (value 0) to toe (the last value), tap UP or DOWN for the next or previous value, or press footswitch n for value n - 1.

It also sends the whole page map from MIDI OUT as one SysEx message, at power-on and again whenever any switch is held for 0.6 seconds: `F0 7D 52 41 4D`, then three bytes per page from 0x00 to 0xFF (0 no RAM, 1 RAM, 2 mirror; then the page it mirrors as high and low nibbles), then the distinct page count as two bytes (count / 16, count % 16), then `F7`.

If it finds nothing, the RAM may be switched in by a port pin rather than by address alone, and its chip select would need tracing on the board. The probe has been run in the uCsim simulator (whose 64KB of external RAM it reports as one block of 0x0100 pages) and tested on the computer against modelled RAM layouts and an empty bus (`make test`), but not on a real FCB1010.
