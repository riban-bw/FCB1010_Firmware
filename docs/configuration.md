# Configuration

[Back to the README](../README.md)

Hold UP and DOWN together for 1 second. The CONFIG LED lights. It flashes while any setting differs from what is saved in the pedal's memory, and stops flashing if you revert every change. If it flashes as soon as you open configuration mode, the last save did not reach the persistent memory. Footswitches and pedals send no MIDI while you are in configuration mode, and incoming MIDI is ignored.

Tap UP or DOWN to move to the next or previous page (1 to 8). To leave, press UP + DOWN together (saves at once), or hold UP (ENTER) for 0.6 seconds to choose a bank to save to (see [Saving](#saving)), or hold DOWN (ESCAPE) for 0.6 seconds to cancel every change. Configuration mode always opens on page 1, and each page is marked by a panel LED.

While you type a number (a footswitch's number or values, a pedal's CC, the bank's program change, or the bank to save to), the dot at the right of the display flashes. In the [editor page](https://riban-bw.github.io/FCB1010_Firmware/editor/), each footswitch shows what it does on the page you are on, under its usual message.

Inside an editor (a footswitch's message, or a pedal's CC) UP and DOWN work with a tap instead: UP (ENTER) keeps the value shown and DOWN (ESCAPE) puts it back, and either takes you back to the page the editor was opened from. Holding them there does the same as a tap, so you cannot leave configuration mode from an editor except with UP + DOWN, which keeps the editor's changes, saves and leaves.

| Page | Panel LED | Display | Controls |
|---|---|---|---|
| 1. Pedal modes | MIDI FUNCTION | Mode of the selected footswitch: `St` (stomp), `+St` (stomp with momentary), `Pu` (momentary, push) or `-Pu` (inverted momentary) - see [Footswitch modes](using.md#footswitch-modes) | Press a footswitch to select it (its LED lights) and see its mode; press it again to change its mode. Hold a footswitch for 0.6 seconds to open its editor (see below). The modes and messages belong to the footswitch bank in use. |
| 2. MIDI channel | MIDI CHANNEL | Channel 1 to 16 | Footswitches 1 to 10 pick channels 1 to 10. Switch 10 flashes `0` for 5 seconds; press 1 to 6 for 11 to 16, or 10 again for 10. After 5 seconds, or on a page change, the channel stays as 10. |
| 3. EXP A | EXP 1 | The setting of the footswitch last pressed (see [Expression pedal pages](#expression-pedal-pages)) | Expression pedal A: footswitch 1 its response (`Ab` immediate, `Pu` pickup), 2 its controller (CC) number, 3 its calibration (heel and toe readings). Each bank has its own response and CC; the calibration is shared. |
| 4. EXP B | EXP 2 | As page 3 | As page 3, for expression pedal B, with the same footswitches. |
| 5. SWITCH 1 relay | SWITCH 1 | Blank, or the trigger point of the pedal last moved | Press footswitches to choose which ones the relay follows (their LEDs light). Move a pedal to the point where the relay should close; move it to the heel to stop following it (EXP 1 / EXP 2 light while a pedal is followed). Each bank has its own, saved with the bank. This is the only page where moving a pedal sets anything. |
| 6. SWITCH 2 relay | SWITCH 2 | As page 5 | As page 5, for the SWITCH 2 relay. |
| 7. Bank program change | DIRECT SELECT | `Pr` and the program (0 to 127), or `--` for none, in turn, a second each | The program change sent when you choose this bank (the bank in use) with UP and DOWN (while DIRECT SELECT is dark). Type it with the footswitches (1 to 9 are digits 1 to 9, 10 is 0), as in the footswitch editor. For none, press and release footswitch 10 when no number is being typed (the display shows `--`, its right-hand dot flashing), then press UP (ENTER); DOWN (ESCAPE) instead keeps the program, and a footswitch starts a new number (10 is then 0). Each bank has its own, saved with the bank. |
| 8. Lamp test and version | All | "riban", then the firmware version | Every LED lights while ". riban ." scrolls across the display, the dots running in from the right and off at the left (2 seconds), then the firmware version shows in three parts (major and minor in decimal, the build in hex): major with a trailing dot (`00.`), minor with dots both sides (`.04.`), and build with a leading dot (`.01`), then it repeats. Each of the ten steps blinks its own footswitch LED, 1 to 10 in turn: the scroll frames " r" to "an" (1 to 5), "n " with the trailing dot running off (6), the three version parts (7 to 9), and a blank followed by the leading dot coming in (10). Hold a footswitch to show its step, for example 8 for the minor version; let go to carry on. |

## Expression pedal pages

Page 3 sets up expression pedal A and page 4 pedal B, with the same footswitches (its EXP LED marks the page):

| Footswitch | Display | What it does |
|---|---|---|
| 1, response | `Ab` (immediate) or `Pu` (pickup) | The first press shows the pedal's response; each press after that switches it between immediate (every move is sent) and pickup (see [Expression pedals](using.md#expression-pedals)). |
| 2, CC | The controller number | The first press shows the pedal's controller number; the next opens its CC editor (below). |
| 3, calibration | The pedal's value, `Lo`, `Hi` or `Er` | The first press shows the pedal's value (0 to 127, as calibrated), following the pedal as you move it. Then each press takes the next step: `Lo` flashes, so put the pedal at the heel; press to take the heel reading, and `Hi` flashes, so put it at the toe; press to take the toe reading, and the value shows again with the new calibration. |

Each page opens showing the pedal's response, with footswitch 1 lit, so a press of footswitch 1 switches it straight away. A footswitch acts when you let go of it, then its LED lights and the display shows its setting. The other footswitches do nothing. If the toe reading is too close to the heel reading, `Er` shows for 2 seconds, then the pedal's value, and the old calibration is kept; press footswitch 3 again to start over. A press while `Er` shows starts over at once. Calibrating with the heel and toe swapped reverses the pedal. Each bank has its own pedal responses and CCs, saved with the bank; a bank never saved uses the default ones (see [Using the pedal](using.md)). The calibration belongs to the pedal and is the same in every bank.

### Pedal CC editor

On page 3 or 4, footswitch 2 pressed a second time opens the editor for that page's pedal's controller number. Its EXP LED, footswitch 2's LED and NUMBER light, and the display shows the number. Type it with the footswitches (1 to 9 are digits 1 to 9, 10 is 0), as in the footswitch editor. Tap UP (ENTER) to keep it or DOWN (ESCAPE) to put the old number back; either way you go back to the pedal's page.

## Footswitch editor

On page 1, hold a footswitch to edit its message. That footswitch's LED stays lit so you can see which one you are editing. The editor opens on the message type, and a panel LED shows which setting you are on:

| Setting | Panel LED | Display | How to set it |
|---|---|---|---|
| Message type | SELECT | `CC`, `nt` (note) or `Pr` (program change, with a small r) | Footswitch 1 for CC, 2 for note, 3 for program change. |
| Number | NUMBER | 0 to 127 | The controller number or note number. A program change has no number, so it is skipped. Type it with the footswitches (1 to 9 are digits 1 to 9, 10 is 0), for example 6 then 10 for 60. |
| ON value | VALUE 1 | 0 to 127 | The value a CC sends when the switch turns ON, or the note on velocity. For a program change, the program sent when it turns ON. Set it the same way. |
| OFF value | VALUE 2 | 0 to 127 | The value a CC sends when the switch turns OFF, or the note off velocity. For a program change, the program sent when it turns OFF (the same as the ON value for a single program). Set it the same way. |

Tap UP (ENTER) to keep the setting shown and go on to the next one; after the OFF value the editor closes and you are back on page 1. Tap DOWN (ESCAPE) to put the setting shown back as it was and go back to page 1; settings you already kept with UP stay. For example, to change only the type: set it, tap UP, then tap DOWN on the number, which leaves the number as it was.

A number is complete once no more digits would fit (13 and up, or three digits), so the next digit you press starts a new number. Like other settings, the message is only stored when you save and leave configuration mode. If the switch was ON when you kept a change to its message, the old message's OFF is sent and the switch turns OFF.

## Saving

Hold UP (ENTER) for 0.6 seconds to save. The display flashes the number of the bank in use, with the dot lit in between, and the page LEDs go dark. Tap UP (ENTER) to save into that bank and leave configuration mode. To save into another bank instead, type its number on the footswitches first (1 to 9 are digits 1 to 9, 10 is 0; for example 1 then 2 for bank 12); tap DOWN (ESCAPE) to go back to the page without saving.

Saving into another bank stores everything you see in configuration mode (the footswitch messages and modes, the relays, the expression pedal settings and the bank's program change) in that bank, and the pedal then plays that bank. The bank you started from keeps what it last had saved. So you can copy a bank: open configuration mode in it and save into another; or use one bank as a template: open it, change what you need and save the result into a new bank. The MIDI channel and the pedal calibration are shared by every bank, and are saved either way. UP + DOWN together always saves into the bank in use at once.

When you save, each footswitch whose mode or message you changed is put in its new mode's resting state, so it does not carry a stale state into its new mode: ON for inverted momentary, OFF for the other modes. If that changes the switch, its new value is sent, so the receiving device stays in step. If a save fails it is not reported: the new settings apply until power-off.

## Configuring from a computer

Every setting can also be changed over MIDI (see [MIDI implementation](midi.md#configuration-over-midi) for the messages), from the "Configure over MIDI" panel on the [editor page](https://riban-bw.github.io/FCB1010_Firmware/editor/). Connect a MIDI interface: its output to the pedal's MIDI IN and, if you want to read the pedal's settings, its input from the pedal's MIDI OUT. Open the page in a browser with Web MIDI (Chrome or Edge), choose "Real pedal", allow MIDI access and pick the ports (with only one of each they are picked for you). The output alone is enough to change settings, load a file and store; the input is only needed for Read and Find. Without it, set "On channel" to the channel the pedal is on now; the panel starts with the editor's settings until you read the pedal or load a file. The same panel can configure the emulated pedal on the page instead, to try it out.

- **Read** loads the pedal's settings into the panel (it needs the MIDI input). If you do not know which channel the pedal is on, press **Find**: it asks on every channel and only the pedal answers.
- Change anything in the panel: footswitch modes and messages, the pedal's MIDI channel, the relays, and each expression pedal's CC and response. The footswitches, relays and pedal settings shown are those of the bank in the **bank** box, and **program change** next to it is that bank's program change (`--` for none, as on page 7); choosing another bank switches the pedal to it (as UP and DOWN do while DIRECT SELECT is dark) and reads that bank, so store any changes to a bank before leaving it. Each change works on the pedal straight away, just as if you had made it in configuration mode, and a footswitch whose mode or message changes goes to its resting state.
- **Copy bank** remembers the settings of the bank shown (its footswitches, relays, expression pedal settings and program change); choose another bank and press **Paste bank** to send them to it. The MIDI channel is left as it is. Like any change, press **Store** to keep the pasted bank.
- **Store** keeps the changes in the pedal after power-off. **Discard changes** puts back what was last stored.
- **Save to file** saves the settings to a file on your computer, named in the **File** box (in Chrome, Edge and Opera a save dialog lets you choose the folder and name), and **Load from file** sends a saved file's settings to the pedal (press **Store** afterwards to keep them). Use it to keep a backup, or several setups to swap between. The file holds one bank's footswitches, relays, expression pedal settings and program change, and the MIDI channel: everything except the pedal calibration, which belongs to each pedal. Loading it fills the bank in use. It is a readable JSON file, so it can also be edited by hand (see [Settings file format](settings-file.md)).
- **Set heel** and **Set toe** calibrate both expression pedals at once: put both pedals at the heel (or toe) and press the button.

The pedal only listens for settings while it is playing (not in configuration mode) and only on its own channel. It never answers a change, only a Read, so it cannot get stuck in a loop if its MIDI OUT is wired back to its MIDI IN.
