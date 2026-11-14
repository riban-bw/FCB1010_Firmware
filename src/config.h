#ifndef CONFIG_H
#define CONFIG_H

// User-tunable settings. Change these and rebuild to remap the pedal.

// MIDI channel used until a different one is saved in configuration mode, 1..16.
#ifndef MIDI_CHANNEL
#define MIDI_CHANNEL 1
#endif

// Pedal mode every footswitch starts with until it is changed in configuration mode:
// 0 = stomp (toggle), 1 = stomp with momentary on a long press, 2 = momentary.
#ifndef DEFAULT_PEDAL_MODE
#define DEFAULT_PEDAL_MODE 1
#endif

// Pressing UP and DOWN together for this long opens configuration mode, and again
// for this long saves and closes it. A shorter press of both moves to the next page.
#ifndef CONFIG_HOLD_MS
#define CONFIG_HOLD_MS 1000
#endif

// Number of footswitches acting as stomp switches (switches 1..10).
#define STOMP_COUNT 10

// Controller number sent by footswitches 1..10 (toggles 127 / 0).
#define STOMP_CC_LIST 20, 21, 22, 23, 24, 25, 26, 27, 28, 29

// Controller numbers sent by expression pedal A and pedal B.
// 7 = channel volume, 11 = expression.
#define PEDAL_CC_LIST 7, 11

// UP / DOWN program change behaviour.
// 1 = wrap around (127 -> 0, 0 -> 127), 0 = stop at the ends.
#ifndef PROGRAM_WRAP
#define PROGRAM_WRAP 1
#endif

// Value added to the program number on the 7-segment display.
// 0 shows the number actually sent (0..127), 1 shows 1..128.
#ifndef DISPLAY_PROGRAM_OFFSET
#define DISPLAY_PROGRAM_OFFSET 0
#endif

// A pedal reading must move this many ADC counts (8-bit) from the
// last sent position before a new controller value is sent, so ADC
// noise does not flood the MIDI line.
#ifndef PEDAL_HYST
#define PEDAL_HYST 2
#endif

// Readings within this many counts of the calibrated heel / toe
// position snap to exactly 0 / 127.
#ifndef PEDAL_END_SNAP
#define PEDAL_END_SNAP 2
#endif

// Calibration is rejected if heel and toe are closer than this.
#define CAL_MIN_SPAN 40

// A footswitch held for longer than this acts as a momentary switch: the state it
// toggled to is undone when the switch is released. Released sooner, it stays latched.
// Each key is checked every 12ms, so the time is rounded down to a multiple of 12ms.
#ifndef MOMENTARY_HOLD_MS
#define MOMENTARY_HOLD_MS 1000
#endif

// Length of one scan tick in milliseconds. This is set by the timer 0 reload in hw.c
// (2000 machine cycles at 12MHz); it is also the time base for the repeat timing below.
#define TICK_MS 2

// UP / DOWN program change. Nothing is sent when the key is pressed. A tap sends one
// step when it is released. If the key is held, after PROGRAM_REPEAT_DELAY_MS the program
// steps and then repeats PROGRAM_REPEAT_HZ times a second, shown on the display only. The
// final program number is sent once, when the key is released.
#ifndef PROGRAM_REPEAT_DELAY_MS
#define PROGRAM_REPEAT_DELAY_MS 400
#endif

#ifndef PROGRAM_REPEAT_HZ
#define PROGRAM_REPEAT_HZ 4
#endif

// Consecutive scans a key must read the same before it counts.
// Each key is scanned about every 12ms, so 3 gives roughly 36ms.
#ifndef DEBOUNCE_SAMPLES
#define DEBOUNCE_SAMPLES 3
#endif

// Electrical level of the key-row inputs when a switch is pressed.
// Taken from the reverse-engineered reference code (pressed = 1).
#ifndef KEY_ACTIVE_LEVEL
#define KEY_ACTIVE_LEVEL 1
#endif

// 2ms ticks to wait at power-up so the key debouncer and pedal averager settle. Key
// events from this time (a switch held at power-on) are discarded.
#define STARTUP_TICKS 100

// Queue sizes in bytes (each a power of two; one slot is always left empty). They are cut
// to fit the 8032's 256 bytes of RAM, and the emulator uses the same sizes.
#define MIDI_TX_QUEUE    16
#define MIDI_RX_QUEUE    8
#define KEY_EVENT_QUEUE  8

#endif
