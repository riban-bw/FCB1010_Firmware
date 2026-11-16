// Host-side tests for the controller logic (src/ctl.c).
// The hardware layer is replaced by recorders so MIDI output, LEDs and the
// display can be checked exactly. Build and run with: make test

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "../src/platform.h"
#include "../src/config.h"
#include "../src/hal.h"
#include "../src/ctl.h"

#include "../src/ctl.c"
// fw_version[] (the firmware links it from its own object).
#include "../src/version.c"

// ---------------------------------------------------------------
// Fake hardware
// ---------------------------------------------------------------

#define LOG_MAX 4096

static uint8_t  midi_log[LOG_MAX];
static int      midi_len;
static int      midi_space = 15;
static uint8_t  leds[10];
static uint8_t  aux[HAL_LED_COUNT];
// Footswitch message records: saved by the firmware, and stored (what a load returns).
static int      msg_saved;
// Footswitch records (type with the pedal mode in bits 2-3, number, on value, off value),
// one per switch and bank: bank b's switch n at b * 10 + n, so 0-9 are bank 1.
#define MSG_RECS (BANK_COUNT * HAL_REC_SW1)
#define MSG_REC(rec) (hal_bank * HAL_REC_SW1 + (rec))
static int      msg_stored_valid[MSG_RECS], msg_was_saved[MSG_RECS];
static uint8_t  stored_msg[MSG_RECS][4], saved_msg[MSG_RECS][4];
// Relay outputs and the saved / stored relay trigger records.
static uint8_t  relays;
static int      sw_saved;
static int      sw_stored_valid[2];
static uint8_t  stored_sw[2][4], saved_sw[2][4];
static uint8_t  status_led;
static int      display;
static uint8_t  blank;
#define SHOWN_ER (-1)
#define SHOWN_LO (-2)
#define SHOWN_HI (-3)
// Any hal_display_text(t), e.g. the "riban" frames HAL_TEXT_SCROLL + n.
#define SHOWN_TEXT(t) (-1 - (int)(t))
// hal_display_hex(v) is recorded as display = SHOWN_HEX(v); dots and the lamp test too.
#define SHOWN_HEX(v) (0x1000 + (v))
#define BOOT_LED (DEFAULT_PEDAL_MODE == 3)
static uint8_t  dots;
static uint8_t  lamp;
static uint8_t  saved_heel[2], saved_toe[2];
static int      cal_saved;
static int      cal_stored_valid;
static uint8_t  stored_heel[2], stored_toe[2];
static int      cfg_saved;
static int      cfg_stored_valid;
static uint8_t  stored_cfg[4], saved_cfg[4];

void hal_midi_tx(uint8_t byte)
{
    if (midi_len < LOG_MAX)
    {
        midi_log[midi_len++] = byte;
    }
}

uint8_t hal_midi_space(void)
{
    return (uint8_t)midi_space;
}

void hal_stomp_led(uint8_t index, uint8_t on)
{
    leds[index] = on;
}

void hal_status_led(uint8_t on)
{
    status_led = on;
}

void hal_aux_led(uint8_t which, uint8_t on)
{
    aux[which] = on;
}

void hal_display(uint8_t value)
{
    display = value;
    blank = 0;
}

void hal_display_text(uint8_t text)
{
    display = -1 - (int)text;
    blank = 0;
}

void hal_display_blank(uint8_t mask)
{
    blank |= mask;
}

void hal_display_hex(uint8_t value)
{
    display = SHOWN_HEX(value);
    blank = 0;
}

void hal_display_dots(uint8_t mask)
{
    dots = mask;
}

void hal_lamp_test(uint8_t how)
{
    lamp = how;
}

void hal_relays(uint8_t mask)
{
    relays = mask;
}

// The settings records through the shared buffer, kept in the per-kind arrays above
// (calibration as heel A, toe A, heel B, toe B, like the EEPROM record). Saves are kept
// apart from what loads return, so every save behaves like a failed one when reloading.
uint8_t hal_rec[4];
uint8_t hal_bank;

uint8_t hal_rec_load(uint8_t rec)
{
    assert(hal_bank < BANK_COUNT);

    if (rec < HAL_REC_SW1)
    {
        if (!msg_stored_valid[MSG_REC(rec)])
        {
            return 0;
        }

        memcpy(hal_rec, stored_msg[MSG_REC(rec)], 4);
        return 1;
    }

    if (rec <= HAL_REC_SW2)
    {
        if (!sw_stored_valid[rec - HAL_REC_SW1])
        {
            return 0;
        }

        memcpy(hal_rec, stored_sw[rec - HAL_REC_SW1], 4);
        return 1;
    }

    if (rec == HAL_REC_CAL)
    {
        if (!cal_stored_valid)
        {
            return 0;
        }

        hal_rec[0] = stored_heel[0];
        hal_rec[1] = stored_toe[0];
        hal_rec[2] = stored_heel[1];
        hal_rec[3] = stored_toe[1];
        return 1;
    }

    if (rec == HAL_REC_CFG && cfg_stored_valid)
    {
        memcpy(hal_rec, stored_cfg, 4);
        return 1;
    }

    return 0;
}

uint8_t hal_rec_save(uint8_t rec)
{
    if (rec < HAL_REC_SW1)
    {
        msg_saved++;
        msg_was_saved[MSG_REC(rec)] = 1;
        memcpy(saved_msg[MSG_REC(rec)], hal_rec, 4);
    }
    else if (rec <= HAL_REC_SW2)
    {
        sw_saved++;
        memcpy(saved_sw[rec - HAL_REC_SW1], hal_rec, 4);
    }
    else if (rec == HAL_REC_CAL)
    {
        cal_saved++;
        saved_heel[0] = hal_rec[0];
        saved_toe[0]  = hal_rec[1];
        saved_heel[1] = hal_rec[2];
        saved_toe[1]  = hal_rec[3];
    }
    else if (rec == HAL_REC_CFG)
    {
        cfg_saved++;
        memcpy(saved_cfg, hal_rec, 4);
    }
    else
    {
        return 0;
    }

    return 1;
}

// ---------------------------------------------------------------
// Tiny test framework
// ---------------------------------------------------------------

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

static void reset_world(void)
{
    memset(midi_log, 0, sizeof(midi_log));
    memset(leds, 0, sizeof(leds));
    memset(aux, 0, sizeof(aux));
    midi_len = 0;
    midi_space = 15;
    status_led = 0;
    display = 0xEE;
    blank = 0xEE;
    dots = 0xEE;
    lamp = 0xEE;
    cal_saved = 0;
    cal_stored_valid = 0;
    cfg_saved = 0;
    cfg_stored_valid = 0;
    relays = 0xEE;
    sw_saved = 0;
    sw_stored_valid[0] = 0;
    sw_stored_valid[1] = 0;
    msg_saved = 0;
    memset(msg_stored_valid, 0, sizeof(msg_stored_valid));
    memset(msg_was_saved, 0, sizeof(msg_was_saved));
    ctl_init();
    // Skip the power-on animation (test_boot_animation covers it).
    boot_end();
}

static void clear_log(void)
{
    midi_len = 0;
}

// Store pedal mode m for footswitch sw (0-9) of bank b (0-based) in the fake EEPROM, in
// its record's type byte (bits 2-3), keeping a stored message or else the default one.
static void store_bank_mode(int b, int sw, uint8_t m)
{
    int r = b * HAL_REC_SW1 + sw;

    if (!msg_stored_valid[r])
    {
        msg_stored_valid[r] = 1;
        stored_msg[r][0] = MT_CC;
        stored_msg[r][1] = stomp_cc[sw];
        stored_msg[r][2] = 127;
        stored_msg[r][3] = 0;
    }

    stored_msg[r][0] = (uint8_t)((stored_msg[r][0] & 3) | (m << 2));
}

// Bank 1's ten modes, given packed two bits per switch (switch 1 lowest), four switches a
// byte.
static void store_modes(uint8_t b0, uint8_t b1, uint8_t b2)
{
    int i;
    uint8_t packed[3];

    packed[0] = b0;
    packed[1] = b1;
    packed[2] = b2;

    for (i = 0; i < STOMP_COUNT; i++)
    {
        store_bank_mode(0, i, (uint8_t)((packed[i >> 2] >> ((i & 3) * 2)) & 3));
    }
}

// Every footswitch record saved so far becomes what a load returns (the fake keeps saves
// apart from loads otherwise).
static void keep_saved_msgs(void)
{
    int r;

    for (r = 0; r < MSG_RECS; r++)
    {
        if (msg_was_saved[r])
        {
            msg_stored_valid[r] = 1;
            memcpy(stored_msg[r], saved_msg[r], 4);
        }
    }
}

// Feed one pedal channel a reading.
static void pedal(uint8_t ch, uint8_t raw)
{
    ctl_pedal_sample(ch, raw);
}

static int cc_count(void)
{
    return midi_len / 3;
}

static uint8_t cc_status(int i) { return midi_log[i * 3 + 0]; }
static uint8_t cc_number(int i) { return midi_log[i * 3 + 1]; }
static uint8_t cc_value(int i)  { return midi_log[i * 3 + 2]; }

static void ticks(int n)
{
    while (n-- > 0)
    {
        ctl_tick();
    }
}

// A footswitch press and release with no hold, then one that is held past the threshold.
static void tap(uint8_t code)
{
    ctl_key_press(code);
    ctl_key_release(code);
}

static void long_press(uint8_t code)
{
    ctl_key_press(code);
    ctl_key_hold(code);
    ctl_key_release(code);
}

static void chord_down(void)
{
    ctl_key_press(CTL_KEY_UP);
    ctl_key_press(CTL_KEY_DOWN);
}

static void chord_up(void)
{
    ctl_key_release(CTL_KEY_UP);
    ctl_key_release(CTL_KEY_DOWN);
}

static void chord_long(void)
{
    chord_down();
    ticks(CHORD_TICKS);
    chord_up();
}

static void chord_short(void)
{
    chord_down();
    ticks(10);
    chord_up();
}

// Pin a switch's mode as if it had been saved and the unit then started: inverted
// momentary starts on (LED lit), every other mode off. Used right after reset_world().
static void set_mode(uint8_t sw, uint8_t m)
{
    uint16_t bit = (uint16_t)((uint16_t)1 << sw);

    pedal_mode_set(sw, m);

    if (m == PM_MOM_INV)
    {
        stomp_state |= bit;
    }
    else
    {
        stomp_state &= (uint16_t)~bit;
    }

    hal_stomp_led(sw, m == PM_MOM_INV);
}

// Configuration mode: a long UP saves and leaves, a long DOWN cancels and leaves.
static void long_nav(uint8_t code)
{
    ctl_key_press(code);
    ticks(5);
    ctl_key_hold(code);
    ctl_key_release(code);
}

static void save_exit(void)   { long_nav(CTL_KEY_UP); }
static void cancel_exit(void) { long_nav(CTL_KEY_DOWN); }

// Key code for footswitch 1..10 (codes 0-4 are switches 1-5, 6-10 are switches 6-10).
static uint8_t sw_code(int sw)
{
    return (uint8_t)((sw <= 5) ? (sw - 1) : sw);
}

// ---------------------------------------------------------------
// Tests
// ---------------------------------------------------------------

static void test_scaling(void)
{
    printf("pedal scaling\n");

    CHECK(ctl_scale(0, 0, 255) == 0);
    CHECK(ctl_scale(255, 0, 255) == 127);
    CHECK(ctl_scale(128, 0, 255) >= 63 && ctl_scale(128, 0, 255) <= 64);

    // Monotonic over the whole range.
    {
        int raw;
        uint8_t last = 0;
        int ok = 1;

        for (raw = 0; raw <= 255; raw++)
        {
            uint8_t v = ctl_scale((uint8_t)raw, 0, 255);

            if (v < last || v > 127)
            {
                ok = 0;
            }
            last = v;
        }
        CHECK(ok);
    }

    // A calibrated window of 30..210.
    CHECK(ctl_scale(30, 30, 210) == 0);
    CHECK(ctl_scale(210, 30, 210) == 127);
    CHECK(ctl_scale(10, 30, 210) == 0);
    CHECK(ctl_scale(250, 30, 210) == 127);
    CHECK(ctl_scale(120, 30, 210) >= 63 && ctl_scale(120, 30, 210) <= 64);

    // A pedal wired backwards (heel reads high) still gives 0 at the heel.
    CHECK(ctl_scale(220, 220, 20) == 0);
    CHECK(ctl_scale(20, 220, 20) == 127);
    CHECK(ctl_scale(120, 220, 20) >= 63 && ctl_scale(120, 220, 20) <= 64);

    // Degenerate calibration must not divide by zero.
    CHECK(ctl_scale(100, 100, 100) == 0);
}

static void test_pedal_cc(void)
{
    int raw;
    int i;
    const uint8_t expected_cc[2] = { PEDAL_CC_LIST };
    uint8_t last;

    printf("expression pedals\n");
    reset_world();

    // First reading after power-up is adopted silently.
    pedal(0, 0);
    pedal(1, 0);
    CHECK(midi_len == 0);

    // Sweep pedal A heel to toe in single-count steps.
    for (raw = 0; raw <= 255; raw++)
    {
        pedal(0, (uint8_t)raw);
    }

    CHECK(midi_len > 0);
    CHECK(midi_len % 3 == 0);

    last = 0;
    for (i = 0; i < cc_count(); i++)
    {
        CHECK(cc_status(i) == (0xB0 | (MIDI_CHANNEL - 1)));
        CHECK(cc_number(i) == expected_cc[0]);

        // Values only ever go up on an upward sweep and never repeat.
        CHECK(cc_value(i) > last || (i == 0 && cc_value(i) >= last));
        CHECK(cc_value(i) <= 127);
        last = cc_value(i);
    }

    CHECK(last == 127);

    // The sweep reaches the full range in a reasonable number of messages.
    CHECK(cc_count() >= 60 && cc_count() <= 127);

    // Pedal B uses its own controller number.
    clear_log();
    pedal(1, 255);
    CHECK(cc_count() == 1);
    CHECK(cc_number(0) == expected_cc[1]);
    CHECK(cc_value(0) == 127);

    // Back down to the heel reaches exactly 0.
    clear_log();
    for (raw = 255; raw >= 0; raw--)
    {
        pedal(1, (uint8_t)raw);
    }
    CHECK(cc_value(cc_count() - 1) == 0);
}

static void test_pedal_noise(void)
{
    int i;
    int start;

    printf("pedal noise rejection\n");
    reset_world();

    // Settle the pedal at mid travel.
    pedal(0, 128);
    pedal(0, 140);
    pedal(0, 128);
    clear_log();

    // One count of ADC jitter around a fixed position must send nothing.
    for (i = 0; i < 2000; i++)
    {
        pedal(0, (uint8_t)(128 + ((i * 7) % 3) - 1));
    }
    CHECK(midi_len == 0);

    // Jitter right on a value boundary must not make it chatter either.
    reset_world();
    pedal(0, 100);
    clear_log();
    start = 0;
    for (i = 0; i < 2000; i++)
    {
        pedal(0, (uint8_t)(((i & 1) ? 101 : 100)));
        start++;
    }
    CHECK(midi_len == 0);

    // Jitter at the heel and toe ends sends nothing once settled.
    reset_world();
    pedal(0, 0);
    clear_log();
    for (i = 0; i < 1000; i++)
    {
        pedal(0, (uint8_t)(i & 1));
    }
    CHECK(midi_len == 0);

    reset_world();
    pedal(0, 255);
    clear_log();
    for (i = 0; i < 1000; i++)
    {
        pedal(0, (uint8_t)(254 + (i & 1)));
    }
    CHECK(midi_len == 0);
}

#define CC_STATUS ((uint8_t)(0xB0 | (MIDI_CHANNEL - 1)))

static void test_stomp_modes(void)
{
    int sw;
    const uint8_t expected_cc[10] = { STOMP_CC_LIST };

    printf("stomp modes\n");
    reset_world();
    // Plain toggles for this loop, whatever DEFAULT_PEDAL_MODE is.
    for (sw = 0; sw < STOMP_COUNT; sw++)
    {
        set_mode((uint8_t)sw, PM_STOMP);
    }

    // Key codes 0..4 are switches 1..5 and 6..10 are switches 6..10.
    for (sw = 1; sw <= 10; sw++)
    {
        uint8_t code = (uint8_t)((sw <= 5) ? (sw - 1) : sw);

        clear_log();
        tap(code);
        CHECK(midi_len == 3);
        CHECK(cc_status(0) == CC_STATUS);
        CHECK(cc_number(0) == expected_cc[sw - 1]);
        CHECK(cc_value(0) == 127);
        CHECK(leds[sw - 1] == 1);

        clear_log();
        tap(code);
        CHECK(midi_len == 3);
        CHECK(cc_value(0) == 0);
        CHECK(leds[sw - 1] == 0);
    }

    // Inverted momentary: 0 while pressed, 127 on release, however long the press,
    // whatever the state before; the LED follows the value sent.
    reset_world();
    set_mode(3, PM_MOM_INV);
    clear_log();
    ctl_key_press(3);
    CHECK(midi_len == 3 && cc_number(0) == stomp_cc[3] && cc_value(0) == 0 && leds[3] == 0);
    ctl_key_release(3);
    CHECK(midi_len == 6 && cc_value(1) == 127 && leds[3] == 1);
    clear_log();
    long_press(3);
    CHECK(midi_len == 6 && cc_value(0) == 0 && cc_value(1) == 127 && leds[3] == 1);
    // Turned off over MIDI IN while held: release still sends 127.
    clear_log();
    ctl_key_press(3);
    ctl_midi_in(CC_STATUS); ctl_midi_in(stomp_cc[3]); ctl_midi_in(0);
    ctl_key_release(3);
    CHECK(midi_len == 6 && cc_value(1) == 127 && leds[3] == 1);
    // MIDI OUT full at release: the 127 goes out as soon as there is room.
    clear_log();
    ctl_key_press(3);
    midi_space = 2;
    ctl_key_release(3);
    CHECK(midi_len == 3 && leds[3] == 0);
    midi_space = 64;
    ticks(1);
    CHECK(midi_len == 6 && cc_value(1) == 127 && leds[3] == 1);
    // Default mode is untouched elsewhere.
    CHECK(pedal_mode_get(2) == DEFAULT_PEDAL_MODE);

    // Stomp with momentary: On, then a long press turns it off and releasing puts it back on.
    reset_world();
    set_mode(0, PM_STOMP_MOM);
    tap(0);
    clear_log();
    ctl_key_press(0);
    CHECK(midi_len == 3 && cc_value(0) == 0 && leds[0] == 0);
    ctl_key_hold(0);
    ctl_key_release(0);
    CHECK(midi_len == 6 && cc_value(1) == 127 && leds[0] == 1);

    // Released before the threshold, it simply stays off.
    reset_world();
    set_mode(0, PM_STOMP_MOM);
    tap(0);
    clear_log();
    tap(0);
    CHECK(midi_len == 3 && cc_value(0) == 0 && leds[0] == 0);

    // Plain stomp mode ignores the hold: a long press is just a latched toggle.
    reset_world();
    set_mode(0, PM_STOMP);
    clear_log();
    long_press(0);
    CHECK(midi_len == 3 && cc_value(0) == 127 && leds[0] == 1);
    long_press(0);
    CHECK(midi_len == 6 && cc_value(1) == 0 && leds[0] == 0);

    // Momentary mode: 127 while pressed, 0 on release, whatever the stored state was.
    reset_world();
    set_mode(2, PM_MOMENTARY);
    clear_log();
    ctl_key_press(2);
    CHECK(midi_len == 3 && cc_number(0) == stomp_cc[2] && cc_value(0) == 127 && leds[2] == 1);
    ctl_key_release(2);
    CHECK(midi_len == 6 && cc_value(1) == 0 && leds[2] == 0);

    // Momentary mode needs no hold event, and a hold event changes nothing.
    clear_log();
    ctl_key_press(2);
    ctl_key_hold(2);
    ctl_key_release(2);
    CHECK(midi_len == 6 && cc_value(0) == 127 && cc_value(1) == 0 && leds[2] == 0);

    // Momentary after a remote ON: Still goes on, then off.
    reset_world();
    set_mode(0, PM_MOMENTARY);
    ctl_midi_in(CC_STATUS);
    ctl_midi_in(stomp_cc[0]);
    ctl_midi_in(127);
    CHECK(leds[0] == 1);
    clear_log();
    ctl_key_press(0);
    ctl_key_release(0);
    CHECK(midi_len == 6 && cc_value(0) == 127 && cc_value(1) == 0 && leds[0] == 0);

    // A stomp press does not change the displayed program number.
    reset_world();
    tap(0);
    CHECK(display == 0);

    // A release without a press (for example after a mode change) does nothing.
    clear_log();
    ctl_key_release(4);
    CHECK(midi_len == 0);

    // Out-of-range key codes are ignored.
    ctl_key_press(12);
    ctl_key_press(200);
    ctl_key_release(12);
    ctl_key_hold(12);
    CHECK(midi_len == 0);

    // A press while MIDI OUT is backed up is ignored entirely.
    reset_world();
    midi_space = 2;
    ctl_key_press(0);
    ctl_key_release(0);
    CHECK(midi_len == 0 && leds[0] == BOOT_LED);

    // A revert that cannot be sent at release is sent from the tick once there is room.
    reset_world();
    set_mode(0, PM_STOMP_MOM);
    tap(0);
    clear_log();
    ctl_key_press(0);
    ctl_key_hold(0);
    midi_space = 2;
    ctl_key_release(0);
    CHECK(leds[0] == 0 && midi_len == 3);
    midi_space = 15;
    ctl_tick();
    CHECK(midi_len == 6 && cc_value(1) == 127 && leds[0] == 1);
}

static void test_program_change(void)
{
    int i;

    printf("program change\n");
    reset_world();
    CHECK(display == 0);

    // Nothing happens on press; a tap sends one change on release.
    clear_log();
    ctl_key_press(CTL_KEY_UP);
    CHECK(midi_len == 0 && display == 0);
    ctl_key_release(CTL_KEY_UP);
    CHECK(midi_len == 2 && midi_log[0] == (0xC0 | (MIDI_CHANNEL - 1)) && midi_log[1] == 1);
    CHECK(display == 1);

    clear_log();
    tap(CTL_KEY_DOWN);
    CHECK(midi_len == 2 && midi_log[1] == 0 && display == 0);

    // The ends follow PROGRAM_WRAP: wrap round, or stay at 0 and send nothing (the program
    // did not change).
    clear_log();
    tap(CTL_KEY_DOWN);
#if PROGRAM_WRAP
    CHECK(midi_len == 2 && midi_log[1] == 127 && display == 127);
    clear_log();
    tap(CTL_KEY_UP);
    CHECK(midi_len == 2 && midi_log[1] == 0 && display == 0);
#else
    CHECK(midi_len == 0 && display == 0 && program == 0);
    for (i = 0; i < 126; i++)
    {
        tap(CTL_KEY_UP);
    }
    clear_log();
    tap(CTL_KEY_UP);
    CHECK(midi_len == 2 && midi_log[1] == 127 && display == 127 && program == 127);
    clear_log();
    tap(CTL_KEY_UP);
    CHECK(midi_len == 0 && display == 127 && program == 127);
    // A hold that does not move it sends nothing either; DOWN from there does.
    ctl_key_press(CTL_KEY_UP);
    ticks(REPEAT_DELAY_TICKS + REPEAT_PERIOD_TICKS * 3);
    ctl_key_release(CTL_KEY_UP);
    CHECK(midi_len == 0 && program == 127);
    tap(CTL_KEY_DOWN);
    CHECK(midi_len == 2 && midi_log[1] == 126);
#endif

    // Just before the delay nothing repeats; at the delay the display steps, MIDI stays silent.
    reset_world();
    clear_log();
    ctl_key_press(CTL_KEY_UP);
    ticks(REPEAT_DELAY_TICKS - 1);
    CHECK(display == 0 && midi_len == 0);
    ticks(1);
    CHECK(display == 1 && midi_len == 0);

    // Then one step every repeat period (4Hz).
    ticks(REPEAT_PERIOD_TICKS - 1);
    CHECK(display == 1);
    ticks(1);
    CHECK(display == 2);

    for (i = 0; i < 4; i++)
    {
        ticks(REPEAT_PERIOD_TICKS);
    }
    CHECK(display == 6 && midi_len == 0);

    // Releasing sends exactly one change for the number on the display, no extra step.
    ctl_key_release(CTL_KEY_UP);
    CHECK(display == 6 && midi_len == 2 && midi_log[1] == 6);

    // DOWN repeats the other way: wrapping to 127, 126, or staying at 0.
    reset_world();
    clear_log();
    ctl_key_press(CTL_KEY_DOWN);
    ticks(REPEAT_DELAY_TICKS);
    ticks(REPEAT_PERIOD_TICKS);
    CHECK(display == (PROGRAM_WRAP ? 126 : 0) && midi_len == 0);
    ctl_key_release(CTL_KEY_DOWN);
    CHECK(PROGRAM_WRAP ? (midi_len == 2 && midi_log[1] == 126) : (midi_len == 0 && program == 0));

    // Held about PROGRAM_REPEAT_FAST_AFTER_MS (4 s), the repeat speeds up to
    // PROGRAM_REPEAT_FAST_HZ (20 Hz): step REPEAT_FAST_STEPS is the first at or after it.
    CHECK(REPEAT_FAST_TICKS * TICK_MS == 1000 / PROGRAM_REPEAT_FAST_HZ);
    CHECK((REPEAT_DELAY_TICKS + REPEAT_PERIOD_TICKS * (REPEAT_FAST_STEPS - 1)) * TICK_MS >= PROGRAM_REPEAT_FAST_AFTER_MS);
    CHECK((REPEAT_DELAY_TICKS + REPEAT_PERIOD_TICKS * (REPEAT_FAST_STEPS - 2)) * TICK_MS < PROGRAM_REPEAT_FAST_AFTER_MS);
    reset_world();
    clear_log();
    ctl_key_press(CTL_KEY_UP);
    ticks(REPEAT_DELAY_TICKS);
    ticks(REPEAT_PERIOD_TICKS * (REPEAT_FAST_STEPS - 2));
    CHECK(display == REPEAT_FAST_STEPS - 1);
    // Still slow just before the fast step.
    ticks(REPEAT_FAST_TICKS);
    CHECK(display == REPEAT_FAST_STEPS - 1);
    ticks(REPEAT_PERIOD_TICKS - REPEAT_FAST_TICKS);
    CHECK(display == REPEAT_FAST_STEPS);
    ticks(REPEAT_FAST_TICKS - 1);
    CHECK(display == REPEAT_FAST_STEPS);
    ticks(1);
    CHECK(display == REPEAT_FAST_STEPS + 1);
    ticks(REPEAT_FAST_TICKS * 10);
    CHECK(display == REPEAT_FAST_STEPS + 11 && midi_len == 0);
    // Keeps going fast to the end: 127 without wrap, round again with it.
    ticks(REPEAT_FAST_TICKS * 200);
    CHECK(display == (PROGRAM_WRAP ? (REPEAT_FAST_STEPS + 211) % 128 : 127) && midi_len == 0);
    ctl_key_release(CTL_KEY_UP);
    CHECK(midi_len == 2 && midi_log[1] == display);

    // The next hold starts slow again.
    clear_log();
    ctl_key_press(CTL_KEY_DOWN);
    i = program;
    ticks(REPEAT_DELAY_TICKS);
    CHECK(program == i - 1);
    ticks(REPEAT_FAST_TICKS);
    CHECK(program == i - 1);
    ticks(REPEAT_PERIOD_TICKS - REPEAT_FAST_TICKS);
    CHECK(program == i - 2);
    ctl_key_release(CTL_KEY_DOWN);
    CHECK(midi_len == 2 && midi_log[1] == i - 2);

    // Without wrap a long DOWN hold stops at 0.
    reset_world();
    clear_log();
    ctl_key_press(CTL_KEY_DOWN);
    // REPEAT_FAST_STEPS slow steps, then 50 fast ones.
    ticks(REPEAT_DELAY_TICKS + REPEAT_PERIOD_TICKS * (REPEAT_FAST_STEPS - 1) + REPEAT_FAST_TICKS * 50);
    CHECK(display == (PROGRAM_WRAP ? (128 - (REPEAT_FAST_STEPS + 50)) & 127 : 0));
    ctl_key_release(CTL_KEY_DOWN);
    CHECK(PROGRAM_WRAP ? (midi_len == 2 && midi_log[1] == display) : midi_len == 0);

    // A hold that reaches the end does send it, once.
    reset_world();
    for (i = 0; i < 3; i++)
    {
        tap(CTL_KEY_UP);
    }
    clear_log();
    ctl_key_press(CTL_KEY_DOWN);
    ticks(REPEAT_DELAY_TICKS + REPEAT_PERIOD_TICKS * 10);
    ctl_key_release(CTL_KEY_DOWN);
    CHECK(midi_len == 2 && midi_log[1] == (PROGRAM_WRAP ? 128 - 8 : 0));

    // A release at exactly the delay (repeating started) does not add a step.
    reset_world();
    ctl_key_press(CTL_KEY_UP);
    ticks(REPEAT_DELAY_TICKS);
    clear_log();
    ctl_key_release(CTL_KEY_UP);
    CHECK(display == 1 && midi_len == 2 && midi_log[1] == 1);

    // A release just under the delay is an ordinary tap.
    reset_world();
    ctl_key_press(CTL_KEY_UP);
    ticks(REPEAT_DELAY_TICKS - 1);
    clear_log();
    ctl_key_release(CTL_KEY_UP);
    CHECK(display == 1 && midi_len == 2);

    // The repeat timer starts fresh for each press.
    ctl_key_press(CTL_KEY_UP);
    ticks(REPEAT_DELAY_TICKS - 1);
    CHECK(display == 1);

    // If MIDI OUT is full at release, the change goes out from the tick, once, with the final value.
    reset_world();
    ctl_key_press(CTL_KEY_UP);
    midi_space = 1;
    clear_log();
    ctl_key_release(CTL_KEY_UP);
    CHECK(midi_len == 0);
    ticks(5);
    CHECK(midi_len == 0);
    midi_space = 15;
    ticks(1);
    CHECK(midi_len == 2 && midi_log[1] == 1);
    ticks(5);
    CHECK(midi_len == 2);

    // The pending change is held back while the next press is being repeated.
    reset_world();
    ctl_key_press(CTL_KEY_UP);
    midi_space = 1;
    ctl_key_release(CTL_KEY_UP);
    ctl_key_press(CTL_KEY_UP);
    midi_space = 15;
    clear_log();
    ticks(REPEAT_DELAY_TICKS + REPEAT_PERIOD_TICKS);
    CHECK(midi_len == 0 && display == 3);
    ctl_key_release(CTL_KEY_UP);
    CHECK(midi_len == 2 && midi_log[1] == 3);
}

static void test_chord_and_config_entry(void)
{
    printf("UP + DOWN chord\n");
    reset_world();

    // Both keys held for just under the hold time: Still in run mode.
    clear_log();
    chord_down();
    ticks(CHORD_TICKS - 1);
    CHECK(status_led == 0 && mode == MODE_RUN);
    ticks(1);
    CHECK(status_led == 1 && mode == MODE_CONFIG);
    // Config mode opens on page 1, pedal modes.
    CHECK(cfg_page == PAGE_MODES && display == pedal_mode_get(0) + 1);
    CHECK(aux[HAL_LED_MIDI_FUNC] == 1 && aux[HAL_LED_MIDI_CHAN] == 0);
    chord_up();
    CHECK(mode == MODE_CONFIG && status_led == 1);
    CHECK(midi_len == 0);

    // A chord that is let go early in run mode only toggles bank mode (see
    // test_bank_mode_toggle): nothing is sent, and the display shows bank 1.
    reset_world();
    clear_log();
    chord_short();
    CHECK(mode == MODE_RUN && midi_len == 0 && bank_mode && display == 1 && program == 0);

    // Once the chord has opened config mode, UP and DOWN stay quiet until both are let go.
    reset_world();
    chord_down();
    ticks(CHORD_TICKS + 3 * REPEAT_DELAY_TICKS);
    CHECK(mode == MODE_CONFIG && cfg_page == PAGE_MODES);
    ctl_key_release(CTL_KEY_UP);
    CHECK(mode == MODE_CONFIG && cfg_page == PAGE_MODES);
    ctl_key_release(CTL_KEY_DOWN);
    CHECK(mode == MODE_CONFIG && cfg_page == PAGE_MODES && midi_len == 0 && cfg_saved == 0);

    // Pressing the second key late still counts as a chord, and a single key before
    // it does not send a program change.
    reset_world();
    ctl_key_press(CTL_KEY_UP);
    ticks(10);
    ctl_key_press(CTL_KEY_DOWN);
    ticks(CHORD_TICKS);
    CHECK(mode == MODE_CONFIG);
    chord_up();
    CHECK(midi_len == 0 && cfg_page == PAGE_MODES);

    // If the first key had already started repeating, the number on the display is sent
    // so the synth is not left behind it.
    reset_world();
    ctl_key_press(CTL_KEY_UP);
    ticks(REPEAT_DELAY_TICKS + REPEAT_PERIOD_TICKS);
    CHECK(display == 2);
    clear_log();
    ctl_key_press(CTL_KEY_DOWN);
    CHECK(midi_len == 2 && midi_log[1] == 2);
    ticks(CHORD_TICKS);
    chord_up();
    CHECK(mode == MODE_CONFIG);

    // The chord timer does not carry over from an earlier chord.
    reset_world();
    chord_down();
    ticks(CHORD_TICKS - 5);
    chord_up();
    chord_down();
    ticks(CHORD_TICKS - 5);
    CHECK(mode == MODE_RUN);
    chord_up();

    // A stomp being held when config mode opens is put back, so nothing stays on.
    reset_world();
    set_mode(0, PM_MOMENTARY);
    ctl_key_press(0);
    clear_log();
    chord_long();
    CHECK(mode == MODE_CONFIG);
    ctl_tick();
    CHECK(midi_len == 3 && cc_number(0) == stomp_cc[0] && cc_value(0) == 0);
    ctl_key_release(0);
    CHECK(midi_len == 3);

    // In config mode footswitches and pedals send no MIDI.
    clear_log();
    tap(1);
    pedal(0, 10);
    pedal(0, 200);
    CHECK(midi_len == 0);
}

static void test_config_channel(void)
{
    printf("config: MIDI channel, pages, save\n");
    reset_world();
    chord_long();
    // Page 1 is pedal modes; UP goes to page 2, the MIDI channel.
    CHECK(mode == MODE_CONFIG && cfg_page == PAGE_MODES);
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_CHANNEL && display == MIDI_CHANNEL);

    // UP / DOWN change the page and never the channel; they do not auto-repeat.
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_MODES && midi_channel == MIDI_CHANNEL);
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_CHANNEL && display == MIDI_CHANNEL);
    ctl_key_press(CTL_KEY_UP);
    ticks(REPEAT_DELAY_TICKS + 3 * REPEAT_PERIOD_TICKS);
    CHECK(cfg_page == PAGE_CHANNEL);
    ctl_key_release(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_HEEL && midi_channel == MIDI_CHANNEL);
    tap(CTL_KEY_DOWN);

    // A short UP + DOWN does nothing here (only the SWITCH pages use it).
    chord_short();
    CHECK(mode == MODE_CONFIG && cfg_page == PAGE_CHANNEL);

    // Channel 4 typed on footswitch 4.
    tap(sw_code(4));
    CHECK(display == 4 && midi_channel == 4);

    // No program change is sent in config mode, and the program number is untouched.
    CHECK(midi_len == 0 && program == 0);

    // A long UP saves and leaves. The CONFIG and page LEDs go out and the display
    // shows the program number again; UP did not step the program.
    save_exit();
    CHECK(mode == MODE_RUN && status_led == 0 && display == 0);
    CHECK(cfg_saved == 1 && saved_cfg[0] == 4);
    CHECK(aux[0] == 0 && aux[1] == 0 && aux[2] == 0 && aux[3] == 0);
    CHECK(cal_saved == 1);
    CHECK(midi_len == 0 && program == 0);

    // Everything now uses channel 4.
    // Toggle mode, so this does not depend on DEFAULT_PEDAL_MODE.
    set_mode(0, PM_STOMP);
    clear_log();
    tap(0);
    CHECK(midi_len == 3 && cc_status(0) == 0xB3);
    clear_log();
    tap(CTL_KEY_UP);
    CHECK(midi_len == 2 && midi_log[0] == 0xC3);

    // MIDI IN follows the new channel and ignores the old one.
    ctl_midi_in(0xB3);
    ctl_midi_in(stomp_cc[1]);
    ctl_midi_in(127);
    CHECK(leds[1] == 1);
    ctl_midi_in((uint8_t)(0xB0 | (MIDI_CHANNEL - 1)));
    ctl_midi_in(stomp_cc[2]);
    ctl_midi_in(127);
    CHECK(leds[2] == BOOT_LED);

    // Saved settings are picked up at the next start-up.
    memcpy(stored_cfg, saved_cfg, 4);
    cfg_stored_valid = 1;
    ctl_init();
    clear_log();
    tap(0);
    CHECK(cc_status(0) == 0xB3);

    // An out-of-range saved channel falls back to the default.
    stored_cfg[0] = 17;
    ctl_init();
    CHECK(midi_channel == MIDI_CHANNEL);
    stored_cfg[0] = 0;
    ctl_init();
    CHECK(midi_channel == MIDI_CHANNEL);

    // Holding a key that is part of the entry chord does not leave config mode, and
    // its hold event (about 1s after its press) is ignored.
    reset_world();
    chord_down();
    ticks(CHORD_TICKS);
    CHECK(mode == MODE_CONFIG);
    ctl_key_hold(CTL_KEY_UP);
    ctl_key_hold(CTL_KEY_DOWN);
    chord_up();
    CHECK(mode == MODE_CONFIG && cfg_saved == 0);

    // UP held while DOWN is also down makes a chord, so its hold does nothing either.
    ctl_key_press(CTL_KEY_UP);
    ctl_key_press(CTL_KEY_DOWN);
    ctl_key_hold(CTL_KEY_UP);
    chord_up();
    CHECK(mode == MODE_CONFIG && cfg_page == PAGE_MODES);

    // A hold event in run mode does nothing to UP / DOWN.
    save_exit();
    CHECK(mode == MODE_RUN);
    ctl_key_press(CTL_KEY_DOWN);
    ctl_key_hold(CTL_KEY_DOWN);
    CHECK(mode == MODE_RUN);
    ctl_key_release(CTL_KEY_DOWN);

    // A long UP + DOWN in config mode saves and leaves, like a long UP. The keys' own
    // hold events and the release do nothing more (no bank mode toggle, no program).
    reset_world();
    chord_long();
    tap(CTL_KEY_UP);
    tap(sw_code(5));
    clear_log();
    chord_down();
    ticks(CHORD_TICKS - 1);
    CHECK(mode == MODE_CONFIG && cfg_saved == 0);
    ticks(1);
    CHECK(mode == MODE_RUN && status_led == 0 && display == 0);
    CHECK(cfg_saved == 1 && saved_cfg[0] == 5 && cal_saved == 1 && midi_channel == 5);
    CHECK(aux[HAL_LED_MIDI_CHAN] == 0 && aux[HAL_LED_MIDI_FUNC] == 0);
    ctl_key_hold(CTL_KEY_UP);
    ctl_key_hold(CTL_KEY_DOWN);
    ticks(CHORD_TICKS);
    chord_up();
    CHECK(mode == MODE_RUN && midi_len == 0 && program == 0 && bank_mode == 0);
    // The next long chord opens config mode again.
    chord_long();
    CHECK(mode == MODE_CONFIG && cfg_page == PAGE_MODES);
}

// A long DOWN drops every change made in configuration mode and saves nothing.
static void test_config_cancel(void)
{
    printf("config: cancel with a long DOWN\n");
    reset_world();
    // Toggle mode, so this does not depend on DEFAULT_PEDAL_MODE.
    set_mode(2, PM_STOMP);
    tap(2);
    CHECK(leds[2] == 1);
    chord_long();
    tap(2);
    tap(2);
    tap(CTL_KEY_UP);
    tap(sw_code(7));
    CHECK(midi_channel == 7 && pedal_mode_get(2) == PM_STOMP_MOM);
    tap(CTL_KEY_UP);
    pedal(0, 20);
    pedal(1, 20);
    tap(0);
    tap(CTL_KEY_UP);
    pedal(0, 220);
    pedal(1, 220);
    tap(0);
    CHECK(heel[0] == 20 && toe[0] == 220);
    clear_log();
    cancel_exit();
    CHECK(mode == MODE_RUN && status_led == 0 && display == 0 && program == 0);
    CHECK(cfg_saved == 0 && cal_saved == 0);
    CHECK(midi_channel == MIDI_CHANNEL && heel[0] == 0 && toe[0] == 255);
    CHECK(aux[0] == 0 && aux[1] == 0 && aux[2] == 0 && aux[3] == 0);

    // Nothing saved, so modes go back to the default; switch 3 stays on with no MIDI
    // (its mode change was cancelled, so it is not turned off).
    CHECK(pedal_mode_get(2) == DEFAULT_PEDAL_MODE);
    ticks(1);
    CHECK(midi_len == 0 && leds[2] == 1);

    // With saved settings, cancel returns to them.
    reset_world();
    cfg_stored_valid = 1;
    stored_cfg[0] = 5;
    stored_cfg[1] = 0;
    stored_cfg[2] = 0;
    stored_cfg[3] = 0;
    store_modes(0, 0, 0);
    cal_stored_valid = 1;
    stored_heel[0] = 40; stored_heel[1] = 50;
    stored_toe[0]  = 140; stored_toe[1] = 150;
    ctl_init();
    chord_long();
    // Switch 1 starts selected on page 1, so one press changes it.
    tap(0);
    tap(CTL_KEY_UP);
    tap(sw_code(9));
    CHECK(midi_channel == 9 && pedal_mode_get(0) == PM_STOMP_MOM);
    cancel_exit();
    CHECK(midi_channel == 5 && pedal_mode_get(0) == PM_STOMP);
    CHECK(heel[0] == 40 && heel[1] == 50 && toe[0] == 140 && toe[1] == 150);
    CHECK(cfg_saved == 0 && cal_saved == 0);
}

static void test_config_modes(void)
{
    int i;

    printf("config: pedal modes\n");
    reset_world();
    chord_long();
    // Footswitch 1 starts selected: its mode digit (mode + 1) shows and its LED is lit.
    CHECK(display == DEFAULT_PEDAL_MODE + 1 && aux[HAL_LED_MIDI_FUNC] == 1 && aux[HAL_LED_MIDI_CHAN] == 0);
    CHECK(aux[HAL_LED_EXP_1] == 0 && aux[HAL_LED_EXP_2] == 0);
    CHECK(leds[0] == 1 && cfg_sel == 0);
    CHECK(pedal_mode_get(0) == DEFAULT_PEDAL_MODE);

    // Selecting footswitch 3 shows its mode and lights its LED alone; nothing changes.
    // It starts in a known mode so the cycle below does not depend on the default.
    set_mode(2, PM_STOMP_MOM);
    tap(2);
    CHECK(cfg_sel == 2 && pedal_mode_get(2) == PM_STOMP_MOM);
    CHECK(display == PM_STOMP_MOM + 1);
    CHECK(leds[2] == 1 && leds[0] == 0);

    // Pressing it again cycles stomp+momentary -> momentary -> inverted momentary ->
    // stomp -> stomp+momentary.
    tap(2);
    CHECK(pedal_mode_get(2) == PM_MOMENTARY && display == PM_MOMENTARY + 1);
    tap(2);
    CHECK(pedal_mode_get(2) == PM_MOM_INV && display == 4);
    tap(2);
    CHECK(pedal_mode_get(2) == PM_STOMP && display == PM_STOMP + 1);
    tap(2);
    CHECK(pedal_mode_get(2) == PM_STOMP_MOM && display == PM_STOMP_MOM + 1);
    tap(2);
    tap(2);
    tap(2);
    CHECK(pedal_mode_get(2) == PM_STOMP && leds[2] == 1);

    // Switch 1 is no longer selected, so its press selects it without a change.
    tap(0);
    CHECK(cfg_sel == 0 && pedal_mode_get(0) == DEFAULT_PEDAL_MODE && leds[0] == 1 && leds[2] == 0);

    // Other pedals are unaffected.
    for (i = 0; i < STOMP_COUNT; i++)
    {
        if (i != 2)
        {
            CHECK(pedal_mode_get(i) == DEFAULT_PEDAL_MODE);
        }
    }

    // Footswitch 10 (key code 10): Last switch, uses the third byte of settings.
    set_mode(9, PM_STOMP_MOM);
    tap(10);
    CHECK(cfg_sel == 9 && display == PM_STOMP_MOM + 1 && leds[9] == 1);
    tap(10);
    CHECK(pedal_mode_get(9) == PM_MOMENTARY);
    CHECK(pedal_mode_get(8) == DEFAULT_PEDAL_MODE && pedal_mode_get(7) == DEFAULT_PEDAL_MODE);

    // UP / DOWN only change the page here, never a mode.
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_CHANNEL && pedal_mode_get(9) == PM_MOMENTARY);
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_MODES && pedal_mode_get(9) == PM_MOMENTARY && cfg_sel == 9);

    // Leave, then check the saved bytes round-trip into a fresh controller.
    save_exit();
    CHECK(mode == MODE_RUN && cfg_saved == 1);
    // The modes are saved in the footswitch records (type byte bits 2-3), not the
    // settings record, whose last three bytes are 0.
    CHECK(msg_was_saved[9] && (saved_msg[9][0] >> 2) == PM_MOMENTARY && (saved_msg[9][0] & 3) == MT_CC);
    CHECK((saved_msg[2][0] >> 2) == PM_STOMP && saved_cfg[1] == 0 && saved_cfg[2] == 0 && saved_cfg[3] == 0);
    memcpy(stored_cfg, saved_cfg, 4);
    cfg_stored_valid = 1;
    keep_saved_msgs();
    ctl_init();
    CHECK(pedal_mode_get(2) == PM_STOMP);
    CHECK(pedal_mode_get(9) == PM_MOMENTARY);
    CHECK(pedal_mode_get(0) == DEFAULT_PEDAL_MODE && pedal_mode_get(5) == DEFAULT_PEDAL_MODE);

    // The new modes behave: 3 is a plain stomp, 10 is momentary.
    clear_log();
    long_press(2);
    CHECK(midi_len == 3 && cc_value(0) == 127);
    clear_log();
    ctl_key_press(10);
    ctl_key_release(10);
    CHECK(midi_len == 6 && cc_number(0) == stomp_cc[9] && cc_value(0) == 127 && cc_value(1) == 0);

    // Every 2-bit value is a mode: type byte 0xFC is a CC in inverted momentary, and
    // the type byte's high bits are ignored.
    for (i = 0; i < 4; i++)
    {
        stored_msg[i][0] = 0xFC;
    }

    stored_msg[8][0] = 0xF0;
    stored_msg[9][0] = 0xF1;
    ctl_init();
    CHECK(pedal_mode_get(0) == PM_MOM_INV && pedal_mode_get(3) == PM_MOM_INV && msg_type(0) == MT_CC);
    CHECK(pedal_mode_get(8) == PM_STOMP && pedal_mode_get(9) == PM_STOMP && msg_type(9) == MT_NOTE);
    // The settings record's last three bytes are not modes.
    stored_cfg[1] = 0xFF;
    stored_cfg[2] = 0xFF;
    stored_cfg[3] = 0xFF;
    ctl_init();
    CHECK(pedal_mode_get(0) == PM_MOM_INV && pedal_mode_get(8) == PM_STOMP && pedal_mode_get(9) == PM_STOMP);

    // Changing a switch to inverted momentary asserts it on saving: its state is set and
    // its LED lights, whether it was off (switch 2) or on (switch 3). Like a change to
    // any mode, the CC is sent only if the state changed: 127 for switch 2, nothing for
    // switch 3 (already on). Cancelling does not.
    reset_world();
    set_mode(1, PM_MOMENTARY);
    set_mode(2, PM_STOMP);
    tap(2);
    CHECK(leds[2] == 1 && leds[1] == 0);
    chord_long();
    tap(1);
    tap(1);
    tap(2);
    tap(2);
    tap(2);
    tap(2);
    CHECK(pedal_mode_get(1) == PM_MOM_INV && pedal_mode_get(2) == PM_MOM_INV);
    clear_log();
    save_exit();
    ticks(1);
    CHECK(midi_len == 3 && cc_number(0) == stomp_cc[1] && cc_value(0) == 127);
    CHECK(leds[1] == 1 && leds[2] == 1 && (stomp_state & 6) == 6);
    clear_log();
    tap(1);
    CHECK(midi_len == 6 && cc_value(0) == 0 && cc_value(1) == 127 && leds[1] == 1);
    // Already inverted momentary and not edited: nothing is sent on leaving.
    chord_long();
    clear_log();
    save_exit();
    ticks(1);
    CHECK(midi_len == 0);
    // A mode change that is cancelled sends nothing.
    set_mode(3, PM_STOMP);
    chord_long();
    tap(3);
    tap(3);
    tap(3);
    tap(3);
    clear_log();
    cancel_exit();
    ticks(1);
    CHECK(midi_len == 0 && leds[3] == 0);

    // Changing the mode of a stomp that is on turns it off when leaving config mode.
    reset_world();
    // Toggle mode, so this does not depend on DEFAULT_PEDAL_MODE.
    set_mode(4, PM_STOMP);
    tap(4);
    CHECK(leds[4] == 1);
    chord_long();
    tap(4);
    tap(4);
    clear_log();
    save_exit();
    ticks(1);
    CHECK(midi_len == 3 && cc_number(0) == stomp_cc[4] && cc_value(0) == 0);
    CHECK(leds[4] == 0 && mode == MODE_RUN);

    // A stomp that is on and not edited stays on.
    reset_world();
    // Toggle mode, so this does not depend on DEFAULT_PEDAL_MODE.
    set_mode(4, PM_STOMP);
    // Switch 3 changes mode but is off and stays off (stomp + momentary), so nothing is sent.
    set_mode(2, PM_STOMP);
    tap(4);
    chord_long();
    tap(2);
    tap(2);
    clear_log();
    save_exit();
    ticks(1);
    CHECK(midi_len == 0 && leds[4] == 1);

    // The same rule the other way: inverted momentary (on at rest) changed to stomp
    // goes to stomp's resting state, off, and CC 0 is sent.
    reset_world();
    set_mode(5, PM_MOM_INV);
    CHECK(leds[5] == 1 && (stomp_state & (1 << 5)) != 0);
    chord_long();
    tap(sw_code(6));
    tap(sw_code(6));
    CHECK(pedal_mode_get(5) == PM_STOMP);
    clear_log();
    save_exit();
    ticks(1);
    CHECK(midi_len == 3 && cc_number(0) == stomp_cc[5] && cc_value(0) == 0);
    CHECK(leds[5] == 0 && (stomp_state & (1 << 5)) == 0);

    // MIDI OUT full when saving: the switch changed to inverted momentary stays as it was
    // until there is room, then its 127 is sent and its LED lights (like the CC 0 above).
    reset_world();
    set_mode(1, PM_MOMENTARY);
    chord_long();
    tap(1);
    tap(1);
    CHECK(pedal_mode_get(1) == PM_MOM_INV);
    clear_log();
    midi_space = 2;
    save_exit();
    ticks(1);
    CHECK(midi_len == 0 && leds[1] == 0 && mode == MODE_RUN);
    midi_space = 15;
    ticks(1);
    CHECK(midi_len == 3 && cc_number(0) == stomp_cc[1] && cc_value(0) == 127);
    CHECK(leds[1] == 1 && (stomp_state & 2) != 0);

    // Pressing a footswitch on the modes page does not send MIDI.
    reset_world();
    chord_long();
    clear_log();
    tap(5 + 1);
    CHECK(midi_len == 0 && cfg_sel == 5);

    // Footswitches on the channel page type the channel and send no MIDI.
    reset_world();
    chord_long();
    tap(CTL_KEY_UP);
    clear_log();
    tap(3);
    CHECK(midi_len == 0 && display == 4 && midi_channel == 4);
}

// Channel entry with footswitches: 1-9 are channels 1-9; 10 is channel 10 and can be
// followed by a second digit while its 0 flashes.
static void test_config_channel_digits(void)
{
    int sw;

    printf("config: MIDI channel from the footswitches\n");
    reset_world();
    chord_long();
    tap(CTL_KEY_UP);
    clear_log();

    // Each of switches 1-9 sets its channel directly.
    for (sw = 1; sw <= 9; sw++)
    {
        tap(sw_code(sw));
        CHECK(midi_channel == sw && display == sw && blank == 0 && chan_digit_pending == 0);
    }

    // Switch 10 sets channel 10 and shows 10; a second digit can follow.
    tap(sw_code(10));
    CHECK(midi_channel == 10 && display == 10 && blank == 0 && chan_digit_pending == 1);

    // Then 1..6 make 11..16.
    tap(sw_code(1));
    CHECK(midi_channel == 11 && display == 11 && blank == 0 && chan_digit_pending == 0);
    for (sw = 2; sw <= 6; sw++)
    {
        tap(sw_code(10));
        tap(sw_code(sw));
        CHECK(midi_channel == 10 + sw && display == 10 + sw && blank == 0);
    }

    // 10 then 10 (as 0) is channel 10, with nothing more to wait for.
    tap(sw_code(10));
    tap(sw_code(10));
    CHECK(midi_channel == 10 && display == 10 && chan_digit_pending == 0);
    tap(sw_code(3));
    CHECK(midi_channel == 3);

    // 10 then 7-9 is not a channel, so 7-9 set channels 7-9.
    tap(sw_code(10));
    tap(sw_code(7));
    CHECK(midi_channel == 7 && display == 7 && blank == 0);

    // 1 never waits: 1 then 6 is channel 6.
    tap(sw_code(1));
    tap(sw_code(6));
    CHECK(midi_channel == 6);

    // The 0 of "10" flashes every CHANNEL_DIGIT_FLASH_MS: units blank, then back.
    tap(sw_code(10));
    CHECK(display == 10 && blank == 0);
    ticks(CHAN_FLASH_TICKS - 1);
    CHECK(display == 10 && blank == 0);
    ticks(1);
    CHECK(display == 10 && blank == 1);
    ticks(CHAN_FLASH_TICKS);
    CHECK(display == 10 && blank == 0);

    // CHANNEL_DIGIT_TIMEOUT_MS after the press the flashing stops: channel 10, shown
    // steadily, and a following digit starts a new entry.
    ticks(CHAN_FLASH_TICKS * CHAN_TIMEOUT_FLASHES - 2 * CHAN_FLASH_TICKS - 1);
    CHECK(chan_digit_pending == 1 && midi_channel == 10);
    ticks(1);
    CHECK(chan_digit_pending == 0 && midi_channel == 10 && display == 10 && blank == 0);
    ticks(CHAN_FLASH_TICKS * 2);
    CHECK(display == 10 && blank == 0);
    tap(sw_code(6));
    CHECK(midi_channel == 6);

    // A page change keeps channel 10 and stops the flashing.
    tap(sw_code(10));
    ticks(CHAN_FLASH_TICKS);
    CHECK(blank == 1);
    tap(CTL_KEY_DOWN);
    CHECK(chan_digit_pending == 0 && cfg_page == PAGE_MODES && midi_channel == 10);
    tap(CTL_KEY_UP);
    CHECK(display == 10 && blank == 0);
    tap(sw_code(4));
    CHECK(midi_channel == 4);

    // Leaving keeps it too: 10 is saved and the program number shows normally.
    tap(sw_code(10));
    save_exit();
    CHECK(mode == MODE_RUN && saved_cfg[0] == 10 && display == 0 && blank == 0);
    ticks(CHAN_FLASH_TICKS * 2);
    CHECK(display == 0 && blank == 0);

    // Nothing was sent, and a typed two-digit channel is saved and used.
    CHECK(midi_len == 0);
    chord_long();
    tap(CTL_KEY_UP);
    tap(sw_code(10));
    tap(sw_code(2));
    save_exit();
    CHECK(mode == MODE_RUN && saved_cfg[0] == 12);
    set_mode(0, PM_STOMP);
    clear_log();
    tap(0);
    CHECK(midi_len == 3 && cc_status(0) == 0xBB);

    // The modes page still uses footswitches to select, not to type.
    chord_long();
    tap(sw_code(3));
    CHECK(cfg_sel == 2 && midi_channel == 12);
}

static void test_config_page_leds(void)
{
    printf("config: pages, LEDs and display\n");
    reset_world();
    chord_long();
    // Page 1 pedal modes, page 2 MIDI channel.
    CHECK(cfg_page == PAGE_MODES && aux[HAL_LED_MIDI_CHAN] == 0 && aux[HAL_LED_MIDI_FUNC] == 1);
    CHECK(aux[HAL_LED_EXP_1] == 0 && aux[HAL_LED_EXP_2] == 0);
    CHECK(display == pedal_mode_get(0) + 1 && leds[0] == 1);
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_CHANNEL && aux[HAL_LED_MIDI_CHAN] == 1 && aux[HAL_LED_MIDI_FUNC] == 0);
    CHECK(aux[HAL_LED_EXP_1] == 0 && aux[HAL_LED_EXP_2] == 0);
    CHECK(display == MIDI_CHANNEL && leds[0] == 0);
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_HEEL && aux[HAL_LED_MIDI_CHAN] == 0 && aux[HAL_LED_MIDI_FUNC] == 0);
    CHECK(aux[HAL_LED_EXP_1] == 1 && aux[HAL_LED_EXP_2] == 1);
    CHECK(display == SHOWN_LO && leds[0] == 0);
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_TOE && aux[HAL_LED_EXP_1] == 1 && aux[HAL_LED_EXP_2] == 1);
    CHECK(display == SHOWN_HI);
    // Pages 5 and 6 light SWITCH 1 / SWITCH 2; page 7 is the last (no wrap).
    CHECK(aux[HAL_LED_SWITCH_1] == 0 && aux[HAL_LED_SWITCH_2] == 0);
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_SW1 && aux[HAL_LED_SWITCH_1] == 1 && aux[HAL_LED_SWITCH_2] == 0);
    CHECK(aux[HAL_LED_EXP_1] == 0 && aux[HAL_LED_EXP_2] == 0 && aux[HAL_LED_MIDI_CHAN] == 0);
    CHECK(display == 0 && blank == 7);
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_SW2 && aux[HAL_LED_SWITCH_1] == 0 && aux[HAL_LED_SWITCH_2] == 1);
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_VERSION && aux[HAL_LED_SWITCH_2] == 0 && lamp == HAL_LAMP_LEDS_STOMPS);
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_VERSION);
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_SW2 && aux[HAL_LED_SWITCH_2] == 1 && lamp == HAL_LAMP_NORMAL && dots == 0);
    tap(CTL_KEY_DOWN);
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_TOE && display == SHOWN_HI && aux[HAL_LED_SWITCH_1] == 0);
    CHECK(aux[HAL_LED_EXP_1] == 1 && aux[HAL_LED_EXP_2] == 1);
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_HEEL && display == SHOWN_LO);
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_CHANNEL && aux[HAL_LED_MIDI_CHAN] == 1 && aux[HAL_LED_MIDI_FUNC] == 0);
    // Leaving page 2 with a "10" waiting keeps channel 10.
    tap(sw_code(10));
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_MODES && aux[HAL_LED_MIDI_FUNC] == 1 && aux[HAL_LED_MIDI_CHAN] == 0);
    CHECK(chan_digit_pending == 0 && midi_channel == 10 && display == pedal_mode_get(0) + 1);
    // DOWN on page 1 stays on page 1 (no wrap).
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_MODES && aux[HAL_LED_MIDI_FUNC] == 1 && aux[HAL_LED_EXP_1] == 0);
    CHECK(aux[HAL_LED_SWITCH_2] == 0 && leds[0] == 1);
    save_exit();
    CHECK(mode == MODE_RUN && aux[HAL_LED_MIDI_CHAN] == 0 && aux[HAL_LED_MIDI_FUNC] == 0);
    CHECK(aux[HAL_LED_EXP_1] == 0 && aux[HAL_LED_EXP_2] == 0 && display == 0);
}

// From the first step of page 7 or power-on: every tick of the "riban" scroll, each frame
// (" r", "ri", "ib", "bA", "An", "n ") for VERSION_SHOW_MS / 2, no dots, every LED lit.
// Returns with the scroll's time used up exactly.
// Footswitch LEDs: every one lit except index off (-1: none).
static int stomp_leds_but(int off)
{
    int k;

    for (k = 0; k < STOMP_COUNT; k++)
    {
        if (leds[k] != (k != off))
        {
            return 0;
        }
    }

    return 1;
}

// The "riban" scroll moves ". ribAn ." across the three dots (bit 2 after the "1", bit 0
// far right): the dot of each text frame.
static const uint8_t scroll_dot_of[HAL_TEXT_SCROLL_FRAMES] = { 0x04, 0x00, 0x00, 0x00, 0x00, 0x01 };

// One scroll frame for its full time: text (HAL_TEXT_* value, or -1 for a blank display)
// and dots. On page 7 footswitch LED dark_led is dark for the first VER_FLASH_STEPS (-1:
// all lit); at power-on HAL_LAMP_LEDS_ON lights them all (not checked here).
static void expect_frame(int text, int dot_mask, int dark_led)
{
    int k, boot = boot_anim;

    for (k = 0; k < VER_STEP_TICKS * VER_SCROLL_STEPS; k++)
    {
        CHECK((text < 0 ? (display == 0 && blank == 7) : (display == SHOWN_TEXT(text) && blank == 0)) &&
              dots == dot_mask && midi_len == 0 &&
              lamp == (boot ? HAL_LAMP_LEDS_ON : HAL_LAMP_LEDS_STOMPS) &&
              (boot || stomp_leds_but((k < VER_STEP_TICKS * VER_FLASH_STEPS) ? dark_led : -1)));
        ticks(1);
    }
}

// The lead-in (power-on and page 7 start here; it ends the blank segment): the display
// blank, the leading dot far right, then between the digits.
static void expect_lead(void)
{
    expect_frame(-1, 0x01, -1);
    expect_frame(-1, 0x02, -1);
}

// The text frames (footswitch f's LED flashing for frame f), then the trailing dot
// running off after "n ": between the digits, then after the "1".
static void expect_scroll(void)
{
    int f;

    for (f = 0; f < HAL_TEXT_SCROLL_FRAMES; f++)
    {
        expect_frame(HAL_TEXT_SCROLL + f, scroll_dot_of[f], f);
    }

    expect_frame(-1, 0x02, -1);
    expect_frame(-1, 0x04, -1);
}

// Page 7: every LED lights at once (no blinking) and stays lit while "riban" scrolls,
// then the version shows as "major." / ".minor." / ".build", VERSION_SHOW_MS each, then a
// blank, repeating from the scroll.
static void test_config_version(void)
{
    int k;

    printf("config: page 7 lamp test and version\n");
    reset_world();
    chord_long();
    CHECK(lamp == HAL_LAMP_NORMAL && dots == 0);

    for (k = 0; k < 6; k++)
    {
        tap(CTL_KEY_UP);
    }

    CHECK(mode == MODE_CONFIG && cfg_page == PAGE_VERSION && status_led == 1);

    for (k = 0; k < 7; k++)
    {
        CHECK(aux[k] == 0);
    }

    // Every footswitch LED lit through the lead-in (the end of segment 10); switch 1's
    // then flashes for the first frame.
    CHECK(stomp_leds_but(-1));

    // At once: LEDs lit and the scroll, each frame half a version part's time.
    CHECK(VER_STEP_TICKS * TICK_MS == 100 && VER_ITEM_STEPS * VER_STEP_TICKS * TICK_MS == VERSION_SHOW_MS);
    CHECK(VER_SCROLL_STEPS * VER_STEP_MS == SCROLL_FRAME_MS && HAL_TEXT_SCROLL_FRAMES == 6);
    clear_log();
    expect_lead();
    expect_scroll();
    CHECK(lamp == HAL_LAMP_LEDS_STOMPS && display == SHOWN_HEX(fw_version[0]) && dots == 0x01);

    for (k = 0; k < VER_STEP_TICKS * VER_ITEM_STEPS - 1; k++)
    {
        ticks(1);
        CHECK(lamp == HAL_LAMP_LEDS_STOMPS && display == SHOWN_HEX(fw_version[0]) && dots == 0x01);
    }

    ticks(1);
    CHECK(display == SHOWN_HEX(fw_version[1]) && dots == 0x05);
    ticks(VER_STEP_TICKS * VER_ITEM_STEPS);
    CHECK(display == SHOWN_HEX(fw_version[2]) && dots == 0x04 && lamp == HAL_LAMP_LEDS_STOMPS);
    // Then a blank display (no dots, LEDs still lit) for one part's time, then major.
    ticks(VER_STEP_TICKS * VER_ITEM_STEPS);
    CHECK(display == 0 && blank == 7 && dots == 0 && lamp == HAL_LAMP_LEDS_STOMPS);
    ticks(VER_STEP_TICKS * VER_ITEM_STEPS - 1);
    CHECK(display == 0 && blank == 7 && dots == 0);
    // Then the lead-in, the scroll again, then major.
    ticks(1);
    expect_lead();
    expect_scroll();
    CHECK(display == SHOWN_HEX(fw_version[0]) && dots == 0x01 && lamp == HAL_LAMP_LEDS_STOMPS);
    CHECK(midi_len == 0);

    // UP on the last page does nothing; a footswitch runs it all again, sending nothing.
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_VERSION && display == SHOWN_HEX(fw_version[0]));
    ticks(VER_STEP_TICKS * VER_ITEM_STEPS);
    CHECK(display == SHOWN_HEX(fw_version[1]));
    // Switch 1 goes back to the scroll's first frame (test_config_version_jump has more).
    tap(sw_code(1));
    CHECK(lamp == HAL_LAMP_LEDS_STOMPS && display == SHOWN_TEXT(HAL_TEXT_SCROLL) && dots == scroll_dot_of[0]);
    CHECK(stomp_leds_but(0) && midi_len == 0);
    // It keeps repeating while the page is open, LEDs lit all the time.
    for (k = 0; k < 3; k++)
    {
        ticks(VER_STEP_TICKS * VER_END_STEP);
        CHECK(lamp == HAL_LAMP_LEDS_STOMPS && display == SHOWN_TEXT(HAL_TEXT_SCROLL) && dots == scroll_dot_of[0]);
    }
    ticks(VER_STEP_TICKS * (VER_SCROLL_END + VER_ITEM_STEPS * 2));
    CHECK(display == SHOWN_HEX(fw_version[2]) && lamp == HAL_LAMP_LEDS_STOMPS);

    // Pedals and a short UP + DOWN do nothing here.
    pedal(0, 10);
    pedal(0, 200);
    chord_short();
    CHECK(mode == MODE_CONFIG && cfg_page == PAGE_VERSION && midi_len == 0);
    CHECK(sw_thr[0] == 0 && sw_thr[1] == 0 && sw_thr[2] == 0 && sw_thr[3] == 0 && sw_mask[1] == 0);

    // DOWN during the lamp test: page 6, LEDs back to normal, and no more steps.
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_SW2 && lamp == HAL_LAMP_NORMAL && dots == 0 && aux[HAL_LED_SWITCH_2] == 1);
    ticks(VER_STEP_TICKS * 20);
    CHECK(lamp == HAL_LAMP_NORMAL && blank == 7);

    // Leaving from page 7 (long UP, or a long UP + DOWN) ends the lamp test and dots.
    tap(CTL_KEY_UP);
    CHECK(lamp == HAL_LAMP_LEDS_STOMPS);
    save_exit();
    CHECK(mode == MODE_RUN && lamp == HAL_LAMP_NORMAL && dots == 0 && display == 0);
    ticks(VER_STEP_TICKS * 2);
    CHECK(lamp == HAL_LAMP_NORMAL);
    chord_long();
    for (k = 0; k < 6; k++)
    {
        tap(CTL_KEY_UP);
    }
    ticks(VER_STEP_TICKS * (VER_LEAD_FRAMES * VER_SCROLL_STEPS + VER_SCROLL_END) + 1);
    CHECK(display == SHOWN_HEX(fw_version[0]) && dots == 0x01);
    chord_long();
    CHECK(mode == MODE_RUN && lamp == HAL_LAMP_NORMAL && dots == 0 && display == 0);
}

// Page 7's ten segments, one per footswitch: the scroll frames " r" to "n " (switches
// 1-6), major, minor, build (7-9) and the blank (10). Each segment flashes its footswitch
// LED dark then lit; pressing a footswitch shows its segment and holds it until release,
// then it carries on from there. Nothing is sent and no setting changes.
static void test_config_version_jump(void)
{
    int k;

    printf("config: page 7 footswitch LEDs mark the segments; a footswitch holds its segment\n");
    reset_world();
    chord_long();

    for (k = 0; k < 6; k++)
    {
        tap(CTL_KEY_UP);
    }

    clear_log();
    CHECK(cfg_page == PAGE_VERSION && cfg_changed == 0 && stomp_leds_but(-1));
    // It opens at the lead-in (the end of segment 10), then segment 1.
    CHECK(display == 0 && blank == 7 && dots == 0x01);
    ticks(VER_STEP_TICKS * VER_LEAD_FRAMES * VER_SCROLL_STEPS);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_SCROLL) && dots == scroll_dot_of[0] && stomp_leds_but(0));

    // Each segment's LED: dark for VER_FLASH_STEPS from its start, then lit.
    for (k = 0; k < STOMP_COUNT; k++)
    {
        // " r" to "An" one frame; "n " and its tail; major, minor, build; blank and lead-in.
        int len = (k < HAL_TEXT_SCROLL_FRAMES - 1) ? VER_SCROLL_STEPS :
                  (k == HAL_TEXT_SCROLL_FRAMES - 1) ? (1 + VER_TAIL_FRAMES) * VER_SCROLL_STEPS :
                  (k < STOMP_COUNT - 1) ? VER_ITEM_STEPS : VER_ITEM_STEPS + VER_LEAD_FRAMES * VER_SCROLL_STEPS;

        CHECK(stomp_leds_but(k));
        ticks(VER_STEP_TICKS * VER_FLASH_STEPS - 1);
        CHECK(stomp_leds_but(k));
        ticks(1);
        CHECK(stomp_leds_but(-1));
        ticks(VER_STEP_TICKS * (len - VER_FLASH_STEPS));
    }

    CHECK(stomp_leds_but(0) && display == SHOWN_TEXT(HAL_TEXT_SCROLL));

    // Switch 8 (minor) held: minor shows with its dots, LED 8 dark, for as long as it is
    // down (two whole cycles here).
    ticks(VER_STEP_TICKS * 3);
    ctl_key_press(sw_code(8));
    CHECK(display == SHOWN_HEX(fw_version[1]) && dots == 0x05 && stomp_leds_but(7) && ver_paused == 1);
    ticks(VER_STEP_TICKS * VER_END_STEP * 2);
    CHECK(display == SHOWN_HEX(fw_version[1]) && dots == 0x05 && stomp_leds_but(7));
    CHECK(lamp == HAL_LAMP_LEDS_STOMPS && midi_len == 0);
    // Released: it carries on from the start of that segment (LED lights after the flash).
    ctl_key_release(sw_code(8));
    CHECK(ver_paused == 0);
    ticks(VER_STEP_TICKS * VER_FLASH_STEPS - 1);
    CHECK(stomp_leds_but(7) && display == SHOWN_HEX(fw_version[1]));
    ticks(1);
    CHECK(stomp_leds_but(-1));
    ticks(VER_STEP_TICKS * (VER_ITEM_STEPS - VER_FLASH_STEPS));
    CHECK(display == SHOWN_HEX(fw_version[2]) && dots == 0x04 && stomp_leds_but(8));

    // Switch 1 from the build: " r", no dots, LED 1 dark, the others lit again.
    ctl_key_press(sw_code(1));
    CHECK(display == SHOWN_TEXT(HAL_TEXT_SCROLL) && dots == scroll_dot_of[0] && stomp_leds_but(0));
    ticks(VER_STEP_TICKS * VER_END_STEP);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_SCROLL) && stomp_leds_but(0));
    ctl_key_release(sw_code(1));
    ticks(VER_STEP_TICKS * VER_SCROLL_STEPS);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_SCROLL + 1) && stomp_leds_but(1));

    // Every switch shows its own segment.
    for (k = 1; k <= STOMP_COUNT; k++)
    {
        ctl_key_press(sw_code(k));

        if (k <= HAL_TEXT_SCROLL_FRAMES)
        {
            CHECK(display == SHOWN_TEXT(HAL_TEXT_SCROLL + k - 1) && dots == scroll_dot_of[k - 1]);
        }
        else if (k < STOMP_COUNT)
        {
            CHECK(display == SHOWN_HEX(fw_version[k - 7]) && dots == fw_version_dots[k - 7]);
        }
        else
        {
            CHECK(display == 0 && blank == 7 && dots == 0);
        }

        CHECK(stomp_leds_but(k - 1));
        ctl_key_release(sw_code(k));
    }

    // After switch 10 (the blank), the lead-in, then the scroll.
    ticks(VER_STEP_TICKS * VER_ITEM_STEPS);
    CHECK(display == 0 && blank == 7 && dots == 0x01);
    ticks(VER_STEP_TICKS * VER_LEAD_FRAMES * VER_SCROLL_STEPS);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_SCROLL) && stomp_leds_but(0));
    CHECK(midi_len == 0 && cfg_changed == 0 && mode == MODE_CONFIG && cfg_page == PAGE_VERSION);

    // DOWN while a switch is held leaves page 7; the release then does nothing.
    ctl_key_press(sw_code(5));
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_SW2 && ver_paused == 0 && lamp == HAL_LAMP_NORMAL);
    ctl_key_release(sw_code(5));
    CHECK(cfg_page == PAGE_SW2 && sw_mask[1] == 0 && midi_len == 0);

    // Leaving config mode while a switch is held: run mode works as usual afterwards.
    tap(CTL_KEY_UP);
    ctl_key_press(sw_code(2));
    CHECK(ver_paused == 1);
    cancel_exit();
    CHECK(mode == MODE_RUN && lamp == HAL_LAMP_NORMAL);
    ctl_key_release(sw_code(2));
    set_mode(0, PM_STOMP);
    clear_log();
    tap(sw_code(1));
    CHECK(midi_len == 3 && cc_number(0) == stomp_cc[0] && cc_value(0) == 127 && leds[0] == 1);
}

// A short UP + DOWN press in run mode toggles bank mode and the DIRECT SELECT LED. It
// sends nothing, changes no program, and config mode leaves it alone.
static void test_bank_mode_toggle(void)
{
    printf("run mode: UP + DOWN tap toggles bank mode\n");
    reset_world();
    CHECK(aux[HAL_LED_DIRECT_SELECT] == 0);
    clear_log();
    chord_short();
    CHECK(mode == MODE_RUN && midi_len == 0 && bank_mode == 1);
    // The display shows the bank (bank 1) while DIRECT SELECT is lit.
    CHECK(aux[HAL_LED_DIRECT_SELECT] == 1 && display == 1 && program == 0);
    chord_short();
    CHECK(midi_len == 0 && bank_mode == 0 && aux[HAL_LED_DIRECT_SELECT] == 0 && display == 0);

    // The second key pressed late still makes a chord; nothing is stepped.
    ctl_key_press(CTL_KEY_DOWN);
    ticks(10);
    ctl_key_press(CTL_KEY_UP);
    ticks(10);
    ctl_key_release(CTL_KEY_DOWN);
    ctl_key_release(CTL_KEY_UP);
    CHECK(midi_len == 0 && bank_mode == 1 && program == 0);

    // It does not wait for MIDI OUT room, since it sends nothing.
    midi_space = 0;
    chord_short();
    CHECK(bank_mode == 0);
    chord_short();
    CHECK(bank_mode == 1);
    midi_space = 15;

    // Opening and leaving config mode sends nothing. DIRECT SELECT is dark while
    // editing and shows the kept state again on leaving (save or cancel). An UP + DOWN
    // tap in config mode does not toggle it either.
    clear_log();
    chord_long();
    CHECK(mode == MODE_CONFIG && aux[HAL_LED_DIRECT_SELECT] == 0 && bank_mode == 1);
    chord_short();
    CHECK(mode == MODE_CONFIG && bank_mode == 1 && aux[HAL_LED_DIRECT_SELECT] == 0);
    tap(CTL_KEY_UP);
    CHECK(aux[HAL_LED_DIRECT_SELECT] == 0);
    save_exit();
    CHECK(mode == MODE_RUN && aux[HAL_LED_DIRECT_SELECT] == 1);
    chord_long();
    cancel_exit();
    CHECK(mode == MODE_RUN && aux[HAL_LED_DIRECT_SELECT] == 1);
    CHECK(midi_len == 0);

    // With bank mode off, the LED stays dark after leaving.
    chord_short();
    CHECK(bank_mode == 0 && aux[HAL_LED_DIRECT_SELECT] == 0);
    chord_long();
    save_exit();
    CHECK(aux[HAL_LED_DIRECT_SELECT] == 0);
    // Back on for the checks below.
    chord_short();
    CHECK(bank_mode == 1 && aux[HAL_LED_DIRECT_SELECT] == 1);

    // A power-on starts with it off.
    ctl_init();
    CHECK(aux[HAL_LED_DIRECT_SELECT] == 0 && bank_mode == 0);
}

// Open a SWITCH page: 1 (page 5) or 2 (page 6).
static void goto_switch_page(int sw)
{
    int i;

    chord_long();

    for (i = 0; i < 3 + sw; i++)
    {
        tap(CTL_KEY_UP);
    }
}

// SWITCH 1 / SWITCH 2 relays: configuration pages, OR of the triggers, LEDs, saving.
static void test_relays(void)
{
    uint8_t v;
    int i;

    printf("relays: SWITCH 1 / SWITCH 2 triggers\n");
    reset_world();
    CHECK(relays == 0 && aux[HAL_LED_SWITCH_1] == 0 && aux[HAL_LED_SWITCH_2] == 0);
    // Toggle mode for the followed switches, whatever DEFAULT_PEDAL_MODE is.
    set_mode(2, PM_STOMP);
    set_mode(9, PM_STOMP);

    // No triggers by default: nothing turns a relay on.
    pedal(0, 255);
    pedal(1, 255);
    tap(2);
    chord_short();
    ticks(2);
    CHECK(relays == 0 && aux[HAL_LED_SWITCH_1] == 0);
    tap(2);
    chord_short();
    CHECK(leds[2] == 0 && bank_mode == 0);

    // Page 5: footswitch 3 and bank mode, all LEDs showing the settings.
    goto_switch_page(1);
    CHECK(cfg_page == PAGE_SW1 && aux[HAL_LED_SWITCH_1] == 1 && aux[HAL_LED_DIRECT_SELECT] == 0);
    for (i = 0; i < STOMP_COUNT; i++)
    {
        CHECK(leds[i] == 0);
    }
    clear_log();
    tap(2);
    CHECK(leds[2] == 1 && sw_mask[0] == 4 && midi_len == 0);
    tap(2);
    CHECK(leds[2] == 0 && sw_mask[0] == 0);
    tap(2);
    chord_short();
    CHECK(sw_mask[0] == (4 | SW_CC_BIT) && aux[HAL_LED_DIRECT_SELECT] == 1);
    CHECK(mode == MODE_CONFIG && cfg_page == PAGE_SW1 && bank_mode == 0 && midi_len == 0);

    // Pedal A sets the trigger value: the first reading is only a reference, small
    // wobble is ignored, a real move sets it, shows it and lights EXP 1.
    pedal(0, 100);
    CHECK(sw_thr[0] == 0 && blank == 7 && aux[HAL_LED_EXP_1] == 0);
    pedal(0, 101);
    CHECK(sw_thr[0] == 0);
    pedal(0, 160);
    v = ctl_scale(160, 0, 255);
    CHECK(sw_thr[0] == v && display == v && blank == 0 && aux[HAL_LED_EXP_1] == 1);
    CHECK(aux[HAL_LED_EXP_2] == 0 && midi_len == 0);

    // Page 6: footswitch 10 and pedal B; page 5's settings are not shown here.
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_SW2 && aux[HAL_LED_SWITCH_2] == 1 && aux[HAL_LED_SWITCH_1] == 0);
    CHECK(leds[2] == 0 && aux[HAL_LED_DIRECT_SELECT] == 0 && aux[HAL_LED_EXP_1] == 0 && blank == 7);
    tap(sw_code(10));
    CHECK(leds[9] == 1 && sw_mask[1] == 0x200);
    pedal(1, 255);
    pedal(1, 0);
    CHECK(sw_thr[3] == 0 && display == 0 && aux[HAL_LED_EXP_2] == 0);
    pedal(1, 64);
    CHECK(sw_thr[3] == ctl_scale(64, 0, 255) && aux[HAL_LED_EXP_2] == 1);

    // Back on page 5 its settings show again.
    tap(CTL_KEY_DOWN);
    CHECK(leds[2] == 1 && leds[9] == 0 && aux[HAL_LED_DIRECT_SELECT] == 1 && aux[HAL_LED_EXP_1] == 1);
    CHECK(aux[HAL_LED_EXP_2] == 0);

    // Saving stores both records: mask low, mask high (bit 2 = bank mode), A, B.
    save_exit();
    CHECK(sw_saved == 2);
    CHECK(saved_sw[0][0] == 4 && saved_sw[0][1] == 4 && saved_sw[0][2] == v && saved_sw[0][3] == 0);
    CHECK(saved_sw[1][0] == 0 && saved_sw[1][1] == 2 && saved_sw[1][2] == 0 && saved_sw[1][3] == ctl_scale(64, 0, 255));
    CHECK(aux[HAL_LED_SWITCH_1] == 0 && aux[HAL_LED_SWITCH_2] == 0 && aux[HAL_LED_DIRECT_SELECT] == 0);

    // Run mode, relay 1: footswitch 3 OR bank mode OR pedal A at v and up.
    pedal(0, 0);
    pedal(1, 0);
    ticks(1);
    CHECK(relays == 0 && aux[HAL_LED_SWITCH_1] == 0);
    tap(2);
    ticks(1);
    CHECK(relays == 1 && aux[HAL_LED_SWITCH_1] == 1 && aux[HAL_LED_SWITCH_2] == 0);
    chord_short();
    ticks(1);
    CHECK(relays == 1 && bank_mode == 1);
    tap(2);
    ticks(1);
    CHECK(relays == 1);
    chord_short();
    ticks(1);
    CHECK(relays == 0 && aux[HAL_LED_SWITCH_1] == 0);
    pedal(0, 255);
    ticks(1);
    CHECK(relays == 1 && sent[0] == 127);
    pedal(0, 0);
    ticks(1);
    CHECK(relays == 0);

    // Relay 2: footswitch 10 OR pedal B; both relays at once.
    tap(sw_code(10));
    ticks(1);
    CHECK(relays == 2 && aux[HAL_LED_SWITCH_2] == 1 && aux[HAL_LED_SWITCH_1] == 0);
    tap(2);
    ticks(1);
    CHECK(relays == 3);
    tap(sw_code(10));
    ticks(1);
    CHECK(relays == 1);
    pedal(1, 255);
    ticks(1);
    CHECK(relays == 3);

    // Config mode leaves the relays alone; its pages own the SWITCH LEDs, and leaving
    // shows the relays again.
    // The fake EEPROM now holds what was saved, so cancelling reloads it.
    memcpy(stored_sw, saved_sw, sizeof(stored_sw));
    sw_stored_valid[0] = 1;
    sw_stored_valid[1] = 1;
    chord_long();
    ticks(5);
    CHECK(relays == 3 && aux[HAL_LED_SWITCH_1] == 0 && aux[HAL_LED_SWITCH_2] == 0);
    cancel_exit();
    CHECK(aux[HAL_LED_SWITCH_1] == 1 && aux[HAL_LED_SWITCH_2] == 1);
    // Cancel reloaded the default pedal modes (nothing saved them): toggles again.
    // Only the mode changes here; the states stay as they are.
    pedal_mode_set(2, PM_STOMP);
    pedal_mode_set(9, PM_STOMP);
    // Right after leaving the pedals have no value yet; their triggers keep the relays.
    tap(2);
    ticks(1);
    CHECK(relays == 3);
    // Pedal B now has a value (0), below its trigger: relay 2 opens; relay 1 still waits.
    pedal(1, 0);
    ticks(1);
    CHECK(relays == 1);
    pedal(0, 0);
    ticks(1);
    CHECK(relays == 0 && aux[HAL_LED_SWITCH_1] == 0 && aux[HAL_LED_SWITCH_2] == 0);

    // Setting a pedal's value to 0 turns its trigger off.
    goto_switch_page(1);
    pedal(0, 255);
    pedal(0, 0);
    CHECK(sw_thr[0] == 0 && aux[HAL_LED_EXP_1] == 0);
    save_exit();
    pedal(0, 0);
    pedal(0, 255);
    ticks(1);
    CHECK(relays == 0 && saved_sw[0][2] == 0);

    // Cancel drops trigger changes.
    goto_switch_page(2);
    tap(0);
    CHECK(sw_mask[1] == 0x201);
    cancel_exit();
    CHECK(sw_mask[1] == 0x200 && sw_thr[3] == ctl_scale(64, 0, 255));

    // Stored records load at start-up; a value over 127 is off, unused mask bits ignored.
    reset_world();
    sw_stored_valid[1] = 1;
    stored_sw[1][0] = 0x01;
    stored_sw[1][1] = 0xFC;
    stored_sw[1][2] = 200;
    stored_sw[1][3] = 50;
    ctl_init();
    CHECK(sw_mask[1] == (0x01 | SW_CC_BIT) && sw_thr[2] == 0 && sw_thr[3] == 50);
    CHECK(sw_mask[0] == 0 && sw_thr[0] == 0 && sw_thr[1] == 0);
    set_mode(0, PM_STOMP);
    tap(0);
    ticks(1);
    CHECK(relays == 2 && aux[HAL_LED_SWITCH_2] == 1);
}

// Run mode: received MIDI flashes the MIDI CHAN LED for RX_FLASH_TICKS.
static void test_midi_flash(void)
{
    printf("MIDI IN flashes MIDI CHAN\n");
    reset_world();
    ctl_midi_in(0xF8);
    ctl_midi_in(0xFE);
    CHECK(aux[HAL_LED_MIDI_CHAN] == 0);
    ctl_midi_in(0x90);
    CHECK(aux[HAL_LED_MIDI_CHAN] == 1);
    ticks(RX_FLASH_TICKS - 1);
    CHECK(aux[HAL_LED_MIDI_CHAN] == 1);
    ticks(1);
    CHECK(aux[HAL_LED_MIDI_CHAN] == 0);

    // Each byte restarts it.
    ctl_midi_in(0xB0);
    ticks(RX_FLASH_TICKS - 5);
    ctl_midi_in(20);
    ticks(RX_FLASH_TICKS - 1);
    CHECK(aux[HAL_LED_MIDI_CHAN] == 1);
    ticks(1);
    CHECK(aux[HAL_LED_MIDI_CHAN] == 0);

    // Entering config mode during a flash: page 1 (modes) has it dark, page 2
    // (channel) lit and steady, and page 3 dark again.
    ctl_midi_in(0xB0);
    chord_long();
    ticks(RX_FLASH_TICKS + 1);
    CHECK(aux[HAL_LED_MIDI_CHAN] == 0);
    tap(CTL_KEY_UP);
    ticks(RX_FLASH_TICKS + 1);
    CHECK(aux[HAL_LED_MIDI_CHAN] == 1);
    tap(CTL_KEY_UP);
    CHECK(aux[HAL_LED_MIDI_CHAN] == 0);
    ctl_midi_in(0xB0);
    ticks(2);
    CHECK(aux[HAL_LED_MIDI_CHAN] == 0);
    save_exit();
    CHECK(aux[HAL_LED_MIDI_CHAN] == 0);
    ctl_midi_in(0xC0);
    CHECK(aux[HAL_LED_MIDI_CHAN] == 1);
}

static void test_config_calibration(void)
{
    printf("config: calibration\n");
    reset_world();
    chord_long();
    tap(CTL_KEY_UP);
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_HEEL && aux[HAL_LED_EXP_1] == 1 && aux[HAL_LED_EXP_2] == 1);
    CHECK(aux[HAL_LED_MIDI_FUNC] == 0 && aux[HAL_LED_MIDI_CHAN] == 0);
    CHECK(display == SHOWN_LO);

    // Page 3: a footswitch takes the heel reading; page 4: the toe reading.
    pedal(0, 20);
    pedal(1, 30);
    tap(sw_code(1));
    CHECK(display == CAL_DONE && heel[0] == 0);
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_TOE && display == SHOWN_HI);
    pedal(0, 220);
    pedal(1, 230);
    tap(sw_code(5));
    CHECK(display == CAL_DONE);
    CHECK(heel[0] == 20 && heel[1] == 30 && toe[0] == 220 && toe[1] == 230);

    // Coming back to a page shows its step again; nothing is sent.
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_HEEL && display == SHOWN_LO);
    tap(CTL_KEY_UP);
    CHECK(display == SHOWN_HI && midi_len == 0);

    save_exit();
    CHECK(cal_saved == 1);
    CHECK(saved_heel[0] == 20 && saved_heel[1] == 30 && saved_toe[0] == 220 && saved_toe[1] == 230);

    // The new range is in use, and the first reading is adopted silently.
    clear_log();
    pedal(0, 120);
    CHECK(midi_len == 0);
    pedal(0, 20);
    CHECK(midi_len == 3 && cc_number(0) == 7 && cc_value(0) == 0);
    pedal(0, 220);
    CHECK(cc_value(1) == 127);
    pedal(1, 30);
    pedal(1, 130);
    CHECK(cc_number(2) == 11 && cc_value(2) == ctl_scale(130, 30, 230));

    // A pedal that did not move enough shows "Er" and changes nothing; pressing again
    // on page 4 retakes the toe reading against the same heel.
    reset_world();
    chord_long();
    tap(CTL_KEY_UP);
    tap(CTL_KEY_UP);
    pedal(0, 100);
    pedal(1, 100);
    tap(0);
    tap(CTL_KEY_UP);
    pedal(0, 100 + CAL_MIN_SPAN - 1);
    pedal(1, 200);
    tap(0);
    CHECK(display == SHOWN_ER && cal_step == CAL_ERROR);
    CHECK(heel[0] == 0 && toe[0] == 255);
    pedal(0, 200);
    tap(0);
    CHECK(display == CAL_DONE && heel[0] == 100 && heel[1] == 100 && toe[0] == 200 && toe[1] == 200);

    // Only one pedal moving is also an error.
    reset_world();
    chord_long();
    tap(CTL_KEY_UP);
    tap(CTL_KEY_UP);
    pedal(0, 10);
    pedal(1, 10);
    tap(0);
    tap(CTL_KEY_UP);
    pedal(0, 200);
    pedal(1, 12);
    tap(0);
    CHECK(display == SHOWN_ER);
    save_exit();
    CHECK(saved_heel[0] == 0 && saved_toe[0] == 255);

    // Page 4 without page 3 checks the toe against the current heel (UP from page 3
    // without a reading there).
    reset_world();
    chord_long();
    tap(CTL_KEY_UP);
    tap(CTL_KEY_UP);
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_TOE && display == SHOWN_HI);
    pedal(0, 200);
    pedal(1, 210);
    tap(0);
    CHECK(display == CAL_DONE && heel[0] == 0 && heel[1] == 0 && toe[0] == 200 && toe[1] == 210);
    pedal(0, 20);
    tap(0);
    CHECK(display == SHOWN_ER && toe[0] == 200);

    // A heel reading without a toe reading is discarded.
    reset_world();
    chord_long();
    tap(CTL_KEY_UP);
    tap(CTL_KEY_UP);
    pedal(0, 50);
    pedal(1, 50);
    tap(0);
    save_exit();
    CHECK(saved_heel[0] == 0 && saved_toe[0] == 255);

    // A wired-the-other-way pedal (toe below heel) is accepted.
    reset_world();
    chord_long();
    tap(CTL_KEY_UP);
    tap(CTL_KEY_UP);
    pedal(0, 200);
    pedal(1, 200);
    tap(0);
    tap(CTL_KEY_UP);
    pedal(0, 20);
    pedal(1, 20);
    tap(0);
    CHECK(display == CAL_DONE);
    save_exit();
    CHECK(saved_heel[0] == 200 && saved_toe[0] == 20);

    // Stored calibration is loaded at start-up.
    reset_world();
    cal_stored_valid = 1;
    stored_heel[0] = 40; stored_heel[1] = 50;
    stored_toe[0]  = 140; stored_toe[1] = 150;
    ctl_init();
    clear_log();
    pedal(0, 40);
    pedal(0, 140);
    CHECK(midi_len == 3 && cc_value(0) == 127);
}

static void test_midi_in(void)
{
    const uint8_t cc[10] = { STOMP_CC_LIST };
    int i;

    printf("MIDI IN\n");
    reset_world();
    // Toggle mode, so this does not depend on DEFAULT_PEDAL_MODE.
    set_mode(0, PM_STOMP);

    // Value >= 64 sets on, below sets off, no MIDI is echoed.
    for (i = 0; i < 10; i++)
    {
        ctl_midi_in(CC_STATUS);
        ctl_midi_in(cc[i]);
        ctl_midi_in(127);
        CHECK(leds[i] == 1);
    }
    CHECK(midi_len == 0);

    ctl_midi_in(CC_STATUS); ctl_midi_in(cc[0]); ctl_midi_in(63);
    CHECK(leds[0] == 0);
    ctl_midi_in(CC_STATUS); ctl_midi_in(cc[0]); ctl_midi_in(64);
    CHECK(leds[0] == 1);

    // A toggle after a remote change starts from the remote state.
    clear_log();
    tap(0);
    CHECK(cc_value(0) == 0 && leds[0] == 0);

    // A CC 20 (the old UP + DOWN controller number) is no longer special: it does not
    // touch bank mode.
    clear_log();
    ctl_midi_in(CC_STATUS); ctl_midi_in(20); ctl_midi_in(127);
    CHECK(bank_mode == 0 && aux[HAL_LED_DIRECT_SELECT] == 0 && midi_len == 0);

    // Running status: One status byte, two pairs.
    reset_world();
    ctl_midi_in(CC_STATUS);
    ctl_midi_in(cc[2]); ctl_midi_in(127);
    ctl_midi_in(cc[3]); ctl_midi_in(127);
    CHECK(leds[2] == 1 && leds[3] == 1);

    // Other channels, other messages, other CC numbers and SysEx are ignored.
    reset_world();
    ctl_midi_in((uint8_t)(0xB0 | (MIDI_CHANNEL % 16))); ctl_midi_in(cc[0]); ctl_midi_in(127);
    ctl_midi_in((uint8_t)(0x90 | (MIDI_CHANNEL - 1))); ctl_midi_in(cc[0]); ctl_midi_in(127);
    ctl_midi_in(CC_STATUS); ctl_midi_in(99); ctl_midi_in(127);
    ctl_midi_in(0xF0); ctl_midi_in(cc[0]); ctl_midi_in(127); ctl_midi_in(0xF7);
    CHECK(leds[0] == BOOT_LED);

    // Real-time bytes in the middle of a message do not break it.
    ctl_midi_in(CC_STATUS); ctl_midi_in(cc[1]); ctl_midi_in(0xF8); ctl_midi_in(127);
    CHECK(leds[1] == 1);

    // Remote changes while in config mode are ignored so the LEDs keep showing settings.
    reset_world();
    chord_long();
    ctl_midi_in(CC_STATUS); ctl_midi_in(cc[5]); ctl_midi_in(127);
    chord_up();
    save_exit();
    CHECK(leds[5] == BOOT_LED && mode == MODE_RUN);
}

static void test_exit_restores_leds(void)
{
    printf("LEDs after config\n");
    reset_world();
    // Toggle mode, so this does not depend on DEFAULT_PEDAL_MODE.
    set_mode(0, PM_STOMP);
    set_mode(3, PM_STOMP);
    tap(0);
    tap(3);
    chord_long();
    tap(CTL_KEY_UP);
    save_exit();
    CHECK(leds[0] == 1 && leds[3] == 1 && leds[1] == BOOT_LED);
    CHECK(status_led == 0);
}

// The CONFIG LED stays lit in config mode until a setting changes, then flashes at
// CFG_FLASH_TICKS per half-period until save or cancel.
static int status_toggles(int n)
{
    int k, flips = 0;
    uint8_t was = status_led;

    for (k = 0; k < n; k++)
    {
        ctl_tick();

        if (status_led != was)
        {
            flips++;
            was = status_led;
        }
    }

    return flips;
}

// Key code of the footswitch for channel 1-10 on the channel page.
static uint8_t chan_key(uint8_t ch)
{
    return (uint8_t)((ch <= 5) ? ch - 1 : ch);
}

// The CONFIG LED compares against the EEPROM, read the way start-up reads it.
static void test_config_led_eeprom(void)
{
    printf("config: CONFIG LED compares the settings with the EEPROM\n");
    // Stored channel 5: steady at entry; another channel flashes; back to 5 is steady.
    reset_world();
    cfg_stored_valid = 1;
    stored_cfg[0] = 5;
    stored_cfg[1] = (uint8_t)(DEFAULT_PEDAL_MODE * 0x55);
    stored_cfg[2] = (uint8_t)(DEFAULT_PEDAL_MODE * 0x55);
    stored_cfg[3] = (uint8_t)(DEFAULT_PEDAL_MODE * 0x05);
    ctl_init();
    CHECK(midi_channel == 5);
    chord_long();
    CHECK(status_led == 1 && status_toggles(CFG_FLASH_TICKS * 3) == 0);
    tap(CTL_KEY_UP);
    tap(chan_key(6));
    CHECK(midi_channel == 6 && status_led == 0 && status_toggles(CFG_FLASH_TICKS * 2) == 2);
    tap(chan_key(5));
    CHECK(midi_channel == 5 && status_led == 1 && status_toggles(CFG_FLASH_TICKS * 3) == 0);
    cancel_exit();

    // Invalid records count as the defaults, as at start-up: a stored channel of 0xEE
    // and stored trigger values over 127 match the default channel and no trigger.
    reset_world();
    cfg_stored_valid = 1;
    stored_cfg[0] = 0xEE;
    stored_cfg[1] = (uint8_t)(DEFAULT_PEDAL_MODE * 0x55);
    stored_cfg[2] = (uint8_t)(DEFAULT_PEDAL_MODE * 0x55);
    stored_cfg[3] = (uint8_t)(DEFAULT_PEDAL_MODE * 0x05 | 0xF0);
    sw_stored_valid[1] = 1;
    stored_sw[1][0] = 0;
    stored_sw[1][1] = 0xF8;
    stored_sw[1][2] = 200;
    stored_sw[1][3] = 128;
    ctl_init();
    CHECK(midi_channel == MIDI_CHANNEL && sw_thr[2] == 0 && sw_thr[3] == 0 && sw_mask[1] == 0);
    chord_long();
    CHECK(status_led == 1 && status_toggles(CFG_FLASH_TICKS * 3) == 0);
    cancel_exit();

    // Stored calibration: a new one flashes; storing the same values again does not.
    reset_world();
    cal_stored_valid = 1;
    stored_heel[0] = 10;
    stored_heel[1] = 20;
    stored_toe[0]  = 200;
    stored_toe[1]  = 210;
    ctl_init();
    CHECK(heel[0] == 10 && toe[1] == 210);
    chord_long();
    CHECK(status_led == 1);
    tap(CTL_KEY_UP);
    tap(CTL_KEY_UP);
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_TOE);
    pedal(0, 200);
    pedal(1, 210);
    tap(0);
    CHECK(status_led == 1 && status_toggles(CFG_FLASH_TICKS * 3) == 0);
    pedal(0, 230);
    pedal(1, 240);
    tap(0);
    CHECK(toe[0] == 230 && status_led == 0 && status_toggles(CFG_FLASH_TICKS * 2) == 2);
    cancel_exit();

    // A save that did not reach the EEPROM (the fake keeps saves apart from what it
    // loads, like a failed write): the next visit flashes at once, with no edits.
    reset_world();
    chord_long();
    tap(CTL_KEY_UP);
    tap(chan_key(MIDI_CHANNEL == 7 ? 8 : 7));
    save_exit();
    CHECK(mode == MODE_RUN && status_led == 0 && cfg_saved > 0);
    chord_long();
    CHECK(mode == MODE_CONFIG && status_led == 0 && status_toggles(CFG_FLASH_TICKS * 2) == 2);
    // Cancel reloads from the EEPROM; then the next visit is steady.
    cancel_exit();
    CHECK(midi_channel == MIDI_CHANNEL);
    chord_long();
    CHECK(status_led == 1 && status_toggles(CFG_FLASH_TICKS * 3) == 0);
    cancel_exit();
}

// Power-on: every LED lights once and stays lit while "riban" scrolls once (no version),
// then run mode with the program number. Nothing is sent. A key ends it early.
static void test_boot_animation(void)
{
    printf("start-up: riban scroll, then run mode\n");
    reset_world();
    clear_log();
    ctl_init();
    CHECK(boot_anim == 1 && mode == MODE_RUN);
    // The lead-in, every frame for its full time, then (on the last tick of the trailing
    // dot) run mode. The first frame is short by the start-up settling (STARTUP_TICKS,
    // during which main() does not call ctl_tick; the tests start straight away), so with
    // it power-on takes ten frames: 10 x SCROLL_FRAME_MS.
    {
        int k, n = 0;

        for (k = 0; k < VER_STEP_TICKS * VER_SCROLL_STEPS - STARTUP_TICKS; k++)
        {
            CHECK(display == 0 && blank == 7 && dots == 0x01 && lamp == HAL_LAMP_LEDS_ON);
            ticks(1);
        }

        CHECK(dots == 0x02);
        ctl_init();

        while (boot_anim && n < 10000)
        {
            ticks(1);
            n++;
        }

        CHECK((n + STARTUP_TICKS) * TICK_MS == 10 * SCROLL_FRAME_MS);
        ctl_init();
        ticks(VER_STEP_TICKS * VER_SCROLL_STEPS - STARTUP_TICKS);
    }
    expect_frame(-1, 0x02, -1);
    expect_scroll();
    // Then run mode (the version never shows): normal LEDs, no dots, program 0, steps
    // back to their idle values.
    CHECK(boot_anim == 0 && lamp == HAL_LAMP_NORMAL && dots == 0 && display == program + DISPLAY_PROGRAM_OFFSET);
    CHECK(cal_step == CAL_DONE && chan_flash_ticks == 0 && mode == MODE_RUN && midi_len == 0);
    ticks(VER_STEP_TICKS * VER_END_STEP);
    CHECK(lamp == HAL_LAMP_NORMAL && display == program + DISPLAY_PROGRAM_OFFSET && midi_len == 0);

    // A footswitch during the animation ends it and then works as usual.
    reset_world();
    set_mode(0, PM_STOMP);
    ctl_init();
    set_mode(0, PM_STOMP);
    clear_log();
    ticks(VER_STEP_TICKS * ((VER_LEAD_FRAMES + 1) * VER_SCROLL_STEPS + 1) - STARTUP_TICKS);
    CHECK(boot_anim == 1 && lamp == HAL_LAMP_LEDS_ON && display == SHOWN_TEXT(HAL_TEXT_SCROLL + 1) && dots == scroll_dot_of[1]);
    tap(0);
    CHECK(boot_anim == 0 && lamp == HAL_LAMP_NORMAL && dots == 0 && display == program + DISPLAY_PROGRAM_OFFSET);
    CHECK(midi_len == 3 && cc_number(0) == stomp_cc[0] && cc_value(0) == 127 && leds[0] == 1);
    ticks(VER_STEP_TICKS * VER_END_STEP);
    CHECK(lamp == HAL_LAMP_NORMAL && midi_len == 3);

    // UP during the animation: it ends, and the release steps the program as usual.
    reset_world();
    ctl_init();
    clear_log();
    ticks(VER_STEP_TICKS);
    tap(CTL_KEY_UP);
    CHECK(boot_anim == 0 && program == 1 && midi_len == 2 && display == 1 + DISPLAY_PROGRAM_OFFSET);

    // MIDI IN during the animation sets the stomp state under it (shown afterwards).
    reset_world();
    set_mode(0, PM_STOMP);
    ctl_init();
    set_mode(0, PM_STOMP);
    ctl_midi_in((uint8_t)(0xB0 | (MIDI_CHANNEL - 1)));
    ctl_midi_in(stomp_cc[0]);
    ctl_midi_in(127);
    CHECK(boot_anim == 1 && leds[0] == 1 && lamp == HAL_LAMP_LEDS_ON);
    ticks(VER_STEP_TICKS * (VER_LEAD_FRAMES * VER_SCROLL_STEPS + VER_BOOT_END_STEP) - STARTUP_TICKS);
    CHECK(boot_anim == 0 && lamp == HAL_LAMP_NORMAL && leds[0] == 1);
}

static void test_config_led_flash(void)
{
    printf("config: CONFIG LED flashes after a change\n");
    reset_world();
    chord_long();
    CHECK(mode == MODE_CONFIG && status_led == 1);
    // Selecting a switch (not the selected one) and changing pages are not changes.
    tap(1);
    tap(CTL_KEY_UP);
    tap(CTL_KEY_DOWN);
    CHECK(status_toggles(CFG_FLASH_TICKS * 4) == 0 && status_led == 1);
    // Cycling switch 2's mode is a change: dark at once, then toggling.
    tap(1);
    CHECK(status_led == 0);
    CHECK(status_toggles(CFG_FLASH_TICKS * 4) == 4);
    // Exact timing: the half-period is CFG_FLASH_TICKS ticks.
    while (status_led != 0) ctl_tick();
    CHECK(status_toggles(CFG_FLASH_TICKS - 1) == 0);
    CHECK(status_toggles(1) == 1 && status_led == 1);
    // Cycling the mode back to where it was (four modes) stops the flash, steady lit,
    // even from the dark half; another change starts it again.
    tap(1);
    tap(1);
    while (status_led != 0) ctl_tick();
    tap(1);
    CHECK(status_led == 1 && status_toggles(CFG_FLASH_TICKS * 4) == 0 && cfg_changed == 0);
    tap(1);
    CHECK(status_led == 0 && status_toggles(CFG_FLASH_TICKS * 2) == 2);
    tap(1);
    tap(1);
    tap(1);
    CHECK(status_led == 1 && status_toggles(CFG_FLASH_TICKS * 2) == 0);
    tap(1);
    // Cancel stops it; the LED is off in run mode and stays off.
    cancel_exit();
    CHECK(mode == MODE_RUN && status_led == 0 && status_toggles(CFG_FLASH_TICKS * 3) == 0);
    // Next entry is steady again.
    chord_long();
    CHECK(status_led == 1 && status_toggles(CFG_FLASH_TICKS * 3) == 0);
    // Channel page: pressing the channel already set is not a change, a new one is.
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_CHANNEL);
    tap((uint8_t)(MIDI_CHANNEL - 1 < 5 ? MIDI_CHANNEL - 1 : MIDI_CHANNEL));
    CHECK(status_led == 1 && status_toggles(CFG_FLASH_TICKS * 2) == 0);
    tap(MIDI_CHANNEL == 3 ? 3 : 2);
    CHECK(status_led == 0 && status_toggles(CFG_FLASH_TICKS * 2) == 2);
    // Back to the entry channel: steady again; then a new channel flashes once more.
    tap((uint8_t)(MIDI_CHANNEL - 1 < 5 ? MIDI_CHANNEL - 1 : MIDI_CHANNEL));
    CHECK(status_led == 1 && status_toggles(CFG_FLASH_TICKS * 2) == 0);
    tap(MIDI_CHANNEL == 3 ? 3 : 2);
    CHECK(status_led == 0);
    // Saving stops it too.
    save_exit();
    CHECK(mode == MODE_RUN && status_led == 0 && status_toggles(CFG_FLASH_TICKS * 3) == 0);
    // A SWITCH page footswitch toggle is a change.
    reset_world();
    goto_switch_page(1);
    CHECK(status_led == 1);
    tap(0);
    CHECK(status_led == 0 && status_toggles(CFG_FLASH_TICKS * 2) == 2);
    // Toggling the same switch back is no change; so is a relay trigger set and reset.
    tap(0);
    CHECK(status_led == 1 && status_toggles(CFG_FLASH_TICKS * 2) == 0);
    pedal(0, 0);
    CHECK(status_led == 1);
    pedal(0, 255);
    CHECK(sw_thr[0] == 127 && status_led == 0);
    pedal(0, 0);
    CHECK(sw_thr[0] == 0 && status_led == 1 && status_toggles(CFG_FLASH_TICKS * 2) == 0);
    cancel_exit();
    // MIDI IN in run mode still flashes MIDI CHAN and leaves CONFIG dark.
    ctl_midi_in(0xFE);
    ctl_midi_in(0x90);
    CHECK(aux[HAL_LED_MIDI_CHAN] == 1 && status_led == 0);
    ticks(RX_FLASH_TICKS);
    CHECK(aux[HAL_LED_MIDI_CHAN] == 0 && status_led == 0);

    // Page 7 with an unsaved change: the CONFIG LED flashes through the version display
    // (the HAL keeps it out of the lamp test's LEDs-on mode); with none it stays lit.
    {
    int k;

    reset_world();
    chord_long();
    tap(1);
    tap(1);
    for (k = 0; k < 6; k++)
    {
        tap(CTL_KEY_UP);
    }
    CHECK(cfg_page == PAGE_VERSION && cfg_changed == 1);
    CHECK(lamp == HAL_LAMP_LEDS_STOMPS && status_toggles(CFG_FLASH_TICKS * 4) == 4);
    // Putting the change back elsewhere and returning: steady.
    for (k = 0; k < 6; k++)
    {
        tap(CTL_KEY_DOWN);
    }
    tap(1);
    tap(1);
    tap(1);
    for (k = 0; k < 6; k++)
    {
        tap(CTL_KEY_UP);
    }
    CHECK(cfg_page == PAGE_VERSION && lamp == HAL_LAMP_LEDS_STOMPS && status_led == 1 && status_toggles(CFG_FLASH_TICKS * 4) == 0);
    cancel_exit();
    }
}

// At start-up, switches saved as inverted momentary are on with their LEDs lit (their
// resting state), and nothing is sent; other modes start off.
static void test_boot_inverted(void)
{
    int i;

    printf("start-up: inverted momentary switches lit\n");
    reset_world();
    cfg_stored_valid = 1;
    stored_cfg[0] = MIDI_CHANNEL;
    // Two bits per switch, switch 1 lowest: switch 2 inverted (3), switch 3 momentary
    // (2), switch 1 stomp; switches 5-8 inverted; switches 9 and 10 stomp.
    store_modes((uint8_t)((PM_MOM_INV << 2) | (PM_MOMENTARY << 4)), 0xFF, 0x00);
    memset(leds, 0xEE, sizeof(leds));
    clear_log();
    ctl_init();
    CHECK(midi_len == 0);

    for (i = 0; i < STOMP_COUNT; i++)
    {
        CHECK(leds[i] == (pedal_mode_get(i) == PM_MOM_INV));
        CHECK(((stomp_state >> i) & 1) == (pedal_mode_get(i) == PM_MOM_INV));
    }

    CHECK(leds[1] == 1 && leds[4] == 1 && leds[7] == 1 && leds[0] == 0 && leds[2] == 0 && leds[8] == 0);
    // A press then works as usual: 0 while pressed, 127 (and lit) again on release.
    ctl_key_press(1);
    CHECK(midi_len == 3 && cc_number(0) == stomp_cc[1] && cc_value(0) == 0 && leds[1] == 0);
    ctl_key_release(1);
    CHECK(midi_len == 6 && cc_value(1) == 127 && leds[1] == 1);
    // A relay following an inverted switch closes on the first tick.
    reset_world();
    sw_stored_valid[0] = 1;
    stored_sw[0][0] = 0x02;
    stored_sw[0][1] = 0;
    stored_sw[0][2] = 0;
    stored_sw[0][3] = 0;
    store_modes((uint8_t)(PM_MOM_INV << 2), 0, 0);
    ctl_init();
    ticks(1);
    CHECK(relays == 1 && aux[HAL_LED_SWITCH_1] == 1 && midi_len == 0);
}

// Set footswitch sw's (0-9) message as if it had been saved.
static void set_msg(uint8_t sw, uint8_t type, uint8_t num, uint8_t on, uint8_t off)
{
    msg[sw * 3]     = (uint8_t)((num & 0x7F) | ((type & 1) << 7));
    msg[sw * 3 + 1] = (uint8_t)((on & 0x7F) | ((type & 2) << 6));
    msg[sw * 3 + 2] = (uint8_t)(off & 0x7F);
}

#define MSG_NUM(sw)  (msg[(sw) * 3] & 0x7F)
#define MSG_ON(sw)   (msg[(sw) * 3 + 1] & 0x7F)
#define MSG_OFF(sw)  (msg[(sw) * 3 + 2])

#define NOTE_ON   (0x90 | (MIDI_CHANNEL - 1))
#define NOTE_OFF  (0x80 | (MIDI_CHANNEL - 1))
#define PROG_CHG  (0xC0 | (MIDI_CHANNEL - 1))

// Only the editor parameter's LED is lit (SELECT type, NUMBER, VALUE 1 on, VALUE 2 off).
static int param_leds(int p)
{
    return aux[HAL_LED_SELECT] == (p == EP_TYPE) && aux[HAL_LED_NUMBER] == (p == EP_NUM) &&
           aux[HAL_LED_VALUE_1] == (p == EP_ON) && aux[HAL_LED_VALUE_2] == (p == EP_OFF);
}

static void test_switch_messages(void)
{
    const uint8_t cc[10] = { STOMP_CC_LIST };
    int i;

    printf("run mode: footswitch messages (CC on / off values, notes, program changes)\n");

    // Default: a CC from STOMP_CC_LIST, 127 on and 0 off.
    reset_world();
    for (i = 0; i < 10; i++)
    {
        CHECK(msg_type((uint8_t)i) == MT_CC && MSG_NUM(i) == cc[i] && MSG_ON(i) == 127 && MSG_OFF(i) == 0);
    }

    // CC with on 100, off 20.
    set_mode(0, PM_STOMP);
    set_msg(0, MT_CC, 50, 100, 20);
    clear_log();
    tap(sw_code(1));
    CHECK(midi_len == 3 && cc_status(0) == CC_STATUS && cc_number(0) == 50 && cc_value(0) == 100 && leds[0] == 1);
    tap(sw_code(1));
    CHECK(midi_len == 6 && cc_number(1) == 50 && cc_value(1) == 20 && leds[0] == 0);

    // Note, stomp mode: note on with the on value as velocity, then note off with the off
    // value.
    set_mode(1, PM_STOMP);
    set_msg(1, MT_NOTE, 60, 90, 40);
    clear_log();
    tap(sw_code(2));
    CHECK(midi_len == 3 && midi_log[0] == NOTE_ON && midi_log[1] == 60 && midi_log[2] == 90 && leds[1] == 1);
    tap(sw_code(2));
    CHECK(midi_len == 6 && midi_log[3] == NOTE_OFF && midi_log[4] == 60 && midi_log[5] == 40 && leds[1] == 0);

    // Note, momentary: on while pressed, off on release.
    set_mode(2, PM_MOMENTARY);
    set_msg(2, MT_NOTE, 36, 127, 0);
    clear_log();
    ctl_key_press(sw_code(3));
    CHECK(midi_len == 3 && midi_log[0] == NOTE_ON && midi_log[1] == 36 && midi_log[2] == 127 && leds[2] == 1);
    ctl_key_release(sw_code(3));
    CHECK(midi_len == 6 && midi_log[3] == NOTE_OFF && midi_log[4] == 36 && midi_log[5] == 0 && leds[2] == 0);

    // Note, stomp with momentary: a long press undoes the note on release.
    set_mode(3, PM_STOMP_MOM);
    set_msg(3, MT_NOTE, 40, 70, 0);
    clear_log();
    long_press(sw_code(4));
    CHECK(midi_len == 6 && midi_log[0] == NOTE_ON && midi_log[3] == NOTE_OFF && leds[3] == 0);

    // Inverted momentary CC: the off value while pressed, the on value on release.
    set_mode(7, PM_MOM_INV);
    set_msg(7, MT_CC, 70, 90, 10);
    clear_log();
    ctl_key_press(sw_code(8));
    ctl_key_release(sw_code(8));
    CHECK(midi_len == 6 && cc_number(0) == 70 && cc_value(0) == 10 && cc_value(1) == 90 && leds[7] == 1);

    // Program change with two programs: a toggle between them, following its pedal mode.
    // The LED shows the state, the display the program sent, and UP / DOWN step from it.
    set_mode(4, PM_STOMP);
    set_msg(4, MT_PROG, 0, 10, 11);
    clear_log();
    tap(sw_code(5));
    CHECK(midi_len == 2 && midi_log[0] == PROG_CHG && midi_log[1] == 10 && leds[4] == 1);
    CHECK(program == 10 && display == 10 + DISPLAY_PROGRAM_OFFSET);
    tap(sw_code(5));
    CHECK(midi_len == 4 && midi_log[2] == PROG_CHG && midi_log[3] == 11 && leds[4] == 0 && program == 11);
    clear_log();
    tap(CTL_KEY_UP);
    CHECK(midi_len == 2 && midi_log[0] == PROG_CHG && midi_log[1] == 12);
    // Momentary: the first program while held, the second on release.
    set_mode(4, PM_MOMENTARY);
    clear_log();
    ctl_key_press(sw_code(5));
    CHECK(midi_len == 2 && midi_log[1] == 10 && leds[4] == 1);
    ctl_key_release(sw_code(5));
    CHECK(midi_len == 4 && midi_log[3] == 11 && leds[4] == 0 && display == 11 + DISPLAY_PROGRAM_OFFSET);
    // Stomp with momentary: a long press goes back on release.
    set_mode(4, PM_STOMP_MOM);
    clear_log();
    long_press(sw_code(5));
    CHECK(midi_len == 4 && midi_log[1] == 10 && midi_log[3] == 11 && leds[4] == 0);
    // Two program change switches are independent (no radio behaviour).
    set_mode(4, PM_STOMP);
    set_mode(5, PM_STOMP);
    set_msg(5, MT_PROG, 0, 20, 21);
    tap(sw_code(5));
    tap(sw_code(6));
    CHECK(leds[4] == 1 && leds[5] == 1 && program == 20);

    // A single program (both programs the same): sent on every press, nothing on release,
    // whatever the pedal mode; its LED is lit only while held.
    set_mode(6, PM_STOMP);
    set_msg(6, MT_PROG, 99, 30, 30);
    clear_log();
    ctl_key_press(sw_code(7));
    CHECK(midi_len == 2 && midi_log[0] == PROG_CHG && midi_log[1] == 30 && leds[6] == 1 && program == 30);
    ctl_key_release(sw_code(7));
    CHECK(midi_len == 2 && leds[6] == 0 && (stomp_state & (1 << 6)) == 0);
    tap(sw_code(7));
    CHECK(midi_len == 4 && midi_log[3] == 30 && leds[6] == 0);
    set_mode(6, PM_MOM_INV);
    clear_log();
    ctl_key_press(sw_code(7));
    CHECK(midi_len == 2 && midi_log[1] == 30 && leds[6] == 1);
    ctl_key_release(sw_code(7));
    CHECK(midi_len == 2 && leds[6] == 0);
    long_press(sw_code(7));
    CHECK(midi_len == 4 && midi_log[3] == 30 && leds[6] == 0);
    // No room in MIDI OUT: ignored, like a stomp.
    clear_log();
    midi_space = 2;
    tap(sw_code(7));
    tap(sw_code(5));
    CHECK(midi_len == 0 && leds[6] == 0 && leds[4] == 1);
    midi_space = 15;

    // MIDI IN: a note on (velocity above 0) on the pedal's channel sets a note switch, a
    // note off or note on with velocity 0 clears it; a CC with the same number does not.
    reset_world();
    set_mode(1, PM_STOMP);
    set_msg(1, MT_NOTE, 60, 90, 0);
    set_mode(0, PM_STOMP);
    set_msg(0, MT_CC, 60, 127, 0);
    ctl_midi_in(NOTE_ON); ctl_midi_in(60); ctl_midi_in(1);
    CHECK(leds[1] == 1 && leds[0] == 0 && midi_len == 0);
    ctl_midi_in(NOTE_ON); ctl_midi_in(60); ctl_midi_in(0);
    CHECK(leds[1] == 0);
    ctl_midi_in(NOTE_ON); ctl_midi_in(60); ctl_midi_in(100);
    ctl_midi_in(NOTE_OFF); ctl_midi_in(60); ctl_midi_in(64);
    CHECK(leds[1] == 0);
    // Running status note on.
    ctl_midi_in(NOTE_ON); ctl_midi_in(61); ctl_midi_in(100); ctl_midi_in(60); ctl_midi_in(100);
    CHECK(leds[1] == 1);
    ctl_midi_in(CC_STATUS); ctl_midi_in(60); ctl_midi_in(127);
    CHECK(leds[0] == 1 && leds[1] == 1);
    // Another channel, and program changes, are ignored.
    ctl_midi_in((uint8_t)(0x80 | (MIDI_CHANNEL % 16))); ctl_midi_in(60); ctl_midi_in(0);
    CHECK(leds[1] == 1);
    set_mode(2, PM_STOMP);
    set_msg(2, MT_PROG, 0, 5, 6);
    ctl_midi_in(PROG_CHG); ctl_midi_in(5);
    CHECK(leds[2] == 0 && midi_len == 0);

    // MIDI IN CC with custom values: the on value is on and the off value off, even when
    // they are the other way round from the 64 rule; other values follow the 64 rule.
    set_mode(3, PM_STOMP);
    set_msg(3, MT_CC, 80, 0, 127);
    ctl_midi_in(CC_STATUS); ctl_midi_in(80); ctl_midi_in(0);
    CHECK(leds[3] == 1);
    ctl_midi_in(CC_STATUS); ctl_midi_in(80); ctl_midi_in(127);
    CHECK(leds[3] == 0);
    ctl_midi_in(CC_STATUS); ctl_midi_in(80); ctl_midi_in(100);
    CHECK(leds[3] == 1);
    ctl_midi_in(CC_STATUS); ctl_midi_in(80); ctl_midi_in(30);
    CHECK(leds[3] == 0);
    set_msg(3, MT_CC, 80, 40, 30);
    ctl_midi_in(CC_STATUS); ctl_midi_in(80); ctl_midi_in(40);
    CHECK(leds[3] == 1 && midi_len == 0);
    ctl_midi_in(CC_STATUS); ctl_midi_in(80); ctl_midi_in(30);
    CHECK(leds[3] == 0);

    // A program change switch starts off at power-on even in inverted momentary mode.
    reset_world();
    msg_stored_valid[0] = 1;
    stored_msg[0][0] = MT_PROG;         // a single program: rests off in any mode
    stored_msg[0][1] = 0;
    stored_msg[0][2] = 5;
    stored_msg[0][3] = 5;
    msg_stored_valid[1] = 1;
    stored_msg[1][0] = MT_NOTE;
    stored_msg[1][1] = 60;
    stored_msg[1][2] = 100;
    stored_msg[1][3] = 30;
    store_modes(0x0F, 0, 0);            // switches 1 and 2 inverted momentary
    ctl_init();
    boot_end();
    CHECK(msg_type(0) == MT_PROG && MSG_ON(0) == 5 && MSG_OFF(0) == 5 && prog_single(0) && msg_type(1) == MT_NOTE && MSG_NUM(1) == 60 &&
          MSG_ON(1) == 100 && MSG_OFF(1) == 30);
    CHECK(leds[0] == 0 && leds[1] == 1 && midi_len == 0);
    // The other records were missing: defaults.
    CHECK(MSG_NUM(2) == cc[2] && MSG_ON(2) == 127 && MSG_OFF(2) == 0 && msg_type(9) == MT_CC && MSG_NUM(9) == cc[9]);

    // A two-program switch in inverted momentary rests on at power-on, sending nothing;
    // pressing it sends the second program, releasing the first.
    reset_world();
    msg_stored_valid[0] = 1;
    stored_msg[0][0] = MT_PROG;
    stored_msg[0][1] = 0;
    stored_msg[0][2] = 5;
    stored_msg[0][3] = 6;
    store_modes(0x03, 0, 0);            // switch 1 inverted momentary
    ctl_init();
    boot_end();
    CHECK(leds[0] == 1 && midi_len == 0);
    ctl_key_press(sw_code(1));
    CHECK(midi_len == 2 && midi_log[1] == 6 && leds[0] == 0);
    ctl_key_release(sw_code(1));
    CHECK(midi_len == 4 && midi_log[3] == 5 && leds[0] == 1);
    // Accepting an edit while it is on sends no program change (and it turns off).
    chord_long();
    clear_log();
    long_press(sw_code(1));
    tap(CTL_KEY_UP);
    CHECK(edit_param == EP_ON);
    tap(sw_code(9));
    long_nav(CTL_KEY_UP);
    CHECK(midi_len == 0 && (stomp_state & 1) == 0 && MSG_ON(0) == 9);
    cancel_exit();
    cfg_stored_valid = 0;

    // A stored switch with the unused type 3 loads as a CC with the same values, and
    // values over 127 lose bit 7.
    reset_world();
    msg_stored_valid[4] = 1;
    stored_msg[4][0] = 3;
    stored_msg[4][1] = 33;
    stored_msg[4][2] = 0x80 | 44;
    stored_msg[4][3] = 0x80 | 12;
    ctl_init();
    boot_end();
    CHECK(msg_type(4) == MT_CC && MSG_NUM(4) == 33 && MSG_ON(4) == 44 && MSG_OFF(4) == 12);
    cfg_stored_valid = 0;
    memset(msg_stored_valid, 0, sizeof(msg_stored_valid));
}

static void test_config_editor(void)
{
    const uint8_t cc[10] = { STOMP_CC_LIST };
    int i;

    printf("config: footswitch editor\n");
    reset_world();
    set_mode(2, PM_STOMP);
    chord_long();
    CHECK(mode == MODE_CONFIG && cfg_page == PAGE_MODES);
    CHECK(param_leds(-1));

    // A short press of the selected switch changes its mode on release, not on press.
    tap(sw_code(3));
    CHECK(cfg_sel == 2 && pedal_mode_get(2) == PM_STOMP);
    ctl_key_press(sw_code(3));
    CHECK(pedal_mode_get(2) == PM_STOMP);
    ctl_key_release(sw_code(3));
    CHECK(pedal_mode_get(2) == PM_STOMP_MOM && display == 2);

    // A long press opens the editor on the message type (SELECT lit) without changing the
    // mode.
    clear_log();
    long_press(sw_code(3));
    CHECK(edit_open && pedal_mode_get(2) == PM_STOMP_MOM && cfg_sel == 2);
    CHECK(param_leds(EP_TYPE) && display == SHOWN_TEXT(HAL_TEXT_CC) && aux[HAL_LED_MIDI_FUNC] == 1);
    for (i = 0; i < 10; i++)
    {
        CHECK(leds[i] == (i == 2));
    }

    // Type: switch 2 = note ("nt"), 3 = program change ("Pr"), 1 = CC, others nothing.
    tap(sw_code(2));
    CHECK(msg_type(2) == MT_NOTE && display == SHOWN_TEXT(HAL_TEXT_NOTE));
    tap(sw_code(7));
    CHECK(msg_type(2) == MT_NOTE);
    // The CONFIG LED starts flashing on the next tick (the settings differ from the EEPROM).
    ticks(1);
    CHECK(cfg_changed && status_led == 0);

    // UP: the number (NUMBER lit).
    tap(CTL_KEY_UP);
    CHECK(edit_param == EP_NUM && param_leds(EP_NUM) && display == cc[2]);
    // Digits: 6, 4 make 64, which is complete (no third digit fits), so 1, 2 then make 12,
    // and 8 starts again (128 is over 127); 10 types 0, so 8, 10 make 80.
    tap(sw_code(6)); tap(sw_code(4));
    CHECK(MSG_NUM(2) == 64 && display == 64 && msg_type(2) == MT_NOTE);
    tap(sw_code(1)); tap(sw_code(2));
    CHECK(MSG_NUM(2) == 12);
    tap(sw_code(8));
    CHECK(MSG_NUM(2) == 8);
    tap(sw_code(10));
    CHECK(MSG_NUM(2) == 80);
    tap(sw_code(1)); tap(sw_code(2)); tap(sw_code(7));
    CHECK(MSG_NUM(2) == 127);
    tap(sw_code(10));
    CHECK(MSG_NUM(2) == 0);
    tap(sw_code(1)); tap(sw_code(10)); tap(sw_code(10));
    CHECK(MSG_NUM(2) == 100 && msg_type(2) == MT_NOTE);

    // UP: the on value (VALUE 1 lit). A new parameter starts a new number.
    tap(CTL_KEY_UP);
    CHECK(edit_param == EP_ON && param_leds(EP_ON) && display == 127);
    tap(sw_code(9)); tap(sw_code(10));
    CHECK(MSG_ON(2) == 90 && msg_type(2) == MT_NOTE);
    // UP: the off value (VALUE 2 lit).
    tap(CTL_KEY_UP);
    CHECK(edit_param == EP_OFF && param_leds(EP_OFF) && display == 0);
    tap(sw_code(3)); tap(sw_code(5));
    CHECK(MSG_OFF(2) == 35 && MSG_ON(2) == 90 && msg_type(2) == MT_NOTE);
    // No wrap: UP stays on the off value, DOWN goes back to the type, then stays.
    tap(CTL_KEY_UP);
    CHECK(edit_param == EP_OFF);
    tap(CTL_KEY_DOWN); tap(CTL_KEY_DOWN); tap(CTL_KEY_DOWN); tap(CTL_KEY_DOWN);
    CHECK(edit_param == EP_TYPE && param_leds(EP_TYPE) && display == SHOWN_TEXT(HAL_TEXT_NOTE));
    // Footswitch holds and the short UP + DOWN do nothing in the editor; nothing is sent.
    long_press(sw_code(5));
    chord_short();
    CHECK(edit_open && msg_type(2) == MT_NOTE && midi_len == 0);

    // A long UP [ENTER] accepts: back to the modes page with switch 3 selected.
    long_nav(CTL_KEY_UP);
    CHECK(!edit_open && mode == MODE_CONFIG && param_leds(-1));
    CHECK(aux[HAL_LED_MIDI_FUNC] == 1 && display == 2 && leds[2] == 1);
    CHECK(msg_type(2) == MT_NOTE && MSG_NUM(2) == 100 && MSG_ON(2) == 90 && MSG_OFF(2) == 35);

    // Open it again, change it, and a long DOWN [ESCAPE] puts the values back.
    long_press(sw_code(3));
    CHECK(edit_open && edit_param == EP_TYPE);
    tap(CTL_KEY_UP); tap(CTL_KEY_UP); tap(CTL_KEY_UP);
    tap(sw_code(7));
    CHECK(MSG_OFF(2) == 7);
    tap(CTL_KEY_DOWN); tap(CTL_KEY_DOWN); tap(CTL_KEY_DOWN);
    tap(sw_code(3));
    CHECK(msg_type(2) == MT_PROG && display == SHOWN_TEXT(HAL_TEXT_PROG));
    // A program change's two programs are VALUE 1 (sent on) and VALUE 2 (sent off); UP and
    // DOWN skip NUMBER.
    tap(CTL_KEY_UP);
    CHECK(edit_param == EP_ON && param_leds(EP_ON));
    tap(sw_code(5));
    CHECK(MSG_ON(2) == 5);
    tap(CTL_KEY_UP);
    CHECK(edit_param == EP_OFF && param_leds(EP_OFF));
    tap(sw_code(6));
    CHECK(MSG_OFF(2) == 6);
    tap(CTL_KEY_UP);
    CHECK(edit_param == EP_OFF);
    tap(CTL_KEY_DOWN);
    CHECK(edit_param == EP_ON);
    tap(CTL_KEY_DOWN);
    CHECK(edit_param == EP_TYPE && param_leds(EP_TYPE));
    tap(CTL_KEY_UP);
    CHECK(edit_param == EP_ON);
    long_nav(CTL_KEY_DOWN);
    CHECK(!edit_open && mode == MODE_CONFIG && msg_type(2) == MT_NOTE && MSG_NUM(2) == 100 &&
          MSG_ON(2) == 90 && MSG_OFF(2) == 35);

    // Save and leave: every footswitch record is saved; switch 3 sends its note.
    save_exit();
    CHECK(mode == MODE_RUN && msg_saved == 10);
    CHECK((saved_msg[2][0] & 3) == MT_NOTE && saved_msg[2][1] == 100 && saved_msg[2][2] == 90 && saved_msg[2][3] == 35);
    CHECK((saved_msg[0][0] & 3) == MT_CC && saved_msg[0][1] == cc[0] && saved_msg[0][2] == 127 && saved_msg[0][3] == 0);
    CHECK(saved_msg[9][1] == cc[9] && param_leds(-1));
    clear_log();
    tap(sw_code(3));
    CHECK(midi_len == 3 && midi_log[0] == NOTE_ON && midi_log[1] == 100 && midi_log[2] == 90);

    // An accepted change to a switch that is on sends the old message's "off" (its off
    // value) at once and turns it off; at the save it goes to its resting state with the
    // new message.
    reset_world();
    set_mode(0, PM_STOMP);
    set_msg(0, MT_CC, cc[0], 127, 9);
    tap(sw_code(1));
    CHECK(leds[0] == 1);
    chord_long();
    clear_log();
    long_press(sw_code(1));
    tap(CTL_KEY_UP);
    tap(sw_code(5)); tap(sw_code(5));
    long_nav(CTL_KEY_UP);
    CHECK(midi_len == 3 && cc_number(0) == cc[0] && cc_value(0) == 9 && (stomp_state & 1) == 0);
    save_exit();
    CHECK(leds[0] == 0 && midi_len == 3);
    clear_log();
    tap(sw_code(1));
    CHECK(midi_len == 3 && cc_number(0) == 55 && cc_value(0) == 127);

    // An inverted momentary switch whose message changes rests on with the new message.
    reset_world();
    set_mode(1, PM_MOM_INV);
    chord_long();
    clear_log();
    long_press(sw_code(2));
    tap(CTL_KEY_UP);
    tap(sw_code(9));
    long_nav(CTL_KEY_UP);
    CHECK(midi_len == 3 && cc_number(0) == cc[1] && cc_value(0) == 0);
    save_exit();
    ticks(1);
    CHECK(midi_len == 6 && cc_number(1) == 9 && cc_value(1) == 127 && leds[1] == 1);

    // Unchanged on accept: nothing is sent and nothing moves.
    reset_world();
    set_mode(0, PM_STOMP);
    tap(sw_code(1));
    chord_long();
    clear_log();
    long_press(sw_code(1));
    long_nav(CTL_KEY_UP);
    save_exit();
    ticks(1);
    CHECK(midi_len == 0 && leds[0] == 1);

    // Only the off value changed: that is a change too.
    reset_world();
    chord_long();
    long_press(sw_code(1));
    tap(CTL_KEY_UP); tap(CTL_KEY_UP); tap(CTL_KEY_UP);
    tap(sw_code(4));
    long_nav(CTL_KEY_UP);
    CHECK(MSG_OFF(0) == 4 && (cfg_dirty & 1));
    cancel_exit();
    CHECK(MSG_OFF(0) == 0);

    // Pedals in the editor: the first reading only records; moving sets the parameter.
    // Type by thirds of the travel (default calibration 0-255).
    reset_world();
    chord_long();
    long_press(sw_code(4));
    pedal(0, 0);
    CHECK(msg_type(3) == MT_CC);
    pedal(0, 128);
    CHECK(msg_type(3) == MT_NOTE && display == SHOWN_TEXT(HAL_TEXT_NOTE));
    pedal(0, 255);
    CHECK(msg_type(3) == MT_PROG);
    pedal(1, 255);
    pedal(1, 10);
    CHECK(msg_type(3) == MT_CC);
    // Values: the pedal's CC value; a pedal ends a typed number.
    tap(CTL_KEY_UP);
    tap(sw_code(1));
    pedal(1, 10);
    CHECK(MSG_NUM(3) == 1);
    pedal(1, 128);
    CHECK(MSG_NUM(3) == ctl_scale(128, 0, 255) && display == ctl_scale(128, 0, 255) && msg_type(3) == MT_CC);
    tap(sw_code(2));
    CHECK(MSG_NUM(3) == 2);
    // Small wobble is ignored.
    pedal(1, 129);
    CHECK(MSG_NUM(3) == 2);
    // The off value from a pedal (a parameter change takes a new first reading).
    tap(CTL_KEY_UP); tap(CTL_KEY_UP);
    pedal(0, 0);
    CHECK(MSG_OFF(3) == 0);
    pedal(0, 200);
    CHECK(MSG_OFF(3) == ctl_scale(200, 0, 255) && msg_type(3) == MT_CC && MSG_NUM(3) == 2);
    CHECK(midi_len == 0);
    // A long UP + DOWN saves and leaves with the editor open: the change is kept.
    chord_long();
    CHECK(mode == MODE_RUN && !edit_open && saved_msg[3][1] == 2 && saved_msg[3][3] == ctl_scale(200, 0, 255) &&
          msg_saved == 10);

    // A long press of the selected switch while DOWN is held does not open the editor
    // (the long DOWN cancels and leaves).
    reset_world();
    chord_long();
    ctl_key_press(CTL_KEY_DOWN);
    ctl_key_press(sw_code(1));
    ctl_key_hold(sw_code(1));
    CHECK(!edit_open);
    ctl_key_hold(CTL_KEY_DOWN);
    CHECK(mode == MODE_RUN);
    ctl_key_release(sw_code(1));
    ctl_key_release(CTL_KEY_DOWN);

    // The editor is only on the modes page: a long press elsewhere does what a press does.
    reset_world();
    chord_long();
    tap(CTL_KEY_UP);
    long_press(sw_code(3));
    CHECK(!edit_open && midi_channel == 3);
    cancel_exit();

    // Cancelling config mode after an accepted change restores the saved message.
    reset_world();
    chord_long();
    long_press(sw_code(1));
    tap(sw_code(2));
    long_nav(CTL_KEY_UP);
    CHECK(msg_type(0) == MT_NOTE);
    cancel_exit();
    CHECK(mode == MODE_RUN && msg_type(0) == MT_CC && MSG_NUM(0) == cc[0] && msg_saved == 0);

    // The CONFIG LED: a change that is put back matches the EEPROM again (steady). A stored
    // record compares the way it loads.
    reset_world();
    msg_stored_valid[0] = 1;
    stored_msg[0][0] = 3 | (DEFAULT_PEDAL_MODE << 2);   // type 3: loads as CC
    stored_msg[0][1] = cc[0];
    stored_msg[0][2] = 127;
    stored_msg[0][3] = 0x80;            // off value 0 with bit 7 set
    chord_long();
    CHECK(!cfg_changed && status_led == 1);
    long_press(sw_code(1));
    tap(sw_code(3));
    ticks(1);
    CHECK(cfg_changed);
    tap(sw_code(1));
    ticks(1);
    CHECK(!cfg_changed && status_led == 1);
    tap(sw_code(2));
    ticks(1);
    CHECK(cfg_changed);
    long_nav(CTL_KEY_DOWN);
    ticks(1);
    CHECK(!cfg_changed && status_led == 1);
    cancel_exit();
    memset(msg_stored_valid, 0, sizeof(msg_stored_valid));
}

// Send one NRPN set on channel ch (1-16): CC 99 = 0x46, CC 98 = p, CC 6 = v.
static void nrpn_on(uint8_t ch, uint8_t p, uint8_t v)
{
    ctl_midi_in((uint8_t)(0xB0 | (ch - 1)));
    ctl_midi_in(99); ctl_midi_in(NRPN_MSB);
    ctl_midi_in(98); ctl_midi_in(p);
    ctl_midi_in(6);  ctl_midi_in(v);
}

static void nrpn(uint8_t p, uint8_t v)
{
    nrpn_on(midi_channel, p, v);
}

// Parse a dump from the MIDI log: values[p] (14-bit for CC 6 + CC 38), seen[p], and
// whether the end marker came last. Returns the number of parameters, or -1 if the bytes
// are not well-formed NRPN messages with running status.
static int dump_values[NP_COUNT], dump_seen[NP_COUNT], dump_end;

static int parse_dump(int from)
{
    int i = from, n = 0, p;

    memset(dump_seen, 0, sizeof(dump_seen));
    dump_end = 0;

    while (i < midi_len)
    {
        if (midi_log[i] != CC_STATUS || midi_log[i + 1] != 99 || midi_log[i + 2] != NRPN_MSB ||
            midi_log[i + 3] != 98 || midi_log[i + 5] != 6)
        {
            return -1;
        }

        p = midi_log[i + 4];
        i += 7;

        if (dump_end)
        {
            return -1;           // nothing after the end marker
        }

        if (p == NP_DUMP_END)
        {
            dump_end = midi_log[i - 1] == 0;
            continue;
        }

        dump_values[p] = midi_log[i - 1];

        if (i < midi_len && midi_log[i] == 38)
        {
            dump_values[p] = dump_values[p] * 128 + midi_log[i + 1];
            i += 2;
        }

        dump_seen[p] = 1;
        n++;
    }

    return n;
}

// Run a dump to the end (or give up after n ticks).
static void dump_run(int n)
{
    nrpn(NP_COMMAND, NC_DUMP);

    while (midi_job && n-- > 0)
    {
        ticks(1);
    }
}

static void test_midi_config(void)
{
    const uint8_t cc[10] = { STOMP_CC_LIST };
    int i, n;

    printf("configuration over MIDI (NRPN)\n");

    // A dump: every known parameter once, in order, then the end marker; settable values
    // in CC 6 alone, the version and calibration as CC 6 + CC 38.
    reset_world();
    set_mode(3, PM_MOM_INV);
    set_msg(4, MT_NOTE, 60, 100, 30);
    sw_mask[1] = (uint16_t)(SW_CC_BIT | (1 << 2));
    sw_thr[1] = 77;
    heel[0] = 3;   toe[0] = 250;
    heel[1] = 129; toe[1] = 200;
    clear_log();
    dump_run(1000);
    n = parse_dump(0);
    CHECK(n == 1 + 3 + 2 + 10 + 40 + 11 + 11 + 4 + 4 && dump_end && midi_job == 0);
    CHECK(dump_values[NP_BANK] == 1 && dump_values[NP_BANKS] == BANK_COUNT);
    CHECK(dump_values[NP_CHANNEL] == MIDI_CHANNEL);
    CHECK(dump_values[NP_VERSION] == fw_version[0] && dump_values[NP_VERSION + 1] == fw_version[1] &&
          dump_values[NP_VERSION + 2] == fw_version[2]);
    CHECK(dump_values[NP_MODE + 3] == PM_MOM_INV && dump_values[NP_MODE] == pedal_mode_get(0));
    CHECK(dump_values[NP_TYPE + 4] == MT_NOTE && dump_values[NP_NUM + 4] == 60 && dump_values[NP_ON + 4] == 100 &&
          dump_values[NP_OFF + 4] == 30);
    CHECK(dump_values[NP_TYPE] == MT_CC && dump_values[NP_NUM] == cc[0] && dump_values[NP_ON] == 127 && dump_values[NP_OFF] == 0);
    CHECK(dump_values[NP_FOLLOW2 + 2] == 1 && dump_values[NP_FOLLOW2 + 10] == 1 && dump_values[NP_FOLLOW2 + 3] == 0 &&
          dump_values[NP_FOLLOW1 + 10] == 0);
    CHECK(dump_values[NP_TRIGGER + 1] == 77 && dump_values[NP_TRIGGER] == 0);
    CHECK(dump_values[NP_CAL] == 3 && dump_values[NP_CAL + 1] == 250 && dump_values[NP_CAL + 2] == 129 &&
          dump_values[NP_CAL + 3] == 200);
    CHECK(!dump_seen[NP_COMMAND] && !dump_seen[NP_MODE + 10] && !dump_seen[NP_FOLLOW1 + 11]);

    // It goes one parameter per tick, waiting while MIDI OUT has less than 9 bytes free
    // (7 for a settable parameter, 9 with CC 38).
    clear_log();
    nrpn(NP_COMMAND, NC_DUMP);
    ticks(1);
    CHECK(midi_len == 7 && midi_log[4] == NP_CHANNEL);
    midi_space = 8;
    ticks(5);
    CHECK(midi_len == 7);
    midi_space = 15;
    ticks(1);
    CHECK(midi_len == 7 + 9 && midi_log[11] == NP_VERSION && midi_log[14] == 38);
    dump_run(1000);

    // A dump fed back in changes nothing and sends nothing.
    clear_log();
    dump_run(1000);
    n = midi_len;
    memcpy(midi_log + 2048, midi_log, (size_t)n);
    clear_log();
    for (i = 0; i < n; i++)
    {
        ctl_midi_in(midi_log[2048 + i]);
    }
    ticks(2);
    CHECK(midi_len == 0 && cfg_dirty == 0 && pedal_mode_get(3) == PM_MOM_INV && msg_type(4) == MT_NOTE &&
          heel[1] == 129 && sw_thr[1] == 77 && midi_channel == MIDI_CHANNEL);

    // Sets apply at once. Pedal mode: the switch goes to its new resting state on the next
    // tick (inverted momentary: on, sending its on value).
    reset_world();
    set_mode(0, PM_STOMP);
    clear_log();
    nrpn(NP_MODE + 0, PM_MOM_INV);
    CHECK(pedal_mode_get(0) == PM_MOM_INV && midi_len == 0);
    ticks(2);
    CHECK(midi_len == 3 && cc_number(0) == cc[0] && cc_value(0) == 127 && leds[0] == 1);
    // Out of range: ignored.
    nrpn(NP_MODE + 1, 4);
    CHECK(pedal_mode_get(1) == DEFAULT_PEDAL_MODE);

    // Message: type, number, on and off; the switch then sends it.
    reset_world();
    set_mode(1, PM_STOMP);
    nrpn(NP_TYPE + 1, MT_NOTE);
    nrpn(NP_NUM + 1, 64);
    nrpn(NP_ON + 1, 99);
    nrpn(NP_OFF + 1, 11);
    nrpn(NP_TYPE + 2, 3);                  // no type 3: ignored
    CHECK(msg_type(1) == MT_NOTE && MSG_NUM(1) == 64 && MSG_ON(1) == 99 && MSG_OFF(1) == 11 && msg_type(2) == MT_CC);
    ticks(1);
    clear_log();
    tap(sw_code(2));
    tap(sw_code(2));
    CHECK(midi_len == 6 && midi_log[0] == NOTE_ON && midi_log[1] == 64 && midi_log[2] == 99 &&
          midi_log[3] == NOTE_OFF && midi_log[5] == 11);
    // A change to a switch that is on sends the old message's off and turns it off.
    tap(sw_code(2));
    clear_log();
    nrpn(NP_NUM + 1, 65);
    CHECK(midi_len == 3 && midi_log[0] == NOTE_OFF && midi_log[1] == 64 && midi_log[2] == 11 && leds[1] == 0);
    ticks(1);
    CHECK(midi_len == 3);
    // A program change switch can be set up too, and sends nothing when changed while on.
    set_mode(5, PM_STOMP);
    nrpn(NP_TYPE + 5, MT_PROG);
    nrpn(NP_ON + 5, 7);
    nrpn(NP_OFF + 5, 8);
    ticks(1);
    clear_log();
    tap(sw_code(6));
    CHECK(midi_len == 2 && midi_log[1] == 7 && leds[5] == 1);
    nrpn(NP_ON + 5, 9);
    CHECK(midi_len == 2 && leds[5] == 0);

    // Relays: follow flags (0 / 1, flipping only on a change) and trigger values.
    reset_world();
    nrpn(NP_FOLLOW1 + 4, 1);
    nrpn(NP_FOLLOW1 + 4, 1);
    nrpn(NP_FOLLOW2 + 10, 1);
    nrpn(NP_FOLLOW1 + 5, 2);               // not 0 or 1: ignored
    nrpn(NP_TRIGGER + 3, 90);
    CHECK(sw_mask[0] == (1 << 4) && sw_mask[1] == SW_CC_BIT && sw_thr[3] == 90);
    nrpn(NP_FOLLOW1 + 4, 0);
    CHECK(sw_mask[0] == 0);

    // Channel: 1-16; afterwards only the new channel is heard.
    reset_world();
    nrpn(NP_CHANNEL, 0);
    nrpn(NP_CHANNEL, 17);
    CHECK(midi_channel == MIDI_CHANNEL);
    nrpn(NP_CHANNEL, (uint8_t)(MIDI_CHANNEL % 16 + 1));
    CHECK(midi_channel == MIDI_CHANNEL % 16 + 1);
    nrpn_on(MIDI_CHANNEL, NP_TRIGGER, 50);
    CHECK(sw_thr[0] == 0);
    nrpn(NP_TRIGGER, 50);
    CHECK(sw_thr[0] == 50);

    // Read-only parameters and the end marker cannot be set.
    reset_world();
    nrpn(NP_CAL, 5);
    nrpn(NP_VERSION, 5);
    nrpn(NP_DUMP_END, 1);
    nrpn(6, 1);
    CHECK(heel[0] == 0 && midi_len == 0 && midi_job == 0);

    // Selection: CC 99 with another value or an RPN select deselects; CC 98 before CC 99
    // works; the selection stays for further CC 6 values (running status too).
    reset_world();
    ctl_midi_in(CC_STATUS);
    ctl_midi_in(98); ctl_midi_in(NP_TRIGGER);
    ctl_midi_in(99); ctl_midi_in(NRPN_MSB);
    ctl_midi_in(6);  ctl_midi_in(20);
    CHECK(sw_thr[0] == 20);
    ctl_midi_in(6);  ctl_midi_in(21);
    CHECK(sw_thr[0] == 21);
    ctl_midi_in(99); ctl_midi_in(1);
    ctl_midi_in(6);  ctl_midi_in(22);
    CHECK(sw_thr[0] == 21);
    ctl_midi_in(99); ctl_midi_in(NRPN_MSB);
    ctl_midi_in(101); ctl_midi_in(0);
    ctl_midi_in(6);  ctl_midi_in(23);
    CHECK(sw_thr[0] == 21);

    // Configuration mode ignores it, and entering it drops a dump in progress.
    reset_world();
    nrpn(NP_COMMAND, NC_DUMP);
    ticks(1);
    chord_long();
    CHECK(mode == MODE_CONFIG && midi_job == 0);
    clear_log();
    nrpn(NP_TRIGGER, 33);
    nrpn(NP_COMMAND, NC_DUMP);
    ticks(5);
    CHECK(sw_thr[0] == 0 && midi_len == 0 && midi_job == 0);
    cancel_exit();

    // Leaving configuration mode from a later page starts no dump (midi_job shares the
    // page number) and leaves no parameter selected (nrpn_lsb shares the selected switch).
    reset_world();
    chord_long();
    tap(CTL_KEY_UP); tap(CTL_KEY_UP); tap(CTL_KEY_UP);
    tap(sw_code(4));
    CHECK(cfg_page == 3);
    cancel_exit();
    clear_log();
    ticks(20);
    CHECK(midi_len == 0 && midi_job == 0);
    ctl_midi_in(CC_STATUS);
    ctl_midi_in(99); ctl_midi_in(NRPN_MSB);
    ctl_midi_in(6);  ctl_midi_in(9);
    ticks(2);
    CHECK(midi_len == 0 && pedal_mode_get(3) == DEFAULT_PEDAL_MODE && sw_thr[0] == 0 && midi_channel == MIDI_CHANNEL);

    // Save: at once, every record, as leaving configuration mode does; a dump requested
    // straight after does not cancel it.
    reset_world();
    nrpn(NP_TRIGGER, 44);
    nrpn(NP_NUM + 9, 3);
    nrpn(NP_COMMAND, NC_SAVE);
    nrpn(NP_COMMAND, NC_DUMP);
    CHECK(cfg_saved == 1 && midi_job == 1);
    dump_run(1000);
    CHECK(cfg_saved == 1 && cal_saved == 1 && sw_saved == 2 && msg_saved == 10 && midi_job == 0);
    CHECK(saved_sw[0][2] == 44 && saved_msg[9][1] == 3 && saved_cfg[0] == MIDI_CHANNEL);

    // Reload: back to what the EEPROM holds; switches to their resting states.
    reset_world();
    set_mode(0, PM_STOMP);
    tap(sw_code(1));
    nrpn(NP_TRIGGER, 44);
    nrpn(NP_ON + 2, 5);
    clear_log();
    nrpn(NP_COMMAND, NC_RELOAD);
    CHECK(sw_thr[0] == 0 && MSG_ON(2) == 127 && midi_job == 0);
    ticks(2);
    CHECK(leds[0] == (DEFAULT_PEDAL_MODE == PM_MOM_INV));

    // Calibration: the pedals' readings become the heel or toe, if each is CAL_MIN_SPAN
    // from the other end; the next reading is only recorded.
    reset_world();
    pedal(0, 20); pedal(1, 30);
    clear_log();
    nrpn(NP_COMMAND, NC_HEEL);
    CHECK(heel[0] == 20 && heel[1] == 30 && toe[0] == 255 && midi_len == 0);
    pedal(0, 200); pedal(1, 220);
    CHECK(midi_len == 0);
    nrpn(NP_COMMAND, NC_TOE);
    CHECK(toe[0] == 200 && toe[1] == 220);
    pedal(0, 40); pedal(1, 230);
    nrpn(NP_COMMAND, NC_TOE);              // pedal A too close to the heel: nothing changes
    CHECK(toe[0] == 200 && toe[1] == 220);
    nrpn(NP_COMMAND, NC_HEEL);             // pedal B too close to the toe
    CHECK(heel[0] == 20 && heel[1] == 30);
}

// Store footswitch sw's (0-9) message in bank b (0-based) of the fake EEPROM, with mode m.
static void store_bank_msg(int b, int sw, uint8_t type, uint8_t num, uint8_t on, uint8_t off, uint8_t m)
{
    int r = b * HAL_REC_SW1 + sw;

    msg_stored_valid[r] = 1;
    stored_msg[r][0] = (uint8_t)(type | (m << 2));
    stored_msg[r][1] = num;
    stored_msg[r][2] = on;
    stored_msg[r][3] = off;
}

// Every switch of banks 1 to n stored in stomp mode (the default messages).
static void stomp_banks(int n)
{
    int b, i;

    for (b = 0; b < n; b++)
    {
        for (i = 0; i < STOMP_COUNT; i++)
        {
            store_bank_mode(b, i, PM_STOMP);
        }
    }
}

// Footswitch banks: with DIRECT SELECT lit (UP + DOWN tapped), UP / DOWN choose a bank of
// footswitch messages and modes instead of a program; the bank stays when DIRECT SELECT
// goes out. Leaving a bank turns its switches off (sending their off), then the new bank's
// switches go to their resting states.
static void test_banks(void)
{
    const uint8_t cc[10] = { STOMP_CC_LIST };
    int i;

    printf("footswitch banks\n");

    // Power-on is bank 1. Bank 2 holds a note on switch 1 (inverted momentary) and a CC
    // on switch 2 (stomp); the rest of bank 2 was never saved.
    reset_world();
    stomp_banks(4);
    store_bank_msg(1, 0, MT_NOTE, 60, 100, 30, PM_MOM_INV);
    store_bank_msg(1, 1, MT_CC, 70, 90, 10, PM_STOMP);
    ctl_init();
    boot_end();
    CHECK(hal_bank == 0 && bank_next == 0 && msg_type(0) == MT_CC && MSG_NUM(0) == cc[0] && pedal_mode_get(0) == PM_STOMP);

    // Switch 3 on, then into bank mode: the display shows bank 1, nothing is sent.
    tap(sw_code(3));
    CHECK(leds[2] == 1);
    clear_log();
    chord_short();
    CHECK(bank_mode && aux[HAL_LED_DIRECT_SELECT] == 1 && display == 1 && midi_len == 0);

    // UP: bank 2 on the display, nothing sent. Bank 2's switch 3 has the same CC as bank
    // 1's (the default), so it stays on; switch 1 is an inverted momentary note no switch
    // of bank 1 uses, so it rests on (sending nothing, as at power-on); switch 2 rests off.
    clear_log();
    tap(CTL_KEY_UP);
    CHECK(display == 2 && program == 0 && bank_busy && hal_bank == 0);
    ticks(1);
    CHECK(!bank_busy && hal_bank == 1 && bank_next == 1 && midi_len == 0);
    CHECK(leds[0] == 1 && leds[1] == 0 && leds[2] == 1 && stomp_state == 0x005);
    CHECK(msg_type(0) == MT_NOTE && pedal_mode_get(0) == PM_MOM_INV && MSG_NUM(1) == 70 && MSG_ON(1) == 90 &&
          MSG_OFF(1) == 10 && msg_type(2) == MT_CC && MSG_NUM(2) == cc[2] && pedal_mode_get(2) == PM_STOMP);
    CHECK(display == 2 && program == 0);
    ticks(3);
    CHECK(midi_len == 0);

    // The footswitches now send bank 2's messages; switch 3 carries on from on.
    clear_log();
    tap(sw_code(2));
    CHECK(midi_len == 3 && cc_number(0) == 70 && cc_value(0) == 90 && leds[1] == 1);
    clear_log();
    tap(sw_code(3));
    CHECK(midi_len == 3 && cc_number(0) == cc[2] && cc_value(0) == 0 && leds[2] == 0);
    tap(sw_code(3));
    CHECK(leds[2] == 1);

    // Out of bank mode: the display shows the program again and UP sends a program change;
    // bank 2 stays.
    clear_log();
    chord_short();
    CHECK(!bank_mode && aux[HAL_LED_DIRECT_SELECT] == 0 && display == 0 && hal_bank == 1 && midi_len == 0);
    tap(CTL_KEY_UP);
    CHECK(midi_len == 2 && midi_log[0] == PROG_CHG && midi_log[1] == 1 && display == 1 && hal_bank == 1);
    clear_log();
    tap(sw_code(2));
    CHECK(midi_len == 3 && cc_number(0) == 70 && cc_value(0) == 10 && leds[1] == 0);

    // Back into bank mode: the bank (2) is shown, not the program; DOWN to bank 1, sending
    // nothing. Switch 3's CC is still on; switch 1 is a CC no switch of bank 2 has, off.
    clear_log();
    chord_short();
    CHECK(bank_mode && display == 2 && program == 1);
    tap(CTL_KEY_DOWN);
    ticks(3);
    CHECK(hal_bank == 0 && display == 1 && midi_len == 0 && leds[0] == 0 && leds[2] == 1 && stomp_state == 0x004);
    CHECK(msg_type(0) == MT_CC && MSG_NUM(0) == cc[0] && pedal_mode_get(0) == PM_STOMP);

    // At bank 1 DOWN stops (or wraps to the last bank with PROGRAM_WRAP).
    clear_log();
    tap(CTL_KEY_DOWN);
    ticks(3);
#if PROGRAM_WRAP
    CHECK(hal_bank == BANK_COUNT - 1 && display == BANK_COUNT);
    tap(CTL_KEY_UP);
    ticks(3);
#endif
    CHECK(hal_bank == 0 && display == 1 && midi_len == 0 && program == 1 && leds[2] == 1);

    // A bank that was never saved has the default messages in DEFAULT_PEDAL_MODE; the
    // CCs it shares with bank 1 keep bank 1's states, whatever the mode.
    bank_next = 6;
    tap(CTL_KEY_UP);
    ticks(2);
    CHECK(hal_bank == 7 && msg_type(4) == MT_CC && MSG_NUM(4) == cc[4] && MSG_ON(4) == 127 && MSG_OFF(4) == 0 &&
          pedal_mode_get(4) == DEFAULT_PEDAL_MODE && stomp_state == 0x004 && midi_len == 0);
    bank_next = 1;
    tap(CTL_KEY_DOWN);
    ticks(3);
    CHECK(hal_bank == 0 && stomp_state == 0x004);

    // Held UP repeats on the display only; the bank loads once, on release, and stops at
    // the last bank (or wraps).
    clear_log();
    ctl_key_press(CTL_KEY_UP);
    ticks(REPEAT_DELAY_TICKS + 2 * REPEAT_PERIOD_TICKS);
    CHECK(display == 4 && hal_bank == 0 && !bank_busy);
    ctl_key_release(CTL_KEY_UP);
    ticks(1);
    CHECK(hal_bank == 3 && display == 4 && midi_len == 0);
    bank_next = BANK_COUNT - 1;
    tap(CTL_KEY_UP);
    ticks(1);
#if PROGRAM_WRAP
    CHECK(hal_bank == 0 && display == 1);
#else
    CHECK(hal_bank == BANK_COUNT - 1 && display == BANK_COUNT);
#endif

    // The change waits for a pending undo to go out (MIDI OUT full), with the old message;
    // a footswitch press before the change is ignored.
    reset_world();
    stomp_banks(2);
    for (i = 0; i < STOMP_COUNT; i++)
    {
        set_mode((uint8_t)i, PM_STOMP);
    }
    set_mode(0, PM_STOMP_MOM);
    store_bank_mode(0, 0, PM_STOMP_MOM);
    store_bank_msg(1, 0, MT_CC, 71, 127, 0, PM_STOMP);
    tap(sw_code(5));
    chord_short();
    ctl_key_press(sw_code(1));
    ctl_key_hold(sw_code(1));
    clear_log();
    midi_space = 2;
    ctl_key_release(sw_code(1));
    CHECK(stomp_revert == 1 && midi_len == 0);
    tap(CTL_KEY_UP);
    ticks(5);
    CHECK(bank_busy && hal_bank == 0 && midi_len == 0);
    midi_space = 15;
    ctl_key_press(sw_code(7));
    ctl_key_release(sw_code(7));
    // The undo goes out as soon as there is room (here on the key event), with the old
    // bank's message; the press itself is ignored.
    CHECK(bank_busy && midi_len == 3 && cc_number(0) == cc[0] && cc_value(0) == 0 && leds[6] == 0);
    ticks(1);
    CHECK(!bank_busy && hal_bank == 1 && midi_len == 3 && cc_number(0) == cc[0] && cc_value(0) == 0);
    CHECK(leds[0] == 0 && leds[4] == 1 && stomp_state == 0x010);

    // Bank 2's switch 1 is CC 71: a CC 71 from MIDI IN sets it, and its press sends it.
    clear_log();
    ctl_midi_in(CC_STATUS); ctl_midi_in(71); ctl_midi_in(127);
    CHECK(leds[0] == 1 && midi_len == 0);

    // A program change switch that is on turns off (no program change is ever carried); a
    // switch held across the change does nothing when let go, but its CC's state (on, as
    // last sent) carries over. In bank mode a program it sends is kept (and UP / DOWN step
    // from it later) while the display shows the bank. The old bank is read back from the
    // EEPROM, so the program change switch is stored.
    reset_world();
    stomp_banks(2);
    store_bank_msg(0, 0, MT_PROG, 0, 5, 6, PM_STOMP);
    store_bank_mode(0, 1, PM_MOMENTARY);
    ctl_init();
    boot_end();
    for (i = 2; i < STOMP_COUNT; i++)
    {
        set_mode((uint8_t)i, PM_STOMP);
    }
    CHECK(msg_type(0) == MT_PROG && pedal_mode_get(1) == PM_MOMENTARY);
    chord_short();
    clear_log();
    tap(sw_code(1));
    CHECK(midi_len == 2 && midi_log[1] == 5 && program == 5 && display == 1 && leds[0] == 1);
    ctl_key_press(sw_code(2));
    CHECK(leds[1] == 1);
    clear_log();
    tap(CTL_KEY_UP);
    ticks(3);
    CHECK(hal_bank == 1 && leds[0] == 0 && leds[1] == 1 && midi_len == 0);
    ctl_key_release(sw_code(2));
    ticks(2);
    CHECK(midi_len == 0 && leds[1] == 1);
    chord_short();
    CHECK(display == 5);

    // Configuration mode edits the current bank: a save writes bank 2's records (with the
    // modes) and leaves bank 1's alone; the CONFIG LED compares with bank 2.
    reset_world();
    stomp_banks(2);
    store_bank_msg(1, 3, MT_CC, 44, 127, 0, PM_MOMENTARY);
    chord_short();
    tap(CTL_KEY_UP);
    ticks(3);
    CHECK(hal_bank == 1 && pedal_mode_get(3) == PM_MOMENTARY);
    chord_long();
    CHECK(mode == MODE_CONFIG && status_led == 1 && status_toggles(CFG_FLASH_TICKS * 3) == 0);
    tap(sw_code(4));
    tap(sw_code(4));
    CHECK(pedal_mode_get(3) == PM_MOM_INV && status_led == 0);
    save_exit();
    CHECK(mode == MODE_RUN && hal_bank == 1 && display == 2 && aux[HAL_LED_DIRECT_SELECT] == 1);
    CHECK(msg_was_saved[HAL_REC_SW1 + 3] && saved_msg[HAL_REC_SW1 + 3][0] == (MT_CC | (PM_MOM_INV << 2)) &&
          saved_msg[HAL_REC_SW1 + 3][1] == 44);
    for (i = 0; i < STOMP_COUNT; i++)
    {
        CHECK(!msg_was_saved[i] && msg_was_saved[HAL_REC_SW1 + i]);
    }
    // Saved, it comes back with the bank.
    keep_saved_msgs();
    tap(CTL_KEY_DOWN);
    ticks(3);
    CHECK(hal_bank == 0 && MSG_NUM(3) == cc[3]);
    tap(CTL_KEY_UP);
    ticks(3);
    CHECK(hal_bank == 1 && MSG_NUM(3) == 44 && pedal_mode_get(3) == PM_MOM_INV && leds[3] == 1);

    // Over MIDI: NP_BANK (1 to BANK_COUNT) changes bank as UP / DOWN do, out-of-range
    // values are ignored, NP_BANKS reads BANK_COUNT, and a dump waits for the change.
    reset_world();
    stomp_banks(3);
    set_mode(0, PM_STOMP);
    store_bank_msg(2, 0, MT_NOTE, 61, 101, 31, PM_STOMP);
    nrpn(NP_BANK, 0);
    nrpn(NP_BANK, BANK_COUNT + 1);
    nrpn(NP_BANKS, 5);
    ticks(1);
    CHECK(hal_bank == 0 && !bank_busy);
    tap(sw_code(1));
    clear_log();
    nrpn(NP_BANK, 3);
    nrpn(NP_COMMAND, NC_DUMP);
    CHECK(bank_busy && display == 0);
    ticks(1);
    // Switch 1 was on (a CC); bank 3's switch 1 is a note, so it is off. Nothing is sent
    // but the dump, which starts once the change is done.
    CHECK(hal_bank == 2 && msg_type(0) == MT_NOTE && MSG_NUM(0) == 61 && leds[0] == 0);
    CHECK(midi_len == 0 || midi_log[1] == 99);
    clear_log();
    dump_run(1000);
    parse_dump(0);
    CHECK(dump_values[NP_BANK] == 3 && dump_values[NP_BANKS] == BANK_COUNT && dump_values[NP_TYPE] == MT_NOTE &&
          dump_values[NP_NUM] == 61);
    // Saving over MIDI writes the current bank.
    nrpn(NP_COMMAND, NC_SAVE);
    CHECK(msg_was_saved[2 * HAL_REC_SW1] && saved_msg[2 * HAL_REC_SW1][1] == 61 && !msg_was_saved[0]);
}

// A CC or note has one state wherever it is used: a switch that sends it sets every
// switch with the same CC or note (by MIDI IN's rule), sending one message.
static void test_shared_state(void)
{
    int i;

    printf("shared CC and note states\n");
    reset_world();
    for (i = 0; i < STOMP_COUNT; i++)
    {
        set_mode((uint8_t)i, PM_STOMP);
    }
    set_msg(0, MT_CC, 40, 127, 0);
    set_msg(1, MT_CC, 40, 100, 20);
    set_msg(2, MT_NOTE, 60, 100, 0);
    set_msg(3, MT_NOTE, 60, 80, 0);
    set_msg(4, MT_CC, 60, 127, 0);
    set_msg(5, MT_CC, 50, 64, 64);
    set_msg(6, MT_CC, 50, 127, 0);
    ticks(2);
    clear_log();

    // CC 40 127 turns switch 2 on too (127 is neither of its values, and from 64 up).
    tap(sw_code(1));
    CHECK(midi_len == 3 && cc_number(0) == 40 && cc_value(0) == 127 && leds[0] == 1 && leds[1] == 1);
    // Switch 2 is on, so it toggles off and sends its off value 20, which turns switch 1 off.
    clear_log();
    tap(sw_code(2));
    CHECK(midi_len == 3 && cc_number(0) == 40 && cc_value(0) == 20 && leds[0] == 0 && leds[1] == 0);
    CHECK((stomp_state & 3) == 0);

    // Notes: note 60 on from switch 3 lights switch 4; switch 5 (CC 60) is not the same.
    clear_log();
    tap(sw_code(3));
    CHECK(midi_len == 3 && midi_log[0] == NOTE_ON && midi_log[1] == 60 && leds[2] == 1 && leds[3] == 1 && leds[4] == 0);
    clear_log();
    tap(sw_code(4));
    CHECK(midi_len == 3 && midi_log[0] == NOTE_OFF && leds[2] == 0 && leds[3] == 0);

    // A CC whose on and off values are the same (64) keeps its own toggle state; switch 7
    // reads 64 as on.
    clear_log();
    tap(sw_code(6));
    CHECK(midi_len == 3 && cc_number(0) == 50 && cc_value(0) == 64 && leds[5] == 1 && leds[6] == 1);
    tap(sw_code(6));
    CHECK(leds[5] == 0 && leds[6] == 1);
    // Switch 7 off sends 0, below 64: switch 6 (already off) stays off.
    tap(sw_code(7));
    CHECK(leds[5] == 0 && leds[6] == 0);

    // An undo (stomp with momentary, held) is shared too.
    set_mode(0, PM_STOMP_MOM);
    ctl_key_press(sw_code(1));
    CHECK(leds[0] == 1 && leds[1] == 1);
    ctl_key_hold(sw_code(1));
    clear_log();
    ctl_key_release(sw_code(1));
    ticks(2);
    CHECK(midi_len == 3 && cc_value(0) == 0 && leds[0] == 0 && leds[1] == 0);

    // Configuration mode changes no footswitch state through a send.
    tap(sw_code(1));
    CHECK(leds[0] == 1 && leds[1] == 1);
    chord_long();
    cancel_exit();
    CHECK(mode == MODE_RUN && leds[0] == 1 && leds[1] == 1);
}

// Every CC and note keeps its last value across any number of bank changes, used by
// the banks in between or not (the user's case: CC 26 on in bank 1, through a bank
// without CC 26, back to bank 1 still on).
static void test_cc_memory(void)
{
    int i;

    printf("CC and note values kept across banks\n");
    reset_world();
    stomp_banks(3);
    store_bank_msg(0, 0, MT_CC, 26, 127, 0, PM_STOMP);
    // Bank 2 has no CC 26 (its switch 6, normally CC 26, is CC 99), and switch 1 is CC 106.
    store_bank_msg(1, 0, MT_CC, 106, 127, 0, PM_STOMP);
    store_bank_msg(1, 5, MT_CC, 99, 127, 0, PM_STOMP);
    // Bank 3: switch 2 is note 60, switch 3 an inverted momentary CC 50.
    store_bank_msg(2, 1, MT_NOTE, 60, 100, 0, PM_STOMP);
    store_bank_msg(2, 2, MT_CC, 50, 127, 0, PM_MOM_INV);
    ctl_init();
    boot_end();
    ticks(2);

    // Switch 1 (CC 26) on lights switch 6 too (also CC 26 in bank 1).
    tap(sw_code(1));
    CHECK(leds[0] == 1 && leds[5] == 1);
    chord_short();
    clear_log();
    tap(CTL_KEY_UP);
    ticks(2);
    // Bank 2: nothing uses CC 26, so nothing is lit, and nothing was sent.
    CHECK(hal_bank == 1 && stomp_state == 0 && midi_len == 0);
    // Bank 3 and back to 2, then 1: CC 26 comes back on.
    tap(CTL_KEY_UP);
    ticks(2);
    CHECK(hal_bank == 2 && leds[0] == 0 && leds[5] == 1);
    tap(CTL_KEY_DOWN);
    ticks(2);
    tap(CTL_KEY_DOWN);
    ticks(2);
    CHECK(hal_bank == 0 && leds[0] == 1 && leds[5] == 1 && midi_len == 0);

    // A CC received while no switch of the bank uses it is remembered too.
    tap(CTL_KEY_UP);
    ticks(2);
    ctl_midi_in(CC_STATUS); ctl_midi_in(26); ctl_midi_in(0);
    ctl_midi_in(CC_STATUS); ctl_midi_in(stomp_cc[1]); ctl_midi_in(127);
    ctl_midi_in(NOTE_ON); ctl_midi_in(60); ctl_midi_in(90);
    CHECK(hal_bank == 1 && leds[1] == 1 && midi_len == 0);
    tap(CTL_KEY_DOWN);
    ticks(2);
    CHECK(hal_bank == 0 && leds[0] == 0 && leds[5] == 0 && leds[1] == 1);

    // Bank 3: the note (on, from MIDI IN) lights switch 2. Switch 3's CC 50 was never seen,
    // so it rests on (inverted momentary), sending nothing.
    tap(CTL_KEY_UP);
    ticks(2);
    tap(CTL_KEY_UP);
    ticks(2);
    CHECK(hal_bank == 2 && leds[1] == 1 && leds[2] == 1 && midi_len == 0);

    // Its rest state is kept as CC 50's value when the bank is left: a bank with CC 50 as
    // a stomp switch shows it on.
    store_bank_msg(3, 4, MT_CC, 50, 127, 0, PM_STOMP);
    tap(CTL_KEY_UP);
    ticks(2);
    CHECK(hal_bank == 3 && leds[4] == 1 && midi_len == 0);

    // Power-on forgets everything: back to bank 1 at rest.
    ctl_init();
    boot_end();
    for (i = 0; i < STOMP_COUNT; i++)
    {
        CHECK(leds[i] == (pedal_mode_get((uint8_t)i) == PM_MOM_INV));
    }
}

int main(void)
{
    test_stomp_modes();
    test_program_change();
    test_chord_and_config_entry();
    test_config_channel();
    test_config_cancel();
    test_config_channel_digits();
    test_config_modes();
    test_config_page_leds();
    test_config_version();
    test_config_version_jump();
    test_bank_mode_toggle();
    test_config_calibration();
    test_relays();
    test_midi_flash();
    test_midi_in();
    test_exit_restores_leds();
    test_boot_inverted();
    test_boot_animation();
    test_config_led_flash();
    test_config_led_eeprom();
    test_switch_messages();
    test_config_editor();
    test_midi_config();
    test_banks();
    test_shared_state();
    test_cc_memory();
    test_scaling();
    test_pedal_cc();
    test_pedal_noise();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
