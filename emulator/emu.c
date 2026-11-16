// Browser emulator hardware layer, compiled to WebAssembly together with the firmware's own
// src/ctl.c (controller behaviour) and src/keys.c (debounce, hold and key events). This file
// plays the part of hw.c and main.c: the HAL services go to JavaScript imports, and emu_tick
// runs one 2ms scan tick the way the timer interrupt and main loop do on the pedal.
// Build with: make emulator

#include "platform.h"
#include "config.h"
#include "hal.h"
#include "ctl.h"
#include "version.h"
#include "keys.h"

#define IMPORT(name) __attribute__((import_module("env"), import_name(name)))
#define EXPORT(name) __attribute__((export_name(name)))

// ---------------------------------------------------------------
// JavaScript side (the panel, the MIDI monitor and browser storage)
// ---------------------------------------------------------------

IMPORT("js_midi_out")   void    js_midi_out(uint8_t byte);
IMPORT("js_stomp_led")  void    js_stomp_led(uint8_t index, uint8_t on);
IMPORT("js_status_led") void    js_status_led(uint8_t on);
IMPORT("js_aux_led")    void    js_aux_led(uint8_t which, uint8_t on);
IMPORT("js_display")    void    js_display(uint8_t value);
IMPORT("js_display_text") void js_display_text(uint8_t text);
IMPORT("js_display_blank") void js_display_blank(uint8_t mask);
IMPORT("js_display_hex")  void js_display_hex(uint8_t value);
IMPORT("js_display_dots") void js_display_dots(uint8_t mask);
IMPORT("js_lamp_test")    void js_lamp_test(uint8_t how);
// Record 0 = settings, 1 = calibration, 2 / 3 = relay triggers; four bytes each. Load returns 1 if one was stored.
IMPORT("js_store_load") uint8_t js_store_load(unsigned record, uint8_t *data);
// Relay outputs, bit 0 = SWITCH 1. Records 2 and 3 are the SWITCH 1 / 2 triggers.
IMPORT("js_relays") void js_relays(uint8_t mask);
IMPORT("js_store_save") uint8_t js_store_save(unsigned record, const uint8_t *data);

// clang may emit calls to these for array loops, and there is no C library here.
void *memset(void *d, int c, unsigned long n)
{
    uint8_t *p = (uint8_t *)d;

    while (n--)
    {
        *p++ = (uint8_t)c;
    }

    return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
    uint8_t *p = (uint8_t *)d;
    const uint8_t *q = (const uint8_t *)s;

    while (n--)
    {
        *p++ = *q++;
    }

    return d;
}

// ---------------------------------------------------------------
// MIDI: The same queue sizes as hw.c, drained at the 31250 baud byte rate
// ---------------------------------------------------------------

#define TX_MASK      (MIDI_TX_QUEUE - 1)
#define RX_MASK      (MIDI_RX_QUEUE - 1)
// One byte is 10 bits at 31250 baud.
#define TX_BYTE_US   320
#define TICK_US      (TICK_MS * 1000)

static uint8_t  txbuf[MIDI_TX_QUEUE];
static uint8_t  tx_head;
static uint8_t  tx_tail;
static uint16_t tx_credit_us;

static uint8_t  rxbuf[MIDI_RX_QUEUE];
static uint8_t  rx_head;
static uint8_t  rx_tail;

void hal_midi_tx(uint8_t byte)
{
    if (((tx_head + 1) & TX_MASK) != tx_tail)
    {
        txbuf[tx_head] = byte;
        tx_head = (uint8_t)((tx_head + 1) & TX_MASK);
    }
}

uint8_t hal_midi_space(void)
{
    return (uint8_t)((MIDI_TX_QUEUE - 1) - ((tx_head - tx_tail) & TX_MASK));
}

uint8_t hal_midi_rx(uint8_t *byte)
{
    if (rx_tail == rx_head)
    {
        return 0;
    }

    *byte = rxbuf[rx_tail];
    rx_tail = (uint8_t)((rx_tail + 1) & RX_MASK);

    return 1;
}

// Send as many queued bytes as the UART would have sent in one tick.
static void tx_drain(void)
{
    if (tx_tail == tx_head)
    {
        // An idle UART starts the next byte at once, but saves up no more than that.
        tx_credit_us = TX_BYTE_US;
        return;
    }

    tx_credit_us = (uint16_t)(tx_credit_us + TICK_US);

    while (tx_tail != tx_head && tx_credit_us >= TX_BYTE_US)
    {
        js_midi_out(txbuf[tx_tail]);
        tx_tail = (uint8_t)((tx_tail + 1) & TX_MASK);
        tx_credit_us = (uint16_t)(tx_credit_us - TX_BYTE_US);
    }
}

// ---------------------------------------------------------------
// Panel and settings EEPROM
// ---------------------------------------------------------------

void hal_stomp_led(uint8_t index, uint8_t on) { js_stomp_led(index, on); }
void hal_status_led(uint8_t on)               { js_status_led(on); }
void hal_aux_led(uint8_t which, uint8_t on)   { js_aux_led(which, on); }
void hal_display(uint8_t value)               { js_display(value); }
void hal_display_text(uint8_t text)           { js_display_text(text); }
void hal_display_blank(uint8_t mask)          { js_display_blank(mask); }
void hal_display_hex(uint8_t value)           { js_display_hex(value); }
void hal_display_dots(uint8_t mask)           { js_display_dots(mask); }
void hal_lamp_test(uint8_t how)               { js_lamp_test(how); }

void    hal_relays(uint8_t mask)          { js_relays(mask); }

// Settings records go to the page's store by HAL_REC_* number (the page names them); a
// footswitch record of bank b (hal_bank) above the first is number + 16 * b.
uint8_t hal_rec[4];
uint8_t hal_bank;
static unsigned rec_id(uint8_t rec) { return (rec < HAL_REC_SW1) ? rec + 16u * hal_bank : rec; }
uint8_t hal_rec_load(uint8_t rec) { return rec < HAL_RECORDS && hal_bank < BANK_COUNT && js_store_load(rec_id(rec), hal_rec); }
uint8_t hal_rec_save(uint8_t rec) { return rec < HAL_RECORDS && hal_bank < BANK_COUNT && js_store_save(rec_id(rec), hal_rec); }

// ---------------------------------------------------------------
// Pedals: Averaged like hw_poll_pedals (four readings each, alternating, every 8 ticks)
// ---------------------------------------------------------------

static uint8_t  pedal_raw[2];
static uint8_t  pedal_phase;
static uint16_t pedal_sum[2];

static uint8_t poll_pedals(uint8_t *a, uint8_t *b)
{
    uint8_t ch = (uint8_t)(pedal_phase & 1);

    pedal_sum[ch] = (uint16_t)(pedal_sum[ch] + pedal_raw[ch]);
    pedal_phase++;

    if (pedal_phase < 8)
    {
        return 0;
    }

    pedal_phase = 0;
    *a = (uint8_t)(pedal_sum[0] >> 2);
    *b = (uint8_t)(pedal_sum[1] >> 2);
    pedal_sum[0] = 0;
    pedal_sum[1] = 0;

    return 1;
}

// ---------------------------------------------------------------
// Main loop, one tick at a time
// ---------------------------------------------------------------

static uint16_t keys_pressed;
static uint8_t  startup_left;

// What the main loop does between ticks: Received MIDI, then any key events.
static void main_pass(void)
{
    uint8_t rx;
    uint8_t e;

    while (hal_midi_rx(&rx))
    {
        ctl_midi_in(rx);
    }

    while ((e = keys_next_event()) != KEY_EV_NONE)
    {
        switch (KEY_EV_TYPE(e))
        {
        case KEY_EV_PRESS:
            ctl_key_press(KEY_EV_CODE(e));
            break;

        case KEY_EV_HOLD:
            ctl_key_hold(KEY_EV_CODE(e));
            break;

        case KEY_EV_RELEASE:
            ctl_key_release(KEY_EV_CODE(e));
            break;

        default:
            break;
        }
    }
}

// Power on: hw_init then ctl_init, then STARTUP_TICKS of settling before the main loop.
EXPORT("emu_power_on")
void emu_power_on(void)
{
    tx_head = 0;
    tx_tail = 0;
    tx_credit_us = TX_BYTE_US;
    rx_head = 0;
    rx_tail = 0;
    pedal_phase = 0;
    pedal_sum[0] = 0;
    pedal_sum[1] = 0;
    startup_left = STARTUP_TICKS;

    keys_init();
    ctl_init();
}

// One 2ms tick: The scan interrupt (keys), the UART, then the main loop's work.
EXPORT("emu_tick")
void emu_tick(void)
{
    uint8_t a;
    uint8_t b;

    keys_scan(keys_pressed);
    tx_drain();

    if (startup_left)
    {
        // Start-up: Pedals are averaged but nothing else runs, and key events are discarded.
        if (poll_pedals(&a, &b))
        {
            ctl_pedal_sample(0, a);
            ctl_pedal_sample(1, b);
        }

        startup_left--;

        if (startup_left == 0)
        {
            keys_flush();
        }

        return;
    }

    ctl_tick();

    if (poll_pedals(&a, &b))
    {
        ctl_pedal_sample(0, a);
        ctl_pedal_sample(1, b);
    }

    main_pass();
}

// Bit n is 1 while key code n is held down (0..4 switches 1-5, 5 DOWN, 6..10 switches 6-10, 11 UP).
EXPORT("emu_set_keys")
void emu_set_keys(uint16_t pressed)
{
    keys_pressed = pressed;
}

EXPORT("emu_set_pedal")
void emu_set_pedal(uint8_t channel, uint8_t raw)
{
    pedal_raw[channel & 1] = raw;
}

// A byte arriving at MIDI IN; dropped if the receive queue is full, as in hw.c.
EXPORT("emu_midi_in")
void emu_midi_in(uint8_t byte)
{
    if (((rx_head + 1) & RX_MASK) != rx_tail)
    {
        rxbuf[rx_head] = byte;
        rx_head = (uint8_t)((rx_head + 1) & RX_MASK);
    }
}

EXPORT("emu_inspect")
uint8_t emu_inspect(uint8_t what)
{
    return ctl_inspect(what);
}

EXPORT("emu_scale")
uint8_t emu_scale(uint8_t raw, uint8_t heel, uint8_t toe)
{
    return ctl_scale(raw, heel, toe);
}

// Firmware constants the page shows: 0 tick ms, 1..10 stomp CCs, 11..12 pedal CCs,
// 13 config hold ms, 14 momentary hold ms, 15 repeat delay ms, 16 repeat rate Hz.
static const uint8_t stomp_cc[STOMP_COUNT] = { STOMP_CC_LIST };
static const uint8_t pedal_cc[2]           = { PEDAL_CC_LIST };

EXPORT("emu_const")
uint16_t emu_const(uint8_t what)
{
    if (what >= 1 && what <= STOMP_COUNT)
    {
        return stomp_cc[what - 1];
    }

    switch (what)
    {
    case 0:  return TICK_MS;
    case 11: return pedal_cc[0];
    case 12: return pedal_cc[1];
    case 13: return CONFIG_HOLD_MS;
    case 14: return MOMENTARY_HOLD_MS;
    case 15: return PROGRAM_REPEAT_DELAY_MS;
    case 16: return PROGRAM_REPEAT_HZ;
    case 18: return fw_version[0];
    case 19: return fw_version[1];
    case 20: return fw_version[2];
    default: return 0;
    }
}
