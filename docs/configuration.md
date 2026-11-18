# Configuration

[Back to the README](../README.md)

Hold UP and DOWN together for 1 second. The CONFIG LED lights. It flashes while any setting differs from what is saved in the pedal's memory, and stops flashing if you revert every change. If it flashes as soon as you open configuration mode, the last save did not reach the persistent memory. Footswitches and pedals send no MIDI while you are in configuration mode, and incoming MIDI is ignored.

Tap UP or DOWN to move to the next or previous page (1 to 9). To leave, press UP + DOWN together (saves at once), or hold UP (ENTER) for 0.6 seconds to choose a bank to save to (see [Saving](#saving)), or hold DOWN (ESCAPE) for 0.6 seconds to cancel every change. Configuration mode always opens on page 1, and each page is marked by a panel LED.

In the [editor page](https://riban-bw.github.io/FCB1010_Firmware/editor/), each footswitch shows what it does on the page you are on, under its usual message.

Inside an editor (a footswitch's settings, or a pedal's CC) and during a pedal calibration, UP and DOWN work with a tap instead (see each editor below). Holding them there does the same as a tap, so you cannot leave configuration mode from an editor or a calibration except with UP + DOWN, which keeps the editor's changes, saves and leaves.

### Typing a number

Every number in configuration mode (the MIDI channel, a footswitch's number and values, a pedal's CC, the bank's program change, and the bank to save to) is typed the same way, on the footswitches: 1 to 9 are the digits 1 to 9 and 10 is 0. The LEDs of the footswitches whose digit can come next light dimly, so you can see what is allowed; the others do nothing.

- Each digit shows at once and takes effect at once: for a channel from 1 to 16, footswitch 1 shows `1`.
- While another digit could still follow, the dot at the right of the display flashes and the digits that fit are dim: after `1`, 0 to 6 (for 10 to 16). A second digit shows the whole number (`12`). Once no more digits would fit, the dot stops flashing and the number is complete; the next digit starts a new number.
- While the dot flashes, tap UP (ENTER) to keep the number as it is, or DOWN (ESCAPE) to put back the value from before you started typing it. Either way you stay where you are: neither changes the page or closes an editor.
- A footswitch pressed while UP or DOWN is held types nothing.

| Page | Panel LED | Display | Controls |
|---|---|---|---|
| 1. Footswitches | MIDI FUNCTION, and the LED of each setting in turn | The selected footswitch's settings in turn (see [Footswitch editor](#footswitch-editor)) | Press a footswitch to select it (its LED lights). The display then shows each of its settings for 0.8 seconds, its panel LED lit: the pedal mode (SWITCH: `St`, `+St`, `Pu` or `-Pu`, see [Footswitch modes](using.md#footswitch-modes)), the message type (SELECT), the number (NUMBER, skipped for a program change), the ON value (VALUE 1) and the OFF value (VALUE 2), then again from the mode. Nothing changes until you edit it: press the selected footswitch again to open its editor (see below). Hold a footswitch for 0.6 seconds to open its editor straight away. The modes and messages belong to the footswitch bank in use. |
| 2. MIDI channel | MIDI CHANNEL | Channel 1 to 16 | Type the channel (see [Typing a number](#typing-a-number)): footswitch 1 then 2 for 12, or 5 alone for 5. |
| 3. EXP A | EXP 1 | The setting of the footswitch last pressed (see [Expression pedal pages](#expression-pedal-pages)) | Expression pedal A: footswitch 6 its controller (CC) number, 8 its response (`Ab` immediate, `P.U.` pickup), 9 its calibration (heel and toe readings). The footswitch selected is lit and the other two dim. Each bank has its own response and CC; the calibration is shared. |
| 4. EXP B | EXP 2 | As page 3 | As page 3, for expression pedal B, with the same footswitches. |
| 5. SWITCH 1 relay | SWITCH 1 | `S1`, or the trigger point of the pedal last moved | Press footswitches to choose which ones the relay follows (their LEDs light; the others are lit dimly). Move a pedal to the point where the relay should close; move it to the heel to stop following it (EXP 1 / EXP 2 light while a pedal is followed, and are lit dimly while it is not). Each bank has its own, saved with the bank. This is the only page where moving a pedal sets anything. |
| 6. SWITCH 2 relay | SWITCH 2 | `S2`, or the trigger point of the pedal last moved | As page 5, for the SWITCH 2 relay. |
| 7. Bank program change | DIRECT SELECT | `Pr` and the program (0 to 127), or `--` for none, in turn, a second each; only the number while you type one | The program change sent when you choose this bank (the bank in use) with UP and DOWN (while DIRECT SELECT is dark). Type it (see [Typing a number](#typing-a-number)); UP (ENTER) and DOWN (ESCAPE) while the dot flashes keep or cancel the number and stay on this page. For none, press and release footswitch 10 when no number is being typed (the display shows `--`, its right-hand dot flashing), then press UP (ENTER); DOWN (ESCAPE) instead keeps the program, and a footswitch starts a new number (10 is then 0). Each bank has its own, saved with the bank. |
| 8. Misc | SELECT | The name of the setting last switched: `CL`, `rS` or `AS` | Footswitch 1 is the MIDI clock display (the visual metronome), 9 running status and 10 active sensing (see [Misc settings](#misc-settings)). Each press switches its setting on or off at once; its LED is lit while on and dim while off. The page opens showing `CL`. These settings are shared by every bank. |
| 9. Lamp test and version | All, then each in turn | "riban", then the firmware version | First every LED lights for 1 second. Then the panel LEDs light one at a time along the row and back (SWITCH 1, SWITCH 2, the SWITCH LED between them and SELECT, SELECT, NUMBER, VALUE 1, VALUE 2, DIRECT SELECT, MIDI FUNCTION, MIDI CHAN), while ". riban ." scrolls across the display, the dots running in from the right and off at the left, then the firmware version shows in three parts (major and minor in decimal, the build in hex): major with a trailing dot (`01.`), minor with dots both sides (`.01.`), and build with a leading dot (`.02`), then it repeats. Each of the ten steps lights its own footswitch LED as it starts, 1 to 10 in turn: the scroll frames " r" to "an" (1 to 5), "n " with the trailing dot running off (6), the three version parts (7 to 9), and a blank followed by the leading dot coming in (10). Hold footswitches 1 to 9 to show their step, for example 8 for the minor version; let go to carry on. Hold footswitch 10 to light every LED and every segment of the display. |

## Expression pedal pages

Page 3 sets up expression pedal A and page 4 pedal B, with the same footswitches (its EXP LED marks the page):

| Footswitch | Display | What it does |
|---|---|---|
| 6, CC | The controller number | Selected when the page opens. A press opens its CC editor (below); after another footswitch, the first press shows the number again and the next opens the editor. |
| 8, response | `Ab` (immediate) or `P.U.` (pickup) | The first press shows the pedal's response; each press after that switches it between immediate (every move is sent) and pickup (see [Expression pedals](using.md#expression-pedals)). |
| 9, calibration | The pedal's value, `Lo`, `Hi` or `Er` | The first press shows the pedal's value (0 to 127, as calibrated), following the pedal as you move it. The next press starts the calibration, which UP and DOWN then step through: `Lo` flashes, so put the pedal at the heel and tap UP (ENTER) to take the heel reading; `Hi` flashes, so put it at the toe and tap UP to take the toe reading, and the value shows again with the new calibration. DOWN (ESCAPE) on `Lo` stops without changing anything, and on `Hi` goes back to `Lo`. |

Each page opens showing the pedal's controller number, with footswitch 6 lit, so a press of footswitch 6 opens its CC editor straight away. The other two footswitches (8 and 9) are lit dimly, to show they can be selected. A footswitch acts when you let go of it, then its LED lights and the display shows its setting. The other footswitches do nothing. While a calibration runs, footswitch 9 (still lit) stops it, as DOWN does on `Lo`, and the other footswitches do nothing. If the toe reading is too close to the heel reading, `Er` shows for 2 seconds, then the pedal's value, and the old calibration is kept; press footswitch 9 again to start over. A press while `Er` shows starts over at once. Calibrating with the heel and toe swapped reverses the pedal. Each bank has its own pedal responses and CCs, saved with the bank; a bank never saved uses the default ones (see [Using the pedal](using.md)). The calibration belongs to the pedal and is the same in every bank.

### Pedal CC editor

On page 3 or 4, footswitch 6 pressed a second time opens the editor for that page's pedal's controller number. Its EXP LED and NUMBER light, and the display shows the number. Type it (see [Typing a number](#typing-a-number)). Then tap UP (ENTER) to keep it or DOWN (ESCAPE) to put the old number back; either way you go back to the pedal's page.

## Footswitch editor

On page 1, select a footswitch and press it again (or hold any footswitch for 0.6 seconds) to edit its settings. The editor opens on the pedal mode, and a panel LED shows which setting you are on (MIDI FUNCTION stays lit, as on page 1). Choices are made with the footswitches: the one chosen is lit and the others you can choose are lit dimly.

| Setting | Panel LED | Display | How to set it |
|---|---|---|---|
| Pedal mode | SWITCH | `St`, `+St`, `Pu` or `-Pu` | Footswitch 1 for stomp, 2 for stomp with momentary, 3 for momentary (push), 4 for inverted momentary (see [Footswitch modes](using.md#footswitch-modes)). |
| Message type | SELECT | `CC`, `nt` (note) or `Pr` (program change, with a small r) | Footswitch 1 for program change, 6 for CC, 10 for note. |
| Number | NUMBER | 0 to 127 | The controller number or note number (see [Typing a number](#typing-a-number)), for example 6 then 10 for 60. A program change has no number, so it is skipped. |
| ON value | VALUE 1 | 0 to 127 | The value a CC sends when the switch turns ON, or the note on velocity. For a program change, the program sent when it turns ON. |
| OFF value | VALUE 2 | 0 to 127 | The value a CC sends when the switch turns OFF, or the note off velocity. For a program change, the program sent when it turns OFF (the same as the ON value for a single program). |

Tap UP (ENTER) to go on to the next setting and DOWN (ESCAPE) to go back to the one before. Tap UP on the OFF value to keep every change and go back to page 1; tap DOWN on the pedal mode to put every setting back as it was when you opened the editor, and go back to page 1. While a number's dot flashes, UP and DOWN keep or cancel the number instead and stay on its setting (see [Typing a number](#typing-a-number)).

Like other settings, the changes are only stored when you save and leave configuration mode. If the switch was ON when you kept a change to its message, the old message's OFF is sent and the switch turns OFF.

## Misc settings

Page 8 holds settings shared by every bank. Each press of a footswitch switches its setting on or off straight away and shows its name; the footswitch's LED is lit while the setting is on and dim while it is off.

| Footswitch | Display | Setting | What it does |
|---|---|---|---|
| 1 | `CL` | MIDI clock display (visual metronome) | When on, MIDI Start or Continue on MIDI IN starts it, and MIDI Stop ends it. The `-` on the left of the display then flashes with the beat of the incoming MIDI clock: lit for the first half of each beat (12 of its 24 clocks) and dark for the second. Once the position in the song is known (after a Start, which begins at beat 1, or a Song Position Pointer) beat 1 of each bar shows `+` instead; 4/4 time is assumed. It shows in run mode only, over whatever the display shows. |
| 9 | `rS` | Running status | When on, MIDI OUT leaves out a message's status byte when it is the same as the last one sent (the standard MIDI running status), which saves a third of the bytes of a run of CCs. Some receivers do not handle it, so it is off unless you turn it on. |
| 10 | `AS` | Active sensing | When on, MIDI OUT sends active sensing (FE) every 250ms, in run and configuration mode, so a receiver that watches for it can tell the pedal is still connected. Off unless you turn it on. |

## Saving

Hold UP (ENTER) for 0.6 seconds to save. The display flashes the number of the bank in use, with the dot lit in between, and the page LEDs go dark. Tap UP (ENTER) to save into that bank and leave configuration mode. To save into another bank instead, type its number first (see [Typing a number](#typing-a-number); for example 1 then 2 for bank 12): while a number can take another digit it shows steadily with its dot flashing, and UP (ENTER) takes it, staying in the prompt. Tap DOWN (ESCAPE) to go back to the page without saving.

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
