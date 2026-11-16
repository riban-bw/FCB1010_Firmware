#ifndef CONFIG_H
#define CONFIG_H

// User-tunable settings. Change these and rebuild to remap the pedal.

// MIDI channel used until a different one is saved in configuration mode, 1..16.
#ifndef MIDI_CHANNEL
#define MIDI_CHANNEL 1
#endif

// Pedal mode every footswitch starts with until it is changed in configuration mode:
// 0 = stomp (toggle), 1 = stomp with momentary on a long press, 2 = momentary.
// 3 = inverted momentary (0 while pressed, 127 on release).
#ifndef DEFAULT_PEDAL_MODE
#define DEFAULT_PEDAL_MODE 0
#endif

// Pressing UP and DOWN together for this long opens configuration mode.
// In configuration mode UP + DOWN does nothing; a long UP (MOMENTARY_HOLD_MS, the
// key hold threshold) saves and closes it, and a long DOWN cancels and closes it.
#ifndef CONFIG_HOLD_MS
#define CONFIG_HOLD_MS 1000
#endif

// Channel page: after switch 10 the display shows 10 and its 0 flashes at this
// half-period while a second digit can be entered; after the timeout the channel stays
// 10. The timeout must be a whole number of half-periods.
#define CHANNEL_DIGIT_FLASH_MS    250
#define CHANNEL_DIGIT_TIMEOUT_MS  5000

// Configuration mode: once any setting has been changed, the CONFIG LED flashes
// (this long on, then the same off) until the changes are saved or cancelled.
#define CONFIG_CHANGED_FLASH_MS   250

// Run mode: the MIDI CHAN LED lights for this long after each received MIDI byte.
// Real-time bytes (clock, active sensing...) do not light it.
#define MIDI_RX_FLASH_MS          60

// Power-on and configuration page 8: every LED lights (once, and stays lit) while
// ". riban ." scrolls across the display, one frame per SCROLL_FRAME_MS (ten frames, so
// power-on takes 10 x SCROLL_FRAME_MS, the start-up settling included). Power-on then
// goes to run mode. Page 8 then shows the firmware version, each part (major, minor,
// build) for VERSION_SHOW_MS, then a blank display for the same time, and repeats.
// Both are multiples of 50, SCROLL_FRAME_MS at least 100, and 10 x SCROLL_FRAME_MS +
// 4 x VERSION_SHOW_MS at most 12750. Smaller SCROLL_FRAME_MS scrolls faster (200:
// power-on in 2 seconds).
#ifndef SCROLL_FRAME_MS
#define SCROLL_FRAME_MS           200
#endif
#define VERSION_SHOW_MS           1000
// At power-on every LED lights for BOOT_LAMP_MS (from power-on, a multiple of 50), then
// shows its normal state while the scroll finishes. Page 8 keeps them lit throughout.
#ifndef BOOT_LAMP_MS
#define BOOT_LAMP_MS              1000
#endif

// Number of footswitches acting as stomp switches (switches 1..10).
#define STOMP_COUNT 10

// Controller number sent by footswitches 1..10 (toggles 127 / 0).
#define STOMP_CC_LIST 21, 22, 23, 24, 25, 26, 27, 28, 29, 30

// Footswitch banks: while DIRECT SELECT is lit (the UP + DOWN toggle), UP / DOWN choose
// one of this many banks, each with its own message and pedal mode for every footswitch.
// Each bank takes ten 8-byte records of the 2KB settings EEPROM, so 1 to 25 fit.
#ifndef BANK_COUNT
#define BANK_COUNT 25
#endif

// Controller numbers sent by expression pedal A and pedal B.
// 7 = channel volume, 11 = expression.
#define PEDAL_CC_LIST 7, 11

// UP / DOWN program change behaviour.
// 1 = wrap around (127 -> 0, 0 -> 127), 0 = stop at the ends.
#ifndef PROGRAM_WRAP
#define PROGRAM_WRAP 0
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
#define MOMENTARY_HOLD_MS 600
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

// Held longer: once the key has been held about PROGRAM_REPEAT_FAST_AFTER_MS (the
// first repeat step at or after it), the repeat speeds up to PROGRAM_REPEAT_FAST_HZ.
#ifndef PROGRAM_REPEAT_FAST_AFTER_MS
#define PROGRAM_REPEAT_FAST_AFTER_MS 2700
#endif

#ifndef PROGRAM_REPEAT_FAST_HZ
#define PROGRAM_REPEAT_FAST_HZ 25
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

// Queue sizes in bytes (each a power of two, at most 256; one slot is always left empty).
// They live in the board's external SRAM, and the emulator uses the same sizes. MIDI OUT
// 128 holds about 40ms of messages at 31250 baud, MIDI IN 128 about 40ms of input (enough
// to ride over an EEPROM save, which blocks the main loop for tens of ms).
#ifndef MIDI_TX_QUEUE
#define MIDI_TX_QUEUE    128
#endif
#ifndef MIDI_RX_QUEUE
#define MIDI_RX_QUEUE    128
#endif
#ifndef KEY_EVENT_QUEUE
#define KEY_EVENT_QUEUE  16
#endif

#endif
