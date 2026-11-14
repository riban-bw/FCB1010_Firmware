#ifndef CTL_H
#define CTL_H

#include "platform.h"

// Key codes as reported by the key scanner. Codes 0..4 are footswitches
// 1..5, 6..10 are footswitches 6..10, and DOWN / UP sit between them.
#define CTL_KEY_DOWN 5
#define CTL_KEY_UP   11

// Load calibration and show the starting program number.
void    ctl_init(void);

// A debounced key press.
void    ctl_key_press(uint8_t code);

// Call once per scan tick (TICK_MS). Times the UP / DOWN auto-repeat and finishes any
// MIDI message that had to wait for room in the MIDI OUT queue.
void    ctl_tick(void);

// The key has now been held down for the hold threshold (sent once per press).
void    ctl_key_hold(uint8_t code);

// A debounced key release. A footswitch that was held past the threshold puts its
// stomp back to the state it had before the press.
void    ctl_key_release(uint8_t code);

// One received MIDI IN byte. A control change on MIDI_CHANNEL whose number matches
// a stomp switch sets that switch on (value 64..127) or off (0..63) and updates its
// LED. Nothing is sent back out, so connecting MIDI OUT to MIDI IN cannot loop.
void    ctl_midi_in(uint8_t byte);

// A new averaged 8-bit reading from pedal channel 0 (A) or 1 (B).
void    ctl_pedal_sample(uint8_t channel, uint8_t raw);

// Map a raw reading to 0..127 given calibrated heel and toe readings.
uint8_t ctl_scale(uint8_t raw, uint8_t heel, uint8_t toe);

#ifdef CTL_INSPECT
// Emulator-only state readout (see the end of ctl.c). Mode: 0 run, 1 config. Page: 0 channel,
// 1 pedal modes, 2 calibration. Calibration step: 0 done, 1 heel, 2 toe. A sent value of 255
// means nothing has been sent for that pedal yet. Pedal modes are read at 16 + switch index.
#define CTL_INSPECT_MODE           0
#define CTL_INSPECT_PROGRAM        1
#define CTL_INSPECT_CHANNEL        2
#define CTL_INSPECT_PAGE           3
#define CTL_INSPECT_SELECTED       4
#define CTL_INSPECT_CAL_STEP       5
#define CTL_INSPECT_HEEL_A         6
#define CTL_INSPECT_HEEL_B         7
#define CTL_INSPECT_TOE_A          8
#define CTL_INSPECT_TOE_B          9
#define CTL_INSPECT_SENT_A         10
#define CTL_INSPECT_SENT_B         11
#define CTL_INSPECT_DISPLAY_ERROR  12
#define CTL_INSPECT_CAL_MIN_SPAN   13
#define CTL_INSPECT_PEDAL_MODE     16

uint8_t ctl_inspect(uint8_t what);
#endif

#endif
