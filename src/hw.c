#include <8051.h>
#include "platform.h"
#include "config.h"
#include "hal.h"
#include "hw.h"
#include "keys.h"

// Hardware map for the Behringer FCB1010, taken from jspyve's public-domain
// reverse-engineering work (github.com/jspyve/FCB1010-lib) and its datasheets:
//   CPU            W78C032, 12MHz, external program ROM (the chip being replaced)
//   latches        memory-mapped at external data address 0x1FF8..0x1FFF
//   key rows       P1.6 = codes 0..5, P1.5 = codes 6..11
//   pedal ADC      ADC0832 on P1.0 (CS), P1.1 (DO), P1.2 (DI), clock on P3.4
//   settings EEPROM 24C16 on I2C, SCL = P3.4 (shared with ADC clock), SDA = P3.5
//   MIDI           UART, 31250 baud

// ---------------------------------------------------------------
// Memory-mapped peripheral latches
// ---------------------------------------------------------------

#define PERIPHERAL_BASE            0x1FF8
#define P_LATCH_COMMONS            0
#define P_LATCH_SEGMENTS           1
#define P_DUMMY_MUX                2
#define P_LATCH_KEY_COLUMNS        5
#define P_RESET_COMMONS_SEGMENTS   6
#define P_RESET_KEY_COLUMNS        7

volatile __xdata __at (PERIPHERAL_BASE) uint8_t PERIPHERALS[8];

// ---------------------------------------------------------------
// LED and 7-segment display (active low, multiplexed one row per tick)
// ---------------------------------------------------------------

// led_map[0..2] = display digits units, tens, hundreds (bit 0 is the decimal point)
// The hundreds position is not a full digit: it can only show "+" or "-" and "1".
// set_digit(2, 1) assumes its "1" uses the B and C segment bits like the other
// digits; this is unverified on a real unit.
// led_map[3..5] = discrete LEDs. A cleared bit lights the LED.
static volatile uint8_t led_map[6];
// Rows 3 and 4 (the footswitch LEDs): a cleared bit lights the LED dimly. The scan drives
// these with the row's lit LEDs only at the start of its slot, for the time the key scan
// takes (about a twentieth of the slot), then the lit LEDs alone (user's request).
static volatile IDATA uint8_t led_dim[2];

static const CODE uint8_t stomp_led_row[10]  = { 4, 4, 4, 3, 3, 4, 4, 4, 4, 4 };
static const CODE uint8_t stomp_led_mask[10] = { 0x04, 0x02, 0x01, 0x80, 0x40,
                                                 0x80, 0x40, 0x20, 0x10, 0x08 };

#define STATUS_LED_ROW  5
#define STATUS_LED_MASK 0x40

// Lamp test with every LED lit (rows 3-5): the led_map bits that still follow their
// own state. HAL_LAMP_LEDS_ON keeps only CONFIG.
static const CODE uint8_t lamp_keep_leds[3]   = { 0x00, 0x00, STATUS_LED_MASK };

// Segment bits: A=0x80 B=0x40 C=0x20 D=0x10 E=0x08 F=0x04 G=0x02
// 0-9, then the hex letters A b C d E F for hal_display_hex.
static const CODE uint8_t digit_segments[16] = {
    0xFC, 0x60, 0xDA, 0xF2, 0x66, 0xB6, 0xBE, 0xE0, 0xFE, 0xF6,
    0xEE, 0x3E, 0x9C, 0x7A, 0x9E, 0x8E
};

void hal_stomp_led(uint8_t index, uint8_t level)
{
    uint8_t row  = stomp_led_row[index];
    uint8_t mask = stomp_led_mask[index];

    if (level == HAL_STOMP_ON)
    {
        led_map[row] &= (uint8_t)~mask;
    }
    else
    {
        led_map[row] |= mask;
    }

    if (level == HAL_STOMP_DIM)
    {
        led_dim[row - 3] &= (uint8_t)~mask;
    }
    else
    {
        led_dim[row - 3] |= mask;
    }
}

void hal_status_led(uint8_t on)
{
    if (on)
    {
        led_map[STATUS_LED_ROW] &= (uint8_t)~STATUS_LED_MASK;
    }
    else
    {
        led_map[STATUS_LED_ROW] |= STATUS_LED_MASK;
    }
}

// Panel LEDs MIDI CHAN, MIDI FUNC, EXP 1 and EXP 2, from the reference LED table:
// all four are on row 3 (masks 0x02, 0x01, 0x08, 0x04).
// DIRECT SELECT is the reference table's LED_DIRECT_BANK_SEL (row 5, mask 0x01).
// SWITCH 1 and SWITCH 2 are the reference table's LED_SWITCH_1 / LED_SWITCH_2 (row 3,
// masks 0x10 and 0x20), not its status LED_SWITCH.
// SELECT, NUMBER, VALUE 1 and VALUE 2 are the reference table's LED_SELECT, LED_NUMBER,
// LED_VALUE_1 and LED_VALUE_2 (row 5, masks 0x10, 0x08, 0x04, 0x02), and SWITCH its
// status LED_SWITCH (row 5, mask 0x20).
static const CODE uint8_t aux_led_row[HAL_LED_COUNT]  = { 3, 3, 3, 3, 5, 3, 3, 5, 5, 5, 5, 5 };
static const CODE uint8_t aux_led_mask[HAL_LED_COUNT] = { 0x02, 0x01, 0x08, 0x04, 0x01, 0x10, 0x20,
                                                          0x10, 0x08, 0x04, 0x02, 0x20 };

void hal_aux_led(uint8_t which, uint8_t on)
{
    uint8_t row;

    if (which >= HAL_LED_COUNT)
    {
        return;
    }

    row = aux_led_row[which];

    if (on == 1)
    {
        led_map[row] &= (uint8_t)~aux_led_mask[which];
    }
    else
    {
        led_map[row] |= aux_led_mask[which];
    }

    // Dim: lit for the key scan's part of the slot only, as a dim footswitch LED.
    if (row == 3)
    {
        if (on == HAL_LED_DIM)
        {
            led_dim[0] &= (uint8_t)~aux_led_mask[which];
        }
        else
        {
            led_dim[0] |= aux_led_mask[which];
        }
    }
}

static void set_digit(uint8_t digit, uint8_t value)
{
    led_map[digit] = (uint8_t)((led_map[digit] & 0x01) | ((uint8_t)~digit_segments[value] & 0xFE));
}

static void clear_digit(uint8_t digit)
{
    led_map[digit] = 0xFF;
}

// Digits by subtraction: an 8-bit division would pull in SDCC's 16-bit library routines.
void hal_display(uint8_t value)
{
    uint8_t d = 0;

    while (value >= 100)
    {
        value = (uint8_t)(value - 100);
        d++;
    }

    if (d)
    {
        set_digit(2, d);
        d = 0;
    }
    else
    {
        clear_digit(2);
    }

    while (value >= 10)
    {
        value = (uint8_t)(value - 10);
        d++;
    }

    if (d || led_map[2] != 0xFF)
    {
        set_digit(1, d);
    }
    else
    {
        clear_digit(1);
    }

    set_digit(0, value);
}

// Segment patterns for the HAL_TEXT_* words, tens then units, same bit order as
// digit_segments. Every letter is full height
// r = A B E F (SCROLL_R, user's request), i = E F (the left side, so it is not a "1"),
// n = A B C E F (an arch), o = A B C D E F.
// "Er" = E (A D E F G), r; "Lo" = L (D E F), o; "Hi" = H (B C E F G), i.
// Then the "riban" frames: " r", "ri", "ib", "ba", "an", "n " (space = 0x00;
// b = C D E F G). The scroll has its own r, SCROLL_R (user's design), and its a is
// SCROLL_A, lowercase or uppercase.
// Then the footswitch message types: "CC" = C (A D E F) twice; "nt" = n, t (D E F G);
// "Pr" = P (A B E F G), a small r (E G; user's request). Then "--" (G twice), then "Ab"
// (A B C E F G, b). Then the pedal modes: "St" = S (A C D F G), t; "St" again (with
// "+"); "Pu" = P, a small u (C D E); "Pu" again (with "-"). Then "PU" = P, U (B C D E F). Then "S1", "S2" (S, digit), "CL" = C, L; "rS" = the large r, S; "AS" =
// A (A B C E F G), S.
// Bits: A 0x80, B 0x40, C 0x20, D 0x10, E 0x08, F 0x04, G 0x02 (0x01 is the dot).
#define SCROLL_R 0xCC   // A B E F: the r with its top-right corner
#define SCROLL_A 0xFA   // lowercase a, A B C D E G (uppercase A is 0xEE, A B C E F G)
static const CODE uint8_t text_segments[HAL_TEXT_COUNT * 2] = {
    0x9E, SCROLL_R, 0x1C, 0xFC, 0x6E, 0x0C,
    0x00, SCROLL_R, SCROLL_R, 0x0C, 0x0C, 0x3E, 0x3E, SCROLL_A, SCROLL_A, 0xEC, 0xEC, 0x00,
    0x9C, 0x9C, 0xEC, 0x1E, 0xCE, 0x0A, 0x02, 0x02, 0xEE, 0x3E,
    0xB6, 0x1E, 0xB6, 0x1E, 0xCE, 0x38, 0xCE, 0x38, 0xCE, 0x7C,
    0xB6, 0x60, 0xB6, 0xDA, 0x9C, 0x1C, SCROLL_R, 0xB6, 0xEE, 0xB6
};

// The leftmost position's sign, lit with its "1" (B C). G drives the "-" bar (seen on a
// real unit); F does not drive the "+" upright, which is one of A, D or E (not in
// jspyve's library), so "+" lights all of them: on this position only the sign, the "1"
// and the dot have LEDs, so the others light nothing.
#define SIGN_MINUS 0x02
#define SIGN_PLUS  (0x02 | 0x04 | 0x08 | 0x10 | 0x80)

// The metronome's sign: the segments the scan ISR lights on the leftmost position on
// top of led_map (a set bit lights).
static volatile IDATA uint8_t disp_sign;

void hal_display_sign(uint8_t sign)
{
    disp_sign = (sign == HAL_SIGN_PLUS) ? SIGN_PLUS : (sign == HAL_SIGN_MINUS) ? SIGN_MINUS : 0;
}

void hal_display_signed(uint8_t value)
{
    uint8_t sign = SIGN_PLUS;

    if (value & 0x80)
    {
        value = (uint8_t)-value;
        sign  = SIGN_MINUS;
    }

    hal_display(value);

    if (value)
    {
        led_map[2] &= (uint8_t)~sign;
    }
}

// One bit per display position (units first); a set bit turns that position off.
void hal_display_blank(uint8_t mask)
{
    uint8_t i;

    for (i = 0; i < 3; i++)
    {
        if (mask & 1)
        {
            clear_digit(i);
        }

        mask >>= 1;
    }
}

// Two hex digits on tens and units, the leftmost position blank (and its dot off).
void hal_display_hex(uint8_t value)
{
    clear_digit(2);
    set_digit(1, (uint8_t)(value >> 4));
    set_digit(0, (uint8_t)(value & 0x0F));
}

// Bit 0 of each digit is its decimal point (a cleared bit lights it).
void hal_display_dots(uint8_t mask)
{
    uint8_t i;

    for (i = 0; i < 3; i++)
    {
        if (mask & 1)
        {
            led_map[i] &= (uint8_t)~0x01;
        }
        else
        {
            led_map[i] |= 0x01;
        }

        mask >>= 1;
    }
}

// The word on tens and units, the leftmost position blank (but for the signs of two pedal
// modes). Decimal points are kept.
void hal_display_text(uint8_t text)
{
    uint8_t i;

    if (text >= HAL_TEXT_COUNT)
    {
        return;
    }

    i = (uint8_t)(text * 2);
    clear_digit(2);

    if (text == HAL_TEXT_MODE + 1)
    {
        led_map[2] &= (uint8_t)~SIGN_PLUS;
    }
    else if (text == HAL_TEXT_MODE + 3)
    {
        led_map[2] &= (uint8_t)~SIGN_MINUS;
    }

    led_map[1] = (uint8_t)((led_map[1] & 0x01) | ((uint8_t)~text_segments[i] & 0xFE));
    led_map[0] = (uint8_t)((led_map[0] & 0x01) | ((uint8_t)~text_segments[i + 1] & 0xFE));
}

// ---------------------------------------------------------------
// Scan timer: LED multiplexing and key matrix, one row / column per tick
// ---------------------------------------------------------------

// Timer 0 reload for TICK_MS milliseconds: One machine cycle is 1us at 12MHz.
#define T0_RELOAD  (0x10000 - 1000 * TICK_MS)

// Per key: Bits 0..6 hold the most recent samples, bit 7 is the pressed state.
static XDATA uint8_t hist[KEY_COUNT];

static volatile XDATA uint8_t evq[KEY_EVENT_QUEUE];
static volatile uint8_t evq_head;
static volatile uint8_t evq_tail;

static volatile uint8_t ticks;
static uint8_t ticks_seen;

static uint8_t scan_row;
static uint8_t scan_common;
static uint8_t scan_key;
static uint8_t scan_idx;
static uint8_t scan_column;
// Relay outputs: bits 0 and 1 of the key column latch
static volatile uint8_t relay_bits;
static volatile BIT_T lamp_leds_on;
static volatile BIT_T lamp_all_on;
// The segments for this scan slot without the dim LEDs, written at the end of the ISR.
static uint8_t scan_seg;

// Scans each key has been down since its debounced press (stops counting at the threshold).
static XDATA uint8_t hold[KEY_COUNT];

// Debounce, hold and event rules are shared with the host tests and editor (keys.h).
// KEY_SCAN checks the queue has room first.
#define PUSH_EVENT(e)                                                          \
    do {                                                                       \
        evq[evq_head] = (e);                                                   \
        evq_head = (uint8_t)((evq_head + 1) & KEY_EVQ_MASK);                   \
    } while (0)
#define SCAN_KEY(idx, level) \
    KEY_SCAN(hist, hold, idx, ((level) == KEY_ACTIVE_LEVEL ? 1 : 0), PUSH_EVENT, KEY_EVQ_FULL(evq_head, evq_tail))

void timer0_isr(void) __interrupt(1)
{
    // Reload first, so the tick is 2 ms plus only the interrupt latency
    TR0 = 0;
    TF0 = 0;
    TH0 = (uint8_t)(T0_RELOAD >> 8);
    TL0 = (uint8_t)(T0_RELOAD & 0xFF);
    TR0 = 1;

    // LED multiplex: Blank, select the common line, then drive the segments.
    PERIPHERALS[P_DUMMY_MUX]      = 0;
    PERIPHERALS[P_LATCH_SEGMENTS] = 0xFF;
    PERIPHERALS[P_LATCH_COMMONS]  = scan_common;
    // A cleared bit lights, so 0x00 lights the whole row.
    scan_seg = lamp_all_on ? 0x00 :
               (lamp_leds_on && scan_row >= 3) ?
               // Every LED lit except CONFIG (it flashes for unsaved changes).
               (uint8_t)(led_map[scan_row] & lamp_keep_leds[scan_row - 3]) :
               led_map[scan_row];

    // The metronome's sign on the leftmost position.
    if (scan_row == 2)
    {
        scan_seg &= (uint8_t)~disp_sign;
    }

    // The dim footswitch LEDs too, until the end of the ISR.
    PERIPHERALS[P_LATCH_SEGMENTS] = (scan_row == 3 || scan_row == 4) ?
                                    (uint8_t)(scan_seg & led_dim[scan_row - 3]) : scan_seg;
    PERIPHERALS[P_DUMMY_MUX]      = 0;

    scan_row++;
    scan_common <<= 1;

    if (scan_row == 6)
    {
        scan_row = 0;
        scan_common = 1;
    }

    // Key matrix: Select one column (bits 2-7), with the relay outputs in bits 0-1,
    // then read both rows.
    PERIPHERALS[P_DUMMY_MUX]         = 0;
    PERIPHERALS[P_LATCH_KEY_COLUMNS] = (uint8_t)(scan_column | relay_bits);
    PERIPHERALS[P_DUMMY_MUX]         = 0;

    // Both rows through one copy of the scan: key k on P1.6, then key k + 6 on P1.5.
    scan_idx = scan_key;

    do
    {
        SCAN_KEY(scan_idx, (scan_idx < KEY_SCAN_SLOTS) ? P1_6 : P1_5);
        scan_idx = (uint8_t)(scan_idx + KEY_SCAN_SLOTS);
    } while (scan_idx < KEY_COUNT);

    scan_key++;
    scan_column <<= 1;

    if (scan_key > 5)
    {
        scan_key = 0;
        scan_column = 4;
    }

    // The rest of the slot: only the LEDs that are fully lit.
    PERIPHERALS[P_LATCH_SEGMENTS] = scan_seg;
    PERIPHERALS[P_DUMMY_MUX]      = 0;

    ticks++;
}

void hal_lamp_test(uint8_t how)
{
    lamp_leds_on = (how == HAL_LAMP_LEDS_ON);
    lamp_all_on  = (how == HAL_LAMP_ALL_ON);
}

void hal_relays(uint8_t mask)
{
    relay_bits = (uint8_t)(mask & 3);
}

uint8_t hw_tick_elapsed(void)
{
    if (ticks_seen != ticks)
    {
        ticks_seen++;
        return 1;
    }

    return 0;
}

uint8_t hw_next_event(void)
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

void hw_flush_events(void)
{
    evq_tail = evq_head;
}

uint8_t hw_key_is_down(uint8_t code)
{
    return (hist[code] & 0x80) ? 1 : 0;
}

// ---------------------------------------------------------------
// MIDI UART, interrupt driven transmit queue
// ---------------------------------------------------------------

// 31 usable bytes is ten messages, far more than the main loop ever queues,
// and a small buffer leaves more of the 256-byte RAM for the stack.
#define TX_SIZE  MIDI_TX_QUEUE
#define TX_MASK  (TX_SIZE - 1)

static volatile XDATA uint8_t txbuf[TX_SIZE];
static volatile uint8_t tx_head;
static volatile uint8_t tx_tail;
static volatile uint8_t tx_busy;

// Received MIDI bytes wait here until the main loop collects them. 15 usable bytes
// is about 5ms of continuous traffic at 31250 baud, and the main loop empties it every pass.
#define RX_SIZE  MIDI_RX_QUEUE
#define RX_MASK  (RX_SIZE - 1)

static volatile XDATA uint8_t rxbuf[RX_SIZE];
static volatile uint8_t rx_head;
static volatile uint8_t rx_tail;

void serial_isr(void) __interrupt(4)
{
    if (RI)
    {
        RI = 0;

        // Drop the byte if the queue is full rather than overwrite older ones.
        if (((rx_head + 1) & RX_MASK) != rx_tail)
        {
            rxbuf[rx_head] = SBUF;
            rx_head = (uint8_t)((rx_head + 1) & RX_MASK);
        }
    }

    if (TI)
    {
        TI = 0;

        if (tx_tail != tx_head)
        {
            SBUF = txbuf[tx_tail];
            tx_tail = (uint8_t)((tx_tail + 1) & TX_MASK);
        }
        else
        {
            tx_busy = 0;
        }
    }
}

void hal_midi_tx(uint8_t byte)
{
    ES = 0;

    if (((tx_head + 1) & TX_MASK) != tx_tail)
    {
        txbuf[tx_head] = byte;
        tx_head = (uint8_t)((tx_head + 1) & TX_MASK);

        if (!tx_busy)
        {
            // Kick the transmitter: the interrupt loads the first byte.
            tx_busy = 1;
            TI = 1;
        }
    }

    ES = 1;
}

uint8_t hal_midi_rx(IDATA uint8_t *byte)
{
    if (rx_tail == rx_head)
    {
        return 0;
    }

    *byte = rxbuf[rx_tail];
    rx_tail = (uint8_t)((rx_tail + 1) & RX_MASK);

    return 1;
}

uint8_t hal_midi_space(void)
{
    uint8_t used;

    ES = 0;
    used = (uint8_t)((tx_head - tx_tail) & TX_MASK);
    ES = 1;

    return (uint8_t)((TX_SIZE - 1) - used);
}

// ---------------------------------------------------------------
// Pedal ADC (ADC0832, bit-banged)
// ---------------------------------------------------------------

#define ADC_CS    P1_0
#define ADC_DO    P1_1
#define ADC_DI    P1_2
#define ADC_CLK   P3_4

#define NOP()  __asm__("nop")

static uint8_t adc_convert(uint8_t channel)
{
    uint8_t result = 0;
    uint8_t i;

    ADC_CS  = 1;
    ADC_CLK = 0;
    ADC_DI  = 1;
    NOP(); NOP(); NOP(); NOP();

    ADC_CS = 0;
    NOP(); NOP(); NOP(); NOP();

    // Start bit
    ADC_DI = 1;
    ADC_CLK = 1; NOP(); NOP();
    ADC_CLK = 0; NOP(); NOP();

    // Single-ended mode
    ADC_DI = 1;
    ADC_CLK = 1; NOP(); NOP();
    ADC_CLK = 0; NOP(); NOP();

    // Channel select
    ADC_DI = (channel == 0) ? 0 : 1;
    ADC_CLK = 1; NOP(); NOP();
    ADC_CLK = 0; NOP(); NOP();

    // Nine clocks: A leading null bit (shifted out of the byte) then 8 data bits
    for (i = 0; i < 9; i++)
    {
        result = (uint8_t)(result << 1);
        ADC_CLK = 1; NOP(); NOP();

        if (ADC_DO)
        {
            result |= 1;
        }

        ADC_CLK = 0; NOP(); NOP();
    }

    NOP(); NOP(); NOP(); NOP();
    ADC_CS = 1;

    return result;
}

// Averages four readings per pedal, alternating channels each tick.
static uint8_t  pedal_phase;
static uint16_t pedal_sum[2];
IDATA uint8_t   hw_pedal[2];

uint8_t hw_poll_pedals(void)
{
    uint8_t ch = (uint8_t)(pedal_phase & 1);

    pedal_sum[ch] += adc_convert(ch);
    pedal_phase++;

    if (pedal_phase < 8)
    {
        return 0;
    }

    pedal_phase = 0;
    hw_pedal[0] = (uint8_t)(pedal_sum[0] >> 2);
    hw_pedal[1] = (uint8_t)(pedal_sum[1] >> 2);
    pedal_sum[0] = 0;
    pedal_sum[1] = 0;

    return 1;
}

// ---------------------------------------------------------------
// I2C (bit-banged) and 24C16 settings EEPROM
// ---------------------------------------------------------------

#define I2C_SCL  P3_4
#define I2C_SDA  P3_5

#define I2C_DELAY()  do { NOP(); NOP(); NOP(); NOP(); NOP(); } while (0)
#define I2C_SCL_SET(v)  (I2C_SCL = (v))
#define I2C_SDA_SET(v)  (I2C_SDA = (v))
#define I2C_SDA_GET()   (I2C_SDA)
#define EEPROM_DESELECT_ADC()  (ADC_CS = 1)

#include "eeprom.inc"

// ---------------------------------------------------------------
// Start-up
// ---------------------------------------------------------------

static void startup_delay(void)
{
    uint16_t n;

    // Roughly 3ms for the supply and latches to settle.
    for (n = 0; n < 1000; n++)
    {
        NOP();
    }
}

void hw_init(void)
{
    uint8_t i;

    startup_delay();

    // Quasi-bidirectional ports: Write 1s so inputs can be read.
    P1 = 0xFF;
    P3 = 0xFF;

    for (i = 0; i < 6; i++)
    {
        led_map[i] = 0xFF;
    }

    led_dim[0] = 0xFF;
    led_dim[1] = 0xFF;
    disp_sign  = 0;

    // The key tables are in the external SRAM, which holds random data at power-on.
    for (i = 0; i < KEY_COUNT; i++)
    {
        hist[i] = 0;
        hold[i] = 0;
    }

    // Clear the display and key-column latches.
    PERIPHERALS[P_DUMMY_MUX]                = 0;
    PERIPHERALS[P_RESET_COMMONS_SEGMENTS]   = 0;
    PERIPHERALS[P_DUMMY_MUX]                = 0;
    PERIPHERALS[P_DUMMY_MUX]                = 0;
    PERIPHERALS[P_RESET_KEY_COLUMNS]        = 0;
    PERIPHERALS[P_DUMMY_MUX]                = 0;

    scan_row    = 0;
    scan_common = 1;
    scan_key    = 0;
    scan_column = 4;

    evq_head = 0;
    evq_tail = 0;
    tx_head  = 0;
    tx_tail  = 0;
    tx_busy  = 0;
    rx_head  = 0;
    rx_tail  = 0;

    // MIDI UART: Mode 1, receiver on, SMOD = 1, timer 1 auto-reload.
    // Baud = 2 * 12MHz / (384 * (256 - 254)) = 31250.
    SCON = 0x50;
    PCON |= 0x80;
    TMOD = 0x21;
    TH1  = 254;
    TL1  = 254;
    TR1  = 1;
    TI   = 0;
    RI   = 0;
    ES   = 1;
    PS   = 1;

    // Scan timer: Timer 0 as a 16-bit timer.
    TH0 = (uint8_t)(T0_RELOAD >> 8);
    TL0 = (uint8_t)(T0_RELOAD & 0xFF);
    TF0 = 0;
    ET0 = 1;
    TR0 = 1;

    EA = 1;
}
