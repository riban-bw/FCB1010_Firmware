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

// ---------------------------------------------------------------
// Fake hardware
// ---------------------------------------------------------------

#define LOG_MAX 4096

static uint8_t  midi_log[LOG_MAX];
static int      midi_len;
static int      midi_space = 15;
static uint8_t  leds[10];
static uint8_t  aux[4];
static uint8_t  status_led;
static uint8_t  display;
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
    cal_saved = 0;
    cal_stored_valid = 0;
    cfg_saved = 0;
    cfg_stored_valid = 0;
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

static void set_mode(uint8_t sw, uint8_t m)
{
    pedal_mode_set(sw, m);
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

    // Stomp with momentary: On, then a long press turns it off and releasing puts it back on.
    reset_world();
    tap(0);
    clear_log();
    ctl_key_press(0);
    CHECK(midi_len == 3 && cc_value(0) == 0 && leds[0] == 0);
    ctl_key_hold(0);
    ctl_key_release(0);
    CHECK(midi_len == 6 && cc_value(1) == 127 && leds[0] == 1);

    // Released before the threshold, it simply stays off.
    reset_world();
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
    CHECK(midi_len == 3 && cc_number(0) == 22 && cc_value(0) == 127 && leds[2] == 1);
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
    ctl_midi_in(20);
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
    CHECK(midi_len == 0 && leds[0] == 0);

    // A revert that cannot be sent at release is sent from the tick once there is room.
    reset_world();
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

    // Wrap-around.
    clear_log();
    tap(CTL_KEY_DOWN);
    CHECK(midi_len == 2 && midi_log[1] == 127 && display == 127);
    clear_log();
    tap(CTL_KEY_UP);
    CHECK(midi_len == 2 && midi_log[1] == 0 && display == 0);

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

    // DOWN repeats the other way and wraps.
    reset_world();
    clear_log();
    ctl_key_press(CTL_KEY_DOWN);
    ticks(REPEAT_DELAY_TICKS);
    ticks(REPEAT_PERIOD_TICKS);
    CHECK(display == 126 && midi_len == 0);
    ctl_key_release(CTL_KEY_DOWN);
    CHECK(midi_len == 2 && midi_log[1] == 126);

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
    CHECK(display == MIDI_CHANNEL && aux[HAL_LED_MIDI_CHAN] == 1);
    chord_up();
    CHECK(mode == MODE_CONFIG && status_led == 1);
    CHECK(midi_len == 0);

    // A chord that is let go early in run mode does nothing at all.
    reset_world();
    clear_log();
    chord_short();
    CHECK(mode == MODE_RUN && midi_len == 0 && display == 0);

    // Once the chord has opened config mode, UP and DOWN stay quiet until both are let go.
    reset_world();
    chord_down();
    ticks(CHORD_TICKS + 3 * REPEAT_DELAY_TICKS);
    CHECK(mode == MODE_CONFIG && display == MIDI_CHANNEL);
    ctl_key_release(CTL_KEY_UP);
    CHECK(display == MIDI_CHANNEL);
    ctl_key_release(CTL_KEY_DOWN);
    CHECK(display == MIDI_CHANNEL && midi_len == 0);

    // Pressing the second key late still counts as a chord, and a single key before
    // it does not send a program change.
    reset_world();
    ctl_key_press(CTL_KEY_UP);
    ticks(10);
    ctl_key_press(CTL_KEY_DOWN);
    ticks(CHORD_TICKS);
    CHECK(mode == MODE_CONFIG);
    chord_up();
    CHECK(midi_len == 0 && display == MIDI_CHANNEL);

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
    CHECK(midi_len == 3 && cc_number(0) == 20 && cc_value(0) == 0);
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
    printf("config: MIDI channel\n");
    reset_world();
    chord_long();
    CHECK(display == 1);

    tap(CTL_KEY_UP);
    CHECK(display == 2);
    tap(CTL_KEY_UP);
    CHECK(display == 3);
    tap(CTL_KEY_DOWN);
    CHECK(display == 2);
    tap(CTL_KEY_DOWN);
    tap(CTL_KEY_DOWN);
    CHECK(display == 16);
    tap(CTL_KEY_UP);
    CHECK(display == 1);

    // Holding repeats through the channels.
    ctl_key_press(CTL_KEY_UP);
    ticks(REPEAT_DELAY_TICKS + 2 * REPEAT_PERIOD_TICKS);
    CHECK(display == 4);
    ctl_key_release(CTL_KEY_UP);
    CHECK(display == 4);

    // No program change is sent in config mode, and the program number is untouched.
    CHECK(midi_len == 0 && program == 0);

    // Hold UP + DOWN to leave config mode. The settings and calibration are saved, the
    // CONFIG and page LEDs go out, and the display shows the program number again.
    chord_long();
    CHECK(mode == MODE_RUN && status_led == 0 && display == 0);
    CHECK(cfg_saved == 1 && saved_cfg[0] == 4);
    CHECK(aux[0] == 0 && aux[1] == 0 && aux[2] == 0 && aux[3] == 0);
    CHECK(cal_saved == 1);

    // Everything now uses channel 4.
    clear_log();
    tap(0);
    CHECK(midi_len == 3 && cc_status(0) == 0xB3);
    clear_log();
    tap(CTL_KEY_UP);
    CHECK(midi_len == 2 && midi_log[0] == 0xC3);

    // MIDI IN follows the new channel and ignores the old one.
    ctl_midi_in(0xB3);
    ctl_midi_in(21);
    ctl_midi_in(127);
    CHECK(leds[1] == 1);
    ctl_midi_in((uint8_t)(0xB0 | (MIDI_CHANNEL - 1)));
    ctl_midi_in(22);
    ctl_midi_in(127);
    CHECK(leds[2] == 0);

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
}

static void test_config_modes(void)
{
    int i;

    printf("config: pedal modes\n");
    reset_world();
    chord_long();
    chord_short();
    CHECK(display == 12 && aux[HAL_LED_MIDI_FUNC] == 1 && aux[HAL_LED_MIDI_CHAN] == 0);
    CHECK(leds[0] == 1);

    // Default mode is 2 (stomp with momentary), shown as the last digit + 1.
    CHECK(pedal_mode_get(0) == PM_STOMP_MOM);

    // Choose footswitch 3 (key code 2): Its number and mode are shown, its LED lit alone.
    tap(2);
    CHECK(cfg_sel == 2);
    CHECK(display == 3 * 10 + PM_STOMP_MOM + 1);
    CHECK(leds[2] == 1 && leds[0] == 0);

    // UP cycles stomp -> stomp+momentary -> momentary -> stomp.
    tap(CTL_KEY_UP);
    CHECK(pedal_mode_get(2) == PM_MOMENTARY && display == 33);
    tap(CTL_KEY_UP);
    CHECK(pedal_mode_get(2) == PM_STOMP && display == 31);
    tap(CTL_KEY_DOWN);
    CHECK(pedal_mode_get(2) == PM_MOMENTARY && display == 33);
    tap(CTL_KEY_DOWN);
    tap(CTL_KEY_DOWN);
    CHECK(pedal_mode_get(2) == PM_STOMP && display == 31);

    // Other pedals are unaffected.
    for (i = 0; i < STOMP_COUNT; i++)
    {
        if (i != 2)
        {
            CHECK(pedal_mode_get(i) == PM_STOMP_MOM);
        }
    }

    // Footswitch 10 (key code 10): Last switch, uses the third byte of settings.
    tap(10);
    CHECK(cfg_sel == 9 && display == 10 * 10 + 2);
    tap(CTL_KEY_UP);
    CHECK(pedal_mode_get(9) == PM_MOMENTARY);
    CHECK(pedal_mode_get(8) == PM_STOMP_MOM && pedal_mode_get(7) == PM_STOMP_MOM);

    // Leave, then check the saved bytes round-trip into a fresh controller.
    chord_long();
    CHECK(mode == MODE_RUN && cfg_saved == 1);
    memcpy(stored_cfg, saved_cfg, 4);
    cfg_stored_valid = 1;
    ctl_init();
    CHECK(pedal_mode_get(2) == PM_STOMP);
    CHECK(pedal_mode_get(9) == PM_MOMENTARY);
    CHECK(pedal_mode_get(0) == PM_STOMP_MOM && pedal_mode_get(5) == PM_STOMP_MOM);

    // The new modes behave: 3 is a plain stomp, 10 is momentary.
    clear_log();
    long_press(2);
    CHECK(midi_len == 3 && cc_value(0) == 127);
    clear_log();
    ctl_key_press(10);
    ctl_key_release(10);
    CHECK(midi_len == 6 && cc_number(0) == 29 && cc_value(0) == 127 && cc_value(1) == 0);

    // A mode value of 3 is not valid and keeps the default.
    stored_cfg[1] = 0xFF;
    ctl_init();
    CHECK(pedal_mode_get(0) == DEFAULT_PEDAL_MODE);

    // Changing the mode of a stomp that is on turns it off when leaving config mode.
    reset_world();
    tap(4);
    CHECK(leds[4] == 1);
    chord_long();
    chord_short();
    tap(4);
    tap(CTL_KEY_UP);
    clear_log();
    chord_long();
    ticks(1);
    CHECK(midi_len == 3 && cc_number(0) == 24 && cc_value(0) == 0);
    CHECK(leds[4] == 0 && mode == MODE_RUN);

    // A stomp that is on and not edited stays on.
    reset_world();
    tap(4);
    chord_long();
    chord_short();
    tap(2);
    tap(CTL_KEY_UP);
    clear_log();
    chord_long();
    ticks(1);
    CHECK(midi_len == 0 && leds[4] == 1);

    // Pressing a footswitch on the modes page does not send MIDI.
    reset_world();
    chord_long();
    chord_short();
    clear_log();
    tap(5 + 1);
    CHECK(midi_len == 0 && cfg_sel == 5);

    // Footswitches do nothing on the channel page.
    reset_world();
    chord_long();
    clear_log();
    tap(3);
    CHECK(midi_len == 0 && display == MIDI_CHANNEL);
}

static void test_config_calibration(void)
{
    printf("config: calibration\n");
    reset_world();
    chord_long();
    chord_short();
    chord_short();
    CHECK(aux[HAL_LED_EXP_1] == 1 && aux[HAL_LED_EXP_2] == 1 && aux[HAL_LED_MIDI_FUNC] == 0);
    CHECK(display == CAL_HEEL);

    // Heel reading, then toe reading, each taken with UP.
    pedal(0, 20);
    pedal(1, 30);
    tap(CTL_KEY_UP);
    CHECK(display == CAL_TOE);
    pedal(0, 220);
    pedal(1, 230);
    tap(CTL_KEY_UP);
    CHECK(display == CAL_DONE);

    // Holding UP does not auto-repeat on this page.
    ctl_key_press(CTL_KEY_UP);
    ticks(REPEAT_DELAY_TICKS + 3 * REPEAT_PERIOD_TICKS);
    CHECK(display == CAL_DONE);
    ctl_key_release(CTL_KEY_UP);
    CHECK(display == CAL_HEEL);
    tap(CTL_KEY_DOWN);
    CHECK(display == CAL_HEEL);

    // Do it again properly and save.
    pedal(0, 20);
    pedal(1, 30);
    tap(CTL_KEY_UP);
    pedal(0, 220);
    pedal(1, 230);
    tap(CTL_KEY_UP);
    chord_long();
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

    // A pedal that did not move enough gives the error display and starts over.
    reset_world();
    chord_long();
    chord_short();
    chord_short();
    pedal(0, 100);
    pedal(1, 100);
    tap(CTL_KEY_UP);
    pedal(0, 100 + CAL_MIN_SPAN - 1);
    pedal(1, 200);
    tap(CTL_KEY_UP);
    CHECK(display == 88);
    chord_long();
    CHECK(saved_heel[0] == 0 && saved_toe[0] == 255);

    // Only one pedal moving is also an error.
    reset_world();
    chord_long();
    chord_short();
    chord_short();
    pedal(0, 10);
    pedal(1, 10);
    tap(CTL_KEY_UP);
    pedal(0, 200);
    pedal(1, 12);
    tap(CTL_KEY_UP);
    CHECK(display == 88);

    // DOWN restarts at the heel step without saving anything.
    reset_world();
    chord_long();
    chord_short();
    chord_short();
    pedal(0, 10);
    pedal(1, 10);
    tap(CTL_KEY_UP);
    CHECK(display == CAL_TOE);
    tap(CTL_KEY_DOWN);
    CHECK(display == CAL_HEEL);

    // A wired-the-other-way pedal (toe below heel) is accepted.
    reset_world();
    chord_long();
    chord_short();
    chord_short();
    pedal(0, 200);
    pedal(1, 200);
    tap(CTL_KEY_UP);
    pedal(0, 20);
    pedal(1, 20);
    tap(CTL_KEY_UP);
    CHECK(display == CAL_DONE);
    chord_long();
    CHECK(saved_heel[0] == 200 && saved_toe[0] == 20);

    // The short chord wraps from the calibration page back to the channel page.
    reset_world();
    chord_long();
    chord_short();
    chord_short();
    chord_short();
    CHECK(cfg_page == PAGE_CHANNEL && display == MIDI_CHANNEL);
    CHECK(aux[HAL_LED_EXP_1] == 0 && aux[HAL_LED_MIDI_CHAN] == 1);

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
    CHECK(leds[0] == 0);

    // Real-time bytes in the middle of a message do not break it.
    ctl_midi_in(CC_STATUS); ctl_midi_in(cc[1]); ctl_midi_in(0xF8); ctl_midi_in(127);
    CHECK(leds[1] == 1);

    // Remote changes while in config mode are ignored so the LEDs keep showing settings.
    reset_world();
    chord_long();
    ctl_midi_in(CC_STATUS); ctl_midi_in(cc[5]); ctl_midi_in(127);
    chord_up();
    chord_long();
    CHECK(leds[5] == 0 && mode == MODE_RUN);
}

static void test_exit_restores_leds(void)
{
    printf("LEDs after config\n");
    reset_world();
    tap(0);
    tap(3);
    chord_long();
    chord_short();
    chord_long();
    CHECK(leds[0] == 1 && leds[3] == 1 && leds[1] == 0);
    CHECK(status_led == 0);
}

int main(void)
{
    test_stomp_modes();
    test_program_change();
    test_chord_and_config_entry();
    test_config_channel();
    test_config_modes();
    test_config_calibration();
    test_midi_in();
    test_exit_restores_leds();
    test_scaling();
    test_pedal_cc();
    test_pedal_noise();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
