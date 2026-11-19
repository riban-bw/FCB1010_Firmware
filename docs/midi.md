# MIDI implementation

[Back to the README](../README.md)

What the pedal sends and receives, and the SysEx messages that configure it.

## Messages

| Message | Sent | Received |
|---|---|---|
| Control change, footswitches | CC 21 to 30 (`STOMP_CC_LIST`), 127 ON, 0 OFF by default; any controller number, ON value and OFF value per switch | A CC on channel 1 with a CC switch's controller number sets that switch: its ON value is ON, its OFF value OFF, and any other value 64 to 127 ON, 0 to 63 OFF |
| Note on / off, footswitches | Note switches: note on (0x9n) with the ON value as velocity when ON, note off (0x8n) with the OFF value as velocity when OFF | A note on with velocity 1 to 127 sets a note switch with that note ON; a note off, or a note on with velocity 0, sets it OFF |
| Control change, pedals | CC 7 and CC 11 (`PEDAL_CC_LIST`) by default, any controller per pedal and bank (configuration page 3, parameters 96 and 97), 0 to 127 | Ignored, except that a pickup pedal waits for its controller's value (see [Expression pedals](using.md#expression-pedals)) |
| Program change | 0 to 127, on release of UP / DOWN; on choosing a footswitch bank that has a program change set (configuration page 6); a program change footswitch sends its ON value as the program when it turns ON and its OFF value when it turns OFF (with both the same, that program on every press) | Ignored |
| SysEx settings (`F0 7D 46 43 ...`) | A settings dump, when asked | Settings, commands and dump requests (see [Configuration over MIDI](#configuration-over-midi)) |
| SysEx Identity Reply / Request | The Identity Reply, when asked and once at power-on | The Identity Request (see [Device identity](#device-identity)) |
| Active sensing (FE) | Every 250ms when turned on (configuration page 7) | Ignored |
| MIDI clock, Start, Continue, Stop, Song Position Pointer | Nothing | The visual metronome, when it is on (configuration page 7; see [Misc settings](configuration.md#misc-settings)) |

- Everything is on one channel: `MIDI_CHANNEL` until a channel is saved in configuration mode.
- MIDI OUT sends every status byte unless running status is turned on (configuration page 7): then a channel message whose status byte is the same as the last one sent leaves it out. A SysEx message (the identity or a settings dump) clears it, so the next message has its status byte.
- Received messages only set the pedal's own state and LEDs; nothing is echoed to MIDI OUT. The next footswitch press toggles from the received state: if CC 21 turned switch 1 ON, pressing switch 1 sends CC 21 value 0.
- A received CC or note sets every footswitch with it, and so does one a footswitch sends (in run mode): footswitches sharing a CC or note share its state. A bank change passes each CC's and note's state on to the new bank's footswitches by the same rule and sends nothing but the bank's program change, if set.
- The receiver handles running status and split messages, takes real-time bytes anywhere (clock, Start, Continue and Stop drive the visual metronome; active sensing is ignored), ignores SysEx other than the Identity Request and its own settings messages, other channels and other controller numbers. Received messages are ignored in configuration mode, except the Identity Request.
- Pedal positions and the program number are not set by MIDI IN, and no state is remembered at power-off.
- A pedal sends a new value only after its reading moves `PEDAL_HYST` counts, and readings near the calibrated ends snap to 0 and 127 (`PEDAL_END_SNAP`).
- If MIDI OUT is full, a footswitch press is ignored rather than changing a state the receiver never hears about. A stomp with momentary revert that cannot be sent at release is sent as soon as there is room.

## Device identity

The pedal answers the standard MIDI Identity Request (Universal Non-Realtime SysEx, General Information), so MIDI software can find it and read its firmware version:

```
F0 7E <device> 06 01 F7
```

`<device>` is 7F (all devices) or the pedal's MIDI channel minus 1 (00 for channel 1); a request for any other device is ignored. It is answered in run mode and in configuration mode, as soon as MIDI OUT has room for the reply. The pedal also sends the reply once at power-on:

```
F0 7E <channel - 1> 06 02 7D 46 43 31 30 <major> <minor> <build> <high bits> F7
```

| Bytes | Meaning |
|---|---|
| `7D` | Manufacturer ID (`SYSEX_ID` in `config.h`): 7D is the ID set aside for non-commercial use; see [The SysEx ID](#the-sysex-id) |
| `46 43` | Device family: "FC" in ASCII |
| `31 30` | Family member: "10" in ASCII |
| `<major> <minor> <build>` | The firmware version (as page 8 shows it), bits 0 to 6 of each |
| `<high bits>` | Bit 7 of the major (bit 0), minor (bit 1) and build (bit 2) numbers, for versions and builds of 128 and up |

Any other SysEx message, except the pedal's own settings messages (below), is ignored.

### The SysEx ID

SysEx messages carry a manufacturer ID so that each maker's devices can tell their own messages from everyone else's. IDs are assigned, and sold, by the MIDI Association. This project has none of its own, so it uses 7D, the ID the MIDI specification sets aside for non-commercial use. Strictly, the MIDI Association says 7D should not be used on a product released to the public, and publishing the firmware could count as that; but it is what most hobby and open-source projects do, and the only risk is another 7D device on the same MIDI connection taking the pedal's messages for its own. The bytes after it, `46 43` ("FC") and, in the identity, `31 30` ("10"), make that unlikely. 7C, the newer non-commercial ID that is allowed on public products, does not fit: it may only be used for MIDI 2.0 endpoint discovery, not for a device's own messages such as the settings below.

The ID is `SYSEX_ID` in `config.h` (one byte, 01 to 7D), used by the identity reply and every settings message, so a build with an assigned ID needs only that change. The editor page takes it from the firmware it runs; the zynthian driver has its own copy (`SYSEX_ID_MANUFACTURER`), to change with it. The examples on this page show 7D.

## Configuration over MIDI

Settings travel as SysEx messages, which other devices and software ignore, so they cannot disturb anything else on the MIDI connection (the protocol used NRPN controllers before; they are ignored now). Every message starts `F0 7D 46 43`: 7D is the ID set aside for non-commercial use, and `46 43` is "FC", as in the [identity](#device-identity). Then comes the device, 7F for any pedal or the pedal's MIDI channel minus 1, and a command:

| Message | Meaning |
|---|---|
| `F0 7D 46 43 <device> 01 <bank> F7` | Ask for a settings dump of a bank: 0 the bank in use (as it is now, saved or not), or 1 to the number of banks (as stored, without changing bank; a bank never stored gives its defaults) |
| `F0 7D 46 43 <channel - 1> 02 <66 data bytes> F7` | The dump (sent only), 69 bytes, about 22ms on the cable; below |
| `F0 7D 46 43 <device> 03 <parameter> <value> ... F7` | Set parameters: up to 106 parameter and value pairs (the table below). The pedal keeps the message until its F7 and then applies the pairs in order, so a message cut short, with an odd count or with more than 106 pairs changes nothing |
| `F0 7D 46 43 <device> 04 <bank> <52 data bytes> F7` | Write a bank: stores the 52 bytes, laid out as the last 52 data bytes of a dump (from the footswitches to the pedal ranges), into bank 1 to the number of banks, without changing bank. 60 bytes, about 19ms on the cable. If it is the bank in use, its settings are loaded from them at once (unsaved changes to it are dropped). The pedal keeps the message until its F7, so a message cut short, of the wrong length or for a bank there is not changes nothing |

The dump's data bytes, after the command byte:

| Bytes | Meaning |
|---|---|
| 1 | The bank described, 1 to the number of banks |
| 1 | MIDI channel, 1 to 16 |
| 3 + 1 | Firmware major, minor and build (bits 0 to 6), then a byte with their bit 7s (bit 0 major, 1 minor, 2 build), as in the identity |
| 1 | The bank in use |
| 1 | The number of banks |
| 1 | LED and display brightness, 0 to 127 |
| 1 | Misc: bit 0 MIDI clock display, bit 1 running status, bit 2 active sensing |
| 4 x 10 | Footswitches 1 to 10: flags (bits 0-1 message type, 2-3 mode, 4 SWITCH 1 follows it, 5 SWITCH 2 follows it), number, ON value, OFF value |
| 4 | Triggers: SWITCH 1 pedal A, pedal B, SWITCH 2 pedal A, pedal B (0 off) |
| 1 | Pedals: bit 0 pedal A pickup, bit 1 pedal B pickup, bit 2 the bank sends its program change |
| 1 | The bank's program change, 0 to 127 (127 while it is off) |
| 2 | Pedal A's and pedal B's controller numbers |
| 4 | Pedal ranges: the value pedal A sends at the heel, pedal B at the heel, pedal A at the toe, pedal B at the toe |

Parameters for a set, with the values each takes (the dump holds the same settings):

| Parameter | Setting | Values |
|---|---|---|
| 0 | MIDI channel | 1 to 16; the pedal listens on the new channel at once |
| 1, 2, 3 | Firmware major, minor and build (read only) | |
| 4 | Command | 2 save, 3 discard unsaved changes, 6 reset every setting of every bank to its default and store it (the calibration is kept; about 2 seconds, the pedal does nothing else meanwhile) |
| 6 | Footswitch bank | 1 to `BANK_COUNT`; setting it changes bank, as UP / DOWN do while DIRECT SELECT is dark |
| 7 | Number of banks, `BANK_COUNT` (read only) | 1 to 30 |
| 8 to 17 | Footswitch 1 to 10 mode (of the bank in use, as are parameters 20 to 87, 92 to 97 and 102 to 105) | 0 stomp, 1 stomp with momentary, 2 momentary, 3 inverted momentary |
| 20 to 29 | Footswitch 1 to 10 message type | 0 CC, 1 note, 2 program change |
| 30 to 39 | Footswitch 1 to 10 number | 0 to 127: controller or note (not used by a program change) |
| 40 to 49 | Footswitch 1 to 10 ON value | 0 to 127: CC value, note on velocity, or the program sent ON |
| 50 to 59 | Footswitch 1 to 10 OFF value | 0 to 127: CC value, note off velocity, or the program sent OFF |
| 60 to 69 | SWITCH 1 follows footswitch 1 to 10 | 0 / 1 |
| 72 to 81 | SWITCH 2, the same | 0 / 1 |
| 84, 85 | SWITCH 1 trigger, pedal A, pedal B | 1 to 127, 0 off |
| 86, 87 | SWITCH 2 trigger, pedal A, pedal B | 1 to 127, 0 off |
| 92 | The program change the bank in use sends when it is chosen (configuration page 6) | 0 to 127; setting it also turns it on. Reads 127 while it is off |
| 93 | Whether the bank in use sends that program change | 0 none (`--` on the pedal), 1 on (with the program parameter 92 reads) |
| 94, 95 | Expression pedal A, pedal B response (configuration page 3, the pedal editor's SELECT step) | 0 immediate, 1 pickup |
| 96, 97 | Expression pedal A, pedal B controller number (configuration page 3) | 0 to 127 |
| 98 | LED and display brightness (configuration page 8), shared | 0 (the minimum) to 127 (full) |
| 99 | MIDI clock display, the visual metronome (configuration page 7), shared | 0 off, 1 on |
| 100 | Running status on MIDI OUT (configuration page 7), shared | 0 off, 1 on |
| 101 | Active sensing on MIDI OUT (configuration page 7), shared | 0 off, 1 on |
| 102, 103 | The value expression pedal A, pedal B sends at the heel (VALUE 1 in the pedal editor, configuration page 3) | 0 to 127; 0 by default |
| 104, 105 | The value pedal A, pedal B sends at the toe (VALUE 2) | 0 to 127; 127 by default. The pedal's travel is spread evenly from the heel value to the toe value, either way round |

- A set takes effect at once but is not saved; command 2 saves everything, as leaving configuration mode does, and command 3 reloads what is saved. Unknown parameters, read-only ones, out-of-range values and values equal to the current one are ignored.
- A footswitch whose mode or message changes goes to its resting state on the next 2ms tick (as on a save in configuration mode). If it was ON when its message changed, the old message's OFF is sent first (not for a program change).
- The pedal calibration is not available over MIDI: it belongs to the unit's own pedals and is set only on configuration page 3, so settings sent from another unit cannot spoil it. Commands 1 (the old NRPN dump), 4 and 5 and parameters 88 to 91 (calibration, in earlier firmware) are ignored.
- A dump goes out as soon as MIDI OUT has room for all of it (it is sent whole, in one message). Another bank's settings are read from the EEPROM first (about 10ms). The pedal never answers a set or a write, so a MIDI loop cannot make it talk to itself, and a dump fed back to it changes nothing.
- A bank change (parameter 6) happens on the next tick, once any pending stomp with momentary revert has gone out, and sends only the new bank's program change, if it has one (configuration page 6, or parameters 92 and 93); the new bank's pedal controllers, pedal responses and relay settings apply from then on. A dump asked for meanwhile waits for it and describes the new bank, but sets sent before it finishes apply to the old bank and are lost, so read back (or wait about 50ms) before setting the new bank's footswitches. Unsaved changes to the old bank's settings are dropped; save (command 2) first to keep them. The states passed on to the new bank are worked out from the old bank's saved records, so with unsaved message changes they follow the saved messages.
- A pedal in pickup mode sends nothing after its controller's value has changed elsewhere (a footswitch sending it, or a CC received on MIDI IN; at power-on it counts as 0) until the pedal reaches that value or passes it. Then it sends as usual. Turning pickup on while the pedal is the last thing that set its controller changes nothing until the value changes elsewhere. After a change of controller (parameters 96 and 97, or a bank whose pedal sends another one) a pickup pedal waits for the new controller's value.
- A bank write stores the bank's EEPROM records (twelve 5-byte records, 15 or so EEPROM write cycles: about 70ms with a 5ms EEPROM, and up to about 150ms with a slow one), and the pedal does nothing else meanwhile. Its MIDI IN queue holds 128 bytes, so it can take one more write while it stores one, but not more. To restore every bank, either ask for a dump of each bank after writing it (the dump goes out only once the write is done, and shows what was stored), or leave about 200ms between writes. The editor page does the first when it has a MIDI input, the second when it has only an output. Restoring all 30 banks then takes about 5 to 6 seconds.
- A single message with every bank in it would not be much quicker: the bytes take about 0.6 seconds on the cable for 30 banks, and the EEPROM writes take the rest either way. It would also need the pedal to hold 1.5KB before writing anything, and one error would lose the lot, so the protocol writes one bank at a time.
- Settings and dump requests are taken in run mode only; in configuration mode they are ignored, and entering it cancels a dump asked for. A save over MIDI blocks the pedal for the EEPROM writes (tens of ms), so wait for it before sending more.
