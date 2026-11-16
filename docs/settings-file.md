# Settings file format

[Back to the README](../README.md)

**Save to file** in the "Configure over MIDI" panel of the [emulator page](https://riban-bw.github.io/FCB1010_Firmware/emulator/) writes the pedal's settings to a file. Type the file name in the **File** box (default `fcb1010-settings.json`; `.json` is added if the name has no extension). In Chrome, Edge and Opera a save dialog opens, starting with that name, so you can choose the folder and change the name there. Other browsers (Firefox, Safari) download the file under that name, into the downloads folder, or ask where if the browser is set to ask where to save downloads. **Load from file** opens the browser's file chooser and **Load from file** sends a file's settings to the pedal (see [Configuring from a computer](configuration.md#configuring-from-a-computer)). The file is plain JSON, so it can be written or edited by hand or by another program.

A file holds the shared settings and the footswitches of one bank: the bank that was in use when it was saved. Loading it fills the bank in use now, so the same file can set up any bank. The pedal calibration is not in the file, since it belongs to one pedal and cannot be set over MIDI.

## Example

The settings out of the box (footswitches 3 to 9 left out here; a real file always has all ten):

```json
{
  "format": "fcb1010-settings",
  "version": 1,
  "channel": 1,
  "footswitches": [
    { "mode": "stomp", "type": "cc", "number": 21, "on": 127, "off": 0 },
    { "mode": "stomp", "type": "cc", "number": 22, "on": 127, "off": 0 },
    ...
    { "mode": "stomp", "type": "cc", "number": 30, "on": 127, "off": 0 }
  ],
  "relays": [
    { "follows": [], "followsUpDown": false, "pedalA": 0, "pedalB": 0 },
    { "follows": [], "followsUpDown": false, "pedalA": 0, "pedalB": 0 }
  ],
  "bankProgram": null,
  "firmware": "00.02.01"
}
```

## Fields

| Field | Value |
|---|---|
| `format` | Always `"fcb1010-settings"` |
| `version` | Always `1`, the version of this format (not of the firmware) |
| `channel` | The pedal's MIDI channel, 1 to 16 |
| `footswitches` | Exactly 10 entries, footswitch 1 first (see below) |
| `relays` | Exactly 2 entries, SWITCH 1 then SWITCH 2 (see below) |
| `bankProgram` | The program change (0 to 127) the bank sends when it is chosen with UP / DOWN (configuration page 7), or `null` for none. Optional: a file without it leaves the bank's setting as it is (files saved before it existed, or from firmware without it) |
| `firmware` | The firmware version the settings were read from, as major.minor.build in hex (`"00.02.01"`). Only written when the settings came from a Read of the pedal; ignored when loading |

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
| `followsUpDown` | `true` if the relay follows program mode (DIRECT SELECT lit) |
| `pedalA`, `pedalB` | The pedal's trigger value, 1 to 127 (the relay closes from that value up), or 0 for not followed |

Numbers must be whole numbers. Other fields are ignored.

## Loading

The whole file is checked before anything is sent. A file with the wrong `format` or `version`, the wrong number of footswitches or relays, an unknown mode or type, or a value out of range is refused with the reason, and the pedal is left as it was.

The settings are then sent as NRPN messages (see [Configuration over MIDI](midi.md#configuration-over-midi)) on the channel the pedal is on now, with the new channel last, so the pedal hears everything before it moves. If the panel has read the pedal first, only the settings that differ are sent; otherwise all of them. Like any change over MIDI they apply at once but are not stored: press **Store** to keep them after power-off. With a MIDI input connected, the panel then reads the pedal back.
