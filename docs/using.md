# Using the pedal

[Back to the README](../README.md)

| Control | What it does |
|---|---|
| Footswitches 1 to 10 | Turn an effect on or off. The LED shows whether it is on. Each switch can instead act while held (see Footswitch modes). In program mode they choose a program instead (see [Changing program](#changing-program)) |
| UP / DOWN | Next / previous footswitch bank. The display shows the bank number, and the new bank takes over when you let go. Hold to scroll. While DIRECT SELECT is lit (program mode), next / previous program instead (see [Changing program](#changing-program)) |
| Expression pedal A | Volume (CC 7), 0 to 127. Each pedal sends every move, or waits to pick up the current value (see [Expression pedals](#expression-pedals)) |
| Expression pedal B | Expression (CC 11), 0 to 127 |
| UP + DOWN tapped together | Turns program mode on or off (DIRECT SELECT lit). In program mode UP and DOWN change program, and the footswitches choose a program by number instead of switching effects (see [Changing program](#changing-program)). Nothing is sent |
| UP + DOWN held 1s | Opens configuration mode (see [Configuration](configuration.md)) |

Everything uses one MIDI channel, channel 1 unless you change it in configuration mode. Footswitches 1 to 10 send CC 21 to 30, with value 127 for ON and 0 for OFF. You can give any footswitch a different message (see [Footswitch messages](#footswitch-messages)).

## The LEDs

| LED | While playing | In configuration mode |
|---|---|---|
| Footswitches 1 to 10 | Lit while that footswitch is ON | Depends on the page: the selected footswitch (page 1), the footswitch being edited (editor), the footswitches a relay follows (pages 5 and 6), footswitches 1 and 2 for the pedals in pickup mode (page 8) |
| SWITCH 1, SWITCH 2 | Lit while that relay is closed | Marks page 5 (SWITCH 1) or page 6 (SWITCH 2) |
| DIRECT SELECT | Lit in program mode: UP, DOWN and the footswitches change program. Dark while UP and DOWN choose footswitch banks | Page 7: lit (it marks the page). Dark on the other pages |
| MIDI FUNCTION | Dark | Marks page 1 (pedal modes) |
| MIDI CHAN | Flickers when MIDI arrives | Marks page 2 (MIDI channel) |
| EXP 1, EXP 2 | Flashing while that pedal waits to pick up (faster the closer it is), otherwise dark | Both mark pages 3 and 4 (calibration). Pages 5 and 6: lit while the relay follows that pedal. Page 8: lit for a pedal in pickup mode |
| SELECT, NUMBER, VALUE 1, VALUE 2 | Dark | Footswitch editor: the setting being edited (type, number, ON value, OFF value) |
| CONFIG | Dark | Lit; flashes while a change is not saved |

For a second at power-on, and throughout configuration page 9, every LED is lit (on page 9 the CONFIG LED still flashes for an unsaved change, and the footswitch LEDs show the steps).

## Footswitch modes

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

## Footswitch messages

Each footswitch sends one of three kinds of message, set in the footswitch editor (see [Configuration](configuration.md#footswitch-editor)):

| Type | Display | ON | OFF |
|---|---|---|---|
| CC | `CC` | The controller number with the ON value | The controller number with the OFF value |
| Note | `nt` | Note on: the note number, with the ON value as velocity | Note off: the note number, with the OFF value as velocity |
| Prog | `Pr` | Program change: the ON value | Program change: the OFF value |

Every type works in any of the footswitch modes. A program change switch has two programs and toggles between them: for example, in stomp mode one press sends program 5 and turns it ON (its LED lights), and the next sends program 6 and turns it OFF. If you give it the same program twice, it sends that program on every press and nothing when you let go, whatever its mode, and its LED lights only while you hold it. Whenever a footswitch sends a program, it becomes the current program: the display shows it in program mode, and UP and DOWN step on from it there.

Footswitches that send the same controller (CC) or the same note share one state. When one of them sends, every other footswitch with that CC or note follows, just as if the message had arrived on MIDI IN (see [Keeping in step with your DAW](#keeping-in-step-with-your-daw)): a note on turns them ON and a note off OFF; a CC value turns a switch ON if it is that switch's ON value and OFF if it is its OFF value, and otherwise ON from 64 up. So two switches on CC 40, one with values 127 / 0 and one with 100 / 20, light and go out together. A switch whose ON and OFF values are the same keeps its own state. The same holds across banks (see [Footswitch banks](#footswitch-banks)).

## Footswitch banks

The pedal holds 40 banks, each with its own message and mode for every footswitch, so you can keep a set of footswitches for each song or rig and switch between them. The relays, the MIDI channel and the pedal calibration are shared by every bank.

While DIRECT SELECT is dark, the display shows the bank number, and UP and DOWN step through the banks. The new bank takes over when you let go. Hold UP or DOWN to scroll through the banks. In program mode (DIRECT SELECT lit, see [Changing program](#changing-program)) the bank you chose stays in use.

Changing bank can send a program change if configured to do so (see [Configuration](configuration.md)). Each controller and note keeps its state, however many banks you pass through. A footswitch of the new bank takes the state its CC or note last had, from a footswitch in any bank or from incoming MIDI: for example, if footswitch 1 is CC 26 and is ON in bank 1, any footswitch with CC 26 in any other bank shows ON, and footswitch 1 is still ON when you come back to bank 1, even through banks that do not use CC 26. The pedal forgets these states when it is switched off. A CC is read by the value last sent, the same way as incoming MIDI, so the switches do not need the same ON and OFF values. A footswitch whose CC or note has not been used since power-on starts in its resting state, without sending anything: ON for inverted momentary and OFF for the other modes. A program change switch always starts OFF. A footswitch you are holding during the change does nothing when you let go (its CC or note keeps the state it had).

Configuration mode edits the bank in use, and saving stores that bank (with the shared settings). A bank you have never saved has the default footswitch messages and modes: CC 21 to 30 in most banks, but in bank 3 the footswitches play notes 48 to 57 (C3 to A3, footswitch 1 the lowest) and in bank 4 notes 60 to 69 (C4 to A4), each sounding while held (momentary, velocity 127). The pedal always starts in bank 1. A program a footswitch sends is remembered too, and UP and DOWN step on from it in program mode.

## Expression pedals

Each expression pedal works in one of two ways, chosen on configuration page 8 (see [Configuration](configuration.md)):

- **Immediate** (the default): every move is sent at once.
- **Pickup**: when its controller's value has changed somewhere else, the pedal sends nothing until it reaches that value. This avoids a jump in, say, volume when your DAW or a footswitch has moved it. The value changes elsewhere when a footswitch sends that controller or your DAW sends it to MIDI IN; at power-on it counts as 0. The pedal picks up when it comes within 2 steps of that value, or passes it, and then sends as usual.

While a pickup pedal waits, its EXP LED flashes: slowly when it is far from the value, faster as it gets closer, and it goes dark once the pedal has picked up. When you move it, the display shows how far it is from the value: `+20` means the pedal is 20 steps above the value (towards the toe), `-20` 20 steps below it (towards the heel). The display goes back to normal 2 seconds after the pedal stops moving.

## The relay outputs (SWITCH 1 and SWITCH 2)

Each of the two relay jacks can follow any mix of footswitches and the expression pedals. The relay closes when any of the things it follows is ON, and opens when they are all OFF. For a pedal, ON means pushed past a threshold you set. The SWITCH 1 and SWITCH 2 LEDs show the relay states. Nothing is set up until you choose, so out of the box the relays stay open.

## Changing program

Tap UP + DOWN together so that DIRECT SELECT lights: this is program mode, and the display shows the program number. Tap them again to go back to footswitch banks; the program you chose is kept. The taps send nothing.

In program mode UP and DOWN step the program number by one, from 0 to 127. The program change is sent when you let go of the switch. Hold the pedal to auto-repeat, initially slowly then rapidly. Program change message is only sent when you release the pedal.

The footswitches choose a program directly, as numbers (the footswitch effects are left as they are, and their LEDs keep showing them):

- **Footswitches 1 to 9** send programs 1 to 9 straight away.
- **Footswitch 10** starts a longer number: the display shows `--`, and the dot at the right flashes until the number is sent or dropped. Then type the digits with the footswitches (1 to 9 are digits 1 to 9, 10 is 0). The program is sent as soon as no more digits would fit, so 5, 6 sends 56 and 1, 2, 3 sends 123 (13 and up, or three digits). For a shorter number, such as 12 or 0, type it and press UP (ENTER). Press DOWN (ESCAPE) to give up: nothing is sent and the display shows the current program again.

## Keeping in step with your DAW

The pedal listens on its MIDI IN socket. When your DAW or synth sends one of the pedal's controllers on the pedal's channel, the matching LED changes to match: the switch's ON value turns it ON and its OFF value turns it OFF, and any other value counts as ON from 64 to 127 and OFF from 0 to 63. A note switch follows its note the same way: a note on turns it ON and a note off turns it OFF. Program changes are ignored. For example, CC 21 value 127 sets stomp 1 ON and lights its LED, and CC 21 value 0 turns it OFF. Every footswitch with that controller or note follows. The next press of that footswitch then carries on from there.

The pedal never sends these back out, so it is safe to connect MIDI OUT and MIDI IN to the same device. The MIDI CHAN LED flickers whenever MIDI arrives.

## Power-on

At power-on a banner scrolls across the display for 2 seconds and every LED lights for the first second. The display then shows the bank number and the pedal LEDs show their normal state Footswitch bank 1 is selected. The device is ready for use immediately, even before the banner finishes scrolling. Stomp states are OFF, except switches in inverted momentary mode, which start ON.