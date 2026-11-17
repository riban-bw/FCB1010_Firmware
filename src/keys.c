#include "platform.h"
#include "config.h"
#include "keys.h"

// Portable key scanner for the host tests and the browser editor. It runs the same
// KEY_SCAN macro as the firmware's scan interrupt (hw.c), in the same
// order: one column per tick, keys k and k + 6 together. Not part of the 8051 build.

static uint8_t hist[KEY_COUNT];
static uint8_t hold[KEY_COUNT];
static uint8_t evq[KEY_EVENT_QUEUE];
static uint8_t evq_head;
static uint8_t evq_tail;
static uint8_t scan_key;

// KEY_SCAN checks the queue has room first.
#define PUSH_EVENT(e)                                                          \
    do {                                                                       \
        evq[evq_head] = (e);                                                   \
        evq_head = (uint8_t)((evq_head + 1) & KEY_EVQ_MASK);                   \
    } while (0)

void keys_init(void)
{
    uint8_t i;

    for (i = 0; i < KEY_COUNT; i++)
    {
        hist[i] = 0;
        hold[i] = 0;
    }

    evq_head = 0;
    evq_tail = 0;
    scan_key = 0;
}

void keys_scan(uint16_t pressed)
{
    KEY_SCAN(hist, hold, scan_key,     (uint8_t)((pressed >> scan_key) & 1), PUSH_EVENT,
             KEY_EVQ_FULL(evq_head, evq_tail));
    KEY_SCAN(hist, hold, scan_key + 6, (uint8_t)((pressed >> (scan_key + 6)) & 1), PUSH_EVENT,
             KEY_EVQ_FULL(evq_head, evq_tail));

    scan_key++;

    if (scan_key >= KEY_SCAN_SLOTS)
    {
        scan_key = 0;
    }
}

uint8_t keys_next_event(void)
{
    uint8_t e;

    if (evq_tail == evq_head)
    {
        return KEY_EV_NONE;
    }

    e = evq[evq_tail];
    evq_tail = (uint8_t)((evq_tail + 1) & KEY_EVQ_MASK);

    return e;
}

void keys_flush(void)
{
    evq_tail = evq_head;
}

uint8_t keys_is_down(uint8_t code)
{
    return (hist[code] & 0x80) ? 1 : 0;
}
