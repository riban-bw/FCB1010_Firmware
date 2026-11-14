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
// led_map[3..5] = discrete LEDs. A cleared bit lights the LED.
static volatile uint8_t led_map[6];

static const CODE uint8_t stomp_led_row[10]  = { 4, 4, 4, 3, 3, 4, 4, 4, 4, 4 };
static const CODE uint8_t stomp_led_mask[10] = { 0x04, 0x02, 0x01, 0x80, 0x40,
                                                 0x80, 0x40, 0x20, 0x10, 0x08 };

#define STATUS_LED_ROW  5
#define STATUS_LED_MASK 0x40

// Segment bits: A=0x80 B=0x40 C=0x20 D=0x10 E=0x08 F=0x04 G=0x02
static const CODE uint8_t digit_segments[10] = {
    0xFC, 0x60, 0xDA, 0xF2, 0x66, 0xB6, 0xBE, 0xE0, 0xFE, 0xF6
};

void hal_stomp_led(uint8_t index, uint8_t on)
{
    uint8_t row  = stomp_led_row[index];
    uint8_t mask = stomp_led_mask[index];

    if (on)
    {
        led_map[row] &= (uint8_t)~mask;
    }
    else
    {
        led_map[row] |= mask;
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
static const CODE uint8_t aux_led_mask[4] = { 0x02, 0x01, 0x08, 0x04 };

void hal_aux_led(uint8_t which, uint8_t on)
{
    if (which > 3)
    {
        return;
    }

    if (on)
    {
        led_map[3] &= (uint8_t)~aux_led_mask[which];
    }
    else
    {
        led_map[3] |= aux_led_mask[which];
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

void hal_display(uint8_t value)
{
    if (value >= 100)
    {
        set_digit(2, (uint8_t)(value / 100));
        value = (uint8_t)(value % 100);
    }
    else
    {
        clear_digit(2);
    }

    if (value >= 10 || led_map[2] != 0xFF)
    {
        set_digit(1, (uint8_t)(value / 10));
        value = (uint8_t)(value % 10);
    }
    else
    {
        clear_digit(1);
    }

    set_digit(0, value);
}

// ---------------------------------------------------------------
// Scan timer: LED multiplexing and key matrix, one row / column per tick
// ---------------------------------------------------------------

// Timer 0 reload for TICK_MS milliseconds: One machine cycle is 1us at 12MHz.
#define T0_RELOAD  (0x10000 - 1000 * TICK_MS)

// Per key: Bits 0..6 hold the most recent samples, bit 7 is the pressed state.
static IDATA uint8_t hist[KEY_COUNT];

static volatile IDATA uint8_t evq[KEY_EVENT_QUEUE];
static volatile uint8_t evq_head;
static volatile uint8_t evq_tail;

static volatile uint8_t ticks;
static uint8_t ticks_seen;

static uint8_t scan_row;
static uint8_t scan_common;
static uint8_t scan_key;
static uint8_t scan_column;

// Scans each key has been down since its debounced press (stops counting at the threshold).
static IDATA uint8_t hold[KEY_COUNT];

// Debounce, hold and event rules are shared with the host tests and emulator (keys.h).
#define PUSH_EVENT(e)  KEY_EVQ_PUSH(evq, evq_head, evq_tail, e)
#define SCAN_KEY(idx, level) \
    KEY_SCAN(hist, hold, idx, ((level) == KEY_ACTIVE_LEVEL ? 1 : 0), PUSH_EVENT)

void timer0_isr(void) __interrupt(1)
{
    TR0 = 0;
    TF0 = 0;

    // LED multiplex: Blank, select the common line, then drive the segments.
    PERIPHERALS[P_DUMMY_MUX]      = 0;
    PERIPHERALS[P_LATCH_SEGMENTS] = 0xFF;
    PERIPHERALS[P_LATCH_COMMONS]  = scan_common;
    PERIPHERALS[P_LATCH_SEGMENTS] = led_map[scan_row];
    PERIPHERALS[P_DUMMY_MUX]      = 0;

    scan_row++;
    scan_common <<= 1;

    if (scan_row == 6)
    {
        scan_row = 0;
        scan_common = 1;
    }

    // Key matrix: Select one column (relay outputs stay off), then read both rows.
    PERIPHERALS[P_DUMMY_MUX]         = 0;
    PERIPHERALS[P_LATCH_KEY_COLUMNS] = scan_column;
    PERIPHERALS[P_DUMMY_MUX]         = 0;

    SCAN_KEY(scan_key,     P1_6);
    SCAN_KEY(scan_key + 6, P1_5);

    scan_key++;
    scan_column <<= 1;

    if (scan_key > 5)
    {
        scan_key = 0;
        scan_column = 4;
    }

    ticks++;

    TH0 = (uint8_t)(T0_RELOAD >> 8);
    TL0 = (uint8_t)(T0_RELOAD & 0xFF);
    TR0 = 1;
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

static volatile IDATA uint8_t txbuf[TX_SIZE];
static volatile uint8_t tx_head;
static volatile uint8_t tx_tail;
static volatile uint8_t tx_busy;

// Received MIDI bytes wait here until the main loop collects them. 15 usable bytes
// is about 5ms of continuous traffic at 31250 baud, and the main loop empties it every pass.
#define RX_SIZE  MIDI_RX_QUEUE
#define RX_MASK  (RX_SIZE - 1)

static volatile IDATA uint8_t rxbuf[RX_SIZE];
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

uint8_t hw_poll_pedals(uint8_t *a, uint8_t *b)
{
    uint8_t ch = (uint8_t)(pedal_phase & 1);

    pedal_sum[ch] += adc_convert(ch);
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
// I2C (bit-banged) and 24C16 settings EEPROM
// ---------------------------------------------------------------

#define I2C_SCL  P3_4
#define I2C_SDA  P3_5

#define I2C_DELAY()  do { NOP(); NOP(); NOP(); NOP(); NOP(); } while (0)

static void i2c_start(void)
{
    I2C_SDA = 1;
    I2C_SCL = 1;
    I2C_DELAY();
    I2C_SDA = 0;
    I2C_DELAY();
    I2C_SCL = 0;
    I2C_DELAY();
}

static void i2c_stop(void)
{
    I2C_SDA = 0;
    I2C_DELAY();
    I2C_SCL = 1;
    I2C_DELAY();
    I2C_SDA = 1;
    I2C_DELAY();
}

// Returns 1 if the device acknowledged.
static uint8_t i2c_write(uint8_t data)
{
    uint8_t i;
    uint8_t ack;

    for (i = 0; i < 8; i++)
    {
        I2C_SDA = (data & 0x80) ? 1 : 0;
        I2C_DELAY();
        I2C_SCL = 1;
        I2C_DELAY();
        I2C_SCL = 0;
        data = (uint8_t)(data << 1);
    }

    I2C_SDA = 1;
    I2C_DELAY();
    I2C_SCL = 1;
    I2C_DELAY();
    ack = I2C_SDA ? 0 : 1;
    I2C_SCL = 0;
    I2C_DELAY();

    return ack;
}

static uint8_t i2c_read(uint8_t send_nak)
{
    uint8_t i;
    uint8_t data = 0;

    I2C_SDA = 1;

    for (i = 0; i < 8; i++)
    {
        I2C_DELAY();
        I2C_SCL = 1;
        I2C_DELAY();
        data = (uint8_t)(data << 1);

        if (I2C_SDA)
        {
            data |= 1;
        }

        I2C_SCL = 0;
    }

    I2C_SDA = send_nak ? 1 : 0;
    I2C_DELAY();
    I2C_SCL = 1;
    I2C_DELAY();
    I2C_SCL = 0;
    I2C_SDA = 1;
    I2C_DELAY();

    return data;
}

#define EEPROM_DEVICE  0xA0

// The 24C16 puts address bits 8..10 into the device address byte.
static uint8_t eeprom_control(uint16_t address)
{
    return (uint8_t)(EEPROM_DEVICE | ((address >> 7) & 0x0E));
}

static uint8_t eeprom_read_byte(uint16_t address, uint8_t *out)
{
    uint8_t ok;

    ADC_CS = 1;

    i2c_start();
    ok = i2c_write(eeprom_control(address));
    ok = (uint8_t)(ok && i2c_write((uint8_t)(address & 0xFF)));

    if (ok)
    {
        i2c_start();
        ok = i2c_write((uint8_t)(eeprom_control(address) | 1));

        if (ok)
        {
            *out = i2c_read(1);
        }
    }

    i2c_stop();

    return ok;
}

static uint8_t eeprom_write_byte(uint16_t address, uint8_t value)
{
    uint8_t ok;
    uint16_t tries;

    ADC_CS = 1;

    i2c_start();
    ok = i2c_write(eeprom_control(address));
    ok = (uint8_t)(ok && i2c_write((uint8_t)(address & 0xFF)));
    ok = (uint8_t)(ok && i2c_write(value));
    i2c_stop();

    if (!ok)
    {
        return 0;
    }

    // Wait for the internal write cycle (about 5ms) by polling for an ACK.
    for (tries = 0; tries < 5000; tries++)
    {
        i2c_start();
        ok = i2c_write(eeprom_control(address));
        i2c_stop();

        if (ok)
        {
            return 1;
        }
    }

    return 0;
}

// Calibration record near the top of the 2KB 24C16, away from the low
// addresses (from 0x000) where Behringer's SysEx documentation places the stock
// preset data, so putting the original chip back is unlikely to be affected.
// Record layout: Magic, heelA, toeA, heelB, toeB, checksum
#define CAL_ADDRESS  0x7F0
#define CAL_MAGIC    0xC1
#define CAL_LENGTH   6

static uint8_t cal_checksum(const uint8_t *r)
{
    return (uint8_t)(r[0] ^ r[1] ^ r[2] ^ r[3] ^ r[4] ^ 0x5A);
}

uint8_t hal_cal_load(uint8_t *heel, uint8_t *toe)
{
    uint8_t rec[CAL_LENGTH];
    uint8_t i;

    for (i = 0; i < CAL_LENGTH; i++)
    {
        if (!eeprom_read_byte((uint16_t)(CAL_ADDRESS + i), &rec[i]))
        {
            return 0;
        }
    }

    if (rec[0] != CAL_MAGIC || rec[5] != cal_checksum(rec))
    {
        return 0;
    }

    heel[0] = rec[1];
    toe[0]  = rec[2];
    heel[1] = rec[3];
    toe[1]  = rec[4];

    return 1;
}

// Settings record next to the calibration record: magic, channel, three bytes of
// packed pedal modes, checksum.
#define CFG_ADDRESS  0x7F8
#define CFG_MAGIC    0xC2
#define CFG_LENGTH   6

static uint8_t cfg_checksum(const uint8_t *r)
{
    return (uint8_t)(r[0] ^ r[1] ^ r[2] ^ r[3] ^ r[4] ^ 0xA5);
}

uint8_t hal_cfg_load(uint8_t *data)
{
    uint8_t rec[CFG_LENGTH];
    uint8_t i;

    for (i = 0; i < CFG_LENGTH; i++)
    {
        if (!eeprom_read_byte((uint16_t)(CFG_ADDRESS + i), &rec[i]))
        {
            return 0;
        }
    }

    if (rec[0] != CFG_MAGIC || rec[5] != cfg_checksum(rec))
    {
        return 0;
    }

    data[0] = rec[1];
    data[1] = rec[2];
    data[2] = rec[3];
    data[3] = rec[4];

    return 1;
}

uint8_t hal_cfg_save(const uint8_t *data)
{
    uint8_t rec[CFG_LENGTH];
    uint8_t check;
    uint8_t i;

    rec[0] = CFG_MAGIC;
    rec[1] = data[0];
    rec[2] = data[1];
    rec[3] = data[2];
    rec[4] = data[3];
    rec[5] = cfg_checksum(rec);

    for (i = 0; i < CFG_LENGTH; i++)
    {
        if (!eeprom_write_byte((uint16_t)(CFG_ADDRESS + i), rec[i]))
        {
            return 0;
        }
    }

    for (i = 0; i < CFG_LENGTH; i++)
    {
        if (!eeprom_read_byte((uint16_t)(CFG_ADDRESS + i), &check) || check != rec[i])
        {
            return 0;
        }
    }

    return 1;
}

uint8_t hal_cal_save(const uint8_t *heel, const uint8_t *toe)
{
    uint8_t rec[CAL_LENGTH];
    uint8_t check;
    uint8_t i;

    rec[0] = CAL_MAGIC;
    rec[1] = heel[0];
    rec[2] = toe[0];
    rec[3] = heel[1];
    rec[4] = toe[1];
    rec[5] = cal_checksum(rec);

    for (i = 0; i < CAL_LENGTH; i++)
    {
        if (!eeprom_write_byte((uint16_t)(CAL_ADDRESS + i), rec[i]))
        {
            return 0;
        }
    }

    for (i = 0; i < CAL_LENGTH; i++)
    {
        if (!eeprom_read_byte((uint16_t)(CAL_ADDRESS + i), &check) || check != rec[i])
        {
            return 0;
        }
    }

    return 1;
}

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
