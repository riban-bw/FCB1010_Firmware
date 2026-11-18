# Settings file format

[Back to the README](../README.md)

**Save to file** in the "Configure over MIDI" panel of the [editor page](https://riban-bw.github.io/FCB1010_Firmware/editor/) writes the pedal's settings to a file. Type the file name in the **File** box (default `fcb1010-settings.json`; `.json` is added if the name has no extension). In Chrome, Edge and Opera a save dialog opens, starting with that name, so you can choose the folder and change the name there. Other browsers (Firefox, Safari) download the file under that name, into the downloads folder, or ask where if the browser is set to ask where to save downloads. **Load from file** opens the browser's file chooser and **Load from file** sends a file's settings to the pedal (see [Configuring from a computer](configuration.md#configuring-from-a-computer)). The file is plain JSON, so it can be written or edited by hand or by another program.

A file holds the global settings, shared by every bank (MIDI channel, brightness, clock display, running status, active sensing), and the settings of one bank (its footswitches, relays, expression pedals and program change): the bank that was in use when it was saved. The editor page's **Copy bank** and **Paste bank** copy a bank's settings from one bank to another without a file, leaving the global settings alone. Loading it fills the bank in use now, so the same file can set up any bank. The pedal calibration is not in the file: it belongs to the expression pedals of one unit, and is only set on the pedal itself (configuration pages 3 and 4), so loading a file from another unit cannot spoil it.

## Example

The settings out of the box (footswitches 3 to 9 left out here; a real file always has all ten):

```json
{
  "format": "fcb1010-settings",
  "version": 2,
  "global": { "channel": 1, "brightness": 127, "clockDisplay": false, "runningStatus": false, "activeSensing": false },
  "footswitches": [
    { "mode": "stomp", "type": "cc", "number": 21, "on": 127, "off": 0 },
    { "mode": "stomp", "type": "cc", "number": 22, "on": 127, "off": 0 },
    ...
    { "mode": "stomp", "type": "cc", "number": 30, "on": 127, "off": 0 }
  ],
  "relays": [
    { "follows": [], "pedalA": 0, "pedalB": 0 },
    { "follows": [], "pedalA": 0, "pedalB": 0 }
  ],
  "bankProgram": null,
  "pedals": [ { "response": "immediate", "cc": 7 }, { "response": "immediate", "cc": 11 } ],
  "firmware": "00.03.00"
}
```

## Fields

| Field | Value |
|---|---|
| `format` | Always `"fcb1010-settings"` |
| `version` | `2`, the version of this format (not of the firmware). Version 1 files, with `channel` at the top instead of `global`, still load |
| `global` | The settings shared by every bank (see below). Optional: a file without it leaves them as they are |
| `footswitches` | Exactly 10 entries, footswitch 1 first (see below) |
| `relays` | Exactly 2 entries, SWITCH 1 then SWITCH 2 (see below) |
| `bankProgram` | The program change (0 to 127) the bank sends when it is chosen with UP / DOWN (configuration page 7), or `null` for none. Optional: a file without it leaves the bank's setting as it is (files saved before it existed, or from firmware without it) |
| `pedals` | Exactly 2 entries, expression pedal A then pedal B, each with `"response"`, `"immediate"` or `"pickup"` (see [Expression pedals](using.md#expression-pedals)), and `"cc"`, the controller number 0 to 127. Optional: a file without it leaves the pedals as they are; it is left out when the pedal did not report it (older firmware). An entry without `"cc"` leaves that pedal's controller as it is; `"cc"` is left out when the pedal did not report it |
| `firmware` | The firmware version the settings were read from, as major.minor.build, major and minor in decimal and the build in hex, as the pedal shows it (`"00.03.00"`). Only written when the settings came from a Read of the pedal; ignored when loading |

The global settings, each optional (left out when the pedal did not report it, from older firmware; a missing one is left as it is):

| Field | Value |
|---|---|
| `channel` | The pedal's MIDI channel, 1 to 16 |
| `brightness` | LED and display brightness, 0 (the minimum) to 127 (full) |
| `clockDisplay` | `true` or `false`: the MIDI clock display (visual metronome) |
| `runningStatus` | `true` or `false`: running status on MIDI OUT |
| `activeSensing` | `true` or `false`: active sensing on MIDI OUT |

Each footswitch:

| Field | Value |
|---|---|
| `mode` | `"stomp"`, `"stomp + momentary"`, `"momentary"` or `"inverted momentary"` (see [Footswitch modes](using.md#footswitch-modes)) |
| `type` | `"cc"`, `"note"` or `"program"` (see [Footswitch messages](using.md#footswitch-messages)) |
| `number` | 0 to 127: the controller or note number. Required for a program change too, though it is not used |
| `on` | 0 to 127: the CC value, note on velocity, or program sent when the switch turns ON |
| `off` | 0 to 127: the CC value, note off velocity, or program sent when the switch turns OFF (for a program change, the same as `on` makes a single program) |

Each relay:

| Field | Value |
|---|---|
| `follows` | The footswitches (1 to 10) the relay follows, in any order, for example `[1, 4]`; `[]` for none |
| `pedalA`, `pedalB` | The pedal's trigger value, 1 to 127 (the relay closes from that value up), or 0 for not followed |

Files from older firmware may have a `followsUpDown` field (relays following program mode, since removed); it is ignored.

Numbers must be whole numbers. Other fields are ignored.

## Loading

The whole file is checked before anything is sent. A file with the wrong `format` or `version`, the wrong number of footswitches or relays, an unknown mode, type or pedal response, a global setting of the wrong kind, or a value out of range (a pedal `cc` over 127, for example) is refused with the reason, and the pedal is left as it was.

The settings are then sent as one SysEx set message (see [Configuration over MIDI](midi.md#configuration-over-midi)), with the new channel last; the pedal applies the message only once it has all of it. If the panel has read the pedal first, only the settings that differ are sent; otherwise all of them. Like any change over MIDI they apply at once but are not stored: press **Store** to keep them after power-off. With a MIDI input connected, the panel then reads the pedal back.
