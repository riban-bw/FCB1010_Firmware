// SRAM probe: a stand-alone test image for the FCB1010 that looks for RAM in the 8051's
// external data space (MOVX, 0x0000-0xFFFF), where the board's KM62256 32KB SRAM should
// sit at an address nobody has documented. Built with `make probe`; it links the normal
// hardware layer (src/hw.c) but none of the controller logic.
//
// At power-on, with interrupts off (the scan interrupt drives the latches at 0x1FF8), it
// tests each 256-byte page at two offsets (0x00 and 0x80): a cell is RAM if it reads back
// 0x55 and then 0xAA, each written just before a write of the complement to a neighbouring
// cell (offset + 0x40), so a floating data bus, which tends to read back the last value
// driven onto it, does not pass. Below 0x8000 every write reaches a latch chosen by A0-A2
// (IC5, a 74HC139, per the schematic): the offsets used (0x00, 0x40, 0x80, 0xC0 and 0x01)
// select only the display commons (0) and segments (1), never the key / relay column (5)
// or the latch resets (6, 7), and the scan interrupt redraws the display afterwards. Then
// every RAM page gets its page number in its first byte (and the complement in the
// second), highest page first, and is read back: a page that reads another page's number
// is a mirror of that one (an address line the chip select does not decode).
//
// Results, as 16-bit values, one at a time on the display:
//   value 0             number of distinct RAM pages (0x0080 = 32KB)
//   values 1, 2, 3      first run of RAM pages: start address, end address, and the
//                       address it mirrors (its own start if it is not a mirror)
//   values 4, 5, 6 ...  the next run, and so on (up to MAX_RUNS runs)
// Each value shows as its number (decimal, no dots), then the high byte with a trailing
// dot ("HH."), then the low byte with a leading dot (".LL"), then a blank, repeating.
// The footswitch LED of the value is lit (value 0 = switch 1 ... value 9 = switch 10;
// SWITCH 1 is lit as well for values 10-19, SWITCH 2 for 20-29).
// Choosing a value: move expression pedal A (heel = value 0, toe = the last), tap UP / DOWN
// for the next / previous, or press footswitch n for value n - 1.
//
// MIDI OUT gets the whole page map as one SysEx message at power-on and again whenever a
// key is held for the long-press time: F0 7D 52 41 4D ('RAM'), then for each page 0x00 to
// 0xFF three bytes: kind (0 no RAM, 1 RAM, 2 mirror), the page it mirrors (or its own
// number) high nibble, low nibble; then the distinct page count high nibble, low nibble;
// then F7. 776 bytes, about a quarter of a second.

#ifndef PROBE_HOST
#include <8051.h>
#include "platform.h"
#include "config.h"
#include "hal.h"
#include "hw.h"
#endif

#define MAX_RUNS   8
#define LATCH_PAGE 0x1F

// One bit per page: 1 if RAM. 32 bytes, plenty here (the probe has no controller state).
static IDATA uint8_t ram_map[32];
// Mirror base page of each run, start and end page.
static IDATA uint8_t run_start[MAX_RUNS];
static IDATA uint8_t run_end[MAX_RUNS];
static IDATA uint8_t run_base[MAX_RUNS];
static IDATA uint8_t run_count;
static IDATA uint8_t distinct_lo;
static IDATA uint8_t distinct_hi;

// External data reads and writes. test/test_probe.c replaces them with a model of the bus.
#ifndef XMEM_RD
#define XMEM_RD(a)     (*(volatile __xdata uint8_t *)(a))
#define XMEM_WR(a, v)  (*(volatile __xdata uint8_t *)(a) = (v))
#endif

#define MAP_GET(p)  ((ram_map[(p) >> 3] >> ((p) & 7)) & 1)
#define MAP_SET(p)  (ram_map[(p) >> 3] |= (uint8_t)(1 << ((p) & 7)))

// 1 if the cell at a reads back what is written there, with a different value written
// to the cell at b in between. Both cells are put back as they were.
static uint8_t cell_holds(uint16_t a, uint16_t b, uint8_t v)
{
    uint8_t keep_a = XMEM_RD(a);
    uint8_t keep_b = XMEM_RD(b);
    uint8_t diff;

    XMEM_WR(a, v);
    XMEM_WR(b, (uint8_t)(v ^ 0xFF));
    diff = (uint8_t)(XMEM_RD(a) ^ v);
    XMEM_WR(a, keep_a);
    XMEM_WR(b, keep_b);

    return (uint8_t)!diff;
}

static uint8_t cell_is_ram(uint16_t a)
{
    uint16_t b = a ^ 0x40;

    return (uint8_t)(cell_holds(a, b, 0x55) && cell_holds(a, b, 0xAA) &&
                     cell_holds(a, b, (uint8_t)(a >> 8) ^ 0x3C));
}

// The page a RAM page mirrors: the number found in its first byte after tagging, if the
// second byte confirms it; otherwise itself.
static uint8_t page_base(uint8_t p)
{
    uint8_t t = XMEM_RD((uint16_t)p << 8);
    uint8_t check = XMEM_RD(((uint16_t)p << 8) | 1);

    check ^= t;

    if (check == 0xFF && MAP_GET(t))
    {
        return t;
    }

    return p;
}

static void probe(void)
{
    uint8_t p;
    uint8_t base;
    uint16_t distinct = 0;

    EA = 0;

    p = 0;
    do
    {
        if (cell_is_ram((uint16_t)p << 8) && cell_is_ram(((uint16_t)p << 8) | 0x80))
        {
            MAP_SET(p);
        }
    } while (++p != 0);

    // Tag every RAM page, highest first, so a mirror ends up holding the lower page's
    // number (the lower one is reported as the original). Page 0x1F's first two bytes
    // are offsets 0 and 1, the display latches in the low half (see above).
    p = 0;
    do
    {
        p--;

        if (MAP_GET(p))
        {
            XMEM_WR((uint16_t)p << 8, p);
            XMEM_WR(((uint16_t)p << 8) | 1, (uint8_t)~p);
        }
    } while (p != 0);

    // Runs: consecutive RAM pages whose base pages are consecutive too.
    run_count = 0;
    p = 0;
    do
    {
        if (MAP_GET(p))
        {
            base = page_base(p);

            if (base == p)
            {
                distinct++;
            }

            if (run_count && run_end[run_count - 1] == (uint8_t)(p - 1) &&
                (uint8_t)(run_base[run_count - 1] + (uint8_t)(p - run_start[run_count - 1])) == base)
            {
                run_end[run_count - 1] = p;
            }
            else if (run_count < MAX_RUNS)
            {
                run_start[run_count] = p;
                run_end[run_count]   = p;
                run_base[run_count]  = base;
                run_count++;
            }
        }
    } while (++p != 0);

    distinct_lo = (uint8_t)distinct;
    distinct_hi = (uint8_t)(distinct >> 8);

    EA = 1;
}

static void midi_out(uint8_t b)
{
    while (hal_midi_space() == 0)
    {
    }

    hal_midi_tx(b);
}

static void send_map(void)
{
    uint8_t p = 0;
    uint8_t base;

    midi_out(0xF0);
    midi_out(0x7D);
    midi_out('R');
    midi_out('A');
    midi_out('M');

    do
    {
        base = MAP_GET(p) ? page_base(p) : p;
        midi_out(MAP_GET(p) ? ((base == p) ? 1 : 2) : 0);
        midi_out((uint8_t)(base >> 4));
        midi_out((uint8_t)(base & 0x0F));
    } while (++p != 0);

    midi_out((uint8_t)((distinct_hi << 4) | (distinct_lo >> 4)));
    midi_out((uint8_t)(distinct_lo & 0x0F));
    midi_out(0xF7);
}

static uint8_t value_count(void)
{
    return (uint8_t)(1 + 3 * run_count);
}

// Value n as high byte (returned) and low byte (in value_lo).
static IDATA uint8_t value_lo;

static uint8_t value_get(uint8_t n)
{
    uint8_t r;

    if (n == 0)
    {
        value_lo = distinct_lo;
        return distinct_hi;
    }

    n--;
    r = (uint8_t)(n / 3);

    switch (n % 3)
    {
    case 0:
        value_lo = 0x00;
        return run_start[r];
    case 1:
        value_lo = 0xFF;
        return run_end[r];
    default:
        value_lo = 0x00;
        return run_base[r];
    }
}

// Display steps of 100ms: number 6, high byte 10, low byte 10, blank 4.
#define STEP_TICKS   (100 / TICK_MS)
#define SHOW_NUMBER  0
#define SHOW_HIGH    6
#define SHOW_LOW     16
#define SHOW_BLANK   26
#define SHOW_CYCLE   30

static IDATA uint8_t shown;
static IDATA uint8_t step;
static IDATA uint8_t step_ticks;

static void show_leds(void)
{
    uint8_t i;

    for (i = 0; i < 10; i++)
    {
        hal_stomp_led(i, (uint8_t)((shown % 10) == i));
    }

    hal_aux_led(HAL_LED_SWITCH_1, (uint8_t)(shown >= 10 && shown < 20));
    hal_aux_led(HAL_LED_SWITCH_2, (uint8_t)(shown >= 20));
}

static void choose(uint8_t n)
{
    if (n >= value_count())
    {
        n = (uint8_t)(value_count() - 1);
    }

    shown = n;
    step = 0;
    step_ticks = 0;
    show_leds();
}

static void show_step(void)
{
    uint8_t hi;

    if (step == SHOW_NUMBER)
    {
        hal_display_dots(0);
        hal_display(shown);
    }
    else if (step == SHOW_HIGH)
    {
        hi = value_get(shown);
        hal_display_hex(hi);
        hal_display_dots(1);
    }
    else if (step == SHOW_LOW)
    {
        value_get(shown);
        hal_display_hex(value_lo);
        hal_display_dots(4);
    }
    else if (step == SHOW_BLANK)
    {
        hal_display_dots(0);
        hal_display_blank(7);
    }
}

// Pedal A selects a value once it has moved this far from the reading last taken, so ADC
// noise near a boundary does not flicker between two values.
#define PEDAL_STEP 8

#ifndef PROBE_HOST
void main(void)
{
    uint8_t ev;
    uint8_t i;
    uint8_t pedal_ref = 0;
    uint8_t pedal_live = 0;

    hw_init();

    // Settle the key debouncer and the pedal averager, as the main firmware does.
    for (i = 0; i < STARTUP_TICKS; i++)
    {
        while (!hw_tick_elapsed())
        {
        }

        hw_poll_pedals();
    }

    hw_flush_events();
    probe();
    send_map();
    choose(0);
    show_step();

    for (;;)
    {
        if (hw_tick_elapsed())
        {
            if (hw_poll_pedals())
            {
                // The first reading is only a reference: the pedal takes over once moved.
                if (!pedal_live)
                {
                    pedal_ref = hw_pedal[0];
                    pedal_live = 1;
                }
                else if ((hw_pedal[0] > pedal_ref ? (uint8_t)(hw_pedal[0] - pedal_ref)
                                                  : (uint8_t)(pedal_ref - hw_pedal[0])) >= PEDAL_STEP)
                {
                    pedal_ref = hw_pedal[0];
                    i = (uint8_t)(((uint16_t)hw_pedal[0] * value_count()) >> 8);

                    if (i != shown)
                    {
                        choose(i);
                        show_step();
                    }
                }
            }

            if (++step_ticks >= STEP_TICKS)
            {
                step_ticks = 0;

                if (++step >= SHOW_CYCLE)
                {
                    step = 0;
                }

                show_step();
            }
        }

        ev = hw_next_event();

        if (ev == KEY_EV_NONE)
        {
            continue;
        }

        if (KEY_EV_TYPE(ev) == KEY_EV_HOLD)
        {
            send_map();
        }
        else if (KEY_EV_TYPE(ev) == KEY_EV_PRESS)
        {
            i = KEY_EV_CODE(ev);

            if (i == 11)
            {
                choose((uint8_t)(shown + 1));
            }
            else if (i == 5)
            {
                choose(shown ? (uint8_t)(shown - 1) : 0);
            }
            else
            {
                // Codes 0-4 are footswitches 1-5, 6-10 footswitches 6-10.
                choose((uint8_t)(i < 5 ? i : i - 1));
            }

            show_step();
        }
    }
}
#endif
