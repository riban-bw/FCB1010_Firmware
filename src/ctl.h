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

// Call once per tick during the start-up settling, before ctl_tick runs: keeps the
// power-on scroll moving.
void    ctl_settle_tick(void);

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
// Editor-only state readout (see the end of ctl.c). Mode: 0 run, 1 config. Page: 0 pedal modes,
// 1 channel, 2 heel calibration, 3 toe calibration, 4 / 5 SWITCH 1 / 2, 6 bank program
// change, 7 pedal pickup, 8 version (lamp test). Calibration step: 0 reading
// taken, 1 heel ("Lo"), 2 toe ("Hi"), 3 error ("Er"). A sent value of 255
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
#define CTL_INSPECT_RELAYS         12
#define CTL_INSPECT_CAL_MIN_SPAN   13
// The footswitch editor: 0 closed, else 1 + its parameter (type, number, on value, off
// value).
#define CTL_INSPECT_EDIT           14
// The loaded footswitch bank, from 0.
#define CTL_INSPECT_BANK           15
#define CTL_INSPECT_PEDAL_MODE     16
// The bank's program change: 0 none, else the program + 1 (after the ten pedal modes).
#define CTL_INSPECT_BANK_PC        30
// Bit 0: program mode (DIRECT SELECT lit); bit 1: a program number being typed (run mode)
// or page 7's "--" after footswitch 10 (config mode).
#define CTL_INSPECT_PC_MODE        31
// Relay trigger values at 32 + relay * 2 + pedal; follow masks at 36 + relay * 2 (low
// byte) and 37 + relay * 2 (high byte). Pages 3 and 4 are SWITCH 1 / 2.
#define CTL_INSPECT_SW_THR         32
#define CTL_INSPECT_SW_MASK        36
// Footswitch messages: the 30 bytes of msg[], three per switch: number (bit 7 = type
// bit 0), on value (bit 7 = type bit 1), off value.
#define CTL_INSPECT_MSG            40
// Pedal pickup: bits 0-1 pedals A / B in pickup mode, bits 2-3 waiting (not picked up).
#define CTL_INSPECT_PICKUP         70
// Pedal A's and B's CCs (71, 72); the pedal page: bit 0 its CC editor open, bit 1 pedal
// B shown (73), and the footswitch last pressed there (74, 0-9).
#define CTL_INSPECT_PEDAL_CC       71
#define CTL_INSPECT_PCC            73
#define CTL_INSPECT_PED_SEL        74
// The bank the save prompt shows (1 to BANK_COUNT), 0 when it is not open.
#define CTL_INSPECT_SAVE           75
// EXP pages: 1 while a heel / toe footswitch's Lo / Hi flashes (the next press takes it).
#define CTL_INSPECT_CAL_ARMED      76

uint8_t ctl_inspect(uint8_t what);
#endif

#endif
