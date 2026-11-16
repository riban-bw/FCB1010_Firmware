#ifndef KEYS_H
#define KEYS_H

#include "platform.h"
#include "config.h"

// Key-event detection (debounce, hold threshold, event queue), shared by the firmware,
// the host tests and the browser emulator. The firmware expands KEY_SCAN inside the timer
// interrupt in hw.c; keys.c wraps the same macros as functions for the host and the emulator.
// It is a macro rather than a function so the 8051 interrupt makes no call: a call from the
// interrupt makes SDCC save every register, and the stack has only about 18 bytes spare.

// Twelve keys, scanned as two rows: keys k and k + 6 are read on the same tick, one
// column per tick, so each key is read every KEY_SCAN_SLOTS ticks.
#define KEY_COUNT       12
#define KEY_SCAN_SLOTS  6

// Key events are one byte: bits 0..3 are the key code (0..11) and bits 4..6 the type.
#define KEY_EV_PRESS    0x00
#define KEY_EV_HOLD     0x20
#define KEY_EV_RELEASE  0x40
#define KEY_EV_NONE     0xFF
#define KEY_EV_CODE(e)  ((uint8_t)((e) & 0x0F))
#define KEY_EV_TYPE(e)  ((uint8_t)((e) & 0x70))

#define KEY_DEBOUNCE_MASK   ((1u << DEBOUNCE_SAMPLES) - 1u)

// Scans per hold threshold.
#define KEY_SCAN_PERIOD_MS  (KEY_SCAN_SLOTS * TICK_MS)
#define KEY_HOLD_SCANS      (MOMENTARY_HOLD_MS / KEY_SCAN_PERIOD_MS)

#if KEY_HOLD_SCANS < 1 || KEY_HOLD_SCANS > 250
#error MOMENTARY_HOLD_MS must be between 12 and 3000
#endif

#define KEY_EVQ_MASK  (KEY_EVENT_QUEUE - 1)

// 1 if a ring buffer of KEY_EVENT_QUEUE bytes has no room for another event.
#define KEY_EVQ_FULL(head, tail)  ((((head) + 1) & KEY_EVQ_MASK) == (tail))

// Add an event to a ring buffer of KEY_EVENT_QUEUE bytes; dropped if the queue is full.
#define KEY_EVQ_PUSH(q, head, tail, e)                                         \
    do {                                                                       \
        if ((((head) + 1) & KEY_EVQ_MASK) != (tail))                           \
        {                                                                      \
            (q)[head] = (uint8_t)(e);                                          \
            (head) = (uint8_t)(((head) + 1) & KEY_EVQ_MASK);                   \
        }                                                                      \
    } while (0)

// One scan of one key. hist[idx]: bits 0..6 are the most recent samples, bit 7 the pressed
// state. hold[idx]: scans the key has been down since its debounced press (stops counting
// at the threshold). pressed is 1 if the key reads as pressed now. push is the name of a
// one-argument macro that queues an event, full an expression that is 1 while the queue
// has no room. Emits a press once debounced, one HOLD after the hold threshold, and a
// release once the key has read up long enough. A change only takes effect once its event
// is queued: with the queue full the key keeps its old state and the event is tried again
// on its next scan (12ms later), so a burst of presses (all twelve keys at once, say) is
// delayed, not lost.
#define KEY_SCAN(hist, hold, idx, pressed, push, full)                         \
    do {                                                                       \
        /* One local (the state in bit 7, the samples below): with two, the */ \
        /* 8051 interrupt spills one to RAM, which raises the stack start. */  \
        uint8_t h_ = (hist)[idx];                                              \
        h_ = (uint8_t)((h_ & 0x80) | (((h_ << 1) | (pressed)) & KEY_DEBOUNCE_MASK)); \
        if ((h_ & KEY_DEBOUNCE_MASK) == KEY_DEBOUNCE_MASK)                     \
        {                                                                      \
            if (!(h_ & 0x80))                                                  \
            {                                                                  \
                if (!(full))                                                   \
                {                                                              \
                    h_ |= 0x80;                                                \
                    (hold)[idx] = 0;                                           \
                    push((idx) | KEY_EV_PRESS);                                \
                }                                                              \
            }                                                                  \
            else if ((uint8_t)((hold)[idx] + 1) < KEY_HOLD_SCANS)              \
            {                                                                  \
                (hold)[idx]++;                                                 \
            }                                                                  \
            else if ((hold)[idx] < KEY_HOLD_SCANS && !(full))                  \
            {                                                                  \
                (hold)[idx]++;                                                 \
                push((idx) | KEY_EV_HOLD);                                     \
            }                                                                  \
        }                                                                      \
        else if ((h_ & KEY_DEBOUNCE_MASK) == 0)                                \
        {                                                                      \
            if ((h_ & 0x80) && !(full))                                        \
            {                                                                  \
                h_ &= 0x7F;                                                    \
                push((idx) | KEY_EV_RELEASE);                                  \
            }                                                                  \
        }                                                                      \
        (hist)[idx] = h_;                                                      \
    } while (0)

// Function form in keys.c, for the host tests and the emulator (not linked into the firmware).
// keys_scan is one scan tick: bit n of pressed is 1 while key code n is held down.
void    keys_init(void);
void    keys_scan(uint16_t pressed);
uint8_t keys_next_event(void);
void    keys_flush(void);
uint8_t keys_is_down(uint8_t code);

#endif
