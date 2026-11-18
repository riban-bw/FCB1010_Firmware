# MIDI implementation

[Back to the README](../README.md)

What the pedal sends and receives, and the NRPN messages that configure it.

## Messages

| Message | Sent | Received |
|---|---|---|
| Control change, footswitches | CC 21 to 30 (`STOMP_CC_LIST`), 127 ON, 0 OFF by default; any controller number, ON value and OFF value per switch | A CC on channel 1 with a CC switch's controller number sets that switch: its ON value is ON, its OFF value OFF, and any other value 64 to 127 ON, 0 to 63 OFF |
| Note on / off, footswitches | Note switches: note on (0x9n) with the ON value as velocity when ON, note off (0x8n) with the OFF value as velocity when OFF | A note on with velocity 1 to 127 sets a note switch with that note ON; a note off, or a note on with velocity 0, sets it OFF |
| Control change, pedals | CC 7 and CC 11 (`PEDAL_CC_LIST`) by default, any controller per pedal and bank (configuration pages 3 and 4, parameters 96 and 97), 0 to 127 | Ignored, except that a pickup pedal waits for its controller's value (see [Expression pedals](using.md#expression-pedals)) |
| Program change | 0 to 127, on release of UP / DOWN; on choosing a footswitch bank that has a program change set (configuration page 7); a program change footswitch sends its ON value as the program when it turns ON and its OFF value when it turns OFF (with both the same, that program on every press) | Ignored |
| NRPN (CC 99, 98, 6, 38) | Every setting, when asked for a dump | Settings and commands (see [Configuration over MIDI](#configuration-over-midi)) |
| SysEx Identity Reply / Request | The Identity Reply, when asked | The Identity Request (see [Device identity](#device-identity)) |

- Everything is on one channel: `MIDI_CHANNEL` until a channel is saved in configuration mode.
- Received messages only set the pedal's own state and LEDs; nothing is echoed to MIDI OUT. The next footswitch press toggles from the received state: if CC 21 turned switch 1 ON, pressing switch 1 sends CC 21 value 0.
- A received CC or note sets every footswitch with it, and so does one a footswitch sends (in run mode): footswitches sharing a CC or note share its state. A bank change passes each CC's and note's state on to the new bank's footswitches by the same rule and sends nothing but the bank's program change, if set.
- The receiver handles running status and split messages, ignores real-time bytes (clock, active sensing), SysEx other than the Identity Request, other channels and other controller numbers. Received messages are ignored in configuration mode, except the Identity Request.
- Pedal positions and the program number are not set by MIDI IN, and no state is remembered at power-off.
- A pedal sends a new value only after its reading moves `PEDAL_HYST` counts, and readings near the calibrated ends snap to 0 and 127 (`PEDAL_END_SNAP`).
- If MIDI OUT is full, a footswitch press is ignored rather than changing a state the receiver never hears about. A stomp with momentary revert that cannot be sent at release is sent as soon as there is room.

## Device identity

The pedal answers the standard MIDI Identity Request (Universal Non-Realtime SysEx, General Information), so MIDI software can find it and read its firmware version:

```
F0 7E <device> 06 01 F7
```

`<device>` is 7F (all devices) or the pedal's MIDI channel minus 1 (00 for channel 1); a request for any other device is ignored. It is answered in run mode and in configuration mode, as soon as MIDI OUT has room for the reply:

```
F0 7E <channel - 1> 06 02 7D 46 43 31 30 <major> <minor> <build> <high bits> F7
```

| Bytes | Meaning |
|---|---|
| `7D` | Manufacturer ID: 7D is the ID set aside for non-commercial use |
| `46 43` | Device family: "FC" in ASCII |
| `31 30` | Family member: "10" in ASCII |
| `<major> <minor> <build>` | The firmware version (as page 8 shows it), bits 0 to 6 of each |
| `<high bits>` | Bit 7 of the major (bit 0), minor (bit 1) and build (bit 2) numbers, for versions and builds of 128 and up |

Any other SysEx message is ignored.

## Configuration over MIDI

Settings travel as NRPN controllers on the pedal's channel, so any MIDI tool can send them. CC 99 (NRPN MSB) with value 70 (0x46) and CC 98 (NRPN LSB) with the parameter number select a parameter, in either order; CC 6 (data entry) then sets it, and further CC 6 messages set it again. CC 99 with any other value, or CC 101 / 100 (RPN select), deselects, so other NRPN traffic on the channel is left alone. CC 38 is ignored when received.

| Parameter (CC 98) | Setting | Values |
|---|---|---|
| 0 | MIDI channel | 1 to 16; the pedal listens on the new channel at once |
| 1, 2, 3 | Firmware major, minor and build (read only) | 0 to 255, sent as CC 6 (bit 7) and CC 38 (bits 0 to 6) |
| 4 | Command (never sent) | 1 dump, 2 save, 3 discard unsaved changes, 4 heel from the pedals' positions, 5 toe from the pedals' positions |
| 5 | End of a dump (sent only) | 0 |
| 6 | Footswitch bank | 1 to `BANK_COUNT`; setting it changes bank, as UP / DOWN do while DIRECT SELECT is dark |
| 7 | Number of banks, `BANK_COUNT` (read only) | 1 to 30, as CC 6 + CC 38 |
| 8 to 17 | Footswitch 1 to 10 mode (of the bank in use, as are parameters 20 to 87 and 92 to 97) | 0 stomp, 1 stomp with momentary, 2 momentary, 3 inverted momentary |
| 20 to 29 | Footswitch 1 to 10 message type | 0 CC, 1 note, 2 program change |
| 30 to 39 | Footswitch 1 to 10 number | 0 to 127: controller or note (not used by a program change) |
| 40 to 49 | Footswitch 1 to 10 ON value | 0 to 127: CC value, note on velocity, or the program sent ON |
| 50 to 59 | Footswitch 1 to 10 OFF value | 0 to 127: CC value, note off velocity, or the program sent OFF |
| 60 to 69 | SWITCH 1 follows footswitch 1 to 10 | 0 / 1 |
| 72 to 81 | SWITCH 2, the same | 0 / 1 |
| 84, 85 | SWITCH 1 trigger, pedal A, pedal B | 1 to 127, 0 off |
| 86, 87 | SWITCH 2 trigger, pedal A, pedal B | 1 to 127, 0 off |
| 88 to 91 | Calibration: heel A, toe A, heel B, toe B (read only) | 0 to 255 raw readings, as CC 6 + CC 38 |
| 92 | The program change the bank in use sends when it is chosen (configuration page 7) | 0 to 127; setting it also turns it on. Reads 127 while it is off |
| 93 | Whether the bank in use sends that program change | 0 none (`--` on the pedal), 1 on (with the program parameter 92 reads) |
| 94, 95 | Expression pedal A, pedal B response (configuration pages 3 and 4) | 0 immediate, 1 pickup |
| 96, 97 | Expression pedal A, pedal B controller number (configuration pages 3 and 4) | 0 to 127 |

- A set takes effect at once but is not saved; command 2 saves everything, as leaving configuration mode does, and command 3 reloads what is saved. Unknown parameters, read-only ones, out-of-range values and values equal to the current one are ignored.
- A footswitch whose mode or message changes goes to its resting state on the next 2ms tick (as on a save in configuration mode). If it was ON when its message changed, the old message's OFF is sent first (not for a program change).
- Commands 4 and 5 take the pedals' current readings as the heel or toe, if both are at least `CAL_MIN_SPAN` from the other end; otherwise nothing changes. Read the calibration back to see whether it took.
- The dump (command 1) sends every parameter except the command and the end marker, one per tick once MIDI OUT has room: CC 99, 98 and 6 with running status, plus CC 38 for the read-only ones, then parameter 5. It takes about 0.2 seconds. A settable value is sent in CC 6 alone, so a dump fed back to the pedal changes nothing. The pedal sends nothing else in reply.
- A bank change (parameter 6) happens on the next tick, once any pending stomp with momentary revert has gone out, and sends only the new bank's program change, if it has one (configuration page 7, or parameters 92 and 93); the new bank's pedal controllers, pedal responses and relay settings apply from then on. A dump asked for meanwhile waits for it, but sets sent before it finishes apply to the old bank and are lost, so read back (or wait about 50ms) before setting the new bank's footswitches. Unsaved changes to the old bank's settings are dropped; save (command 2) first to keep them. The states passed on to the new bank are worked out from the old bank's saved records, so with unsaved message changes they follow the saved messages.
- A pedal in pickup mode sends nothing after its controller's value has changed elsewhere (a footswitch sending it, or a CC received on MIDI IN; at power-on it counts as 0) until the pedal reaches that value or passes it. Then it sends as usual. Turning pickup on while the pedal is the last thing that set its controller changes nothing until the value changes elsewhere. After a change of controller (parameters 96 and 97, or a bank whose pedal sends another one) a pickup pedal waits for the new controller's value.
- Settings are taken in run mode only; in configuration mode they are ignored, and entering it cancels a dump in progress. A save over MIDI blocks the pedal for the EEPROM writes (tens of ms), so wait for it before sending more.
