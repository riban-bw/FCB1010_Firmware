# riban FCB1010 stomp-box firmware

The Behringer FCB1010 is a sturdy MIDI foot controller, but its stock firmware is built around presets and banks, which makes it awkward to use as a simple row of effect switches. This replacement firmware turns it into a straightforward MIDI stomp-box: each of the ten footswitches is an on/off switch with its own LED, UP and DOWN step through programs, and the two expression pedals send continuous controllers. Everything is set up on the pedal itself, with no computer or editor needed.

It suits anyone who wants to switch effects in a DAW, a plugin host or a MIDI-capable amp or synth the way they would on a pedalboard: press to turn on, press again to turn off, and see at a glance what is on.

## Features

- 10 footswitches, each sending its own MIDI controller (CC), note or program change, with a choice of four switch behaviours
- 25 banks of footswitch settings, chosen with UP and DOWN
- Program change from UP and DOWN, with fast scrolling when held
- 2 expression pedals
- 2 relay outputs (the SWITCH 1 and SWITCH 2 jacks), triggered by any footswitch or pedal, to switch amp channels or other gear
- Footswitches that send the same controller or note share its state, in every bank
- Follows incoming MIDI, so its LEDs stay in step with your DAW or synth
- Selectable MIDI channel
- Simple configuration on the pedal, saved in its memory
- Or configure it from a computer over MIDI, with a web page (no special software or SysEx)

[Try the emulator in your browser](https://riban-bw.github.io/FCB1010_Firmware/emulator/). It runs this firmware's own control code, so you can try the footswitches, pedals and configuration pages before flashing a chip. Its on-screen hints explain each control as you use it.

## Using the pedal

| Control | What it does |
|---|---|
| Footswitches 1 to 10 | Turn an effect on or off. The LED shows whether it is on. Each switch can instead act while held (see Footswitch modes) |
| UP / DOWN | Next / previous program. The display shows the program number, and the program change is sent when you let go. Hold to scroll. While DIRECT SELECT is lit, switches to next / previous footswitch bank instead (see Footswitch banks) |
| Expression pedal A | Volume (CC 7), 0 to 127 |
| Expression pedal B | Expression (CC 11), 0 to 127 |
| UP + DOWN tapped together | Turns bank select on or off. While DIRECT SELECT is lit, UP and DOWN choose footswitch banks (see Footswitch banks). Nothing is sent |
| UP + DOWN held 1s | Opens configuration mode (see Configuration) |

Everything uses one MIDI channel, channel 1 unless you change it in configuration mode. Footswitches 1 to 10 send CC 21 to 30, with value 127 for ON and 0 for OFF. You can give any footswitch a different message (see [Footswitch messages](#footswitch-messages)).

### The LEDs

| LED | While playing | In configuration mode |
|---|---|---|
| Footswitches 1 to 10 | Lit while that footswitch is ON | Depends on the page: the selected footswitch (page 1), the footswitch being edited (editor), the footswitches a relay follows (pages 5 and 6) |
| SWITCH 1, SWITCH 2 | Lit while that relay is closed | Marks page 5 (SWITCH 1) or page 6 (SWITCH 2) |
| DIRECT SELECT | Lit while bank select is on: UP and DOWN choose footswitch banks | Pages 5 and 6: lit if the relay follows bank select. Dark on the other pages |
| MIDI FUNCTION | Dark | Marks page 1 (pedal modes) |
| MIDI CHAN | Flickers when MIDI arrives | Marks page 2 (MIDI channel) |
| EXP 1, EXP 2 | Dark | Both mark pages 3 and 4 (calibration). Pages 5 and 6: lit while the relay follows that pedal |
| SELECT, NUMBER, VALUE 1, VALUE 2 | Dark | Footswitch editor: the setting being edited (type, number, ON value, OFF value) |
| CONFIG | Dark | Lit; flashes while a change is not saved |

At power-on and on configuration page 7 every LED is lit (on page 7 the CONFIG LED still flashes for an unsaved change, and the footswitch LEDs show the steps).

### Footswitch modes

Each footswitch has one of four modes, chosen in configuration mode (default: stomp).

| Mode | Display digit | Behaviour |
|---|---|---|
| Stomp | 1 | Press to turn ON, press again to turn OFF |
| Stomp with momentary | 2 | As stomp, but a hold of more than 0.6s undoes it on release (see below) |
| Momentary | 3 | ON while pressed, OFF when released |
| Inverted momentary | 4 | OFF while pressed, ON when released. Its LED is lit while it rests, from power-on onwards |

In stomp with momentary mode a press always acts immediately: it toggles the effect. What happens when you let go depends on how long you held it:

- **Released within 0.6 seconds:** the effect stays as it is now, like a normal stomp.
- **Released after more than 0.6 seconds:** the effect goes back to how it was before the press.

For example, with an effect on, pressing its switch turns it off. If you let go within 0.6 seconds it stays off. If you hold it longer and then let go, it comes back on. This lets you drop an effect out (or bring one in) for just as long as you hold the switch.

### Footswitch messages

Each footswitch sends one of three kinds of message, set in the footswitch editor (see [Configuration](#configuration)):

| Type | Display | ON | OFF |
|---|---|---|---|
| CC | `CC` | The controller number with the ON value | The controller number with the OFF value |
| Note | `nt` | Note on: the note number, with the ON value as velocity | Note off: the note number, with the OFF value as velocity |
| Prog | `Pr` | Program change: the ON value | Program change: the OFF value |

Every type works in any of the footswitch modes. A program change switch has two programs and toggles between them: for example, in stomp mode one press sends program 5 and turns it ON (its LED lights), and the next sends program 6 and turns it OFF. If you give it the same program twice, it sends that program on every press and nothing when you let go, whatever its mode, and its LED lights only while you hold it. Whenever a footswitch sends a program, the display shows it and UP and DOWN step on from it.

Footswitches that send the same controller (CC) or the same note share one state. When one of them sends, every other footswitch with that CC or note follows, just as if the message had arrived on MIDI IN (see [Keeping in step with your DAW](#keeping-in-step-with-your-daw)): a note on turns them ON and a note off OFF; a CC value turns a switch ON if it is that switch's ON value and OFF if it is its OFF value, and otherwise ON from 64 up. So two switches on CC 40, one with values 127 / 0 and one with 100 / 20, light and go out together. A switch whose ON and OFF values are the same keeps its own state. The same holds across banks (see [Footswitch banks](#footswitch-banks)).

### Changing program

UP and DOWN step the program number by one, from 0 to 127 (stopping at 0 and 127; a press that cannot change the number sends nothing). The program change is sent when you let go of the switch. Hold the pedal to auto-repeat, initially slowly then rapidly, and only the program you stop on is sent.

### Footswitch banks

The pedal holds 25 banks, each with its own message and mode for every footswitch, so you can keep a set of footswitches for each song or rig and switch between them. The relays, the MIDI channel and the pedal calibration are shared by every bank.

Tap UP + DOWN together so that DIRECT SELECT lights. The display shows the bank number, and UP and DOWN now step through the banks (stopping at banks 1 and 25) instead of sending program changes. The new bank takes over when you let go; hold UP or DOWN to scroll through the banks first, and only the one you stop on is loaded. Tap UP + DOWN again to go back to changing programs: the display shows the program number again, and the bank you chose stays in use. The taps send nothing.

Changing bank sends nothing, and each controller and note keeps its state, however many banks you pass through. A footswitch of the new bank takes the state its CC or note last had, from a footswitch in any bank or from incoming MIDI: for example, if footswitch 1 is CC 26 and is ON in bank 1, any footswitch with CC 26 in any other bank shows ON, and footswitch 1 is still ON when you come back to bank 1, even through banks that do not use CC 26. The pedal forgets these states when it is switched off. A CC is read by the value last sent, the same way as incoming MIDI, so the switches do not need the same ON and OFF values. A footswitch whose CC or note has not been used since power-on starts in its resting state, without sending anything: ON for inverted momentary and OFF for the other modes. A program change switch always starts OFF. A footswitch you are holding during the change does nothing when you let go (its CC or note keeps the state it had).

Configuration mode edits the bank in use, and saving stores that bank (with the shared settings). A bank you have never saved has the default footswitch messages and modes. The pedal always starts in bank 1. A footswitch program change is still remembered while DIRECT SELECT is lit, and UP and DOWN step on from it once DIRECT SELECT is off again.

### The relay outputs (SWITCH 1 and SWITCH 2)

Each of the two relay jacks can follow any mix of footswitches, bank select (DIRECT SELECT) and the expression pedals. The relay closes when any of the things it follows is ON, and opens when they are all OFF. For a pedal, ON means pushed past a threshold you set. The SWITCH 1 and SWITCH 2 LEDs show the relay states. Nothing is set up until you choose, so out of the box the relays stay open.

### Keeping in step with your DAW

The pedal listens on its MIDI IN socket. When your DAW or synth sends one of the pedal's controllers on the pedal's channel, the matching LED changes to match: the switch's ON value turns it ON and its OFF value turns it OFF, and any other value counts as ON from 64 to 127 and OFF from 0 to 63. A note switch follows its note the same way: a note on turns it ON and a note off turns it OFF. Program changes are ignored. For example, CC 21 value 127 sets stomp 1 ON and lights its LED, and CC 21 value 0 turns it OFF. Every footswitch with that controller or note follows. The next press of that footswitch then carries on from there.

The pedal never sends these back out, so it is safe to connect MIDI OUT and MIDI IN to the same device. The MIDI CHAN LED flickers whenever MIDI arrives.

### Power-on

At power-on every LED lights and "riban" scrolls across the display. Then the display shows the program number and the pedal is ready, in footswitch bank 1. Pressing any switch skips this, and the press works as usual. Stomp states are OFF, except switches in inverted momentary mode, which start ON.

## Configuration

Hold UP and DOWN together for 1 second. The CONFIG LED lights. It flashes while any setting differs from what is saved in the pedal's memory, and stops flashing if you revert every change. If it flashes as soon as you open configuration mode, the last save did not reach the persistent memory. Footswitches and pedals send no MIDI while you are in configuration mode, and incoming MIDI is ignored.

Tap UP or DOWN to move to the next or previous page. To leave, hold UP (ENTER) for 0.6 seconds, or UP + DOWN together for 1 second, to save, or hold DOWN (ESCAPE) for 0.6 seconds to cancel every change. Configuration mode always opens on page 1, and each page is marked by a panel LED.

| Page | Panel LED | Display | Controls |
|---|---|---|---|
| 1. Pedal modes | MIDI FUNCTION | Mode of the selected footswitch (1 to 4 - see [Footswitch modes](#footswitch-modes) above) | Press a footswitch to select it (its LED lights) and see its mode; press it again to change its mode. Hold a footswitch for 0.6 seconds to open its editor (see below). The modes and messages belong to the footswitch bank in use. |
| 2. MIDI channel | MIDI CHANNEL | Channel 1 to 16 | Footswitches 1 to 10 pick channels 1 to 10. Switch 10 flashes `0` for 5 seconds; press 1 to 6 for 11 to 16, or 10 again for 10. After 5 seconds, or on a page change, the channel stays as 10. |
| 3. Heel calibration | EXP 1 and EXP 2 | `Lo`, then `0` once the reading is taken | Put both pedals at the heel and press any footswitch. |
| 4. Toe calibration | EXP 1 and EXP 2 | `Hi`, then `0` once the reading is taken, or `Er` | Put both pedals at the toe and press any footswitch. `Er` means a pedal hardly moved between heel and toe; press a footswitch to try again. Calibrating with the heel and toe swapped reverses the pedal. |
| 5. SWITCH 1 relay | SWITCH 1 | Blank, or the trigger point of the pedal last moved | Press footswitches to choose which ones the relay follows (their LEDs light). Tap UP + DOWN to follow bank select (DIRECT SELECT lights). Move a pedal to the point where the relay should close; move it to the heel to stop following it (EXP 1 / EXP 2 light while a pedal is followed). |
| 6. SWITCH 2 relay | SWITCH 2 | As page 5 | As page 5, for the SWITCH 2 relay. |
| 7. Lamp test and version | All | "riban", then the firmware version | Every LED lights while "riban" scrolls, then the firmware version shows in three parts: major with a trailing dot (`00.`), minor with dots both sides (`.01.`), and build with a leading dot (`.0E`), then it repeats. Each of the ten steps (six scroll frames, the three version parts and a blank) blinks its own footswitch LED, 1 to 10 in turn. Hold a footswitch to show its step, for example 8 for the minor version; let go to carry on. |

#### Footswitch editor

On page 1, hold a footswitch to edit its message. That footswitch's LED stays lit so you can see which one you are editing. Tap UP and DOWN to move between its settings. A panel LED shows which one you are on:

| Setting | Panel LED | Display | How to set it |
|---|---|---|---|
| Message type | SELECT | `CC`, `nt` (note) or `Pr` (program change) | Footswitch 1 for CC, 2 for note, 3 for program change. Or move either expression pedal: the bottom third is CC, the middle third note, the top third program change. |
| Number | NUMBER | 0 to 127 | The controller number or note number. A program change has no number, so UP and DOWN skip it. Type it with the footswitches (1 to 9 are digits 1 to 9, 10 is 0), for example 6 then 10 for 60, or move either expression pedal. |
| ON value | VALUE 1 | 0 to 127 | The value a CC sends when the switch turns ON, or the note on velocity. For a program change, the program sent when it turns ON. Set it the same way. |
| OFF value | VALUE 2 | 0 to 127 | The value a CC sends when the switch turns OFF, or the note off velocity. For a program change, the program sent when it turns OFF (the same as the ON value for a single program). Set it the same way. |

A number is complete once no more digits would fit (13 and up, or three digits), so the next digit you press starts a new number. Hold UP (ENTER) for 0.6 seconds to keep your changes and go back to page 1, or hold DOWN (ESCAPE) for 0.6 seconds to put the old values back. Like other settings, the message is only stored when you save and leave configuration mode. If the switch was ON when you changed its message, the old message's OFF is sent and the switch turns OFF.

When you save, each footswitch whose mode or message you changed is put in its new mode's resting state, so it does not carry a stale state into its new mode: ON for inverted momentary, OFF for the other modes. If that changes the switch, its new value is sent, so the receiving device stays in step. If a save fails it is not reported: the new settings apply until power-off.

### Configuring from a computer

Every setting can also be changed over MIDI, from the "Configure over MIDI" panel on the [emulator page](https://riban-bw.github.io/FCB1010_Firmware/emulator/). Connect a MIDI interface: its output to the pedal's MIDI IN and, if you want to read the pedal's settings, its input from the pedal's MIDI OUT. Open the page in a browser with Web MIDI (Chrome or Edge), choose "Real pedal", allow MIDI access and pick the ports (with only one of each they are picked for you). The output alone is enough to change settings, load a file and store; the input is only needed for Read and Find. Without it, set "On channel" to the channel the pedal is on now; the panel starts with the emulator's settings until you read the pedal or load a file. The same panel can configure the emulated pedal instead, to try it out.

- **Read** loads the pedal's settings into the panel (it needs the MIDI input). If you do not know which channel the pedal is on, press **Find**: it asks on every channel and only the pedal answers.
- Change anything in the panel: footswitch modes and messages, the pedal's MIDI channel, the relays. The footswitches shown are those of the bank in the **bank** box; choosing another bank switches the pedal to it (as UP and DOWN do while DIRECT SELECT is lit) and reads that bank, so store any changes to a bank before leaving it. Each change works on the pedal straight away, just as if you had made it in configuration mode, and a footswitch whose mode or message changes goes to its resting state.
- **Store** keeps the changes in the pedal after power-off. **Discard changes** puts back what was last stored.
- **Save to file** saves the settings to a file on your computer, and **Load from file** sends a saved file's settings to the pedal (press **Store** afterwards to keep them). Use it to keep a backup, or several setups to swap between. The file holds one bank's footswitches and the shared settings, everything except the pedal calibration, which belongs to each pedal; loading it fills the bank in use. It is a readable JSON file, so it can also be edited by hand.
- **Set heel** and **Set toe** calibrate the expression pedals: put both pedals at the heel (or toe) and press the button.

The pedal only listens for settings while it is playing (not in configuration mode) and only on its own channel. It never answers a change, only a Read, so it cannot get stuck in a loop if its MIDI OUT is wired back to its MIDI IN.

## Installing

### BEWARE: This replaces the stock firmware completely

The stock presets, banks, direct-select modes and the Behringer SysEx editor do not work with this firmware. Put the original chip back to restore stock behaviour. This firmware keeps its settings in the same memory chip as the stock presets: its shared settings and footswitch bank 1 are away from where the stock firmware keeps them, but every other bank you save overwrites part of them. If you want the stock presets back later, save them with a SysEx dump from the stock firmware before you change chips, or be prepared to set them up again.

### Flashing to an EPROM or EEPROM

The program memory is a 28-pin chip in a socket. The EPROM used in my FCB1010 is an HT27C512-70, which is 64KB. Some units have the smaller 27C256 (32KB) or 27C128 (16KB). The firmware is the same for each; only the image file and the replacement chip differ.

| Chip in the socket | Size | Image to write | Replacement chips |
|---|---|---|---|
| 27C512 (for example HT27C512) | 64KB | `build/fcb1010_27c512.bin` | A 27C512-pinout part: W27C512 (electrically erasable, so it can be reprogrammed in the programmer), or a one-time programmable AT27C512R |
| 27C256 | 32KB | `build/fcb1010_27c256.bin` | A 27C256-pinout part such as an AT27C256R, or an AT28C256 EEPROM on an adapter (see step 3) |
| 27C128 | 16KB | `build/fcb1010_27c128.bin` | A 27C128-pinout part, or a larger chip of the socket's pinout with its image |

The 27C512 image is the 32KB firmware twice, so it runs whether the socket drives the chip's top address line (pin 1, A15) high or low. All three images are built by `make` and offered by the GitHub build.

1. **Identify the stock chip.** Open the case (16 screws on the underside). The program memory is the 28-pin socketed chip, often under a label or hot glue. Read the part number and find it in the table above; go by what is printed on yours. I recommend taking a photo of the original physical installation to have a record of what it looked like before you started to make changes.
2. **Back up the original.** Read the stock chip with your programmer and save the file. Put a fresh label on it and keep it. This may allow you to flash the stock firmware to your new, target (E)EPROM to validate the flashing process. It is unlikely that the original chip is rewritable.
3. **Choose a replacement with the same pinout as the socket.** The 27C512 and 27C256 differ on pin 1 (A15 on the 27C512, Vpp on the 27C256), so use a part of the same family as the stock chip. A parallel EEPROM such as the AT28C256 differs from both on pins 1 and 27: pin 27 is /WE on the EEPROM but address line A14 on the EPROMs, so a plain swap can cause spurious write pulses and corrupt the code. If you want an AT28C256 in a 27C256 socket, use a 28C256-to-27C256 adapter board (for example [this PCBWay design](https://www.pcbway.com/project/shareproject/28C256_to_27C256.html), which has jumpers for both pinouts). I haven't tested any of these in an FCB1010.
4. **Program the chip.** With a programmer such as a TL866 and the open-source `minipro` tool, the commands look like this (the device name must match your chip exactly; list the names with `minipro -l | grep -i 27c512`):

   ```
   minipro -p "W27C512@DIP28" -w build/fcb1010_27c512.bin   # 27C512 socket
   minipro -p "W27C512@DIP28" -m build/fcb1010_27c512.bin   # verify
   minipro -p "AT27C256R@DIP28" -w build/fcb1010_27c256.bin # 27C256 socket
   ```

   `minipro` erases an electrically erasable chip such as the W27C512 before writing it. A used UV-erasable EPROM (one with a window) must be erased under UV light first.
5. **Install it with the pedal unplugged from power.** Match pin 1 to the notch on the socket, the same as the old chip was.
6. **Power on.** "riban" should scroll across the display, then it should show `0`. Press UP and release it, and the display should show `1`.

To lower the risk, program a spare chip first and keep the original in a safe place. The firmware has not yet run on a real FCB1010 (see What has and has not been tested).

## Technical reference

The rest of this document is for building, changing or checking the firmware.

### MIDI implementation

| Message | Sent | Received |
|---|---|---|
| Control change, footswitches | CC 21 to 30 (`STOMP_CC_LIST`), 127 ON, 0 OFF by default; any controller number, ON value and OFF value per switch | A CC on channel 1 with a CC switch's controller number sets that switch: its ON value is ON, its OFF value OFF, and any other value 64 to 127 ON, 0 to 63 OFF |
| Note on / off, footswitches | Note switches: note on (0x9n) with the ON value as velocity when ON, note off (0x8n) with the OFF value as velocity when OFF | A note on with velocity 1 to 127 sets a note switch with that note ON; a note off, or a note on with velocity 0, sets it OFF |
| Control change, pedals | CC 7 and CC 11 (`PEDAL_CC_LIST`), 0 to 127 | Ignored |
| Program change | 0 to 127, on release of UP / DOWN; a program change footswitch sends its ON value as the program when it turns ON and its OFF value when it turns OFF (with both the same, that program on every press) | Ignored |
| NRPN (CC 99, 98, 6, 38) | Every setting, when asked for a dump | Settings and commands (see [Configuration over MIDI](#configuration-over-midi)) |

- Everything is on one channel: `MIDI_CHANNEL` until a channel is saved in configuration mode.
- Received messages only set the pedal's own state and LEDs; nothing is echoed to MIDI OUT. The next footswitch press toggles from the received state: if CC 21 turned switch 1 ON, pressing switch 1 sends CC 21 value 0.
- A received CC or note sets every footswitch with it, and so does one a footswitch sends (in run mode): footswitches sharing a CC or note share its state. A bank change passes each CC's and note's state on to the new bank's footswitches by the same rule and sends nothing.
- The receiver handles running status and split messages, ignores real-time bytes (clock, active sensing), SysEx, other channels and other controller numbers. Received messages are ignored in configuration mode.
- Pedal positions and the program number are not set by MIDI IN, and no state is remembered at power-off.
- A pedal sends a new value only after its reading moves `PEDAL_HYST` counts, and readings near the calibrated ends snap to 0 and 127 (`PEDAL_END_SNAP`).
- If MIDI OUT is full, a footswitch press is ignored rather than changing a state the receiver never hears about. A stomp with momentary revert that cannot be sent at release is sent as soon as there is room.

### Configuration over MIDI

Settings travel as NRPN controllers on the pedal's channel, so any MIDI tool can send them. CC 99 (NRPN MSB) with value 70 (0x46) and CC 98 (NRPN LSB) with the parameter number select a parameter, in either order; CC 6 (data entry) then sets it, and further CC 6 messages set it again. CC 99 with any other value, or CC 101 / 100 (RPN select), deselects, so other NRPN traffic on the channel is left alone. CC 38 is ignored when received.

| Parameter (CC 98) | Setting | Values |
|---|---|---|
| 0 | MIDI channel | 1 to 16; the pedal listens on the new channel at once |
| 1, 2, 3 | Firmware major, minor and build (read only) | 0 to 255, sent as CC 6 (bit 7) and CC 38 (bits 0 to 6) |
| 4 | Command (never sent) | 1 dump, 2 save, 3 discard unsaved changes, 4 heel from the pedals' positions, 5 toe from the pedals' positions |
| 5 | End of a dump (sent only) | 0 |
| 6 | Footswitch bank | 1 to `BANK_COUNT`; setting it changes bank, as UP / DOWN do in bank mode |
| 7 | Number of banks, `BANK_COUNT` (read only) | 1 to 25, as CC 6 + CC 38 |
| 8 to 17 | Footswitch 1 to 10 mode (of the bank in use, as are parameters 20 to 59) | 0 stomp, 1 stomp with momentary, 2 momentary, 3 inverted momentary |
| 20 to 29 | Footswitch 1 to 10 message type | 0 CC, 1 note, 2 program change |
| 30 to 39 | Footswitch 1 to 10 number | 0 to 127: controller or note (not used by a program change) |
| 40 to 49 | Footswitch 1 to 10 ON value | 0 to 127: CC value, note on velocity, or the program sent ON |
| 50 to 59 | Footswitch 1 to 10 OFF value | 0 to 127: CC value, note off velocity, or the program sent OFF |
| 60 to 70 | SWITCH 1 follows footswitch 1 to 10, then bank select | 0 / 1 |
| 72 to 82 | SWITCH 2, the same | 0 / 1 |
| 84, 85 | SWITCH 1 trigger, pedal A, pedal B | 1 to 127, 0 off |
| 86, 87 | SWITCH 2 trigger, pedal A, pedal B | 1 to 127, 0 off |
| 88 to 91 | Calibration: heel A, toe A, heel B, toe B (read only) | 0 to 255 raw readings, as CC 6 + CC 38 |

- A set takes effect at once but is not saved; command 2 saves everything, as leaving configuration mode does, and command 3 reloads what is saved. Unknown parameters, read-only ones, out-of-range values and values equal to the current one are ignored.
- A footswitch whose mode or message changes goes to its resting state on the next 2ms tick (as on a save in configuration mode). If it was ON when its message changed, the old message's OFF is sent first (not for a program change).
- Commands 4 and 5 take the pedals' current readings as the heel or toe, if both are at least `CAL_MIN_SPAN` from the other end; otherwise nothing changes. Read the calibration back to see whether it took.
- The dump (command 1) sends every parameter except the command and the end marker, one per tick once MIDI OUT has room: CC 99, 98 and 6 with running status, plus CC 38 for the read-only ones, then parameter 5. It takes about 0.2 seconds. A settable value is sent in CC 6 alone, so a dump fed back to the pedal changes nothing. The pedal sends nothing else in reply.
- A bank change (parameter 6) happens on the next tick, once any pending stomp with momentary revert has gone out, and sends nothing. A dump asked for meanwhile waits for it, but sets sent before it finishes apply to the old bank and are lost, so read back (or wait about 50ms) before setting the new bank's footswitches. Unsaved changes to the old bank's footswitches are dropped; save (command 2) first to keep them. The states passed on to the new bank are worked out from the old bank's saved records, so with unsaved message changes they follow the saved messages.
- Settings are taken in run mode only; in configuration mode they are ignored, and entering it cancels a dump in progress. A save over MIDI blocks the pedal for the EEPROM writes (tens of ms), so wait for it before sending more.

### Timing details

- **Long press.** The threshold is `MOMENTARY_HOLD_MS` in `src/config.h` (currently 600). The key scanner checks each switch every 12ms and starts timing once the press has been debounced (`DEBOUNCE_SAMPLES`), so the real threshold is about 0.64s. The same hold saves (UP) or cancels (DOWN) in configuration mode. If a CC arrives over MIDI IN during the hold, the release still goes back to the state before the press. A switch already down at power-on does nothing when released. UP and DOWN are not affected in run mode.
- **Program repeat.** Held: after 400ms the number auto-repeats at 4Hz on the display only, speeding up to about 25Hz once the key has been held for `PROGRAM_REPEAT_FAST_AFTER_MS` (2.7s), and one program change is sent on release.
- **Configuration.** UP + DOWN held 1s opens configuration mode (`CONFIG_HOLD_MS`); held 1s again in configuration mode it saves and leaves. A short UP + DOWN toggles bank select in run mode and does nothing in configuration mode except on the relay pages.
- **Bank change.** In bank mode UP / DOWN repeat at the program rates; the bank is loaded on release, on the next 2ms tick (later only if a stomp with momentary revert is waiting for room in MIDI OUT). Loading reads the new bank's ten footswitch records from the EEPROM. The last value of every CC (0-127) and the state of every note is kept in a 256-byte table in the external SRAM, set whenever one is sent or received in run mode and, at a bank change, from the old bank's switch states; the new bank's switches take their state from it. It is cleared at power-on.
- **Page 7 and power-on.** Each "riban" frame shows for half of `VERSION_SHOW_MS`, and each version part shows in hex, one part per second, followed by a blank of the same length. During each step its footswitch LED is dark for about the first half of a scroll frame. Power-on shows the scroll only, once.
- All timing comes from a 2ms timer tick and assumes the FCB1010's 12MHz crystal, as does the 31250 baud MIDI rate.

### Building

Requires SDCC 4.x, `make` (and `gcc` for the host tests) (`apt install sdcc make gcc`).

```
make              # the ROM images in build/ (see below) and the emulator page
make test         # 6437 logic checks on this computer
make check        # every test suite, without bumping the build number
make emulator     # update emulator/index.html and run its checks (needs clang and Node)
test/sim_smoke.sh # runs the real image in the uCsim 8051 simulator (apt install sdcc-ucsim)
```

There is one ROM image per chip size, made by `tools/rom_image.py`: `build/fcb1010_27c128.bin` (16KB), `build/fcb1010_27c256.bin` (32KB, also for a 28C256) and `build/fcb1010_27c512.bin` (64KB, the 32KB image twice). Each is the code padded with 0xFF and repeated to fill the chip; the build fails if the code outgrows a chip. The code is linked for addresses below 32KB only, since how the FCB1010 wires the top address line of a 27C512 socket is not known.

If clang (with the wasm32 target) is installed, `make` also builds the browser emulator in `emulator/index.html`, so changes to `src/config.h` show up there too; otherwise it prints a note and leaves the page as it was.

The code is about 14.7KB (15014 bytes at the last build), so it needs a 16KB or larger chip. Each build checks the size against the 32KB ROM (the build fails if it does not fit).

Each build also updates this README: the code size above, the firmware version on configuration page 7, and every value taken from `src/config.h` (the option table below and the numbers quoted elsewhere). A GitHub Actions workflow (`.github/workflows/build.yml`) builds and tests every push and offers the firmware images as a download; it fails if the README was not updated after a change to `src/config.h`.

### Firmware version

The version shows on configuration page 7 as major, minor and build. Major and minor are set by hand in `src/version.h`. The build number in `src/version_build.inc` goes up by one each time `make` rebuilds the firmware image; test builds (`make check`, or `make BUILD_BUMP=0`) leave it alone.

### Compile-time configuration

These options are in `src/config.h`. Change them and rebuild. The current values are filled in by the build.

| Option | Current value | Meaning |
|---|---|---|
| `MIDI_CHANNEL` | 1 | MIDI channel (1 to 16) used until a channel is saved in configuration mode |
| `STOMP_CC_LIST` | 21, 22, 23, 24, 25, 26, 27, 28, 29, 30 | Controller number of each footswitch, 1 to 10 |
| `PEDAL_CC_LIST` | 7, 11 | Controller numbers of expression pedals A and B |
| `DEFAULT_PEDAL_MODE` | 0 | Mode of every footswitch until one is saved: 0 stomp, 1 stomp with momentary, 2 momentary, 3 inverted momentary |
| `MOMENTARY_HOLD_MS` | 600 | Long press time (ms): stomp with momentary reverts after it, and the configuration save / cancel hold |
| `CONFIG_HOLD_MS` | 1000 | UP + DOWN hold (ms) that opens configuration mode |
| `PROGRAM_WRAP` | 0 | 1: program change wraps between 127 and 0 (and the bank between the last and 1); 0: both stop at the ends |
| `BANK_COUNT` | 25 | Number of footswitch banks, 1 to 25 (each takes ten 8-byte EEPROM records; 25 is all that fit below the shared records) |
| `DISPLAY_PROGRAM_OFFSET` | 0 | Added to the displayed program number: 0 shows 0 to 127, 1 shows 1 to 128 |
| `PROGRAM_REPEAT_DELAY_MS` | 400 | UP / DOWN hold (ms) before auto-repeat starts |
| `PROGRAM_REPEAT_HZ` | 4 | Auto-repeat rate (steps per second) |
| `PROGRAM_REPEAT_FAST_AFTER_MS` | 2700 | Hold time (ms) after which the repeat speeds up |
| `PROGRAM_REPEAT_FAST_HZ` | 25 | Fast auto-repeat rate (steps per second) |
| `PEDAL_HYST` | 2 | ADC counts a pedal must move before a new value is sent (noise filter) |
| `PEDAL_END_SNAP` | 2 | Readings this close (ADC counts) to the calibrated heel or toe send exactly 0 or 127 |
| `CAL_MIN_SPAN` | 40 | Smallest heel-to-toe movement (ADC counts) accepted by calibration |
| `CHANNEL_DIGIT_FLASH_MS` | 250 | Flash half-period (ms) of the `0` after switch 10 on the channel page |
| `CHANNEL_DIGIT_TIMEOUT_MS` | 5000 | How long (ms) the channel page waits for a second digit after switch 10 |
| `CONFIG_CHANGED_FLASH_MS` | 250 | CONFIG LED flash half-period (ms) while a change is unsaved |
| `MIDI_RX_FLASH_MS` | 60 | How long (ms) the MIDI CHAN LED lights for each received byte |
| `VERSION_SHOW_MS` | 1000 | Time (ms) each version part shows on page 7; each "riban" frame shows for half of it. A multiple of 200, at most 3600 |
| `DEBOUNCE_SAMPLES` | 3 | Consecutive key scans (12ms apart) that must agree before a press or release counts |
| `KEY_ACTIVE_LEVEL` | 1 | Input level of a pressed key. Change it if every key reads inverted |
| `STARTUP_TICKS` | 100 | 2ms ticks to settle at power-on; key events in this time are discarded |
| `MIDI_TX_QUEUE` | 128 | MIDI OUT queue size in bytes (a power of two) |
| `MIDI_RX_QUEUE` | 128 | MIDI IN queue size in bytes (a power of two) |
| `KEY_EVENT_QUEUE` | 16 | Key event queue size (a power of two) |

`STOMP_COUNT` and `TICK_MS` describe the hardware and should not be changed. The queues are in the external SRAM (see Memory); each can be up to 256 bytes.

### Settings storage

Settings are stored in the 24C16 EEPROM (2KB, addresses 0x000-0x7FF) as 6-byte records, each 8-byte aligned (its last two bytes, 0x796-0x797, 0x79E-0x79F and so on, are unused). The four shared records are at the top (0x7E0-0x7FF). Each footswitch bank has ten records, one per footswitch: bank 1 at 0x790-0x7DF and each further bank 80 bytes (0x50) lower, so bank b starts at 0x790 - 0x50 × (b - 1) (bank 2 at 0x740, bank 25 at 0x010). The 2KB holds 256 records: 4 shared, 250 for 25 banks, and 2 spare (0x000-0x00F).

The stock firmware keeps its presets from 0x000 upwards, so saving any bank but bank 1 can overwrite them (bank 1 and the shared records are where the stock presets are least likely to be). A bank that has never been saved still holds whatever was there before, which fails the magic and checksum test and loads as the defaults. To go back to the stock firmware, restore its presets from a SysEx backup or reset them to the factory settings.

| Address | Record | Byte 0 | Byte 1 | Byte 2 | Byte 3 | Byte 4 | Byte 5 |
|---|---|---|---|---|---|---|---|
| bank start + 8 × (n - 1) | Footswitch n (1-10) of a bank (bank 1: 0x790, 0x798 ... 0x7D8) | magic 0xD0 + (n - 1) | bits 0-1: message type; bits 2-3: pedal mode | number (0-127) | ON value (0-127) | OFF value (0-127) | checksum; salts 0x96, 0x69, 0x55, 0xAA, 0x33, 0xCC, 0x0F, 0xF0, 0x5F, 0xF5 for switches 1-10 (the same in every bank) |
| 0x7E0 | SWITCH 1 relay | magic 0xC3 | followed footswitches 1-8 (bit 0 = switch 1) | bits 0-1: footswitches 9-10; bit 2: follows bank select | EXP 1 trigger value (1-127, 0 = off) | EXP 2 trigger value (1-127, 0 = off) | checksum, salt 0x3C |
| 0x7E8 | SWITCH 2 relay | magic 0xC4 | as SWITCH 1 | as SWITCH 1 | as SWITCH 1 | as SWITCH 1 | checksum, salt 0xC3 |
| 0x7F0 | Pedal calibration | magic 0xC1 | EXP 1 heel reading | EXP 1 toe reading | EXP 2 heel reading | EXP 2 toe reading | checksum, salt 0x5A |
| 0x7F8 | Channel | magic 0xC2 | MIDI channel (1-16) | unused (saved as 0) | unused (saved as 0) | unused (saved as 0) | checksum, salt 0xA5 |

- **Checksum:** the XOR of bytes 0-4 and the record's salt.
- **Pedal modes:** in bits 2-3 of each footswitch record's type byte: 0 stomp, 1 stomp with momentary, 2 momentary and 3 inverted momentary. Bits 4-7 are ignored. (Earlier builds kept every mode in the channel record; their footswitch records read as stomp mode, and their stored modes are ignored, so set the modes again.)
- **Calibration:** the readings are the raw 8-bit ADC values.
- **Footswitch messages:** the type is 0 CC, 1 note, 2 program change; type 3 is unused and loads as CC with the same values. Values over 127 lose bit 7. A program change sends the ON value as the program when ON and the OFF value when OFF (both the same: a single program, sent on every press); it ignores the number. (Builds before this change used the number and the ON value; a program change record saved by one now sends its old second program when on and its old OFF value, normally 0, when off, so set it again.)
- **Missing or invalid records:** a record with the wrong magic or checksum loads as the defaults:
  - footswitch messages are CC `STOMP_CC_LIST` with ON value 127 and OFF value 0;
  - relays follow nothing and have no triggers;
  - calibration is heel 0, toe 255;
  - the channel and pedal modes come from `src/config.h` (a footswitch record that is missing or invalid has `DEFAULT_PEDAL_MODE`).
- **Out-of-range values:** a stored channel outside 1-16 loads as the default channel, and a trigger value over 127 loads as off (no trigger).
- **Saving:** a save writes each record in one page write (the six bytes are within one 16-byte page) and reads it back in one sequential read. Loading reads a record the same way.
- **CONFIG LED:** it compares the settings being edited with what is read back from these records (the bank in use and the shared ones).

### Hardware notes

The hardware map (latch addresses, LED and key wiring, ADC and EEPROM pins) comes from jspyve's public-domain reverse-engineering library at github.com/jspyve/FCB1010-lib, not from Behringer documents or from my own measurements. If the key polarity or an LED is wrong on your unit, `KEY_ACTIVE_LEVEL` in `src/config.h` or the LED tables in `src/hw.c` are where to look. The relay outputs are driven from bits 0 and 1 of the key column latch, as in jspyve's library; whether a set bit closes or opens a relay, and which bit is which jack, is unverified. The letters on the display (`Er`, `Lo`, `Hi`, the "riban" scroll and the hex digits) are drawn from segment tables in `src/hw.c` that have not been checked on a real display.

### Finding the SRAM (probe image)

The board has a 32KB static RAM (IC14). The factory schematic puts it at 0x8000-0xFFFF, and the firmware uses it there (see Memory), but that has not been checked on a real board. `make probe` builds a separate test image that looks for it: `build/probe_27c512.bin` (and `probe_27c256.bin`, `probe_27c128.bin`), flashed like the firmware. It shares the display, LED, key and pedal code with the firmware, and nothing else.

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

### What has and has not been tested

Tested:
- The controller logic: stomp toggling, all four pedal modes, CC, note and program change footswitch messages and the footswitch editor (typed and pedal values, accept, revert, saved records), program change on release with auto-repeat, the UP + DOWN chord, every configuration page and its saved bytes, pedal scaling, hysteresis, calibration flow, a full MIDI buffer, bad input, and MIDI IN parsing including running status, split messages, real-time bytes, SysEx, other channels and the 63/64 threshold, plus the momentary long-press behaviour: every switch, two switches at once, duplicate or unmatched hold and release events, a full MIDI queue at press and at release, calibration, and incoming MIDI CC changes during a hold (`make test`).
- The browser emulator, which runs the same control code, with a scripted smoke test (`make emulator`), including its MIDI configuration panel driving the emulated pedal (Find, read, set, save, discard).
- Configuration over MIDI in the controller logic: every parameter, the dump and its pacing, a dump fed back in, selection and deselection, the commands, and the run-mode-only rule (`make test`).
- Footswitch banks in the controller logic: choosing with UP / DOWN in bank mode (taps, held repeat, ends), states carried over by CC and note (nothing sent), resting states of the rest, program change switches, holds and pending reverts across a change, saving and comparing the bank in use, and the bank parameter over MIDI (`make test`); shared CC and note states within a bank (`make test`); in the emulator and its configuration panel (`make emulator`); and in the simulator, with the stack depth of the EEPROM reads (`test/sim_smoke.sh`).
- The real 8051 image in a simulator: key scan and debounce, toggle on and off, program change, MIDI UART output with the correct bytes, the UP + DOWN chord opening and closing configuration mode (read back from RAM, including the stack depth of the save path), the long-press behaviour just under (stays latched) and just over (reverts) the long-press time, the power-on and page 7 LEDs and display, and the stack under receive load (`test/sim_smoke.sh`).

Not tested, because it needs real hardware:
- **Everything on an actual FCB1010** (see Hardware notes).
- The pedal ADC reading and the EEPROM calibration and settings save and load. The settings records have host-level round-trip tests only, and the simulator does not model them. The code is adapted from the same library.
- **MIDI IN on the real image.** The simulator's UART input would not deliver bytes to the firmware. The parser is tested on the host, and the simulator confirms the receive interrupt queues and drains bytes without disturbing key handling or the stack, but the step from a received byte to a lit LED has not been run end to end on the 8051 image. The FCB1010's MIDI IN feeds the same UART, per the reference code, which receives SysEx through it.
- **The 0.6 second long-press threshold on real hardware.** It is counted from the 2ms scan timer, which assumes a 12MHz crystal. The simulator measures it correctly in simulated time, but a different crystal speed or a slower scan would shift it.
- The configuration panel with a real pedal and MIDI interface. Its emulator target is tested, and so is its Web MIDI code against a simulated interface, but not with a real one.
- The 31250 baud MIDI rate on real silicon. It is computed for a 12MHz crystal; the simulator confirms the bytes but not the timing.

A wrong LED or key map does no electrical harm, but it will look wrong.

### Memory

The 8032 has 256 bytes of internal RAM, which holds the stack, the flags and the small state. The larger buffers and tables live in the board's 32KB SRAM, which the factory schematic places at 0x8000-0xFFFF in the external data space (its chip select is A15 through an inverter; below 0x8000 every write reaches the display and key latches, so nothing is placed there). This is from the schematic only: it has not been checked on a real board, and the probe image (above) is the way to check it before trusting the firmware. In the SRAM: the MIDI OUT queue (128 bytes, about 40ms of messages), the MIDI IN queue (128 bytes, enough to ride over an EEPROM save), the key event queue (16), the key debounce and hold counters, the footswitch messages of the bank in use and the CC and note state table (256 bytes), about 580 bytes in all. The stack starts at 0x77 with 137 bytes free, and peaks at 0x98 in the simulator (with both interrupts nested on top). If you add features, check `build/fcb1010.mem` for the stack headroom and run `test/sim_smoke.sh`, which fails if the stack gets too close to the end.

### Source layout

```
src/ctl.c      controller behaviour (portable, unit tested)
src/hw.c       FCB1010 hardware: LED and key scan, 7-segment display, MIDI UART, ADC, I2C EEPROM
src/keys.h     key debounce and hold detection, shared by the firmware, tests and emulator
src/main.c     start-up and main loop
src/config.h   compile-time options (see Compile-time configuration)
src/version.h  firmware major and minor version
test/          host tests, emulator smoke test and simulator script
emulator/      browser emulator (runs ctl.c compiled to WebAssembly)
tools/         build hooks: size check, build number, README updates
```
