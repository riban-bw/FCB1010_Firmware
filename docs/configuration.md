# Configuration

[Back to the README](../README.md)

Hold UP and DOWN together for 1 second. The CONFIG LED lights. It flashes while any setting differs from what is saved in the pedal's memory, and stops flashing if you revert every change. If it flashes as soon as you open configuration mode, the last save did not reach the persistent memory. Footswitches and pedals send no MIDI while you are in configuration mode, and incoming MIDI is ignored.

Tap UP or DOWN to move to the next or previous page (1 to 8). To leave, press UP + DOWN together (saves at once), or hold UP (ENTER) for 0.6 seconds, to save, or hold DOWN (ESCAPE) for 0.6 seconds to cancel every change. Configuration mode always opens on page 1, and each page is marked by a panel LED.

| Page | Panel LED | Display | Controls |
|---|---|---|---|
| 1. Pedal modes | MIDI FUNCTION | Mode of the selected footswitch (1 to 4 - see [Footswitch modes](using.md#footswitch-modes)) | Press a footswitch to select it (its LED lights) and see its mode; press it again to change its mode. Hold a footswitch for 0.6 seconds to open its editor (see below). The modes and messages belong to the footswitch bank in use. |
| 2. MIDI channel | MIDI CHANNEL | Channel 1 to 16 | Footswitches 1 to 10 pick channels 1 to 10. Switch 10 flashes `0` for 5 seconds; press 1 to 6 for 11 to 16, or 10 again for 10. After 5 seconds, or on a page change, the channel stays as 10. |
| 3. Heel calibration | EXP 1 and EXP 2 | `Lo`, then `0` once the reading is taken | Put both pedals at the heel and press any footswitch. |
| 4. Toe calibration | EXP 1 and EXP 2 | `Hi`, then `0` once the reading is taken, or `Er` | Put both pedals at the toe and press any footswitch. `Er` means a pedal hardly moved between heel and toe; press a footswitch to try again. Calibrating with the heel and toe swapped reverses the pedal. |
| 5. SWITCH 1 relay | SWITCH 1 | Blank, or the trigger point of the pedal last moved | Press footswitches to choose which ones the relay follows (their LEDs light). Move a pedal to the point where the relay should close; move it to the heel to stop following it (EXP 1 / EXP 2 light while a pedal is followed). |
| 6. SWITCH 2 relay | SWITCH 2 | As page 5 | As page 5, for the SWITCH 2 relay. |
| 7. Bank program change | DIRECT SELECT | The program (0 to 127), or `--` for none | The program change sent when you choose this bank (the bank in use) with UP and DOWN (while DIRECT SELECT is dark). Type it with the footswitches (1 to 9 are digits 1 to 9, 10 is 0), as in the footswitch editor, or move either expression pedal. For none, press and release footswitch 10 when no number is being typed (the display shows `--`, its right-hand dot flashing), then press UP (ENTER); DOWN (ESCAPE) instead keeps the program, and a footswitch starts a new number (10 is then 0). Each bank has its own, saved with the bank. |
| 8. Lamp test and version | All | "riban", then the firmware version | Every LED lights while ". riban ." scrolls across the display, the dots running in from the right and off at the left (2 seconds), then the firmware version shows in three parts: major with a trailing dot (`00.`), minor with dots both sides (`.02.`), and build with a leading dot (`.04`), then it repeats. Each of the ten steps blinks its own footswitch LED, 1 to 10 in turn: the scroll frames " r" to "an" (1 to 5), "n " with the trailing dot running off (6), the three version parts (7 to 9), and a blank followed by the leading dot coming in (10). Hold a footswitch to show its step, for example 8 for the minor version; let go to carry on. |

## Footswitch editor

On page 1, hold a footswitch to edit its message. That footswitch's LED stays lit so you can see which one you are editing. Tap UP and DOWN to move between its settings. A panel LED shows which one you are on:

| Setting | Panel LED | Display | How to set it |
|---|---|---|---|
| Message type | SELECT | `CC`, `nt` (note) or `Pr` (program change) | Footswitch 1 for CC, 2 for note, 3 for program change. Or move either expression pedal: the bottom third is CC, the middle third note, the top third program change. |
| Number | NUMBER | 0 to 127 | The controller number or note number. A program change has no number, so UP and DOWN skip it. Type it with the footswitches (1 to 9 are digits 1 to 9, 10 is 0), for example 6 then 10 for 60, or move either expression pedal. |
| ON value | VALUE 1 | 0 to 127 | The value a CC sends when the switch turns ON, or the note on velocity. For a program change, the program sent when it turns ON. Set it the same way. |
| OFF value | VALUE 2 | 0 to 127 | The value a CC sends when the switch turns OFF, or the note off velocity. For a program change, the program sent when it turns OFF (the same as the ON value for a single program). Set it the same way. |

A number is complete once no more digits would fit (13 and up, or three digits), so the next digit you press starts a new number. Hold UP (ENTER) for 0.6 seconds to keep your changes and go back to page 1, or hold DOWN (ESCAPE) for 0.6 seconds to put the old values back. Like other settings, the message is only stored when you save and leave configuration mode. If the switch was ON when you changed its message, the old message's OFF is sent and the switch turns OFF.

When you save, each footswitch whose mode or message you changed is put in its new mode's resting state, so it does not carry a stale state into its new mode: ON for inverted momentary, OFF for the other modes. If that changes the switch, its new value is sent, so the receiving device stays in step. If a save fails it is not reported: the new settings apply until power-off.

## Configuring from a computer

Every setting can also be changed over MIDI (see [MIDI implementation](midi.md#configuration-over-midi) for the messages), from the "Configure over MIDI" panel on the [emulator page](https://riban-bw.github.io/FCB1010_Firmware/emulator/). Connect a MIDI interface: its output to the pedal's MIDI IN and, if you want to read the pedal's settings, its input from the pedal's MIDI OUT. Open the page in a browser with Web MIDI (Chrome or Edge), choose "Real pedal", allow MIDI access and pick the ports (with only one of each they are picked for you). The output alone is enough to change settings, load a file and store; the input is only needed for Read and Find. Without it, set "On channel" to the channel the pedal is on now; the panel starts with the emulator's settings until you read the pedal or load a file. The same panel can configure the emulated pedal instead, to try it out.

- **Read** loads the pedal's settings into the panel (it needs the MIDI input). If you do not know which channel the pedal is on, press **Find**: it asks on every channel and only the pedal answers.
- Change anything in the panel: footswitch modes and messages, the pedal's MIDI channel, the relays. The footswitches shown are those of the bank in the **bank** box, and **program change** next to it is that bank's program change (`--` for none, as on page 7); choosing another bank switches the pedal to it (as UP and DOWN do while DIRECT SELECT is dark) and reads that bank, so store any changes to a bank before leaving it. Each change works on the pedal straight away, just as if you had made it in configuration mode, and a footswitch whose mode or message changes goes to its resting state.
- **Store** keeps the changes in the pedal after power-off. **Discard changes** puts back what was last stored.
- **Save to file** saves the settings to a file on your computer, named in the **File** box (in Chrome, Edge and Opera a save dialog lets you choose the folder and name), and **Load from file** sends a saved file's settings to the pedal (press **Store** afterwards to keep them). Use it to keep a backup, or several setups to swap between. The file holds one bank's footswitches and program change and the shared settings, everything except the pedal calibration, which belongs to each pedal; loading it fills the bank in use. It is a readable JSON file, so it can also be edited by hand (see [Settings file format](settings-file.md)).
- **Set heel** and **Set toe** calibrate the expression pedals: put both pedals at the heel (or toe) and press the button.

The pedal only listens for settings while it is playing (not in configuration mode) and only on its own channel. It never answers a change, only a Read, so it cannot get stuck in a loop if its MIDI OUT is wired back to its MIDI IN.
