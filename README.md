# riban FCB1010 stomp-box firmware

The Behringer FCB1010 is a sturdy MIDI foot controller, but its stock firmware is built around presets and banks, which makes it awkward to use as a simple row of effect switches. This replacement firmware turns it into a straightforward MIDI stomp-box: each of the ten footswitches is an on/off switch with its own LED, UP and DOWN step through programs, and the two expression pedals send continuous controllers. Everything is set up on the pedal itself, with no computer or editor needed.

It suits anyone who wants to switch effects in a DAW, a plugin host or a MIDI-capable amp or synth the way they would on a pedalboard: press to turn on, press again to turn off, and see at a glance what is on.

## Features

- 10 footswitches, each sending its own MIDI controller (CC), note or program change, with a choice of four switch behaviours
- 25 banks of footswitch settings, chosen with UP and DOWN
- Program mode (UP + DOWN tapped): program change from UP and DOWN, with fast scrolling when held, or typed on the footswitches
- 2 expression pedals
- 2 relay outputs (the SWITCH 1 and SWITCH 2 jacks), triggered by any footswitch or pedal, to switch amp channels or other gear
- Footswitches that send the same controller or note share its state, in every bank
- Follows incoming MIDI, so its LEDs stay in step with your DAW or synth
- Selectable MIDI channel
- Simple configuration on the pedal, saved in its memory
- Or configure it from a computer over MIDI, with a web page (no special software or SysEx)

[Try the emulator in your browser](https://riban-bw.github.io/FCB1010_Firmware/emulator/). It runs this firmware's own control code, so you can try the footswitches, pedals and configuration pages before flashing a chip. Its on-screen hints explain each control as you use it.
## Documentation

For players:

- [Using the pedal](docs/using.md): the controls, the LEDs, footswitch modes and messages, programs, banks, relays and following your DAW
- [Configuration](docs/configuration.md): the configuration pages on the pedal, the footswitch editor, and configuring from a computer
- [Installing](docs/installing.md): which chip to program and how to fit it. **This replaces the stock firmware completely**: read the warning there first

Technical reference:

- [MIDI implementation](docs/midi.md): what is sent and received, and the NRPN messages for configuration over MIDI
- [Settings file format](docs/settings-file.md): the JSON file the configuration panel saves and loads
- [Technical implementation](docs/technical.md): how the firmware works inside, its timing, memory, EEPROM settings layout and hardware notes
- [Building](docs/building.md): building and testing, the firmware version, compile-time options and the source layout
- [What has and has not been tested](docs/testing.md)
- [Finding the SRAM](docs/sram-probe.md): a test image that looks for the board's external RAM

## Quick start

1. Read the warning in [Installing](docs/installing.md), back up the original chip, and program a new one with the image for your chip size (`build/fcb1010_27c512.bin` for a 27C512).
2. Power on: "riban" scrolls, then the display shows `1` (footswitch bank 1).
3. Press a footswitch to turn its effect on and off, UP or DOWN to change footswitch bank, and UP + DOWN tapped together to switch UP, DOWN and the footswitches to changing program; [Using the pedal](docs/using.md) lists the messages each control sends out of the box. Hold UP and DOWN together to configure it (see [Configuration](docs/configuration.md)).
