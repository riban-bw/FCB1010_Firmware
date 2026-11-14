#ifndef HW_H
#define HW_H

#include "platform.h"
#include "keys.h"

// FCB1010 hardware layer (W78C032 / 8051 core, 12MHz).

// Interrupt handlers. main.c must see these prototypes so SDCC emits the
// vector table entries.
void timer0_isr(void) __interrupt(1);
void serial_isr(void) __interrupt(4);

// Set up ports, latches, MIDI UART, scan timer and enable interrupts.
void    hw_init(void);

// Returns 1 once per 2ms scan tick.
uint8_t hw_tick_elapsed(void);

// Call once per tick. Returns 1 when new averaged readings for both
// pedals are available in *a and *b.
uint8_t hw_poll_pedals(uint8_t *a, uint8_t *b);

// Key events are encoded as in keys.h (KEY_EV_PRESS, KEY_EV_HOLD, KEY_EV_RELEASE).

// Next key event, or KEY_EV_NONE. A press is reported once debounced, HOLD once the key
// has been down for MOMENTARY_HOLD_MS, and RELEASE once it has been up long enough.
uint8_t hw_next_event(void);

// Discard any queued key presses.
void    hw_flush_events(void);

// 1 if the key is currently held down (after debouncing).
uint8_t hw_key_is_down(uint8_t code);

#endif
