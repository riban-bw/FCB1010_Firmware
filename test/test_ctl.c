// Host-side tests for the controller logic (src/ctl.c).
// The hardware layer is replaced by recorders so MIDI output, LEDs and the
// display can be checked exactly. Build and run with: make test

#include <stdio.h>
#include <stdlib.h>
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
static uint8_t  aux[7];
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

uint8_t hal_cal_load(uint8_t *heel, uint8_t *toe)
{
    if (!cal_stored_valid)
    {
        return 0;
    }

    heel[0] = stored_heel[0];
    heel[1] = stored_heel[1];
    toe[0]  = stored_toe[0];
    toe[1]  = stored_toe[1];

    return 1;
}

uint8_t hal_cal_save(const uint8_t *heel, const uint8_t *toe)
{
    cal_saved++;
    saved_heel[0] = heel[0];
    saved_heel[1] = heel[1];
    saved_toe[0]  = toe[0];
    saved_toe[1]  = toe[1];

    return 1;
}

uint8_t hal_cfg_load(uint8_t *data)
{
    if (!cfg_stored_valid)
    {
        return 0;
    }

    memcpy(data, stored_cfg, 4);
    return 1;
}

uint8_t hal_cfg_save(const uint8_t *data)
{
    cfg_saved++;
    memcpy(saved_cfg, data, 4);
    return 1;
}

void hal_relays(uint8_t mask)
{
    relays = mask;
}

uint8_t hal_sw_load(uint8_t relay, uint8_t *data)
{
    if (!sw_stored_valid[relay])
    {
        return 0;
    }

    memcpy(data, stored_sw[relay], 4);
    return 1;
}

uint8_t hal_sw_save(uint8_t relay, const uint8_t *data)
{
    sw_saved++;
    memcpy(saved_sw[relay], data, 4);
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
    ctl_init();
}

static void clear_log(void)
{
    midi_len = 0;
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

    // A chord that is let go early in run mode only toggles CHORD_CC (see test_chord_cc).
    reset_world();
    clear_log();
    chord_short();
    CHECK(mode == MODE_RUN && midi_len == 3 && cc_number(0) == CHORD_CC && display == 0);

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
    // hold events and the release do nothing more (no CHORD_CC toggle, no program).
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
    CHECK(mode == MODE_RUN && midi_len == 0 && program == 0 && chord_cc_on == 0);
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
    memcpy(stored_cfg, saved_cfg, 4);
    cfg_stored_valid = 1;
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

    // Every 2-bit value is a mode now: 0xFF is inverted momentary for switches 1-4,
    // and unused high bits of the last byte are ignored.
    stored_cfg[1] = 0xFF;
    stored_cfg[3] = 0xF0;
    ctl_init();
    CHECK(pedal_mode_get(0) == PM_MOM_INV && pedal_mode_get(3) == PM_MOM_INV);
    CHECK(pedal_mode_get(8) == PM_STOMP && pedal_mode_get(9) == PM_STOMP && pedal_modes[2] == 0);

    // Changing a switch to inverted momentary asserts it on saving: its state is set and
    // its LED lights, whether it was off (switch 2) or on (switch 3), but no CC is sent.
    // Cancelling does not.
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
    CHECK(midi_len == 0);
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
    // Switch 3 cycles to stomp + momentary (not inverted momentary, which would send 127).
    set_mode(2, PM_STOMP);
    tap(4);
    chord_long();
    tap(2);
    tap(2);
    clear_log();
    save_exit();
    ticks(1);
    CHECK(midi_len == 0 && leds[4] == 1);

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
    CHECK(cfg_page == PAGE_VERSION && aux[HAL_LED_SWITCH_2] == 0 && lamp == HAL_LAMP_ALL_ON);
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

// Page 7: every LED and segment blinks LAMP_TEST_BLINKS times, then the version shows as
// "major." / ".minor." / ".build", VERSION_SHOW_MS each, repeating (no more blinks).
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
    CHECK(display == 0 && blank == 7 && dots == 0);

    for (k = 0; k < 7; k++)
    {
        CHECK(aux[k] == 0);
    }

    for (k = 0; k < STOMP_COUNT; k++)
    {
        CHECK(leds[k] == 0);
    }

    // Three blinks: on, off, on, off, on, off, one step each.
    for (k = 0; k < VER_BLINK_STEPS; k++)
    {
        CHECK(lamp == ((k & 1) ? HAL_LAMP_ALL_OFF : HAL_LAMP_ALL_ON));
        ticks(VER_STEP_TICKS - 1);
        CHECK(lamp == ((k & 1) ? HAL_LAMP_ALL_OFF : HAL_LAMP_ALL_ON));
        ticks(1);
    }

    CHECK(VER_BLINK_STEPS == 6 && VER_STEP_TICKS * TICK_MS == 250);
    // The LEDs stay lit after the blinks.
    CHECK(lamp == HAL_LAMP_LEDS_ON && display == SHOWN_HEX(fw_version[0]) && dots == 0x01);
    ticks(VER_STEP_TICKS * VER_ITEM_STEPS - 1);
    CHECK(display == SHOWN_HEX(fw_version[0]) && dots == 0x01);
    ticks(1);
    CHECK(display == SHOWN_HEX(fw_version[1]) && dots == 0x05);
    ticks(VER_STEP_TICKS * VER_ITEM_STEPS);
    CHECK(display == SHOWN_HEX(fw_version[2]) && dots == 0x04 && lamp == HAL_LAMP_LEDS_ON);
    // Then a blank display (no dots, LEDs still lit) for one part's time, then major.
    ticks(VER_STEP_TICKS * VER_ITEM_STEPS);
    CHECK(display == 0 && blank == 7 && dots == 0 && lamp == HAL_LAMP_LEDS_ON);
    ticks(VER_STEP_TICKS * VER_ITEM_STEPS - 1);
    CHECK(display == 0 && blank == 7 && dots == 0);
    ticks(1);
    CHECK(display == SHOWN_HEX(fw_version[0]) && dots == 0x01 && lamp == HAL_LAMP_LEDS_ON);
    CHECK(midi_len == 0);

    // UP on the last page does nothing; a footswitch runs it all again, sending nothing.
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_VERSION && display == SHOWN_HEX(fw_version[0]));
    tap(3);
    CHECK(lamp == HAL_LAMP_ALL_ON && dots == 0 && blank == 7 && leds[2] == 0 && midi_len == 0);
    ticks(VER_STEP_TICKS);
    CHECK(lamp == HAL_LAMP_ALL_OFF);

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
    CHECK(lamp == HAL_LAMP_ALL_ON);
    save_exit();
    CHECK(mode == MODE_RUN && lamp == HAL_LAMP_NORMAL && dots == 0 && display == 0);
    ticks(VER_STEP_TICKS * 2);
    CHECK(lamp == HAL_LAMP_NORMAL);
    chord_long();
    for (k = 0; k < 6; k++)
    {
        tap(CTL_KEY_UP);
    }
    ticks(VER_STEP_TICKS * VER_BLINK_STEPS + 1);
    CHECK(display == SHOWN_HEX(fw_version[0]) && dots == 0x01);
    chord_long();
    CHECK(mode == MODE_RUN && lamp == HAL_LAMP_NORMAL && dots == 0 && display == 0);
}

// A short UP + DOWN press in run mode toggles CHORD_CC (127, then 0) and the DIRECT
// SELECT LED; it changes no program, and config mode leaves it alone.
static void test_chord_cc(void)
{
    printf("run mode: UP + DOWN tap toggles CHORD_CC\n");
    reset_world();
    CHECK(aux[HAL_LED_DIRECT_SELECT] == 0);
    clear_log();
    chord_short();
    CHECK(mode == MODE_RUN && midi_len == 3);
    CHECK(cc_status(0) == CC_STATUS && cc_number(0) == CHORD_CC && cc_value(0) == 127);
    CHECK(aux[HAL_LED_DIRECT_SELECT] == 1 && display == 0 && program == 0);
    clear_log();
    chord_short();
    CHECK(midi_len == 3 && cc_number(0) == CHORD_CC && cc_value(0) == 0);
    CHECK(aux[HAL_LED_DIRECT_SELECT] == 0);

    // The second key pressed late still makes a chord; nothing is stepped.
    clear_log();
    ctl_key_press(CTL_KEY_DOWN);
    ticks(10);
    ctl_key_press(CTL_KEY_UP);
    ticks(10);
    ctl_key_release(CTL_KEY_DOWN);
    ctl_key_release(CTL_KEY_UP);
    CHECK(midi_len == 3 && cc_number(0) == CHORD_CC && cc_value(0) == 127 && program == 0);

    // Opening and leaving config mode sends no CHORD_CC. DIRECT SELECT is dark while
    // editing and shows the kept state again on leaving (save or cancel). An UP + DOWN
    // tap in config mode does not toggle it either.
    clear_log();
    chord_long();
    CHECK(mode == MODE_CONFIG && aux[HAL_LED_DIRECT_SELECT] == 0 && chord_cc_on == 1);
    chord_short();
    CHECK(mode == MODE_CONFIG && chord_cc_on == 1 && aux[HAL_LED_DIRECT_SELECT] == 0);
    tap(CTL_KEY_UP);
    CHECK(aux[HAL_LED_DIRECT_SELECT] == 0);
    save_exit();
    CHECK(mode == MODE_RUN && aux[HAL_LED_DIRECT_SELECT] == 1);
    chord_long();
    cancel_exit();
    CHECK(mode == MODE_RUN && aux[HAL_LED_DIRECT_SELECT] == 1);
    CHECK(midi_len == 0);

    // With CHORD_CC off, the LED stays dark after leaving.
    chord_short();
    CHECK(chord_cc_on == 0 && aux[HAL_LED_DIRECT_SELECT] == 0);
    chord_long();
    save_exit();
    CHECK(aux[HAL_LED_DIRECT_SELECT] == 0);
    // Back on for the checks below.
    chord_short();
    CHECK(chord_cc_on == 1 && aux[HAL_LED_DIRECT_SELECT] == 1);
    clear_log();

    // With MIDI OUT backed up the tap is ignored entirely.
    midi_space = 2;
    chord_short();
    CHECK(midi_len == 0 && aux[HAL_LED_DIRECT_SELECT] == 1);
    midi_space = 15;

    // A power-on starts with it off.
    ctl_init();
    CHECK(aux[HAL_LED_DIRECT_SELECT] == 0 && chord_cc_on == 0);
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
    CHECK(leds[2] == 0 && chord_cc_on == 0);

    // Page 5: footswitch 3 and CHORD_CC, all LEDs showing the settings.
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
    CHECK(mode == MODE_CONFIG && cfg_page == PAGE_SW1 && chord_cc_on == 0 && midi_len == 0);

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

    // Saving stores both records: mask low, mask high (bit 2 = CHORD_CC), A, B.
    save_exit();
    CHECK(sw_saved == 2);
    CHECK(saved_sw[0][0] == 4 && saved_sw[0][1] == 4 && saved_sw[0][2] == v && saved_sw[0][3] == 0);
    CHECK(saved_sw[1][0] == 0 && saved_sw[1][1] == 2 && saved_sw[1][2] == 0 && saved_sw[1][3] == ctl_scale(64, 0, 255));
    CHECK(aux[HAL_LED_SWITCH_1] == 0 && aux[HAL_LED_SWITCH_2] == 0 && aux[HAL_LED_DIRECT_SELECT] == 0);

    // Run mode, relay 1: footswitch 3 OR CHORD_CC OR pedal A at v and up.
    pedal(0, 0);
    pedal(1, 0);
    ticks(1);
    CHECK(relays == 0 && aux[HAL_LED_SWITCH_1] == 0);
    tap(2);
    ticks(1);
    CHECK(relays == 1 && aux[HAL_LED_SWITCH_1] == 1 && aux[HAL_LED_SWITCH_2] == 0);
    chord_short();
    ticks(1);
    CHECK(relays == 1 && chord_cc_on == 1);
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

    // CHORD_CC sets the DIRECT SELECT state and LED, sends nothing, and the next UP + DOWN
    // tap toggles from that state. Config mode ignores it.
    clear_log();
    ctl_midi_in(CC_STATUS); ctl_midi_in(CHORD_CC); ctl_midi_in(127);
    CHECK(chord_cc_on == 1 && aux[HAL_LED_DIRECT_SELECT] == 1 && midi_len == 0);
    ctl_midi_in(CC_STATUS); ctl_midi_in(CHORD_CC); ctl_midi_in(0);
    CHECK(chord_cc_on == 0 && aux[HAL_LED_DIRECT_SELECT] == 0 && midi_len == 0);
    ctl_midi_in(CC_STATUS); ctl_midi_in(CHORD_CC); ctl_midi_in(100);
    CHECK(chord_cc_on == 1 && aux[HAL_LED_DIRECT_SELECT] == 1);
    chord_short();
    CHECK(chord_cc_on == 0 && midi_len == 3 && cc_number(0) == CHORD_CC && cc_value(0) == 0);
    chord_long();
    ctl_midi_in(CC_STATUS); ctl_midi_in(CHORD_CC); ctl_midi_in(127);
    CHECK(chord_cc_on == 0 && aux[HAL_LED_DIRECT_SELECT] == 0);
    cancel_exit();

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
    ticks(VER_STEP_TICKS * VER_BLINK_STEPS);
    CHECK(lamp == HAL_LAMP_LEDS_ON && status_toggles(CFG_FLASH_TICKS * 4) == 4);
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
    ticks(VER_STEP_TICKS * VER_BLINK_STEPS);
    CHECK(cfg_page == PAGE_VERSION && lamp == HAL_LAMP_LEDS_ON && status_led == 1 && status_toggles(CFG_FLASH_TICKS * 4) == 0);
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
    stored_cfg[1] = (uint8_t)((PM_MOM_INV << 2) | (PM_MOMENTARY << 4));
    stored_cfg[2] = 0xFF;
    stored_cfg[3] = 0x00;
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
    cfg_stored_valid = 1;
    stored_cfg[0] = MIDI_CHANNEL;
    stored_cfg[1] = (uint8_t)(PM_MOM_INV << 2);
    stored_cfg[2] = 0;
    stored_cfg[3] = 0;
    ctl_init();
    ticks(1);
    CHECK(relays == 1 && aux[HAL_LED_SWITCH_1] == 1 && midi_len == 0);
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
    test_chord_cc();
    test_config_calibration();
    test_relays();
    test_midi_flash();
    test_midi_in();
    test_exit_restores_leds();
    test_boot_inverted();
    test_config_led_flash();
    test_scaling();
    test_pedal_cc();
    test_pedal_noise();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
