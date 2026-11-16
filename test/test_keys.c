// Host-side tests for the key-event detection shared by the firmware and emulator
// (src/keys.h macros, via src/keys.c). Build and run with: make test

#include <stdio.h>

// The queue tests fill the queue with the twelve keys, so they use the old 8-entry queue
// (the firmware's is larger now).
#define KEY_EVENT_QUEUE 8
#include "../src/platform.h"
#include "../src/config.h"
#include "../src/keys.h"

#include "../src/keys.c"

static int checks;
static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond))                                                         \
        {                                                                    \
            failures++;                                                      \
            printf("  FAIL line %d: %s\n", __LINE__, #cond);                 \
        }                                                                    \
    } while (0)

#define KEY(c)  ((uint16_t)(1u << (c)))

// The UP key code, as in ctl.h.
#define UP_CODE 11

// Run ticks with the given keys held until an event appears; returns the tick count
// (1 = the first tick) and stores the event, or returns -1 if none within max ticks.
static int ticks_to_event(uint16_t pressed, int max, uint8_t *event)
{
    int t;

    for (t = 1; t <= max; t++)
    {
        keys_scan(pressed);
        *event = keys_next_event();

        if (*event != KEY_EV_NONE)
        {
            return t;
        }
    }

    return -1;
}

static void run(uint16_t pressed, int n)
{
    while (n-- > 0)
    {
        keys_scan(pressed);
    }
}

static void test_press_hold_release(void)
{
    uint8_t e = 0;
    int t;

    keys_init();

    // Key 0 is read on ticks 1, 7, 13...; three pressed readings in a row make a press.
    t = ticks_to_event(KEY(0), 100, &e);
    CHECK(t == 1 + (DEBOUNCE_SAMPLES - 1) * KEY_SCAN_SLOTS);
    CHECK(e == (KEY_EV_PRESS | 0));
    CHECK(keys_is_down(0));
    CHECK(!keys_is_down(6));

    // HOLD after KEY_HOLD_SCANS more readings, about MOMENTARY_HOLD_MS.
    t = ticks_to_event(KEY(0), 2000, &e);
    CHECK(t == KEY_HOLD_SCANS * KEY_SCAN_SLOTS);
    CHECK(e == (KEY_EV_HOLD | 0));
    CHECK(t * TICK_MS <= MOMENTARY_HOLD_MS && t * TICK_MS > MOMENTARY_HOLD_MS - KEY_SCAN_PERIOD_MS);

    // Only one HOLD however long it stays down.
    CHECK(ticks_to_event(KEY(0), 3000, &e) == -1);

    // Release after three released readings.
    t = ticks_to_event(0, 100, &e);
    CHECK(t >= (DEBOUNCE_SAMPLES - 1) * KEY_SCAN_SLOTS + 1 && t <= DEBOUNCE_SAMPLES * KEY_SCAN_SLOTS);
    CHECK(e == (KEY_EV_RELEASE | 0));
    CHECK(!keys_is_down(0));
    CHECK(ticks_to_event(0, 200, &e) == -1);
}

static void test_short_press_has_no_hold(void)
{
    uint8_t e = 0;

    keys_init();
    CHECK(ticks_to_event(KEY(UP_CODE), 100, &e) > 0);
    CHECK(e == (KEY_EV_PRESS | UP_CODE));
    run(KEY(UP_CODE), (KEY_HOLD_SCANS - 2) * KEY_SCAN_SLOTS);
    CHECK(keys_next_event() == KEY_EV_NONE);
    CHECK(ticks_to_event(0, 100, &e) > 0);
    CHECK(e == (KEY_EV_RELEASE | UP_CODE));
    CHECK(ticks_to_event(0, 3000, &e) == -1);
}

static void test_bounce_is_filtered(void)
{
    uint8_t e = 0;
    int i;

    keys_init();

    // A contact that reads pressed on alternate scans never makes a press.
    for (i = 0; i < 60; i++)
    {
        run((i & 1) ? KEY(3) : 0, KEY_SCAN_SLOTS);
    }

    CHECK(keys_next_event() == KEY_EV_NONE);
    CHECK(!keys_is_down(3));

    // Once pressed, a single released reading does not release it.
    CHECK(ticks_to_event(KEY(3), 100, &e) > 0);
    CHECK(e == (KEY_EV_PRESS | 3));
    run(0, KEY_SCAN_SLOTS);
    run(KEY(3), KEY_SCAN_SLOTS * 3);
    CHECK(keys_next_event() == KEY_EV_NONE);
    CHECK(keys_is_down(3));
}

static void test_both_rows_and_queue(void)
{
    uint8_t e;
    int n = 0;

    keys_init();

    // Keys 0 and 6 share a column and are read on the same tick, lower row first.
    run(KEY(0) | KEY(6), 1 + (DEBOUNCE_SAMPLES - 1) * KEY_SCAN_SLOTS);
    CHECK(keys_next_event() == (KEY_EV_PRESS | 0));
    CHECK(keys_next_event() == (KEY_EV_PRESS | 6));
    CHECK(keys_next_event() == KEY_EV_NONE);

    // All twelve pressed without the main loop reading: the queue takes the first
    // KEY_EVENT_QUEUE - 1 presses. The others are not lost: those keys stay up until their
    // press fits, so once the queue is read their presses come on the next scans, each once.
    keys_init();
    run(0x0FFF, DEBOUNCE_SAMPLES * KEY_SCAN_SLOTS);

    while (keys_next_event() != KEY_EV_NONE)
    {
        n++;
    }

    CHECK(n == KEY_EVENT_QUEUE - 1);
    CHECK(keys_is_down(0) && !keys_is_down(UP_CODE));
    {
        uint16_t seen = 0;
        int dup = 0;
        int i;

        // Again from the start, this time reading the queue only once it is full.
        keys_init();
        run(0x0FFF, DEBOUNCE_SAMPLES * KEY_SCAN_SLOTS);

        for (i = 0; i < 4 * KEY_SCAN_SLOTS; i++)
        {
            while ((e = keys_next_event()) != KEY_EV_NONE)
            {
                if (KEY_EV_TYPE(e) == KEY_EV_PRESS)
                {
                    dup += (seen & KEY(KEY_EV_CODE(e))) ? 1 : 0;
                    seen |= KEY(KEY_EV_CODE(e));
                }
            }

            keys_scan(0x0FFF);
        }

        // Every key pressed exactly once, UP (scanned last) included.
        CHECK(seen == 0x0FFF && dup == 0 && keys_is_down(UP_CODE));
    }

    // A HOLD that finds the queue full waits for room too, and comes once.
    keys_init();
    run(KEY(0), 1 + (DEBOUNCE_SAMPLES - 1) * KEY_SCAN_SLOTS);
    CHECK(keys_next_event() == (KEY_EV_PRESS | 0));
    // Fill the queue with presses of other keys, not read.
    run(KEY(0) | 0x0FFE, DEBOUNCE_SAMPLES * KEY_SCAN_SLOTS);
    run(KEY(0) | 0x0FFE, KEY_HOLD_SCANS * KEY_SCAN_SLOTS);
    n = 0;
    while ((e = keys_next_event()) != KEY_EV_NONE)
    {
        CHECK(e != (KEY_EV_HOLD | 0));
        n++;
    }
    CHECK(n == KEY_EVENT_QUEUE - 1);
    {
        int holds = 0;
        int i;

        for (i = 0; i < 4 * KEY_SCAN_SLOTS; i++)
        {
            keys_scan(KEY(0) | 0x0FFE);

            while ((e = keys_next_event()) != KEY_EV_NONE)
            {
                holds += (e == (KEY_EV_HOLD | 0));
            }
        }

        CHECK(holds == 1);
    }

    // Flush discards anything waiting.
    keys_init();
    run(KEY(2), DEBOUNCE_SAMPLES * KEY_SCAN_SLOTS);
    keys_flush();
    CHECK(keys_next_event() == KEY_EV_NONE);
    CHECK(keys_is_down(2));
    e = 0;
    CHECK(ticks_to_event(0, 100, &e) > 0 && e == (KEY_EV_RELEASE | 2));
}

int main(void)
{
    test_press_hold_release();
    test_short_press_has_no_hold();
    test_bounce_is_filtered();
    test_both_rows_and_queue();

    printf("\nkeys: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
