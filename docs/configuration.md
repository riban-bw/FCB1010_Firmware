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
| 1. Footswitches | MIDI FUNCTION, and the LED of each setting in turn | The selected footswitch's number, then its settings in turn (see [Footswitch editor](#footswitch-editor)) | Press a footswitch to select it (its LED lights). The display shows its number for 1.6 seconds, then each of its settings for 0.8 seconds with its panel LED lit: the pedal mode (SWITCH: `St`, `+St`, `Pu` or `-Pu`, see [Footswitch modes](using.md#footswitch-modes)), the message type (SELECT), the number (NUMBER, skipped for a program change), the ON value (VALUE 1) and the OFF value (VALUE 2), then again from the number. Nothing changes until you edit it: press the selected footswitch again to open its editor (see below). Hold a footswitch for 0.6 seconds to open its editor straight away. The modes and messages belong to the footswitch bank in use. |
| 2. MIDI channel | MIDI CHANNEL | Channel 1 to 16 | Type the channel (see [Typing a number](#typing-a-number)): footswitch 1 then 2 for 12, or 5 alone for 5. |
| 3. Expression pedals | EXP 1 or EXP 2, for the pedal shown | The setting of the footswitch last pressed (see [Expression pedal page](#expression-pedal-page)) | Both expression pedals: footswitch 8 opens pedal A's editor and 9 pedal B's (response, controller number and range), 4 is pedal A's calibration and 5 pedal B's (heel and toe readings). The footswitch selected is lit and the other three dim. Each bank has its own responses, CCs and ranges; the calibration is shared. |
| 4. SWITCH 1 relay | SWITCH 1 | `S.1`, or the trigger point of the pedal last moved | Press footswitches to choose which ones the relay follows (their LEDs light; the others are lit dimly). Move a pedal to the point where the relay should close; move it to the heel to stop following it (EXP 1 / EXP 2 light while a pedal is followed, and are lit dimly while it is not). Each bank has its own, saved with the bank. This is the only page where moving a pedal sets anything. |
| 5. SWITCH 2 relay | SWITCH 2 | `S.2`, or the trigger point of the pedal last moved | As page 4, for the SWITCH 2 relay. |
| 6. Bank program change | DIRECT SELECT | `Pr` and the program (0 to 127), or `--` for none, in turn, a second each; only the number while you type one | The program change sent when you choose this bank (the bank in use) with UP and DOWN (while DIRECT SELECT is dark). Type it (see [Typing a number](#typing-a-number)); UP (ENTER) and DOWN (ESCAPE) while the dot flashes keep or cancel the number and stay on this page. For none, press and release footswitch 10 when no number is being typed (the display shows `--`, its right-hand dot flashing), then press UP (ENTER); DOWN (ESCAPE) instead keeps the program, and a footswitch starts a new number (10 is then 0). Each bank has its own, saved with the bank. |
| 7. Misc | SELECT | The name of the setting last switched (`CL`, `r.S.` or `A.S.`) and `On` or `Of`, in turn, a second each | Footswitch 1 is the MIDI clock display (the visual metronome), 9 running status and 10 active sensing (see [Misc settings](#misc-settings)). Each press switches its setting on or off at once; its LED is lit while on and dim while off. The page opens on the clock display. These settings are shared by every bank. |
| 8. Lamp test and version | All, then each in turn | "riban", then the firmware version | First every LED lights for 1 second. Then the panel LEDs light one at a time along the row and back (SWITCH 1, SWITCH 2, the SWITCH LED between them and SELECT, SELECT, NUMBER, VALUE 1, VALUE 2, DIRECT SELECT, MIDI FUNCTION, MIDI CHAN), while ". riban ." scrolls across the display, the dots running in from the right and off at the left, then the firmware version shows in three parts (major and minor in decimal, the build in hex): major with a trailing dot (`01.`), minor with dots both sides (`.06.`), and build with a leading dot (`.0B`), then it repeats. Each of the ten steps lights its own footswitch LED as it starts, 1 to 10 in turn: the scroll frames " r" to "an" (1 to 5), "n " with the trailing dot running off (6), the three version parts (7 to 9), and a blank followed by the leading dot coming in (10). Hold footswitches 1 to 9 to show their step, for example 8 for the minor version; let go to carry on. Hold footswitch 10 to light every LED and every segment of the display. Meanwhile EXP 1 lights while the panel LEDs run along the row and EXP 2 while they run back. Move pedal B to set the LED brightness (see [LED brightness](#led-brightness)). |

## Expression pedal page

Page 3 sets up both expression pedals. EXP 1 lights for pedal A and EXP 2 for pedal B, whichever the footswitch last pressed belongs to:

| Footswitch | Display | What it does |
|---|---|---|
| 8, pedal A | Pedal A's controller number | Selected when the page opens. A press opens pedal A's editor (below): its response, controller number and range. |
| 9, pedal B | Pedal B's controller number | The first press shows pedal B's controller number; the next opens pedal B's editor. |
| 4, calibrate pedal A | Pedal A's value, `Lo`, `Hi` or `Er` | The first press shows the pedal's value (0 to 127, as calibrated), following the pedal as you move it. The next press starts the calibration, which UP and DOWN then step through: `Lo` flashes, so put the pedal at the heel and tap UP (ENTER) to take the heel reading; `Hi` flashes, so put it at the toe and tap UP to take the toe reading, and the value shows again with the new calibration. DOWN (ESCAPE) on `Lo` stops without changing anything, and on `Hi` goes back to `Lo`. |
| 5, calibrate pedal B | Pedal B's value, `Lo`, `Hi` or `Er` | The same for pedal B. |

The page opens showing pedal A's controller number, with footswitch 8 lit, so a press of footswitch 8 opens pedal A's editor straight away. The other three footswitches (9, 4 and 5) are lit dimly, to show they can be selected. A footswitch you have not just pressed only shows its setting; pressed again, it acts. It acts when you let go of it, then its LED lights and the display shows its setting. The other footswitches do nothing. While a calibration runs, its footswitch (still lit) stops it, as DOWN does on `Lo`, and the other footswitches do nothing. If the toe reading is too close to the heel reading, `Er` shows for 2 seconds, then the pedal's value, and the old calibration is kept; press the footswitch again to start over. A press while `Er` shows starts over at once. Calibrating with the heel and toe swapped reverses the pedal. Each bank has its own pedal responses, CCs and ranges, saved with the bank; a bank never saved uses the default ones (see [Using the pedal](using.md)). The calibration belongs to the pedal and is the same in every bank.

### Pedal editor

On page 3, footswitch 8 (pedal A) or 9 (pedal B) pressed a second time opens the editor for that pedal. Like the footswitch editor, it takes you through its settings one at a time, with the same LEDs, and the LED of the one you are on flickers (the pedal's EXP LED stays lit):

| Step | LED | Setting |
|---|---|---|
| 1 | SELECT | The response: footswitch 1 immediate (`Ab`, every move is sent), footswitch 2 pickup (`P.U.`, see [Expression pedals](using.md#expression-pedals)). The one chosen is lit, the other dim |
| 2 | NUMBER | The controller number the pedal sends, 0 to 127 |
| 3 | VALUE 1 | The value it sends at the heel, 0 to 127 (0 by default) |
| 4 | VALUE 2 | The value it sends at the toe, 0 to 127 (127 by default) |

The display shows the setting; type a number (see [Typing a number](#typing-a-number)). UP (ENTER) goes to the next step, and on VALUE 2 keeps everything and goes back to the page. DOWN (ESCAPE) goes back a step, and on SELECT puts everything back as it was and goes back to the page. While a number can take another digit (the dot flashes), UP and DOWN first end or cancel that number.

The pedal's travel is spread evenly between VALUE 1 and VALUE 2: with VALUE 1 at 10 and VALUE 2 at 100, the heel sends 10, the toe 100 and halfway about 55. VALUE 1 can be the larger, which reverses the pedal (the heel sends the higher value). Pickup and the relay triggers on the SWITCH pages work with the values the pedal sends, so a trigger set by moving the pedal is the value it sends there.

## Footswitch editor

On page 1, select a footswitch and press it again (or hold any footswitch for 0.6 seconds) to edit its settings. The editor opens on the pedal mode, and the panel LED of the setting you are on flickers (the same LEDs as on page 1, where they are steady; MIDI FUNCTION stays lit). Choices are made with the footswitches: the one chosen is lit and the others you can choose are lit dimly.

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

Page 7 holds settings shared by every bank. Each press of a footswitch switches its setting on or off straight away; the display then shows its name and `On` or `Of` in turn, a second each, for as long as you stay on the page; the footswitch's LED is lit while the setting is on and dim while it is off.

| Footswitch | Display | Setting | What it does |
|---|---|---|---|
| 1 | `CL` | MIDI clock display (visual metronome) | When on, MIDI Start or Continue on MIDI IN starts it, and MIDI Stop ends it. The `-` on the left of the display then flashes with the beat of the incoming MIDI clock: lit for the first half of each beat (12 of its 24 clocks) and dark for the second. Once the position in the song is known (after a Start, which begins at beat 1, or a Song Position Pointer) beat 1 of each bar shows `+` instead; 4/4 time is assumed. It shows in run mode only, over whatever the display shows. |
| 9 | `r.S.` | Running status | When on, MIDI OUT leaves out a message's status byte when it is the same as the last one sent (the standard MIDI running status), which saves a third of the bytes of a run of CCs. Some receivers do not handle it, so it is off unless you turn it on. |
| 10 | `A.S.` | Active sensing | When on, MIDI OUT sends active sensing (FE) every 250ms, in run and configuration mode, so a receiver that watches for it can tell the pedal is still connected. Off unless you turn it on. |

## LED brightness

On page 8 (lamp test and version), move pedal B to set the brightness of every LED and the display, from full at the toe to the minimum at the heel. The page shows it as you move the pedal; the first reading after opening the page is only a starting point, so nothing changes until the pedal moves. It is used while playing and on page 8; the other configuration pages always use full brightness, so you can still read them. At the heel every LED is at its dimmest, but still lit. The brightness is saved like the other settings, shared by every bank (full until one is saved).

## Saving

Hold UP (ENTER) for 0.6 seconds to save. The display flashes the number of the bank in use, with the dot lit in between, and the page LEDs go dark. Tap UP (ENTER) to save into that bank and leave configuration mode. To save into another bank instead, type its number first (see [Typing a number](#typing-a-number); for example 1 then 2 for bank 12): while a number can take another digit it shows steadily with its dot flashing, and UP (ENTER) takes it, staying in the prompt. Tap DOWN (ESCAPE) to go back to the page without saving.

Saving into another bank stores everything you see in configuration mode (the footswitch messages and modes, the relays, the expression pedal settings and the bank's program change) in that bank, and the pedal then plays that bank. The bank you started from keeps what it last had saved. So you can copy a bank: open configuration mode in it and save into another; or use one bank as a template: open it, change what you need and save the result into a new bank. The shared settings (the MIDI channel, the brightness, the misc settings and the pedal calibration) belong to every bank, and are saved either way. If they are the only settings you changed, a long UP saves and leaves at once without asking for a bank. UP + DOWN together always saves into the bank in use at once.

When you save, each footswitch whose mode or message you changed is put in its new mode's resting state, so it does not carry a stale state into its new mode: ON for inverted momentary, OFF for the other modes. If that changes the switch, its new value is sent, so the receiving device stays in step. If a save fails it is not reported: the new settings apply until power-off.

## Configuring from a computer

Every setting can also be changed over MIDI (see [MIDI implementation](midi.md#configuration-over-midi) for the messages), from the "Configure over MIDI" panel on the [editor page](https://riban-bw.github.io/FCB1010_Firmware/editor/). Connect a MIDI interface: its output to the pedal's MIDI IN and, if you want to read the pedal's settings, its input from the pedal's MIDI OUT. Open the page in a browser with Web MIDI (Chrome or Edge), choose "Real pedal", allow MIDI access and pick the ports (with only one of each they are picked for you). Allow SysEx access too when the browser asks: the settings travel as SysEx messages, which reach the pedal whatever its MIDI channel and which other MIDI devices and software ignore. The output alone is enough to change settings, load a file and store; the input is only needed for Read. The panel starts with the editor's settings until you read the pedal or load a file. The same panel can configure the emulated pedal on the page instead, to try it out.

- **Read** loads the pedal's settings into the panel (it needs the MIDI input): the bank in use as it is now, then every other bank as stored, without the pedal changing bank (all 30 banks take under a second). The pedal answers on whatever MIDI channel it is set to, and the panel shows which.
- Change anything in the panel: the **Global** settings shared by every bank (MIDI channel, brightness, clock display, running status, active sensing), footswitch modes and messages, the relays, and each expression pedal's CC and response. The footswitches, relays and pedal settings shown are those of the bank in the **bank** box, and **program change** next to it is that bank's program change (`--` for none, as on page 6). Choosing another bank shows it straight away without changing the pedal's bank. A change to the bank in use works on the pedal straight away, just as if you had made it in configuration mode (a footswitch whose mode or message changes goes to its resting state), and **Store** keeps it. A change to another bank is stored into that bank at once, and the pedal stays on its bank. **Use on pedal**, next to the bank box, switches the pedal to the bank shown (as UP and DOWN do while DIRECT SELECT is dark), so store any changes to the bank in use before leaving it. Before a read, choosing a bank switches the pedal to it.
- **Copy bank** remembers the settings of the bank shown (its footswitches, relays, expression pedal settings and program change), which need not be the bank in use; **Paste bank** puts them into the bank shown then, like any change: into the bank in use live (press **Store** to keep it), or into another bank stored at once. The global settings are left as they are.
- **Store** keeps the changes in the pedal after power-off. **Discard changes** puts back what was last stored. **Reset to defaults** (after you confirm) puts every setting of every bank, and the shared settings, back to their defaults and stores them, as holding footswitch 9 at power-on does (see [Resetting to defaults](using.md#resetting-to-defaults)); the pedal calibration is kept. The panel then finds the pedal again (its MIDI channel may have changed) and reads every bank.
- **Save to file** saves the settings to a file on your computer, named in the **File** box (in Chrome, Edge and Opera a save dialog lets you choose the folder and name), and **Load from file** sends a saved file's settings to the pedal and stores them (it asks first). Use it to keep a backup, or several setups to swap between. The file holds the global settings and every bank's footswitches, relays, expression pedal settings and program change: everything except the pedal calibration, which belongs to each pedal. Press **Read** first, so that every bank is in the file; without a MIDI input from the pedal only the bank in use is. Loading stores each bank in the file into that bank, and only the banks that differ from the pedal's, without changing the pedal's bank (about 5 seconds for 30 banks, with a progress bar). To move one bank's settings to another bank, use **Copy bank** and **Paste bank**. It is a readable JSON file, so it can also be edited by hand (see [Settings file format](settings-file.md)).

The pedal only listens for settings while it is playing (not in configuration mode), and only to messages for any pedal or for its own channel. It never answers a change, only a Read, so it cannot get stuck in a loop if its MIDI OUT is wired back to its MIDI IN.
