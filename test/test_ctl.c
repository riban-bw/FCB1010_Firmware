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

// The misc toggles, one byte in ctl.c (opt_misc), as flags for the checks.
#define opt_metro   ((opt_misc & OPT_METRO) != 0)
#define opt_runstat ((opt_misc & OPT_RUNSTAT) != 0)
#define opt_asense  ((opt_misc & OPT_ASENSE) != 0)

static void set_opt(uint8_t bit, int on)
{
    opt_misc = (uint8_t)(on ? (opt_misc | bit) : (opt_misc & ~bit));
}

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
// The per-bank records, eleven per bank: footswitch n (type with the pedal mode in bits
// 2-3, the relay follows in bits 4-5 and a piece of the pedal settings in bits 6-7,
// number, on value, off value) at b * 11 + n, then the bank record (the four relay
// triggers, more pedal settings in their bit 7) at b * 11 + 10. So 0-10 are bank 1.
#define BANK_RECS HAL_REC_CAL
#define MSG_RECS (BANK_COUNT * BANK_RECS)
#define MSG_REC(rec) (hal_bank * BANK_RECS + (rec))
static int      msg_stored_valid[MSG_RECS], msg_was_saved[MSG_RECS];
// Set: a footswitch or bank record saved is also what loads (otherwise saves are kept apart).
static int      eeprom_keeps;
static uint8_t  stored_msg[MSG_RECS][4], saved_msg[MSG_RECS][4];
// Relay outputs, and how many bank records were saved.
static uint8_t  relays;
static int      bank_saved;
// Pedal range records saved (HAL_REC_RANGE).
static int      range_saved;
static uint8_t  status_led;
static int      display;
static uint8_t  blank;
#define SHOWN_ER (-1)
#define SHOWN_LO (-2)
#define SHOWN_HI (-3)
// Any hal_display_text(t), e.g. the "riban" frames HAL_TEXT_SCROLL + n.
#define SHOWN_TEXT(t) (-1 - (int)(t))
#define SHOWN_MODE(m) SHOWN_TEXT(HAL_TEXT_MODE + (m))
// hal_display_hex(v) is recorded as display = SHOWN_HEX(v); dots and the lamp test too.
#define SHOWN_HEX(v) (0x1000 + (v))
// Version part i as page 8 shows it: major and minor in decimal (two BCD digits, or
// hal_display's number from 100), the build in hex.
#define VER_BCD(v) (((v) / 10) * 16 + (v) % 10)
#define VER_SHOWN(i) ((i) == 2 ? SHOWN_HEX(fw_version[2]) : fw_version[i] >= 100 ? (int)fw_version[i] : SHOWN_HEX(VER_BCD(fw_version[i])))
// hal_display_signed(v), with v as a signed value -127..127.
#define SHOWN_SIGNED(v) (0x2000 + (v))
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

void hal_display_signed(uint8_t value)
{
    display = SHOWN_SIGNED((int)(int8_t)value);
    blank = 0;
}

void hal_display_dots(uint8_t mask)
{
    dots = mask;
}

// The LED brightness last set (hal_led_level).
static uint8_t hw_level = 127;

void hal_led_level(uint8_t level)
{
    hw_level = level;
}

// The metronome's sign (HAL_SIGN_*), and how many times it changed.
static uint8_t disp_sign;
static int     sign_changes;

void hal_display_sign(uint8_t sign)
{
    if (sign != disp_sign)
    {
        sign_changes++;
    }

    disp_sign = sign;
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

    if (rec < HAL_REC_CAL)
    {
        if (!msg_stored_valid[MSG_REC(rec)])
        {
            return 0;
        }

        memcpy(hal_rec, stored_msg[MSG_REC(rec)], 4);
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
    if (rec < HAL_REC_CAL)
    {
        if (rec == HAL_REC_BANK)
        {
            bank_saved++;
        }
        else if (rec == HAL_REC_RANGE)
        {
            range_saved++;
        }
        else
        {
            msg_saved++;
        }

        msg_was_saved[MSG_REC(rec)] = 1;
        memcpy(saved_msg[MSG_REC(rec)], hal_rec, 4);

        // As a real EEPROM: what is saved is what loads.
        if (eeprom_keeps)
        {
            memcpy(stored_msg[MSG_REC(rec)], hal_rec, 4);
            msg_stored_valid[MSG_REC(rec)] = 1;
        }
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

// ctl_init without the identity reply it queues for the first tick (test_sysex_identity
// covers that), so a test's first MIDI OUT bytes are its own.
static void power_on(void)
{
    ctl_init();
    sx_reply = 0;
}

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
    memset(stored_cfg, 0, sizeof(stored_cfg));
    relays = 0xEE;
    bank_saved = 0;
    range_saved = 0;
    msg_saved = 0;
    memset(msg_stored_valid, 0, sizeof(msg_stored_valid));
    memset(msg_was_saved, 0, sizeof(msg_was_saved));
    disp_sign = HAL_SIGN_NONE;
    power_on();
    sign_changes = 0;
    // Skip the power-on animation (test_boot_animation covers it).
    boot_end();
}

// Program mode (DIRECT SELECT lit), as after a short UP + DOWN: UP / DOWN step the program.
// Power-on is bank mode. Nothing is sent for it.
static void program_mode(void)
{
    if (!pc_mode)
    {
        pc_mode_toggle();
    }
}

static void clear_log(void)
{
    midi_len = 0;
}

// Store pedal mode m for footswitch sw (0-9) of bank b (0-based) in the fake EEPROM, in
// its record's type byte (bits 2-3), keeping a stored message or else the default one.
// The packed pedal settings of a bank never saved: no program change, then pedal A's and
// B's CCs with their pickup bits (see rec_pack).
static uint8_t def_y(int j)
{
    return bank_def[j];
}

// Footswitch record n's bits 6-7 for the packed pedal settings y[3].
static uint8_t piece_of(const uint8_t *y, int n)
{
    return (uint8_t)(((y[n >> 2] >> ((n & 3) * 2)) & 3) << 6);
}

// Make bank b's stored records valid where they are not, as the defaults would load
// (default messages and modes, no follows, the default pedal settings, no triggers).
static void store_bank_defaults(int b)
{
    uint8_t y[3];
    int n;
    int r;

    y[0] = def_y(0);
    y[1] = def_y(1);
    y[2] = def_y(2);

    for (n = 0; n < STOMP_COUNT; n++)
    {
        r = b * BANK_RECS + n;

        if (!msg_stored_valid[r])
        {
            msg_stored_valid[r] = 1;
            stored_msg[r][0] = (uint8_t)(MT_CC | (DEFAULT_PEDAL_MODE << 2) | piece_of(y, n));
            stored_msg[r][1] = stomp_cc[n];
            stored_msg[r][2] = 127;
            stored_msg[r][3] = 0;
        }
    }

    r = b * BANK_RECS + HAL_REC_BANK;

    if (!msg_stored_valid[r])
    {
        msg_stored_valid[r] = 1;

        for (n = 0; n < 4; n++)
        {
            stored_msg[r][n] = (uint8_t)(((y[2] >> (4 + n)) & 1) << 7);
        }
    }
}

static void store_bank_mode(int b, int sw, uint8_t m)
{
    int r = b * BANK_RECS + sw;

    if (!msg_stored_valid[r])
    {
        uint8_t y[3] = { def_y(0), def_y(1), def_y(2) };

        msg_stored_valid[r] = 1;
        stored_msg[r][0] = (uint8_t)(MT_CC | piece_of(y, sw));
        stored_msg[r][1] = stomp_cc[sw];
        stored_msg[r][2] = 127;
        stored_msg[r][3] = 0;
    }

    stored_msg[r][0] = (uint8_t)((stored_msg[r][0] & 0xF3) | (m << 2));
}

// Relay r's saved settings for bank b, in the old shared record's form: follow mask low
// byte, high byte, pedal A trigger, pedal B trigger (from the per-bank records).
static uint8_t saved_sw_byte(int b, int r, int k)
{
    int n;
    unsigned m = 0;

    if (k >= 2)
    {
        return (uint8_t)(saved_msg[b * BANK_RECS + HAL_REC_BANK][r * 2 + k - 2] & 0x7F);
    }

    for (n = 0; n < STOMP_COUNT; n++)
    {
        if (saved_msg[b * BANK_RECS + n][0] & (0x10 << r))
        {
            m |= 1u << n;
        }
    }

    return (uint8_t)(k ? m >> 8 : m & 0xFF);
}

#define saved_sw(r, k) saved_sw_byte(0, (r), (k))

// Byte j of bank b's packed pedal settings (0 the program change, 1 / 2 pedal A / B's CC
// with its pickup bit) from its saved or stored records.
static uint8_t y_of(uint8_t recs[][4], int b, int j)
{
    int n;
    uint8_t v = 0;

    for (n = 0; n < STOMP_COUNT; n++)
    {
        if ((n >> 2) == j)
        {
            v |= (uint8_t)((recs[b * BANK_RECS + n][0] >> 6) << ((n & 3) * 2));
        }
    }

    if (j == 2)
    {
        for (n = 0; n < 4; n++)
        {
            v |= (uint8_t)((recs[b * BANK_RECS + HAL_REC_BANK][n] >> 7) << (4 + n));
        }
    }

    return v;
}

#define saved_y(b, j) y_of(saved_msg, (b), (j))

// Store byte j of bank b's packed pedal settings (other settings left as stored, or the
// defaults).
static void store_y(int b, int j, uint8_t v)
{
    int n;
    uint8_t y[3];

    store_bank_defaults(b);
    y[0] = y[1] = y[2] = 0;
    y[j] = v;

    for (n = 0; n < STOMP_COUNT; n++)
    {
        if ((n >> 2) == j)
        {
            uint8_t *m = stored_msg[b * BANK_RECS + n];

            m[0] = (uint8_t)((m[0] & 0x3F) | piece_of(y, n));
        }
    }

    if (j == 2)
    {
        for (n = 0; n < 4; n++)
        {
            uint8_t *t = stored_msg[b * BANK_RECS + HAL_REC_BANK];

            t[n] = (uint8_t)((t[n] & 0x7F) | (((v >> (4 + n)) & 1) << 7));
        }
    }
}

// Store relay r's follow mask and triggers for bank b (other settings left as stored, or
// the defaults).
static void store_relay(int b, int r, unsigned mask, uint8_t thr_a, uint8_t thr_b)
{
    int n;
    uint8_t *t = stored_msg[b * BANK_RECS + HAL_REC_BANK];

    store_bank_defaults(b);

    for (n = 0; n < STOMP_COUNT; n++)
    {
        uint8_t *m = stored_msg[b * BANK_RECS + n];

        m[0] = (uint8_t)((m[0] & ~(0x10 << r)) | (((mask >> n) & 1) ? (0x10 << r) : 0));
    }

    t[r * 2]     = (uint8_t)((t[r * 2] & 0x80) | (thr_a & 0x7F));
    t[r * 2 + 1] = (uint8_t)((t[r * 2 + 1] & 0x80) | (thr_b & 0x7F));
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

// A long UP asks which bank to save to (the bank in use first), and a short UP saves
// there; with only shared settings changed it saves and leaves on the next tick.
static void save_exit(void)
{
    long_nav(CTL_KEY_UP);

    if (save_prompt)
    {
        CHECK(save_bank == hal_bank + 1 && mode == MODE_CONFIG);
        tap(CTL_KEY_UP);
    }
    else
    {
        CHECK(mode == MODE_CONFIG && save_due);
        ticks(1);
    }
}
static void cancel_exit(void) { long_nav(CTL_KEY_DOWN); }

// In the footswitch editor: short UP [ENTER] through the remaining parameters, keeping
// each, until it closes.
static void edit_close(void)
{
    int n = 0;

    while (edit_open && n++ < 10)
    {
        tap(CTL_KEY_UP);
    }
}

// In configuration mode, go to page p (PAGE_*) with short UP / DOWN.
static void to_page(int p)
{
    int n = 0;

    while (cfg_page != p && n++ < PAGE_COUNT)
    {
        tap((cfg_page < p) ? CTL_KEY_UP : CTL_KEY_DOWN);
    }
}

// Key code for footswitch 1..10 (codes 0-4 are switches 1-5, 6-10 are switches 6-10).
static uint8_t sw_code(int sw)
{
    return (uint8_t)((sw <= 5) ? (sw - 1) : sw);
}

// Page 1: a press of a switch that is not selected selects it; a press of the selected
// switch moves it to the next pedal mode through the editor (pressed again it opens on
// MIDI FUNCTION, switches 1-4 choose the mode, UP through the rest accepts). A helper for
// the tests of what a mode change does; test_config_editor checks the editor itself.
static void mode_tap(uint8_t code)
{
    uint8_t i = stomp_index(code);

    if (i != cfg_sel)
    {
        tap(code);
        ticks(CONFIG_SWITCH_SHOW_MS / TICK_MS + 1);
        return;
    }

    tap(code);
    tap(sw_code(((pedal_mode_get(i) + 1) & 3) + 1));
    edit_close();
    // The change is compared with the EEPROM on the next tick; page 1 then shows the
    // switch's number, then its mode.
    ticks(CONFIG_SWITCH_SHOW_MS / TICK_MS + 1);
}

// On pedal ch's EXP page: footswitch 9 (selected first if need be) until "Lo" flashes,
// then UP takes the heel reading lo, and UP again the toe reading hi.
static void calibrate(uint8_t ch, uint8_t lo, uint8_t hi)
{
    int n;

    if (ped_sel != PED_CAL)
    {
        tap(sw_code(PED_CAL + 1));
    }

    for (n = 0; n < 3 && cal_step != CAL_HEEL; n++)
    {
        tap(sw_code(PED_CAL + 1));
    }

    pedal(ch, lo);
    tap(CTL_KEY_UP);
    pedal(ch, hi);
    tap(CTL_KEY_UP);
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
    int i, ok;
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

    // Every message (one check): the pedal's CC on the channel; values only ever go up on
    // an upward sweep and never repeat.
    last = 0;
    ok = 1;

    for (i = 0; i < cc_count(); i++)
    {
        ok = ok && cc_status(i) == (0xB0 | (MIDI_CHANNEL - 1)) && cc_number(i) == expected_cc[0] &&
             (cc_value(i) > last || (i == 0 && cc_value(i) >= last)) && cc_value(i) <= 127;
        last = cc_value(i);
    }

    CHECK(ok);

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
    CHECK(display == 1);

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
    program_mode();
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
    program_mode();
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
    program_mode();
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
    program_mode();
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
    program_mode();
    clear_log();
    ctl_key_press(CTL_KEY_DOWN);
    // REPEAT_FAST_STEPS slow steps, then 50 fast ones.
    ticks(REPEAT_DELAY_TICKS + REPEAT_PERIOD_TICKS * (REPEAT_FAST_STEPS - 1) + REPEAT_FAST_TICKS * 50);
    CHECK(display == (PROGRAM_WRAP ? (128 - (REPEAT_FAST_STEPS + 50)) & 127 : 0));
    ctl_key_release(CTL_KEY_DOWN);
    CHECK(PROGRAM_WRAP ? (midi_len == 2 && midi_log[1] == display) : midi_len == 0);

    // A hold that reaches the end does send it, once.
    reset_world();
    program_mode();
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
    program_mode();
    ctl_key_press(CTL_KEY_UP);
    ticks(REPEAT_DELAY_TICKS);
    clear_log();
    ctl_key_release(CTL_KEY_UP);
    CHECK(display == 1 && midi_len == 2 && midi_log[1] == 1);

    // A release just under the delay is an ordinary tap.
    reset_world();
    program_mode();
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
    program_mode();
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
    program_mode();
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
    // Config mode opens on page 1, footswitch 1's number first.
    CHECK(cfg_page == PAGE_MODES && display == 1);
    CHECK(aux[HAL_LED_MIDI_FUNC] == 1 && aux[HAL_LED_MIDI_CHAN] == 0);
    chord_up();
    CHECK(mode == MODE_CONFIG && status_led == 1);
    CHECK(midi_len == 0);

    // A chord that is let go early in run mode only toggles program mode (see
    // test_bank_mode_toggle): nothing is sent, and the display shows the program.
    reset_world();
    clear_log();
    chord_short();
    CHECK(mode == MODE_RUN && midi_len == 0 && pc_mode && display == DISPLAY_PROGRAM_OFFSET && program == 0);

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
    program_mode();
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
    // Page 1 is pedal modes, page 2 the MIDI channel (user's order), page 8 the version.
    CHECK(mode == MODE_CONFIG && cfg_page == PAGE_MODES);
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_CHANNEL && display == MIDI_CHANNEL && PAGE_CHANNEL == 1 && PAGE_MISC == 7 && PAGE_VERSION == 8);

    // UP / DOWN change the page and never the channel; they do not auto-repeat.
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_MODES && midi_channel == MIDI_CHANNEL);
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_CHANNEL && display == MIDI_CHANNEL);
    ctl_key_press(CTL_KEY_UP);
    ticks(REPEAT_DELAY_TICKS + 3 * REPEAT_PERIOD_TICKS);
    CHECK(cfg_page == PAGE_CHANNEL);
    ctl_key_release(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_EXP_A && midi_channel == MIDI_CHANNEL);
    tap(CTL_KEY_DOWN);

    CHECK(mode == MODE_CONFIG && cfg_page == PAGE_CHANNEL);

    // Channel 4 typed on footswitch 4.
    tap(sw_code(4));
    CHECK(display == 4 && midi_channel == 4);

    // No program change is sent in config mode, and the program number is untouched.
    CHECK(midi_len == 0 && program == 0);

    // A long UP saves and leaves. The CONFIG and page LEDs go out and the display
    // shows the program number again; UP did not step the program.
    save_exit();
    CHECK(mode == MODE_RUN && status_led == 0 && display == 1);
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
    program_mode();
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
    power_on();
    clear_log();
    tap(0);
    CHECK(cc_status(0) == 0xB3);

    // An out-of-range saved channel falls back to the default.
    stored_cfg[0] = 17;
    power_on();
    CHECK(midi_channel == MIDI_CHANNEL);
    stored_cfg[0] = 0;
    power_on();
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

    // UP + DOWN in config mode saves and leaves at once (on the next tick), like a long
    // UP. The keys' own hold events and the release do nothing more (no program mode
    // toggle, no program).
    reset_world();
    chord_long();
    to_page(PAGE_CHANNEL);
    tap(sw_code(5));
    clear_log();
    chord_down();
    CHECK(mode == MODE_CONFIG && cfg_saved == 0);
    ticks(1);
    CHECK(mode == MODE_RUN && status_led == 0 && display == 1);
    CHECK(cfg_saved == 1 && saved_cfg[0] == 5 && cal_saved == 1 && midi_channel == 5);
    CHECK(aux[HAL_LED_MIDI_CHAN] == 0 && aux[HAL_LED_MIDI_FUNC] == 0);
    ctl_key_hold(CTL_KEY_UP);
    ctl_key_hold(CTL_KEY_DOWN);
    ticks(CHORD_TICKS);
    chord_up();
    CHECK(mode == MODE_RUN && midi_len == 0 && program == 0 && pc_mode == 0);
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
    mode_tap(2);
    mode_tap(2);
    to_page(PAGE_CHANNEL);
    tap(sw_code(7));
    CHECK(midi_channel == 7 && pedal_mode_get(2) == PM_STOMP_MOM);
    to_page(PAGE_EXP_A);
    calibrate(0, 20, 220);
    CHECK(heel[0] == 20 && toe[0] == 220);
    clear_log();
    cancel_exit();
    CHECK(mode == MODE_RUN && status_led == 0 && display == 1 && program == 0);
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
    power_on();
    chord_long();
    // Switch 1 starts selected on page 1, so one press changes it.
    mode_tap(0);
    to_page(PAGE_CHANNEL);
    tap(sw_code(9));
    CHECK(midi_channel == 9 && pedal_mode_get(0) == PM_STOMP_MOM);
    cancel_exit();
    CHECK(midi_channel == 5 && pedal_mode_get(0) == PM_STOMP);
    CHECK(heel[0] == 40 && heel[1] == 50 && toe[0] == 140 && toe[1] == 150);
    CHECK(cfg_saved == 0 && cal_saved == 0);
}

static void test_config_modes(void)
{
    int i, ok = 1;

    printf("config: pedal modes\n");
    reset_world();
    chord_long();
    // Footswitch 1 starts selected: its number shows and its LED is lit.
    CHECK(display == 1 && aux[HAL_LED_MIDI_FUNC] == 1 && aux[HAL_LED_MIDI_CHAN] == 0);
    CHECK(aux[HAL_LED_EXP_1] == 0 && aux[HAL_LED_EXP_2] == 0);
    CHECK(leds[0] == 1 && cfg_sel == 0);
    CHECK(pedal_mode_get(0) == DEFAULT_PEDAL_MODE);

    // Selecting footswitch 3 shows its mode and lights its LED alone; nothing changes.
    // It starts in a known mode so the cycle below does not depend on the default.
    set_mode(2, PM_STOMP_MOM);
    mode_tap(2);
    CHECK(cfg_sel == 2 && pedal_mode_get(2) == PM_STOMP_MOM);
    CHECK(display == SHOWN_MODE(PM_STOMP_MOM));
    CHECK(leds[2] == 1 && leds[0] == 0);

    // Pressing it again opens the editor (test_config_editor); mode_tap chooses the next
    // mode there. Only that switch changes; footswitch 10 is the last.
    set_mode(2, PM_MOM_INV);
    mode_tap(2);
    CHECK(pedal_mode_get(2) == PM_STOMP && display == SHOWN_MODE(PM_STOMP) && leds[2] == 1);

    for (i = 0; i < STOMP_COUNT; i++)
    {
        ok = ok && (i == 2 || pedal_mode_get(i) == DEFAULT_PEDAL_MODE);
    }

    CHECK(ok);
    set_mode(9, PM_STOMP_MOM);
    mode_tap(10);
    CHECK(cfg_sel == 9 && display == SHOWN_MODE(PM_STOMP_MOM) && leds[9] == 1 && leds[2] == 0);
    mode_tap(10);
    CHECK(pedal_mode_get(9) == PM_MOMENTARY && pedal_mode_get(8) == DEFAULT_PEDAL_MODE);

    // UP / DOWN only change the page here, never a mode.
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_CHANNEL && pedal_mode_get(9) == PM_MOMENTARY);
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_MODES && pedal_mode_get(9) == PM_MOMENTARY && cfg_sel == 9);

    // Leave, then check the saved bytes round-trip into a fresh controller.
    save_exit();
    CHECK(mode == MODE_RUN && cfg_saved == 1);
    // The modes are saved in the footswitch records (type byte bits 2-3), not the
    // settings record, which holds the channel, then three 0s.
    CHECK(msg_was_saved[9] && ((saved_msg[9][0] >> 2) & 3) == PM_MOMENTARY && (saved_msg[9][0] & 3) == MT_CC);
    CHECK(((saved_msg[2][0] >> 2) & 3) == PM_STOMP && saved_cfg[1] == 0 &&
          saved_cfg[2] == (LEVEL_VALID | LED_BRIGHTNESS) && saved_cfg[3] == MISC_DEFAULT);
    memcpy(stored_cfg, saved_cfg, 4);
    cfg_stored_valid = 1;
    keep_saved_msgs();
    power_on();
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
    power_on();
    CHECK(pedal_mode_get(0) == PM_MOM_INV && pedal_mode_get(3) == PM_MOM_INV && msg_type(0) == MT_CC);
    CHECK(pedal_mode_get(8) == PM_STOMP && pedal_mode_get(9) == PM_STOMP && msg_type(9) == MT_NOTE);
    // The settings record's last three bytes are not modes, and without bit 7 byte 3 is
    // not the misc settings either (the defaults).
    stored_cfg[1] = 0xFF;
    stored_cfg[2] = 0xFF;
    stored_cfg[3] = 0x7F;
    power_on();
    CHECK(pedal_mode_get(0) == PM_MOM_INV && pedal_mode_get(8) == PM_STOMP && pedal_mode_get(9) == PM_STOMP);
    CHECK(opt_metro == DEFAULT_METRONOME && opt_runstat == DEFAULT_RUNNING_STATUS);

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
    mode_tap(1);
    mode_tap(1);
    mode_tap(2);
    mode_tap(2);
    mode_tap(2);
    mode_tap(2);
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
    mode_tap(3);
    mode_tap(3);
    mode_tap(3);
    mode_tap(3);
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
    mode_tap(4);
    mode_tap(4);
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
    mode_tap(2);
    mode_tap(2);
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
    mode_tap(sw_code(6));
    mode_tap(sw_code(6));
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
    mode_tap(1);
    mode_tap(1);
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
    to_page(PAGE_CHANNEL);
    clear_log();
    tap(3);
    CHECK(midi_len == 0 && display == 4 && midi_channel == 4);
}

// The footswitch LEDs: lit (1) where on has a bit, dim (2) where dim has one, else dark.
static int stomp_leds_are(unsigned on, unsigned dim)
{
    int k;

    for (k = 0; k < STOMP_COUNT; k++)
    {
        if (leds[k] != (((on >> k) & 1) ? HAL_STOMP_ON : ((dim >> k) & 1) ? HAL_STOMP_DIM : HAL_STOMP_OFF))
        {
            return 0;
        }
    }

    return 1;
}

// Footswitch LED bits for the digits in the string ("0" is switch 10).
static unsigned digit_leds(const char *d)
{
    unsigned m = 0;

    for (; *d; d++)
    {
        m |= 1u << ((*d == '0') ? 9 : *d - '1');
    }

    return m;
}

// Channel entry, the shared number entry (user's design): switches 1-9 type 1-9 and 10
// types 0; the digits that can come next are lit dimly; while a further digit can follow
// the right-hand dot flashes, UP ends the number and DOWN puts the old value back.
static void test_config_channel_digits(void)
{
    int sw;

    printf("config: MIDI channel from the footswitches\n");
    reset_world();
    chord_long();
    to_page(PAGE_CHANNEL);
    clear_log();
    // Channels 1-16: the first digit is 1-9 (switch 10, 0, is dark).
    CHECK(stomp_leds_are(0, digit_leds("123456789")) && !num_typing && dots == 0);

    // 2-9 are whole channels at once (nothing can follow).
    for (sw = 2; sw <= 9; sw++)
    {
        tap(sw_code(sw));
        CHECK(midi_channel == sw && display == sw && !num_typing);
        CHECK(stomp_leds_are(0, digit_leds("123456789")));
    }

    // 1 can be followed by 0-6: it is channel 1 already, the dot flashes and 0-6 are dim.
    tap(sw_code(1));
    CHECK(midi_channel == 1 && display == 1 && num_typing && stomp_leds_are(0, digit_leds("0123456")));
    ticks(1);
    CHECK(dots == 1);
    ticks(CHAN_FLASH_TICKS);
    CHECK(dots == 0);
    ticks(CHAN_FLASH_TICKS);
    CHECK(dots == 1);
    // 7 cannot follow: nothing happens.
    tap(sw_code(7));
    CHECK(midi_channel == 1 && num_typing);
    // 6 makes 16, which can take no more: the dot stops.
    tap(sw_code(6));
    CHECK(midi_channel == 16 && display == 16 && !num_typing && stomp_leds_are(0, digit_leds("123456789")));
    ticks(CHAN_FLASH_TICKS * 3);
    CHECK(dots == 0);

    // 1 then 10 (as 0) is channel 10.
    tap(sw_code(1));
    tap(sw_code(10));
    CHECK(midi_channel == 10 && display == 10 && !num_typing);

    // UP while the dot flashes ends the number there, on this page.
    tap(sw_code(1));
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_CHANNEL && midi_channel == 1 && !num_typing);
    ticks(CHAN_FLASH_TICKS * 2);
    CHECK(dots == 0);
    // Then the next digit starts a new number: 5.
    tap(sw_code(5));
    CHECK(midi_channel == 5 && !num_typing);

    // DOWN while the dot flashes puts back the channel from before the number.
    tap(sw_code(1));
    CHECK(midi_channel == 1);
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_CHANNEL && midi_channel == 5 && display == 5 && !num_typing);
    // With no number being typed, UP and DOWN change the page.
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_MODES && midi_channel == 5);
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_CHANNEL && display == 5);

    // Leaving with a number open keeps it: 1 is saved and the program number shows.
    tap(sw_code(1));
    save_exit();
    CHECK(mode == MODE_RUN && saved_cfg[0] == 1 && display == 1 && blank == 0 && !num_typing);
    ticks(CHAN_FLASH_TICKS * 2);
    CHECK(dots == 0);

    // Nothing was sent, and a typed two-digit channel is saved and used.
    CHECK(midi_len == 0);
    chord_long();
    to_page(PAGE_CHANNEL);
    tap(sw_code(1));
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

    // A footswitch pressed while UP or DOWN is held types nothing, on any page with a
    // number (the simulator's row 2 presses UP with switches 6-10, in an order that
    // depends on the key scan). Page 7: 1 typed, then UP held with 3: still 1, and UP's
    // release takes it and stays.
    to_page(PAGE_CHANNEL);
    ctl_key_press(CTL_KEY_UP);
    tap(sw_code(3));
    CHECK(midi_channel == 12 && !num_typing);
    ctl_key_release(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_EXP_A && midi_channel == 12);
    to_page(PAGE_BANK_PC);
    tap(sw_code(1));
    CHECK(num_typing && bank_pc == 2);
    ctl_key_press(CTL_KEY_UP);
    tap(sw_code(3));
    ctl_key_release(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_BANK_PC && bank_pc == 2 && !num_typing);
    ctl_key_press(CTL_KEY_DOWN);
    tap(sw_code(6));
    ctl_key_release(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_SW2 && bank_pc == 2);
    cancel_exit();
}

static void test_config_page_leds(void)
{
    printf("config: pages, LEDs and display\n");
    reset_world();
    chord_long();
    // Page 1 pedal modes.
    CHECK(cfg_page == PAGE_MODES && aux[HAL_LED_MIDI_CHAN] == 0 && aux[HAL_LED_MIDI_FUNC] == 1);
    CHECK(aux[HAL_LED_EXP_1] == 0 && aux[HAL_LED_EXP_2] == 0);
    CHECK(display == 1 && leds[0] == 1);
    // Page 2 the MIDI channel (user's order).
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_CHANNEL && aux[HAL_LED_MIDI_CHAN] == 1 && aux[HAL_LED_MIDI_FUNC] == 0);
    CHECK(display == MIDI_CHANNEL && stomp_leds_are(0, digit_leds("123456789")));
    // Page 3 EXP A: footswitch 6 (its CC, user's request) and EXP 1 lit; page 4 EXP B,
    // EXP 2 lit.
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_EXP_A && aux[HAL_LED_MIDI_CHAN] == 0 && aux[HAL_LED_MIDI_FUNC] == 0);
    CHECK(aux[HAL_LED_EXP_1] == 1 && aux[HAL_LED_EXP_2] == 0 && aux[HAL_LED_NUMBER] == 0);
    CHECK(display == pedal_cc[0] && dots == 0);
    // Footswitch 6 (the CC) lit, 8 (response) and 9 (calibration) dim (user's request).
    CHECK(stomp_leds_are(1u << PED_CC, PED_MASK));
    CHECK(lamp == HAL_LAMP_NORMAL);
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_EXP_B && aux[HAL_LED_EXP_1] == 0 && aux[HAL_LED_EXP_2] == 1 && pcc_b == 1);
    CHECK(display == pedal_cc[1] && stomp_leds_are(1u << PED_CC, PED_MASK));
    // Pages 5 and 6 light SWITCH 1 / SWITCH 2.
    CHECK(aux[HAL_LED_SWITCH_1] == 0 && aux[HAL_LED_SWITCH_2] == 0);
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_SW1 && aux[HAL_LED_SWITCH_1] == 1 && aux[HAL_LED_SWITCH_2] == 0);
    // No triggers: EXP 1 / EXP 2 dim, and no follows: every footswitch LED dim (user's
    // request).
    CHECK(aux[HAL_LED_EXP_1] == HAL_LED_DIM && aux[HAL_LED_EXP_2] == HAL_LED_DIM && aux[HAL_LED_MIDI_CHAN] == 0);
    // The display shows "S1" (user's request), "S2" on page 6.
    CHECK(display == SHOWN_TEXT(HAL_TEXT_S1) && stomp_leds_are(0, ALL_SWITCHES));
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_SW2 && aux[HAL_LED_SWITCH_1] == 0 && aux[HAL_LED_SWITCH_2] == 1 && display == SHOWN_TEXT(HAL_TEXT_S2));
    // Page 7 the bank's program change: DIRECT SELECT, "Pr" first.
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_BANK_PC && aux[HAL_LED_SWITCH_2] == 0 && aux[HAL_LED_DIRECT_SELECT] == 1 && lamp == HAL_LAMP_NORMAL && dots == 0);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_PROG));
    // Page 8 misc: SELECT, footswitches 1, 9 and 10 dim (all off), "CL".
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_MISC && aux[HAL_LED_DIRECT_SELECT] == 0 && aux[HAL_LED_SELECT] == 1 && lamp == HAL_LAMP_NORMAL);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_CL) && stomp_leds_are(0, MISC_MASK));
    // Page 9 the version, the last (no wrap).
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_VERSION && aux[HAL_LED_SELECT] == 0 && lamp == HAL_LAMP_LEDS_ON);
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_VERSION);
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_MISC && lamp == HAL_LAMP_NORMAL && dots == 0);
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_BANK_PC && lamp == HAL_LAMP_NORMAL && dots == 0 && aux[HAL_LED_DIRECT_SELECT] == 1);
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_SW2 && aux[HAL_LED_SWITCH_2] == 1 && aux[HAL_LED_DIRECT_SELECT] == 0);
    tap(CTL_KEY_DOWN);
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_EXP_B && aux[HAL_LED_SWITCH_1] == 0 && aux[HAL_LED_EXP_2] == 1);
    tap(CTL_KEY_DOWN);
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_CHANNEL && aux[HAL_LED_MIDI_CHAN] == 1 && aux[HAL_LED_EXP_1] == 0);
    // Page 2's digits are dim (1-9: channels 1-16).
    CHECK(stomp_leds_are(0, digit_leds("123456789")));
    tap(sw_code(1));
    tap(sw_code(10));
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_MODES && !num_typing && midi_channel == 10 && aux[HAL_LED_MIDI_CHAN] == 0);
    CHECK(aux[HAL_LED_MIDI_FUNC] == 1 && display == 1);
    // DOWN on page 1 stays on page 1 (no wrap).
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_MODES && aux[HAL_LED_MIDI_FUNC] == 1 && aux[HAL_LED_EXP_1] == 0);
    CHECK(aux[HAL_LED_SWITCH_2] == 0 && leds[0] == 1);
    save_exit();
    CHECK(mode == MODE_RUN && aux[HAL_LED_MIDI_CHAN] == 0 && aux[HAL_LED_MIDI_FUNC] == 0);
    CHECK(aux[HAL_LED_EXP_1] == 0 && aux[HAL_LED_EXP_2] == 0 && display == 1);
}

// From the first step of page 8 or power-on: every tick of the "riban" scroll, each frame
// (" r", "ri", "ib", "ba", "an", "n ") for SCROLL_FRAME_MS each, no dots, every LED lit.
// Returns with the scroll's time used up exactly.
// Page 8's footswitch LEDs: only index on lit (-1: none; user's request, they once were
// all lit with one dark).
static int stomp_leds_but(int on)
{
    int k;

    for (k = 0; k < STOMP_COUNT; k++)
    {
        if (leds[k] != (k == on))
        {
            return 0;
        }
    }

    return 1;
}

// The "riban" scroll moves ". riban ." across the three dots (bit 2 after the "1", bit 0
// far right): the dot of each text frame.
static const uint8_t scroll_dot_of[HAL_TEXT_SCROLL_FRAMES] = { 0x04, 0x00, 0x00, 0x00, 0x00, 0x01 };

// One scroll frame for its full time: text (HAL_TEXT_* value, or -1 for a blank display)
// and dots. On page 8 footswitch LED dark_led is the only one lit for the first
// VER_FLASH_STEPS (-1: none), and the lamp test is checked apart (test_config_version);
// at power-on HAL_LAMP_LEDS_ON lights them all (not checked here).
static void expect_frame(int text, int dot_mask, int dark_led)
{
    int k, boot = boot_anim, ok = 1;

    // Every tick of the frame, counted as one check.
    for (k = 0; k < VER_STEP_TICKS * VER_SCROLL_STEPS; k++)
    {
        ok = ok && (text < 0 ? (display == 0 && blank == 7) : (display == SHOWN_TEXT(text) && blank == 0)) &&
             dots == dot_mask && midi_len == 0 &&
             (!boot || lamp == HAL_LAMP_LEDS_ON) &&
             (boot || stomp_leds_but((k < VER_STEP_TICKS * VER_FLASH_STEPS) ? dark_led : -1));
        ticks(1);
    }

    CHECK(ok);
}

// The lead-in (power-on and page 8 start here; it ends the blank segment): the display
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

// Page 8: every LED lights at once (no blinking) and stays lit while "riban" scrolls,
// then the version shows as "major." / ".minor." / ".build", VERSION_SHOW_MS each, then a
// blank, repeating from the scroll.
static void test_config_version(void)
{
    int k, bad;

    printf("config: page 9 lamp test and version\n");
    reset_world();
    chord_long();
    CHECK(lamp == HAL_LAMP_NORMAL && dots == 0);

    for (k = 0; k < PAGE_VERSION; k++)
    {
        tap(CTL_KEY_UP);
    }

    CHECK(mode == MODE_CONFIG && cfg_page == PAGE_VERSION && status_led == 1);

    for (k = 0; k < HAL_LED_COUNT; k++)
    {
        CHECK(aux[k] == 0);
    }

    // Every LED lit for LAMP_TEST_MS (the lamp test), the footswitch LEDs dark under it
    // through the lead-in (the end of segment 10); switch 1's then lights for the first
    // frame.
    CHECK(stomp_leds_but(-1) && lamp == HAL_LAMP_LEDS_ON);

    // At once: LEDs lit and the scroll, each frame half a version part's time.
    CHECK(VER_STEP_TICKS * TICK_MS == VER_STEP_MS && VER_ITEM_STEPS * VER_STEP_TICKS * TICK_MS == VERSION_SHOW_MS);
    CHECK(VER_SCROLL_STEPS * VER_STEP_MS == SCROLL_FRAME_MS && HAL_TEXT_SCROLL_FRAMES == 6);
    clear_log();
    expect_lead();
    expect_scroll();
    CHECK(lamp == HAL_LAMP_NORMAL && display == VER_SHOWN(0) && dots == 0x01);

    bad = 0;

    for (k = 0; k < VER_STEP_TICKS * VER_ITEM_STEPS - 1; k++)
    {
        ticks(1);
        bad += !(lamp == HAL_LAMP_NORMAL && display == VER_SHOWN(0) && dots == 0x01);
    }

    CHECK(bad == 0);

    ticks(1);
    CHECK(display == VER_SHOWN(1) && dots == 0x05);
    ticks(VER_STEP_TICKS * VER_ITEM_STEPS);
    CHECK(display == VER_SHOWN(2) && dots == 0x04 && lamp == HAL_LAMP_NORMAL);
    // Then a blank display (no dots) for one part's time, then major.
    ticks(VER_STEP_TICKS * VER_ITEM_STEPS);
    CHECK(display == 0 && blank == 7 && dots == 0 && lamp == HAL_LAMP_NORMAL);
    ticks(VER_STEP_TICKS * VER_ITEM_STEPS - 1);
    CHECK(display == 0 && blank == 7 && dots == 0);
    // Then the lead-in, the scroll again, then major.
    ticks(1);
    expect_lead();
    expect_scroll();
    CHECK(display == VER_SHOWN(0) && dots == 0x01 && lamp == HAL_LAMP_NORMAL);
    CHECK(midi_len == 0);

    // A footswitch runs it all again, sending nothing.
    CHECK(cfg_page == PAGE_VERSION && display == VER_SHOWN(0));
    ticks(VER_STEP_TICKS * VER_ITEM_STEPS);
    CHECK(display == VER_SHOWN(1));
    // Switch 1 goes back to the scroll's first frame (test_config_version_jump has more).
    tap(sw_code(1));
    CHECK(lamp == HAL_LAMP_NORMAL && display == SHOWN_TEXT(HAL_TEXT_SCROLL) && dots == scroll_dot_of[0]);
    CHECK(stomp_leds_but(0) && midi_len == 0);
    // It keeps repeating while the page is open.
    for (k = 0; k < 3; k++)
    {
        ticks(VER_STEP_TICKS * VER_END_STEP);
        CHECK(lamp == HAL_LAMP_NORMAL && display == SHOWN_TEXT(HAL_TEXT_SCROLL) && dots == scroll_dot_of[0]);
    }
    ticks(VER_STEP_TICKS * (VER_SCROLL_END + VER_ITEM_STEPS * 2));
    CHECK(display == VER_SHOWN(2) && lamp == HAL_LAMP_NORMAL);

    // Pedals do nothing here.
    pedal(0, 10);
    pedal(0, 200);
    CHECK(mode == MODE_CONFIG && cfg_page == PAGE_VERSION && midi_len == 0);
    CHECK(sw_thr[0] == 0 && sw_thr[1] == 0 && sw_thr[2] == 0 && sw_thr[3] == 0 && sw_mask[1] == 0);

    // DOWN during the lamp test: page 8, LEDs back to normal, and no more steps.
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_MISC && lamp == HAL_LAMP_NORMAL && dots == 0 && aux[HAL_LED_SELECT] == 1);
    ticks(VER_STEP_TICKS * 5);
    CHECK(lamp == HAL_LAMP_NORMAL && display == SHOWN_TEXT(HAL_TEXT_CL));

    // Page 8 again: every LED lit for exactly LAMP_TEST_MS, then the panel LEDs one at a
    // time along the row (SWITCH 1, SWITCH 2, SWITCH, SELECT, NUMBER, VALUE 1, VALUE 2,
    // DIRECT SELECT, MIDI FUNCTION, MIDI CHAN) and back, LAMP_CHASE_MS each (user's
    // request). CONFIG and the EXP LEDs are not in it: EXP 1 is lit on the way along and
    // EXP 2 on the way back (user's request).
    tap(CTL_KEY_UP);
    CHECK(lamp == HAL_LAMP_LEDS_ON && aux[HAL_LED_DIRECT_SELECT] == 0);
    {
        static const uint8_t row[10] = { HAL_LED_SWITCH_1, HAL_LED_SWITCH_2, HAL_LED_SWITCH, HAL_LED_SELECT,
                                         HAL_LED_NUMBER, HAL_LED_VALUE_1, HAL_LED_VALUE_2,
                                         HAL_LED_DIRECT_SELECT, HAL_LED_MIDI_FUNC, HAL_LED_MIDI_CHAN };
        int step, bad = 0, lit, n;

        ticks(LAMP_TEST_MS / TICK_MS - 1);
        CHECK(lamp == HAL_LAMP_LEDS_ON);
        ticks(1);
        CHECK(lamp == HAL_LAMP_NORMAL);

        // Two whole runs up and back, each position checked over its whole time.
        for (step = 0; step < 2 * 18; step++)
        {
            int pos = step % 18;
            int want = row[(pos < 10) ? pos : 18 - pos];

            for (k = 0; k < LAMP_CHASE_MS / TICK_MS; k++)
            {
                lit = 0;

                for (n = 0; n < HAL_LED_COUNT; n++)
                {
                    lit += (aux[n] && n != HAL_LED_EXP_1 && n != HAL_LED_EXP_2) ? 1 : 0;
                }

                bad += !(lit == 1 && aux[want] == 1 && lamp == HAL_LAMP_NORMAL);
                bad += !(aux[HAL_LED_EXP_1] == (pos < 9) && aux[HAL_LED_EXP_2] == (pos >= 9));
                ticks(1);
            }
        }

        CHECK(bad == 0);
    }

    // Leaving from page 7 (long UP, or a long UP + DOWN) ends the lamp test and dots.
    tap(CTL_KEY_DOWN);
    tap(CTL_KEY_UP);
    CHECK(lamp == HAL_LAMP_LEDS_ON);
    save_exit();
    CHECK(mode == MODE_RUN && lamp == HAL_LAMP_NORMAL && dots == 0 && display == 1);
    ticks(VER_STEP_TICKS * 2);
    CHECK(lamp == HAL_LAMP_NORMAL);
    chord_long();
    for (k = 0; k < PAGE_VERSION; k++)
    {
        tap(CTL_KEY_UP);
    }
    ticks(VER_STEP_TICKS * (VER_LEAD_FRAMES * VER_SCROLL_STEPS + VER_SCROLL_END) + 1);
    CHECK(display == VER_SHOWN(0) && dots == 0x01);
    chord_long();
    CHECK(mode == MODE_RUN && lamp == HAL_LAMP_NORMAL && dots == 0 && display == 1);
}

// Page 8's ten segments, one per footswitch: the scroll frames " r" to "n " (switches
// 1-6), major, minor, build (7-9) and the blank (10). Each segment flashes its footswitch
// LED dark then lit; pressing a footswitch shows its segment and holds it until release,
// then it carries on from there. Nothing is sent and no setting changes.
static void test_config_version_jump(void)
{
    int k;

    printf("config: page 8 footswitch LEDs mark the segments; a footswitch holds its segment\n");
    reset_world();
    chord_long();

    for (k = 0; k < PAGE_VERSION; k++)
    {
        tap(CTL_KEY_UP);
    }

    clear_log();
    CHECK(cfg_page == PAGE_VERSION && cfg_changed == 0 && stomp_leds_but(-1));
    // It opens at the lead-in (the end of segment 10), then segment 1.
    CHECK(display == 0 && blank == 7 && dots == 0x01);
    ticks(VER_STEP_TICKS * VER_LEAD_FRAMES * VER_SCROLL_STEPS);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_SCROLL) && dots == scroll_dot_of[0] && stomp_leds_but(0));

    // Each segment's LED: lit for VER_FLASH_STEPS from its start, then dark.
    for (k = 0; k < STOMP_COUNT; k++)
    {
        // " r" to "an" one frame; "n " and its tail; major, minor, build; blank and lead-in.
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
    CHECK(display == VER_SHOWN(1) && dots == 0x05 && stomp_leds_but(7) && ver_paused == 1);
    ticks(VER_STEP_TICKS * VER_END_STEP * 2);
    CHECK(display == VER_SHOWN(1) && dots == 0x05 && stomp_leds_but(7));
    CHECK(lamp == HAL_LAMP_NORMAL && midi_len == 0);
    // Released: it carries on from the start of that segment (LED lights after the flash).
    ctl_key_release(sw_code(8));
    CHECK(ver_paused == 0);
    ticks(VER_STEP_TICKS * VER_FLASH_STEPS - 1);
    CHECK(stomp_leds_but(7) && display == VER_SHOWN(1));
    ticks(1);
    CHECK(stomp_leds_but(-1));
    ticks(VER_STEP_TICKS * (VER_ITEM_STEPS - VER_FLASH_STEPS));
    CHECK(display == VER_SHOWN(2) && dots == 0x04 && stomp_leds_but(8));

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
            CHECK(display == VER_SHOWN(k - 7) && dots == fw_version_dots[k - 7]);
        }
        else
        {
            // Switch 10 lights every LED and display segment while held (user's
            // request), the build still showing under it.
            CHECK(lamp == HAL_LAMP_ALL_ON && ver_paused && display == VER_SHOWN(2));
            ticks(VER_STEP_TICKS * VER_END_STEP);
            CHECK(lamp == HAL_LAMP_ALL_ON && display == VER_SHOWN(2));
            ctl_key_release(sw_code(k));
            CHECK(lamp == HAL_LAMP_NORMAL && !ver_paused);
            break;
        }

        CHECK(stomp_leds_but(k - 1));
        ctl_key_release(sw_code(k));
    }

    // After switch 9 (the build), the blank, the lead-in, then the scroll.
    ticks(VER_STEP_TICKS * VER_ITEM_STEPS);
    CHECK(display == 0 && blank == 7 && dots == 0);
    ticks(VER_STEP_TICKS * VER_ITEM_STEPS);
    CHECK(display == 0 && blank == 7 && dots == 0x01);
    ticks(VER_STEP_TICKS * VER_LEAD_FRAMES * VER_SCROLL_STEPS);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_SCROLL) && stomp_leds_but(0));
    CHECK(midi_len == 0 && cfg_changed == 0 && mode == MODE_CONFIG && cfg_page == PAGE_VERSION);

    // DOWN while a switch is held leaves page 8; the release then does nothing.
    ctl_key_press(sw_code(5));
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_MISC && ver_paused == 0 && lamp == HAL_LAMP_NORMAL);
    ctl_key_release(sw_code(5));
    CHECK(cfg_page == PAGE_MISC && midi_len == 0 && cfg_changed == 0);

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
    CHECK(mode == MODE_RUN && midi_len == 0 && pc_mode == 1);
    // The display shows the program while DIRECT SELECT is lit, the bank (1) otherwise.
    CHECK(aux[HAL_LED_DIRECT_SELECT] == 1 && display == DISPLAY_PROGRAM_OFFSET && program == 0);
    chord_short();
    CHECK(midi_len == 0 && pc_mode == 0 && aux[HAL_LED_DIRECT_SELECT] == 0 && display == 1);

    // The second key pressed late still makes a chord; nothing is stepped.
    ctl_key_press(CTL_KEY_DOWN);
    ticks(10);
    ctl_key_press(CTL_KEY_UP);
    ticks(10);
    ctl_key_release(CTL_KEY_DOWN);
    ctl_key_release(CTL_KEY_UP);
    CHECK(midi_len == 0 && pc_mode == 1 && program == 0);

    // It does not wait for MIDI OUT room, since it sends nothing.
    midi_space = 0;
    chord_short();
    CHECK(pc_mode == 0);
    chord_short();
    CHECK(pc_mode == 1);
    midi_space = 15;

    // Opening and leaving config mode sends nothing. DIRECT SELECT is dark while
    // editing and shows the kept state again on leaving (save or cancel). An UP + DOWN
    // in config mode saves and leaves without toggling it.
    clear_log();
    chord_long();
    CHECK(mode == MODE_CONFIG && aux[HAL_LED_DIRECT_SELECT] == 0 && pc_mode == 1);
    chord_short();
    CHECK(mode == MODE_RUN && pc_mode == 1 && aux[HAL_LED_DIRECT_SELECT] == 1);
    chord_long();
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
    CHECK(pc_mode == 0 && aux[HAL_LED_DIRECT_SELECT] == 0);
    chord_long();
    save_exit();
    CHECK(aux[HAL_LED_DIRECT_SELECT] == 0);
    // Back on for the checks below.
    chord_short();
    CHECK(pc_mode == 1 && aux[HAL_LED_DIRECT_SELECT] == 1);

    // A power-on starts with it off.
    power_on();
    CHECK(aux[HAL_LED_DIRECT_SELECT] == 0 && pc_mode == 0);
}

// Open a SWITCH page: 1 (page 6) or 2 (page 7).
static void goto_switch_page(int sw)
{
    int i;

    chord_long();

    for (i = 0; i < PAGE_SW1 - 1 + sw; i++)
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
    chord_short();
    tap(2);
    CHECK(leds[2] == 0 && pc_mode == 0);

    // Page 5: footswitch 3 and bank mode, all LEDs showing the settings.
    goto_switch_page(1);
    CHECK(cfg_page == PAGE_SW1 && aux[HAL_LED_SWITCH_1] == 1 && aux[HAL_LED_DIRECT_SELECT] == 0);
    for (i = 0; i < STOMP_COUNT; i++)
    {
        CHECK(leds[i] == HAL_STOMP_DIM);
    }
    clear_log();
    tap(2);
    CHECK(leds[2] == 1 && sw_mask[0] == 4 && midi_len == 0);
    tap(2);
    CHECK(leds[2] == HAL_STOMP_DIM && sw_mask[0] == 0);
    tap(2);
    CHECK(sw_mask[0] == 4 && aux[HAL_LED_DIRECT_SELECT] == 0);
    CHECK(mode == MODE_CONFIG && cfg_page == PAGE_SW1 && pc_mode == 0 && midi_len == 0);

    // Pedal A sets the trigger value: the first reading is only a reference, small
    // wobble is ignored, a real move sets it, shows it and lights EXP 1.
    pedal(0, 100);
    CHECK(sw_thr[0] == 0 && display == SHOWN_TEXT(HAL_TEXT_S1) && aux[HAL_LED_EXP_1] == HAL_LED_DIM);
    pedal(0, 101);
    CHECK(sw_thr[0] == 0);
    pedal(0, 160);
    v = ctl_scale(160, 0, 255);
    CHECK(sw_thr[0] == v && display == v && blank == 0 && aux[HAL_LED_EXP_1] == 1);
    CHECK(aux[HAL_LED_EXP_2] == HAL_LED_DIM && midi_len == 0);

    // Page 6: footswitch 10 and pedal B; page 5's settings are not shown here.
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_SW2 && aux[HAL_LED_SWITCH_2] == 1 && aux[HAL_LED_SWITCH_1] == 0);
    CHECK(leds[2] == HAL_STOMP_DIM && aux[HAL_LED_DIRECT_SELECT] == 0 && aux[HAL_LED_EXP_1] == HAL_LED_DIM && display == SHOWN_TEXT(HAL_TEXT_S2));
    tap(sw_code(10));
    CHECK(leds[9] == 1 && sw_mask[1] == 0x200);
    pedal(1, 255);
    pedal(1, 0);
    CHECK(sw_thr[3] == 0 && display == 0 && aux[HAL_LED_EXP_2] == HAL_LED_DIM);
    pedal(1, 64);
    CHECK(sw_thr[3] == ctl_scale(64, 0, 255) && aux[HAL_LED_EXP_2] == 1);

    // Back on page 5 its settings show again.
    tap(CTL_KEY_DOWN);
    CHECK(leds[2] == 1 && leds[9] == HAL_STOMP_DIM && aux[HAL_LED_DIRECT_SELECT] == 0 && aux[HAL_LED_EXP_1] == 1);
    CHECK(aux[HAL_LED_EXP_2] == HAL_LED_DIM);

    // Saving stores them with the bank: the follows in the footswitch records, the
    // triggers in the bank record (as mask low, mask high, A, B here).
    save_exit();
    CHECK(bank_saved == 1);
    CHECK(saved_sw(0, 0) == 4 && saved_sw(0, 1) == 0 && saved_sw(0, 2) == v && saved_sw(0, 3) == 0);
    CHECK(saved_sw(1, 0) == 0 && saved_sw(1, 1) == 2 && saved_sw(1, 2) == 0 && saved_sw(1, 3) == ctl_scale(64, 0, 255));
    CHECK(aux[HAL_LED_SWITCH_1] == 0 && aux[HAL_LED_SWITCH_2] == 0 && aux[HAL_LED_DIRECT_SELECT] == 0);

    // Run mode, relay 1: footswitch 3 OR pedal A at v and up; program mode does not
    // move it.
    pedal(0, 0);
    pedal(1, 0);
    ticks(1);
    CHECK(relays == 0 && aux[HAL_LED_SWITCH_1] == 0);
    tap(2);
    ticks(1);
    CHECK(relays == 1 && aux[HAL_LED_SWITCH_1] == 1 && aux[HAL_LED_SWITCH_2] == 0);
    tap(2);
    ticks(1);
    CHECK(relays == 0);
    chord_short();
    ticks(1);
    CHECK(relays == 0 && pc_mode == 1);
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
    keep_saved_msgs();
    chord_long();
    ticks(5);
    CHECK(relays == 3 && aux[HAL_LED_SWITCH_1] == 0 && aux[HAL_LED_SWITCH_2] == 0);
    cancel_exit();
    CHECK(aux[HAL_LED_SWITCH_1] == 1 && aux[HAL_LED_SWITCH_2] == 1);
    // Cancel reloaded the default pedal modes (nothing saved them): toggles again.
    // Only the mode changes here; the states stay as they are.
    pedal_mode_set(2, PM_STOMP);
    pedal_mode_set(9, PM_STOMP);
    // The pedals send at once here, whatever PEDAL_PICKUP_A / _B say.
    pk_mask = 0;
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
    CHECK(sw_thr[0] == 0 && aux[HAL_LED_EXP_1] == HAL_LED_DIM);
    save_exit();
    pedal(0, 0);
    pedal(0, 255);
    ticks(1);
    CHECK(relays == 0 && saved_sw(0, 2) == 0);

    // Cancel drops trigger changes.
    goto_switch_page(2);
    tap(0);
    CHECK(sw_mask[1] == 0x201);
    cancel_exit();
    CHECK(sw_mask[1] == 0x200 && sw_thr[3] == ctl_scale(64, 0, 255));

    // Stored records load at start-up; a trigger's bit 7 belongs to the pedal settings.
    reset_world();
    store_relay(0, 1, 0x01, 200, 50);
    power_on();
    CHECK(sw_mask[1] == 0x01 && sw_thr[2] == (200 & 0x7F) && sw_thr[3] == 50);
    CHECK(sw_mask[0] == 0 && sw_thr[0] == 0 && sw_thr[1] == 0);
    set_mode(0, PM_STOMP);
    tap(0);
    ticks(1);
    CHECK(relays == 2 && aux[HAL_LED_SWITCH_2] == 1);
}

// Run mode: each message sent flashes the MIDI FUNCTION LED for TX_FLASH_TICKS (user's
// request): footswitches, pedals, program changes, a dump. In configuration mode the LED
// marks page 1 instead.
static void test_midi_out_flash(void)
{
    printf("MIDI OUT flashes MIDI FUNCTION\n");
    reset_world();
    set_mode(0, PM_STOMP);
    CHECK(aux[HAL_LED_MIDI_FUNC] == 0);
    tap(sw_code(1));
    ticks(1);
    CHECK(midi_len == 3 && aux[HAL_LED_MIDI_FUNC] == 1);
    ticks(TX_FLASH_TICKS - 2);
    CHECK(aux[HAL_LED_MIDI_FUNC] == 1);
    ticks(1);
    CHECK(aux[HAL_LED_MIDI_FUNC] == 0);
    // Another message restarts it; with none sent it stays dark.
    ticks(TX_FLASH_TICKS * 2);
    CHECK(aux[HAL_LED_MIDI_FUNC] == 0);
    pk_mask = 0;
    pedal(0, 10);
    pedal(0, 200);
    ticks(1);
    CHECK(aux[HAL_LED_MIDI_FUNC] == 1);
    ticks(TX_FLASH_TICKS);
    CHECK(aux[HAL_LED_MIDI_FUNC] == 0);
    program_mode();
    clear_log();
    tap(sw_code(3));
    ticks(1);
    CHECK(midi_len == 2 && aux[HAL_LED_MIDI_FUNC] == 1);
    // Received MIDI does not light it.
    ticks(TX_FLASH_TICKS);
    ctl_midi_in(0xB0); ctl_midi_in(21); ctl_midi_in(127);
    ticks(1);
    CHECK(aux[HAL_LED_MIDI_FUNC] == 0 && aux[HAL_LED_MIDI_CHAN] == 1);
    // Entering configuration mode during a flash: page 1 keeps it lit, and it stays lit.
    tap(sw_code(3));
    chord_long();
    ticks(TX_FLASH_TICKS * 2);
    CHECK(mode == MODE_CONFIG && aux[HAL_LED_MIDI_FUNC] == 1);
    tap(CTL_KEY_UP);
    CHECK(aux[HAL_LED_MIDI_FUNC] == 0);
    save_exit();
    ticks(TX_FLASH_TICKS * 2);
    CHECK(mode == MODE_RUN && aux[HAL_LED_MIDI_FUNC] == 0);
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

    // Entering config mode during a flash: page 1 (modes) has it dark, page 6
    // (channel) lit and steady, and page 7 dark again.
    ctl_midi_in(0xB0);
    chord_long();
    ticks(RX_FLASH_TICKS + 1);
    CHECK(aux[HAL_LED_MIDI_CHAN] == 0);
    to_page(PAGE_CHANNEL);
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
    int k;

    printf("config: calibration (EXP pages)\n");
    reset_world();
    chord_long();
    to_page(PAGE_EXP_A);
    CHECK(aux[HAL_LED_EXP_1] == 1 && aux[HAL_LED_EXP_2] == 0);
    CHECK(aux[HAL_LED_MIDI_FUNC] == 0 && aux[HAL_LED_MIDI_CHAN] == 0);

    // Footswitch 9 calibrates the page's pedal (user's design). Selected, it shows the
    // pedal's value, following the pedal, and takes nothing.
    pedal(0, 128);
    pedal(1, 30);
    tap(sw_code(PED_CAL + 1));
    CHECK(ped_sel == PED_CAL && cal_step == CAL_DONE && !cal_armed && display == ctl_scale(128, 0, 255));
    CHECK(stomp_leds_are(1u << PED_CAL, PED_MASK) && aux[HAL_LED_EXP_1] == 1 && aux[HAL_LED_EXP_2] == 0 && cfg_changed == 0);
    pedal(0, 255);
    CHECK(display == 127);
    // The other pedal does not show.
    pedal(1, 200);
    CHECK(display == 127);
    // Pressed again: "Lo" flashing (blank half the time), nothing taken.
    pedal(0, 20);
    tap(sw_code(PED_CAL + 1));
    CHECK(display == SHOWN_LO && cal_armed && cal_step == CAL_HEEL && heel[0] == 0 && cfg_changed == 0);
    ticks(CHAN_FLASH_TICKS);
    CHECK(blank == 7 && cal_armed);
    ticks(CHAN_FLASH_TICKS);
    CHECK(display == SHOWN_LO && blank == 0);
    // A pedal move does not show while it flashes.
    pedal(0, 21);
    CHECK(display == SHOWN_LO);
    pedal(0, 20);
    // While it runs UP and DOWN step it (user's request): the other footswitches do
    // nothing, footswitch 9 lit and the others dark, and a long UP or DOWN acts as a
    // short one. Footswitch 9 cancels it (user's request).
    tap(sw_code(PED_PICKUP + 1));
    tap(sw_code(PED_CC + 1));
    CHECK(display == SHOWN_LO && cal_step == CAL_HEEL && ped_sel == PED_CAL && stomp_leds_are(1u << PED_CAL, 0) && !pcc_open);
    tap(sw_code(PED_CAL + 1));
    CHECK(cfg_page == PAGE_EXP_A && cal_step == CAL_DONE && !cal_armed && display == ctl_scale(20, 0, 255) && cfg_changed == 0);
    tap(sw_code(PED_CAL + 1));
    CHECK(cal_step == CAL_HEEL && cal_armed);
    // So does DOWN on "Lo": the value again, nothing taken, still on the page.
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_EXP_A && cal_step == CAL_DONE && !cal_armed && display == ctl_scale(20, 0, 255) && cfg_changed == 0);
    tap(sw_code(PED_CAL + 1));
    CHECK(cal_step == CAL_HEEL && cal_armed);
    // UP: the heel reading is taken (kept until the toe is good), "Hi" flashing.
    ctl_key_press(CTL_KEY_UP);
    ctl_key_hold(CTL_KEY_UP);
    ctl_key_release(CTL_KEY_UP);
    CHECK(display == SHOWN_HI && cal_armed && cal_step == CAL_TOE && heel[0] == 0 && cfg_changed == 0 && !save_prompt);
    ticks(CHAN_FLASH_TICKS);
    CHECK(blank == 7);
    ticks(CHAN_FLASH_TICKS);
    CHECK(display == SHOWN_HI && blank == 0);
    // DOWN on "Hi" goes back to "Lo"; UP takes the heel again.
    tap(CTL_KEY_DOWN);
    CHECK(display == SHOWN_LO && cal_step == CAL_HEEL && cal_armed && mode == MODE_CONFIG);
    tap(CTL_KEY_UP);
    CHECK(cal_step == CAL_TOE);
    // UP on "Hi": the toe reading. Both are stored, and the pedal's value shows again.
    pedal(0, 220);
    tap(CTL_KEY_UP);
    CHECK(heel[0] == 20 && toe[0] == 220 && heel[1] == 0 && toe[1] == 255 && cfg_changed == 1 && cfg_page == PAGE_EXP_A);
    CHECK(cal_step == CAL_DONE && !cal_armed && display == 127 && blank == 0);
    ticks(CHAN_FLASH_TICKS * 3);
    CHECK(display == 127 && blank == 0);
    pedal(0, 120);
    CHECK(display == ctl_scale(120, 20, 220));
    // Page 4, EXP B: the same footswitches for pedal B (user's request), EXP 2 lit.
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_EXP_B && pcc_b == 1 && ped_sel == PED_CC && aux[HAL_LED_EXP_1] == 0 && aux[HAL_LED_EXP_2] == 1 && leds[PED_CC] == 1);
    calibrate(1, 30, 230);
    CHECK(display == 127 && heel[1] == 30 && toe[1] == 230 && heel[0] == 20 && toe[0] == 220 && midi_len == 0);

    // Only footswitches 6, 8 and 9 do anything.
    for (k = 1; k <= 10; k++)
    {
        if (k != PED_CC + 1 && k != PED_PICKUP + 1 && k != PED_CAL + 1)
        {
            tap(sw_code(k));
        }
    }

    CHECK(ped_sel == PED_CAL && cal_step == CAL_DONE && stomp_leds_are(1u << PED_CAL, PED_MASK) && !cal_armed);
    // A footswitch acts when let go, and only with UP and DOWN up at both ends: with UP
    // down (the simulator's row 2) nothing happens, and UP's release changes page.
    // Footswitch 6 first shows the CC (user's request), then opens its editor.
    ctl_key_press(sw_code(PED_CC + 1));
    CHECK(!pcc_open && ped_sel == PED_CAL);
    ctl_key_release(sw_code(PED_CC + 1));
    CHECK(!pcc_open && ped_sel == PED_CC && display == pedal_cc[1] && stomp_leds_are(1u << PED_CC, PED_MASK) && aux[HAL_LED_NUMBER] == 0);
    tap(sw_code(PED_CC + 1));
    CHECK(pcc_open && pcc_b == 1 && aux[HAL_LED_NUMBER] == 1);
    tap(CTL_KEY_DOWN);
    CHECK(!pcc_open && cfg_page == PAGE_EXP_B && ped_sel == PED_CC);
    ctl_key_press(CTL_KEY_UP);
    tap(sw_code(PED_CC + 1));
    tap(sw_code(PED_PICKUP + 1));
    CHECK(!pcc_open && ped_sel == PED_CC && pk_mask == (PEDAL_PICKUP_A | (PEDAL_PICKUP_B << 1)));
    ctl_key_release(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_SW1);
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_EXP_B && ped_sel == PED_CC && !pcc_open);
    ctl_key_press(sw_code(PED_CC + 1));
    ctl_key_press(CTL_KEY_DOWN);
    ctl_key_release(CTL_KEY_DOWN);
    ctl_key_release(sw_code(PED_CC + 1));
    CHECK(!pcc_open && cfg_page == PAGE_EXP_A);

    // Coming back to the page starts on footswitch 6 again; nothing is sent.
    tap(sw_code(PED_CAL + 1));
    tap(CTL_KEY_UP);
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_EXP_A && ped_sel == PED_CC && pcc_b == 0 && leds[PED_CC] == 1 && midi_len == 0);

    save_exit();
    CHECK(cal_saved == 1);
    CHECK(saved_heel[0] == 20 && saved_heel[1] == 30 && saved_toe[0] == 220 && saved_toe[1] == 230);

    // The new range is in use, and the first reading is adopted silently (the pedals
    // sending at once, whatever PEDAL_PICKUP_A / _B say).
    pk_mask = 0;
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

    // A toe less than CAL_MIN_SPAN from the heel shows "Er" (steady) for
    // CAL_ERROR_SHOW_MS, then the pedal's value, and changes nothing.
    reset_world();
    chord_long();
    to_page(PAGE_EXP_A);
    calibrate(0, 100, 100 + CAL_MIN_SPAN - 1);
    CHECK(display == SHOWN_ER && cal_step == CAL_ERROR && !cal_armed && heel[0] == 0 && toe[0] == 255 && cfg_changed == 0);
    ticks(CAL_ERROR_BLINKS * CHAN_FLASH_TICKS - 1);
    CHECK(display == SHOWN_ER && blank == 0);
    ticks(1);
    CHECK(cal_step == CAL_DONE && display == ctl_scale(100 + CAL_MIN_SPAN - 1, 0, 255));
    // The same below the heel; footswitch 9 while "Er" shows starts again ("Lo").
    calibrate(0, 100, 100 - CAL_MIN_SPAN + 1);
    CHECK(display == SHOWN_ER && heel[0] == 0);
    tap(sw_code(PED_CAL + 1));
    CHECK(display == SHOWN_LO && cal_step == CAL_HEEL && cal_armed);
    calibrate(0, 100, 200);
    CHECK(cal_step == CAL_DONE && display == 127 && heel[0] == 100 && toe[0] == 200);
    // Each pedal on its own: pedal B is untouched.
    CHECK(heel[1] == 0 && toe[1] == 255);
    cancel_exit();
    CHECK(heel[0] == 0 && toe[0] == 255);

    // A wired-the-other-way pedal (toe below heel) is accepted.
    reset_world();
    chord_long();
    to_page(PAGE_EXP_B);
    calibrate(1, 200, 20);
    CHECK(cal_step == CAL_DONE && display == 127);
    save_exit();
    CHECK(saved_heel[1] == 200 && saved_toe[1] == 20);

    // Stored calibration is loaded at start-up.
    reset_world();
    cal_stored_valid = 1;
    stored_heel[0] = 40; stored_heel[1] = 50;
    stored_toe[0]  = 140; stored_toe[1] = 150;
    power_on();
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
    power_on();
    CHECK(midi_channel == 5);
    chord_long();
    CHECK(status_led == 1 && status_toggles(CFG_FLASH_TICKS * 3) == 0);
    to_page(PAGE_CHANNEL);
    tap(chan_key(6));
    ticks(1);
    CHECK(midi_channel == 6 && status_led == 0 && status_toggles(CFG_FLASH_TICKS * 2) == 2);
    tap(chan_key(5));
    ticks(1);
    CHECK(midi_channel == 5 && status_led == 1 && status_toggles(CFG_FLASH_TICKS * 3) == 0);
    cancel_exit();

    // Invalid records count as the defaults, as at start-up: a stored channel of 0xEE and
    // stray bytes after it match the default channel; stored default bank records match.
    reset_world();
    cfg_stored_valid = 1;
    stored_cfg[0] = 0xEE;
    stored_cfg[1] = (uint8_t)(DEFAULT_PEDAL_MODE * 0x55);
    stored_cfg[2] = (uint8_t)(DEFAULT_PEDAL_MODE * 0x55);
    stored_cfg[3] = (uint8_t)(DEFAULT_PEDAL_MODE * 0x05 | 0xF0);
    store_bank_defaults(0);
    power_on();
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
    power_on();
    CHECK(heel[0] == 10 && toe[1] == 210);
    chord_long();
    CHECK(status_led == 1);
    to_page(PAGE_EXP_A);
    calibrate(0, 10, 200);
    CHECK(status_led == 1 && status_toggles(CFG_FLASH_TICKS * 3) == 0);
    calibrate(0, 10, 230);
    CHECK(toe[0] == 230 && status_led == 0 && status_toggles(CFG_FLASH_TICKS * 2) == 2);
    cancel_exit();

    // A save that did not reach the EEPROM (the fake keeps saves apart from what it
    // loads, like a failed write): the next visit flashes at once, with no edits.
    reset_world();
    chord_long();
    to_page(PAGE_CHANNEL);
    tap(chan_key(MIDI_CHANNEL == 7 ? 8 : 7));
    ticks(1);
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
    power_on();
    CHECK(boot_anim == 1 && mode == MODE_RUN);
    // Tick by tick from power-on: the start-up settling (STARTUP_TICKS, during which main()
    // calls ctl_settle_tick, not ctl_tick), then ctl_tick. Power-on takes ten frames,
    // 10 x SCROLL_FRAME_MS, every frame its full time with the settling counted, then (on
    // the last tick of the trailing dot) run mode. Holds whatever the frame length,
    // including frames shorter than the settling.
    {
        static const uint8_t lead_tail[4] = { 0x01, 0x02, 0x02, 0x04 };
        int k, f, n = 0, bad = 0;
        int frame_ticks = VER_STEP_TICKS * VER_SCROLL_STEPS;

        CHECK(display == 0 && blank == 7 && dots == 0x01 && lamp == HAL_LAMP_LEDS_ON);

        for (k = 0; k < 10 * frame_ticks && boot_anim; k++)
        {
            f = k / frame_ticks;

            if (f < 2 || f >= 8)
            {
                bad += !(display == 0 && blank == 7 && dots == lead_tail[f < 2 ? f : f - 6]);
            }
            else
            {
                bad += !(display == SHOWN_TEXT(HAL_TEXT_SCROLL + f - 2) && blank == 0 && dots == scroll_dot_of[f - 2]);
            }

            // Every LED lit for BOOT_LAMP_MS from power-on, then the normal LEDs.
            bad += (lamp != ((k * TICK_MS < BOOT_LAMP_MS) ? HAL_LAMP_LEDS_ON : HAL_LAMP_NORMAL) || midi_len != 0);

            // The settling, then one ctl_tick per 2ms.
            if (k < STARTUP_TICKS)
            {
                ctl_settle_tick();
            }
            else
            {
                ticks(1);
                n++;
            }
        }

        CHECK(bad == 0);
        CHECK(!boot_anim && (n + STARTUP_TICKS) * TICK_MS == 10 * SCROLL_FRAME_MS);
    }
    // Then run mode (the version never shows): normal LEDs, no dots, bank 1 on the display (bank mode), steps
    // back to their idle values.
    CHECK(boot_anim == 0 && lamp == HAL_LAMP_NORMAL && dots == 0 && display == 1 && !pc_mode && aux[HAL_LED_DIRECT_SELECT] == 0);
    CHECK(cal_step == CAL_DONE && chan_flash_ticks == 0 && mode == MODE_RUN && midi_len == 0);
    ticks(VER_STEP_TICKS * VER_END_STEP);
    CHECK(lamp == HAL_LAMP_NORMAL && display == 1 && midi_len == 0);

    // A footswitch during the animation ends it and then works as usual.
    reset_world();
    set_mode(0, PM_STOMP);
    power_on();
    set_mode(0, PM_STOMP);
    clear_log();
    ticks(VER_STEP_TICKS * ((VER_LEAD_FRAMES + 1) * VER_SCROLL_STEPS + 1));
    CHECK(boot_anim == 1 && display == SHOWN_TEXT(HAL_TEXT_SCROLL + 1) && dots == scroll_dot_of[1]);
    tap(0);
    CHECK(boot_anim == 0 && lamp == HAL_LAMP_NORMAL && dots == 0 && display == 1);
    CHECK(midi_len == 3 && cc_number(0) == stomp_cc[0] && cc_value(0) == 127 && leds[0] == 1);
    ticks(VER_STEP_TICKS * VER_END_STEP);
    CHECK(lamp == HAL_LAMP_NORMAL && midi_len == 3);

    // UP during the animation: it ends, and the release steps the program as usual.
    reset_world();
    power_on();
    clear_log();
    ticks(VER_STEP_TICKS);
    tap(CTL_KEY_UP);
    CHECK(boot_anim == 0 && bank_next == 1 && bank_busy && program == 0 && midi_len == 0 && display == 2);

    // MIDI IN during the animation sets the stomp state under it (shown afterwards).
    reset_world();
    set_mode(0, PM_STOMP);
    power_on();
    set_mode(0, PM_STOMP);
    ctl_midi_in((uint8_t)(0xB0 | (MIDI_CHANNEL - 1)));
    ctl_midi_in(stomp_cc[0]);
    ctl_midi_in(127);
    CHECK(boot_anim == 1 && leds[0] == 1 && lamp == HAL_LAMP_LEDS_ON);
    ticks(VER_STEP_TICKS * (VER_LEAD_FRAMES * VER_SCROLL_STEPS + VER_BOOT_END_STEP));
    CHECK(boot_anim == 0 && lamp == HAL_LAMP_NORMAL && leds[0] == 1);
}

static void test_config_led_flash(void)
{
    printf("config: CONFIG LED flashes after a change\n");
    reset_world();
    chord_long();
    CHECK(mode == MODE_CONFIG && status_led == 1);
    // Selecting a switch (not the selected one) and changing pages are not changes.
    mode_tap(1);
    tap(CTL_KEY_UP);
    tap(CTL_KEY_DOWN);
    CHECK(status_toggles(CFG_FLASH_TICKS * 4) == 0 && status_led == 1);
    // Cycling switch 2's mode is a change: dark at once, then toggling.
    mode_tap(1);
    CHECK(status_led == 0);
    CHECK(status_toggles(CFG_FLASH_TICKS * 4) == 4);
    // Exact timing: the half-period is CFG_FLASH_TICKS ticks (from an edge).
    while (status_led == 0) ctl_tick();
    while (status_led != 0) ctl_tick();
    CHECK(status_toggles(CFG_FLASH_TICKS - 1) == 0);
    CHECK(status_toggles(1) == 1 && status_led == 1);
    // Cycling the mode back to where it was (four modes) stops the flash, steady lit,
    // even from the dark half; another change starts it again.
    mode_tap(1);
    mode_tap(1);
    while (status_led != 0) ctl_tick();
    mode_tap(1);
    CHECK(status_led == 1 && status_toggles(CFG_FLASH_TICKS * 4) == 0 && cfg_changed == 0);
    mode_tap(1);
    CHECK(status_led == 0 && status_toggles(CFG_FLASH_TICKS * 2) == 2);
    mode_tap(1);
    mode_tap(1);
    mode_tap(1);
    CHECK(status_led == 1 && status_toggles(CFG_FLASH_TICKS * 2) == 0);
    mode_tap(1);
    // Cancel stops it; the LED is off in run mode and stays off.
    cancel_exit();
    CHECK(mode == MODE_RUN && status_led == 0 && status_toggles(CFG_FLASH_TICKS * 3) == 0);
    // Next entry is steady again.
    chord_long();
    CHECK(status_led == 1 && status_toggles(CFG_FLASH_TICKS * 3) == 0);
    // Channel page: pressing the channel already set is not a change, a new one is.
    to_page(PAGE_CHANNEL);
    CHECK(cfg_page == PAGE_CHANNEL);
    tap((uint8_t)(MIDI_CHANNEL - 1 < 5 ? MIDI_CHANNEL - 1 : MIDI_CHANNEL));
    ticks(1);
    CHECK(status_led == 1 && status_toggles(CFG_FLASH_TICKS * 2) == 0);
    tap(MIDI_CHANNEL == 3 ? 3 : 2);
    ticks(1);
    CHECK(status_led == 0 && status_toggles(CFG_FLASH_TICKS * 2) == 2);
    // Back to the entry channel: steady again; then a new channel flashes once more.
    tap((uint8_t)(MIDI_CHANNEL - 1 < 5 ? MIDI_CHANNEL - 1 : MIDI_CHANNEL));
    ticks(1);
    CHECK(status_led == 1 && status_toggles(CFG_FLASH_TICKS * 2) == 0);
    tap(MIDI_CHANNEL == 3 ? 3 : 2);
    ticks(1);
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

    // Page 8 with an unsaved change: the CONFIG LED flashes through the version display
    // (the HAL keeps it out of the lamp test's LEDs-on mode); with none it stays lit.
    {
    int k;

    reset_world();
    chord_long();
    mode_tap(1);
    mode_tap(1);
    for (k = 0; k < PAGE_VERSION; k++)
    {
        tap(CTL_KEY_UP);
    }
    CHECK(cfg_page == PAGE_VERSION && cfg_changed == 1);
    CHECK(lamp == HAL_LAMP_LEDS_ON && status_toggles(CFG_FLASH_TICKS * 4) == 4);
    // Putting the change back elsewhere and returning: steady.
    for (k = 0; k < PAGE_VERSION; k++)
    {
        tap(CTL_KEY_DOWN);
    }
    mode_tap(1);
    mode_tap(1);
    mode_tap(1);
    for (k = 0; k < PAGE_VERSION; k++)
    {
        tap(CTL_KEY_UP);
    }
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
    store_modes((uint8_t)((PM_MOM_INV << 2) | (PM_MOMENTARY << 4)), 0xFF, 0x00);
    memset(leds, 0xEE, sizeof(leds));
    clear_log();
    power_on();
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
    store_modes((uint8_t)(PM_MOM_INV << 2), 0, 0);
    store_relay(0, 0, 0x02, 0, 0);
    power_on();
    ticks(1);
    CHECK(relays == 1 && aux[HAL_LED_SWITCH_1] == 1 && midi_len == 0);
}

// Set footswitch sw's (0-9) message as if it had been saved.
static void set_msg(uint8_t sw, uint8_t type, uint8_t num, uint8_t on, uint8_t off)
{
    msg[sw * MSG_STRIDE]     = (uint8_t)(num & 0x7F);
    msg[sw * MSG_STRIDE + 1] = (uint8_t)(on & 0x7F);
    msg[sw * MSG_STRIDE + 2] = (uint8_t)(off & 0x7F);
    msg[sw * MSG_STRIDE + MSG_TYPE] = (uint8_t)(type & 3);
}

#define MSG_NUM(sw)  (msg[(sw) * MSG_STRIDE])
#define MSG_ON(sw)   (msg[(sw) * MSG_STRIDE + 1])
#define MSG_OFF(sw)  (msg[(sw) * MSG_STRIDE + 2])

#define NOTE_ON   (0x90 | (MIDI_CHANNEL - 1))
#define NOTE_OFF  (0x80 | (MIDI_CHANNEL - 1))
#define PROG_CHG  (0xC0 | (MIDI_CHANNEL - 1))

// Only the editor parameter's LED is lit (SELECT type, NUMBER, VALUE 1 on, VALUE 2 off;
// MIDI FUNCTION, the pedal mode, is checked apart).
static int param_leds(int p)
{
    // MIDI FUNCTION marks page 1 and the editor; SWITCH is the pedal mode (user's request).
    return aux[HAL_LED_MIDI_FUNC] == 1 && aux[HAL_LED_SWITCH] == (p == EP_FUNC) &&
           aux[HAL_LED_SELECT] == (p == EP_TYPE) && aux[HAL_LED_NUMBER] == (p == EP_NUM) &&
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
    // The LED shows the state; the program sent becomes the current program, so in
    // program mode UP / DOWN step from it (the display shows the bank until then).
    set_mode(4, PM_STOMP);
    set_msg(4, MT_PROG, 0, 10, 11);
    clear_log();
    tap(sw_code(5));
    CHECK(midi_len == 2 && midi_log[0] == PROG_CHG && midi_log[1] == 10 && leds[4] == 1);
    CHECK(program == 10 && display == 1);
    tap(sw_code(5));
    CHECK(midi_len == 4 && midi_log[2] == PROG_CHG && midi_log[3] == 11 && leds[4] == 0 && program == 11);
    clear_log();
    program_mode();
    CHECK(display == 11 + DISPLAY_PROGRAM_OFFSET);
    tap(CTL_KEY_UP);
    CHECK(midi_len == 2 && midi_log[0] == PROG_CHG && midi_log[1] == 12);
    pc_mode_toggle();
    // Momentary: the first program while held, the second on release.
    set_mode(4, PM_MOMENTARY);
    clear_log();
    ctl_key_press(sw_code(5));
    CHECK(midi_len == 2 && midi_log[1] == 10 && leds[4] == 1);
    ctl_key_release(sw_code(5));
    CHECK(midi_len == 4 && midi_log[3] == 11 && leds[4] == 0 && display == 1);
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
    power_on();
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
    power_on();
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
    tap(CTL_KEY_UP);
    CHECK(edit_param == EP_ON);
    tap(sw_code(9));
    edit_close();
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
    power_on();
    boot_end();
    CHECK(msg_type(4) == MT_CC && MSG_NUM(4) == 33 && MSG_ON(4) == 44 && MSG_OFF(4) == 12);
    cfg_stored_valid = 0;
    memset(msg_stored_valid, 0, sizeof(msg_stored_valid));
}

// Page 1 and the footswitch editor (user's design): page 1 selects a footswitch and shows
// its settings in turn; pressed again (or held) it opens the editor on MIDI FUNCTION, UP
// and DOWN move through MIDI FUNCTION, SELECT, NUMBER, VALUE 1, VALUE 2, UP on VALUE 2
// accepts and DOWN on MIDI FUNCTION cancels.
static void test_config_editor(void)
{
    const uint8_t cc[10] = { STOMP_CC_LIST };
    const int show = CONFIG_SHOW_MS / TICK_MS;
    const int swshow = CONFIG_SWITCH_SHOW_MS / TICK_MS;

    printf("config: footswitch editor\n");
    reset_world();
    set_mode(2, PM_STOMP);
    chord_long();
    CHECK(mode == MODE_CONFIG && cfg_page == PAGE_MODES && cfg_sel == 0);

    // Page 1 shows switch 1's number for CONFIG_SWITCH_SHOW_MS (no setting LED), then its
    // settings in turn, CONFIG_SHOW_MS each, each with its LED, steady (user's request):
    // the pedal mode (SWITCH), type (SELECT), number, on value, off value.
    CHECK(display == 1 && param_leds(-1));
    CHECK(stomp_leds_are(1, 0));
    ticks(swshow - 1);
    CHECK(display == 1);
    ticks(1);
    CHECK(display == SHOWN_MODE(pedal_mode_get(0)) && param_leds(EP_FUNC));
    ticks(show - 1);
    CHECK(display == SHOWN_MODE(pedal_mode_get(0)) && aux[HAL_LED_MIDI_FUNC] == 1);
    ticks(1);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_CC) && param_leds(EP_TYPE));
    ticks(show);
    CHECK(display == cc[0] && param_leds(EP_NUM));
    ticks(show);
    CHECK(display == 127 && param_leds(EP_ON));
    ticks(show);
    CHECK(display == 0 && param_leds(EP_OFF));
    ticks(show);
    CHECK(display == 1 && param_leds(-1));
    ticks(swshow);
    CHECK(display == SHOWN_MODE(pedal_mode_get(0)) && param_leds(EP_FUNC));
    CHECK(midi_len == 0 && !edit_open && cfg_changed == (DEFAULT_PEDAL_MODE != PM_STOMP));

    // A program change has no number: it is skipped.
    set_msg(1, MT_PROG, 99, 3, 4);
    tap(sw_code(2));
    ticks(swshow + show);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_PROG));
    ticks(show);
    CHECK(display == 3 && param_leds(EP_ON));
    set_msg(1, MT_CC, cc[1], 127, 0);

    // Selecting switch 3 starts again there; nothing changes.
    tap(sw_code(3));
    CHECK(cfg_sel == 2 && display == 3 && !edit_open && stomp_leds_are(1u << 2, 0));
    ticks(swshow);
    CHECK(display == SHOWN_MODE(PM_STOMP));
    ticks(show);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_CC));

    // Pressed again it opens the editor on release (not on press), on the pedal mode:
    // MIDI FUNCTION lit, footswitch 1 (St) lit and 2-4 (+St, Pu, -Pu) dim.
    ctl_key_press(sw_code(3));
    CHECK(!edit_open);
    ctl_key_release(sw_code(3));
    CHECK(edit_open && edit_param == EP_FUNC && param_leds(EP_FUNC));
    CHECK(display == SHOWN_MODE(PM_STOMP) && stomp_leds_are(1u << PM_STOMP, 0x0F));
    // The display stays on the setting being edited.
    ticks(show * 2);
    CHECK(display == SHOWN_MODE(PM_STOMP) && edit_param == EP_FUNC);
    tap(sw_code(3));
    CHECK(pedal_mode_get(2) == PM_MOMENTARY && display == SHOWN_MODE(PM_MOMENTARY) && stomp_leds_are(1u << 2, 0x0F));
    tap(sw_code(7));
    CHECK(pedal_mode_get(2) == PM_MOMENTARY);
    tap(sw_code(2));
    CHECK(pedal_mode_get(2) == PM_STOMP_MOM && display == SHOWN_MODE(PM_STOMP_MOM));
    // The CONFIG LED follows on the next tick (whether the settings differ from the EEPROM).
    ticks(1);
    CHECK(cfg_changed == (DEFAULT_PEDAL_MODE != PM_STOMP_MOM));

    // UP: the type (SELECT lit): switch 6 (CC) lit, 1 (program change) and 10 (note) dim
    // (user's layout); the others do nothing.
    tap(CTL_KEY_UP);
    CHECK(edit_param == EP_TYPE && param_leds(EP_TYPE));
    CHECK(display == SHOWN_TEXT(HAL_TEXT_CC) && stomp_leds_are(1u << 5, TYPE_MASK));
    tap(sw_code(10));
    CHECK(msg_type(2) == MT_NOTE && display == SHOWN_TEXT(HAL_TEXT_NOTE) && stomp_leds_are(1u << 9, TYPE_MASK));
    tap(sw_code(2));
    tap(sw_code(3));
    CHECK(msg_type(2) == MT_NOTE);

    // UP: the number (NUMBER lit), any first digit (all dim, none being typed).
    tap(CTL_KEY_UP);
    CHECK(edit_param == EP_NUM && param_leds(EP_NUM) && display == cc[2] && !num_typing);
    CHECK(stomp_leds_are(0, digit_leds("0123456789")));
    // 6: 60-69 can follow, so the dot flashes and 0-9 are dim; 4 makes 64, complete.
    tap(sw_code(6));
    CHECK(MSG_NUM(2) == 6 && display == 6 && num_typing && stomp_leds_are(0, digit_leds("0123456789")));
    ticks(1);
    CHECK(dots == 1);
    tap(sw_code(4));
    CHECK(MSG_NUM(2) == 64 && display == 64 && !num_typing && msg_type(2) == MT_NOTE);
    ticks(1);
    CHECK(dots == 0);
    // 1, 2: 120-127 can follow (0-7 dim); 8 cannot; 7 makes 127.
    tap(sw_code(1)); tap(sw_code(2));
    CHECK(MSG_NUM(2) == 12 && num_typing && stomp_leds_are(0, digit_leds("01234567")));
    tap(sw_code(8));
    CHECK(MSG_NUM(2) == 12 && num_typing);
    tap(sw_code(7));
    CHECK(MSG_NUM(2) == 127 && !num_typing);
    // 10 types 0, complete at once (nothing follows a leading 0); 1, 10, 10 make 100.
    tap(sw_code(10));
    CHECK(MSG_NUM(2) == 0 && !num_typing);
    tap(sw_code(1)); tap(sw_code(10)); tap(sw_code(10));
    CHECK(MSG_NUM(2) == 100 && !num_typing && msg_type(2) == MT_NOTE);
    // 5 then DOWN: back to 100, still on NUMBER; 5 then UP: 5, still on NUMBER.
    tap(sw_code(5));
    CHECK(MSG_NUM(2) == 5 && num_typing);
    tap(CTL_KEY_DOWN);
    CHECK(edit_open && edit_param == EP_NUM && MSG_NUM(2) == 100 && display == 100 && !num_typing);
    tap(sw_code(5));
    tap(CTL_KEY_UP);
    CHECK(edit_open && edit_param == EP_NUM && MSG_NUM(2) == 5 && !num_typing);
    tap(sw_code(1)); tap(sw_code(10)); tap(sw_code(10));

    // UP: the on value (VALUE 1 lit). A new parameter starts a new number.
    tap(CTL_KEY_UP);
    CHECK(edit_param == EP_ON && param_leds(EP_ON) && display == 127 && !num_typing);
    tap(sw_code(9)); tap(sw_code(10));
    CHECK(MSG_ON(2) == 90 && msg_type(2) == MT_NOTE);
    // UP: the off value (VALUE 2 lit).
    tap(CTL_KEY_UP);
    CHECK(edit_param == EP_OFF && param_leds(EP_OFF) && display == 0);
    tap(sw_code(3)); tap(sw_code(5));
    CHECK(MSG_OFF(2) == 35 && MSG_ON(2) == 90 && msg_type(2) == MT_NOTE);
    // DOWN steps back through them, keeping each: on value, number, type, pedal mode.
    tap(CTL_KEY_DOWN);
    CHECK(edit_param == EP_ON && display == 90);
    tap(CTL_KEY_DOWN);
    CHECK(edit_param == EP_NUM && display == 100);
    tap(CTL_KEY_DOWN);
    CHECK(edit_param == EP_TYPE && display == SHOWN_TEXT(HAL_TEXT_NOTE));
    tap(CTL_KEY_DOWN);
    CHECK(edit_param == EP_FUNC && display == SHOWN_MODE(PM_STOMP_MOM) && edit_open);
    // UP back to VALUE 2; UP there accepts and closes the editor: page 1, switch 3's
    // settings from the first.
    tap(CTL_KEY_UP); tap(CTL_KEY_UP); tap(CTL_KEY_UP); tap(CTL_KEY_UP);
    CHECK(edit_param == EP_OFF && edit_open);
    tap(CTL_KEY_UP);
    CHECK(!edit_open && mode == MODE_CONFIG && cfg_page == PAGE_MODES && param_leds(-1));
    CHECK(aux[HAL_LED_MIDI_FUNC] == 1 && display == 3 && stomp_leds_are(1u << 2, 0));
    CHECK(msg_type(2) == MT_NOTE && MSG_NUM(2) == 100 && MSG_ON(2) == 90 && MSG_OFF(2) == 35);
    CHECK(pedal_mode_get(2) == PM_STOMP_MOM && midi_len == 0);
    // With no editor, UP / DOWN change the page.
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_CHANNEL);
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_MODES && cfg_sel == 2);

    // A hold opens it too. A program change's two programs are VALUE 1 (sent on) and
    // VALUE 2 (sent off): NUMBER is skipped both ways. DOWN on MIDI FUNCTION cancels
    // everything changed since it opened. Footswitch holds do nothing in it.
    long_press(sw_code(3));
    CHECK(edit_open && edit_param == EP_FUNC);
    tap(sw_code(4));
    long_press(sw_code(5));
    CHECK(pedal_mode_get(2) == PM_MOM_INV && midi_len == 0);
    tap(CTL_KEY_UP);
    tap(sw_code(1));
    CHECK(msg_type(2) == MT_PROG && display == SHOWN_TEXT(HAL_TEXT_PROG) && stomp_leds_are(1, TYPE_MASK));
    tap(CTL_KEY_UP);
    CHECK(edit_param == EP_ON && param_leds(EP_ON) && display == 90);
    tap(sw_code(5));
    CHECK(MSG_ON(2) == 5 && num_typing);
    tap(CTL_KEY_DOWN);
    CHECK(edit_param == EP_ON && MSG_ON(2) == 90);
    tap(sw_code(5));
    tap(CTL_KEY_UP);
    tap(CTL_KEY_DOWN);
    CHECK(edit_param == EP_TYPE && MSG_ON(2) == 5);
    tap(CTL_KEY_DOWN);
    CHECK(edit_param == EP_FUNC);
    tap(CTL_KEY_DOWN);
    CHECK(!edit_open && pedal_mode_get(2) == PM_STOMP_MOM && msg_type(2) == MT_NOTE && MSG_NUM(2) == 100 &&
          MSG_ON(2) == 90 && MSG_OFF(2) == 35 && display == 3);

    // A long UP is a short one in the editor (it does not save and leave), and so is a
    // long DOWN.
    long_press(sw_code(3));
    long_nav(CTL_KEY_UP);
    CHECK(edit_open && mode == MODE_CONFIG && edit_param == EP_TYPE && cfg_saved == 0);
    long_nav(CTL_KEY_DOWN);
    CHECK(edit_open && mode == MODE_CONFIG && edit_param == EP_FUNC);
    long_nav(CTL_KEY_DOWN);
    CHECK(!edit_open && mode == MODE_CONFIG);

    // Save and leave: every footswitch record is saved; switch 3 sends its note.
    save_exit();
    CHECK(mode == MODE_RUN && msg_saved == 10);
    CHECK((saved_msg[2][0] & 3) == MT_NOTE && saved_msg[2][1] == 100 && saved_msg[2][2] == 90 && saved_msg[2][3] == 35);
    CHECK(((saved_msg[2][0] >> 2) & 3) == PM_STOMP_MOM);
    CHECK((saved_msg[0][0] & 3) == MT_CC && saved_msg[0][1] == cc[0] && saved_msg[0][2] == 127 && saved_msg[0][3] == 0);
    CHECK(saved_msg[9][1] == cc[9] && aux[HAL_LED_SWITCH] == 0 && aux[HAL_LED_SELECT] == 0 && aux[HAL_LED_NUMBER] == 0 &&
          aux[HAL_LED_VALUE_1] == 0 && aux[HAL_LED_VALUE_2] == 0);
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
    tap(CTL_KEY_UP); tap(CTL_KEY_UP);
    tap(sw_code(5)); tap(sw_code(5));
    CHECK(midi_len == 0);
    edit_close();
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
    tap(CTL_KEY_UP); tap(CTL_KEY_UP);
    tap(sw_code(9));
    edit_close();
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
    edit_close();
    save_exit();
    ticks(1);
    CHECK(midi_len == 0 && leds[0] == 1);

    // Only the off value changed: that is a change too; so is only the mode.
    reset_world();
    chord_long();
    long_press(sw_code(1));
    tap(CTL_KEY_UP); tap(CTL_KEY_UP); tap(CTL_KEY_UP); tap(CTL_KEY_UP);
    tap(sw_code(4));
    edit_close();
    CHECK(MSG_OFF(0) == 4 && (cfg_dirty & 1));
    long_press(sw_code(2));
    tap(sw_code(((DEFAULT_PEDAL_MODE + 1) & 3) + 1));
    edit_close();
    CHECK((cfg_dirty & 2) && pedal_mode_get(1) == ((DEFAULT_PEDAL_MODE + 1) & 3));
    cancel_exit();
    CHECK(MSG_OFF(0) == 0 && pedal_mode_get(1) == DEFAULT_PEDAL_MODE);

    // Pedals set nothing in the editor (user's request): neither the type nor a value.
    reset_world();
    chord_long();
    long_press(sw_code(4));
    tap(CTL_KEY_UP);
    pedal(0, 0);
    pedal(0, 128);
    pedal(0, 255);
    pedal(1, 255);
    pedal(1, 10);
    CHECK(msg_type(3) == MT_CC && display == SHOWN_TEXT(HAL_TEXT_CC));
    tap(CTL_KEY_UP);
    tap(sw_code(2)); tap(CTL_KEY_UP);
    pedal(1, 128);
    pedal(1, 10);
    CHECK(MSG_NUM(3) == 2 && display == 2 && msg_type(3) == MT_CC);
    tap(CTL_KEY_UP); tap(CTL_KEY_UP);
    pedal(0, 0);
    pedal(0, 200);
    CHECK(MSG_OFF(3) == 0 && MSG_NUM(3) == 2 && edit_param == EP_OFF);
    // Nothing is sent before the editor closes.
    CHECK(midi_len == 0);
    // A long UP + DOWN saves and leaves with the editor open: the change is kept.
    tap(sw_code(9));
    chord_long();
    CHECK(mode == MODE_RUN && !edit_open && saved_msg[3][1] == 2 && saved_msg[3][3] == 9 &&
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
    // Nor does a short press of it with DOWN held.
    chord_long();
    ctl_key_press(CTL_KEY_DOWN);
    tap(sw_code(1));
    ctl_key_release(CTL_KEY_DOWN);
    CHECK(!edit_open && cfg_page == PAGE_MODES);
    cancel_exit();

    // The editor is only on the modes page: a long press elsewhere does what a press does.
    reset_world();
    chord_long();
    to_page(PAGE_CHANNEL);
    long_press(sw_code(3));
    CHECK(!edit_open && midi_channel == 3);
    cancel_exit();

    // Cancelling config mode after an accepted change restores the saved message.
    reset_world();
    chord_long();
    long_press(sw_code(1));
    tap(CTL_KEY_UP);
    tap(sw_code(10));
    edit_close();
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
    tap(CTL_KEY_UP);
    tap(sw_code(1));
    ticks(1);
    CHECK(cfg_changed);
    tap(sw_code(6));
    ticks(1);
    CHECK(!cfg_changed && status_led == 1);
    tap(sw_code(10));
    ticks(1);
    CHECK(cfg_changed);
    // DOWN back to MIDI FUNCTION, DOWN again cancels: as it was.
    long_nav(CTL_KEY_DOWN);
    long_nav(CTL_KEY_DOWN);
    ticks(1);
    CHECK(!edit_open && !cfg_changed && status_led == 1);
    cancel_exit();
    memset(msg_stored_valid, 0, sizeof(msg_stored_valid));
}

// A SysEx configuration message to device dev (7F all, or a channel - 1): F0 7D 46 43
// <dev> <cmd>, the n bytes in d, F7.
static void sx_send(uint8_t dev, uint8_t cmd, const uint8_t *d, int n)
{
    int i;

    ctl_midi_in(0xF0); ctl_midi_in(0x7D); ctl_midi_in(0x46); ctl_midi_in(0x43);
    ctl_midi_in(dev);  ctl_midi_in(cmd);

    for (i = 0; i < n; i++)
    {
        ctl_midi_in(d[i]);
    }

    ctl_midi_in(0xF7);
}

// Set parameter p to v, addressed to channel ch (1-16), or to the pedal's channel.
static void mset_on(uint8_t ch, uint8_t p, uint8_t v)
{
    uint8_t d[2];

    d[0] = p;
    d[1] = v;
    sx_send((uint8_t)(ch - 1), SX_CMD_SET, d, 2);
}

// What a dump would give for parameter p (the few the tests read back).
static uint8_t param_get(uint8_t p)
{
    if (p == NP_BANK_PC) return BANK_PC_PROGRAM;
    if (p == NP_BANK_PC_ON) return (uint8_t)(bank_pc != 0);
    if (p >= NP_PEDAL_CC) return pedal_cc[p - NP_PEDAL_CC];
    return (uint8_t)((pk_mask >> (p - NP_PICKUP)) & 1);
}

static void mset(uint8_t p, uint8_t v)
{
    mset_on(midi_channel, p, v);
}

// Ask for a dump of bank b (0 the bank in use).
static void sx_get(uint8_t b)
{
    sx_send(0x7F, SX_CMD_GET, &b, 1);
}

// Parse a SysEx dump in the MIDI log from index from into the parameters it stands for:
// dump_values[p], dump_seen[p], dump_bank_of (the bank described), and dump_end (the
// whole message, with its F7, well formed). Returns the number of parameters, or -1.
static int dump_values[NP_COUNT], dump_seen[NP_COUNT], dump_end, dump_bank_of;

static void dump_put(int p, int v)
{
    dump_values[p] = v;
    dump_seen[p]   = 1;
}

static int parse_dump(int from)
{
    const uint8_t *d = midi_log + from;
    int n = 0, i, k, f;

    memset(dump_seen, 0, sizeof(dump_seen));
    dump_end = 0;

    if (midi_len - from < SX_DUMP_LEN || d[0] != 0xF0 || d[1] != 0x7D || d[2] != 0x46 || d[3] != 0x43 ||
        d[4] != midi_channel - 1 || d[5] != SX_CMD_DUMP || d[SX_DUMP_LEN - 1] != 0xF7)
    {
        return -1;
    }

    for (i = 1; i < SX_DUMP_LEN - 1; i++)
    {
        if (d[i] >= 0x80)
        {
            return -1;
        }
    }

    dump_end     = 1;
    dump_bank_of = d[6];
    d += 7;
    dump_put(NP_CHANNEL, d[0]);
    dump_put(NP_VERSION, d[1] | ((d[4] & 1) << 7));
    dump_put(NP_VERSION + 1, d[2] | ((d[4] & 2) << 6));
    dump_put(NP_VERSION + 2, d[3] | ((d[4] & 4) << 5));
    dump_put(NP_BANK, d[5]);
    dump_put(NP_BANKS, d[6]);
    dump_put(NP_LEVEL, d[7]);
    dump_put(NP_METRO, d[8] & 1);
    dump_put(NP_RUNSTAT, (d[8] >> 1) & 1);
    dump_put(NP_ASENSE, (d[8] >> 2) & 1);
    d += 9;

    for (i = 0; i < STOMP_COUNT; i++, d += 4)
    {
        f = d[0];
        dump_put(NP_TYPE + i, f & 3);
        dump_put(NP_MODE + i, (f >> 2) & 3);
        dump_put(NP_FOLLOW1 + i, (f >> 4) & 1);
        dump_put(NP_FOLLOW2 + i, (f >> 5) & 1);
        dump_put(NP_NUM + i, d[1]);
        dump_put(NP_ON + i, d[2]);
        dump_put(NP_OFF + i, d[3]);
    }

    for (k = 0; k < 4; k++)
    {
        dump_put(NP_TRIGGER + k, d[k]);
    }

    d += 4;
    dump_put(NP_PICKUP, d[0] & 1);
    dump_put(NP_PICKUP + 1, (d[0] >> 1) & 1);
    dump_put(NP_BANK_PC_ON, (d[0] >> 2) & 1);
    dump_put(NP_BANK_PC, d[1]);
    dump_put(NP_PEDAL_CC, d[2]);
    dump_put(NP_PEDAL_CC + 1, d[3]);
    d += 4;

    for (k = 0; k < 4; k++)
    {
        dump_put(NP_RANGE + k, d[k]);
    }

    for (i = 0; i < NP_COUNT; i++)
    {
        n += dump_seen[i];
    }

    return n;
}

// The number of parameters in a dump.
#define DUMP_PARAMS (1 + 3 + 2 + 1 + 3 + 7 * STOMP_COUNT + 4 + 6 + 4)

// Ask for a dump of bank b (0 the bank in use) and run it (giving up after n ticks),
// with room in MIDI OUT for it.
static void dump_run_bank(uint8_t b, int n)
{
    int space = midi_space;

    sx_get(b);
    midi_space = 128;

    while (sx_dump_due && n-- > 0)
    {
        ticks(1);
    }

    midi_space = space;
}

static void dump_run(int n)
{
    dump_run_bank(0, n);
}

static void test_midi_config(void)
{
    const uint8_t cc[10] = { STOMP_CC_LIST };
    uint8_t d[8];
    int i, n;

    printf("configuration over MIDI (SysEx)\n");

    // A dump: one SysEx message with the shared settings and the bank's (no calibration).
    reset_world();
    set_mode(3, PM_MOM_INV);
    set_msg(4, MT_NOTE, 60, 100, 30);
    sw_mask[1] = (uint16_t)(1 << 2);
    sw_thr[1] = 77;
    heel[0] = 3;   toe[0] = 250;
    heel[1] = 129; toe[1] = 200;
    clear_log();
    dump_run(1000);
    n = parse_dump(0);
    CHECK(n == DUMP_PARAMS && dump_end && midi_len == SX_DUMP_LEN && !sx_dump_due && dump_bank_of == 1);
    CHECK(dump_values[NP_BANK] == 1 && dump_values[NP_BANKS] == BANK_COUNT);
    CHECK(dump_values[NP_BANK_PC] == 127 && dump_values[NP_BANK_PC_ON] == 0);
    CHECK(dump_values[NP_CHANNEL] == MIDI_CHANNEL && dump_values[NP_LEVEL] == led_level);
    CHECK(dump_values[NP_VERSION] == fw_version[0] && dump_values[NP_VERSION + 1] == fw_version[1] &&
          dump_values[NP_VERSION + 2] == fw_version[2]);
    CHECK(dump_values[NP_MODE + 3] == PM_MOM_INV && dump_values[NP_MODE] == pedal_mode_get(0));
    CHECK(dump_values[NP_TYPE + 4] == MT_NOTE && dump_values[NP_NUM + 4] == 60 && dump_values[NP_ON + 4] == 100 &&
          dump_values[NP_OFF + 4] == 30);
    CHECK(dump_values[NP_TYPE] == MT_CC && dump_values[NP_NUM] == cc[0] && dump_values[NP_ON] == 127 && dump_values[NP_OFF] == 0);
    CHECK(dump_values[NP_FOLLOW2 + 2] == 1 && dump_values[NP_FOLLOW2 + 3] == 0 && dump_values[NP_FOLLOW1 + 2] == 0);
    CHECK(dump_values[NP_TRIGGER + 1] == 77 && dump_values[NP_TRIGGER] == 0);
    CHECK(dump_values[NP_PEDAL_CC] == pedal_cc[0] && dump_values[NP_PEDAL_CC + 1] == pedal_cc[1] &&
          dump_values[NP_PICKUP] == (pk_mask & 1) && dump_values[NP_PICKUP + 1] == ((pk_mask >> 1) & 1));
    // The calibration is not reported: it is set on the pedal only.
    CHECK(!dump_seen[88] && !dump_seen[89] && !dump_seen[90] && !dump_seen[91]);
    // A program change on: its program and the flag.
    bank_pc = 6;
    pk_mask = 2;
    clear_log();
    dump_run(1000);
    parse_dump(0);
    CHECK(dump_values[NP_BANK_PC] == 5 && dump_values[NP_BANK_PC_ON] == 1 && dump_values[NP_PICKUP] == 0 &&
          dump_values[NP_PICKUP + 1] == 1);
    bank_pc = 0;

    // It waits for MIDI OUT room for the whole message, then goes in one tick.
    clear_log();
    sx_get(0);
    midi_space = SX_DUMP_LEN - 1;
    ticks(5);
    CHECK(midi_len == 0 && sx_dump_due);
    midi_space = SX_DUMP_LEN;
    ticks(1);
    CHECK(midi_len == SX_DUMP_LEN && !sx_dump_due && parse_dump(0) == DUMP_PARAMS);
    midi_space = 15;

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
          heel[1] == 129 && sw_thr[1] == 77 && midi_channel == MIDI_CHANNEL && !sx_dump_due);

    // Sets apply at once. Pedal mode: the switch goes to its new resting state on the next
    // tick (inverted momentary: on, sending its on value).
    reset_world();
    set_mode(0, PM_STOMP);
    clear_log();
    mset(NP_MODE + 0, PM_MOM_INV);
    CHECK(pedal_mode_get(0) == PM_MOM_INV && midi_len == 0);
    ticks(2);
    CHECK(midi_len == 3 && cc_number(0) == cc[0] && cc_value(0) == 127 && leds[0] == 1);
    // Out of range: ignored.
    mset(NP_MODE + 1, 4);
    CHECK(pedal_mode_get(1) == DEFAULT_PEDAL_MODE);

    // Message: type, number, on and off; the switch then sends it.
    reset_world();
    set_mode(1, PM_STOMP);
    mset(NP_TYPE + 1, MT_NOTE);
    mset(NP_NUM + 1, 64);
    mset(NP_ON + 1, 99);
    mset(NP_OFF + 1, 11);
    mset(NP_TYPE + 2, 3);                  // no type 3: ignored
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
    mset(NP_NUM + 1, 65);
    CHECK(midi_len == 3 && midi_log[0] == NOTE_OFF && midi_log[1] == 64 && midi_log[2] == 11 && leds[1] == 0);
    ticks(1);
    CHECK(midi_len == 3);
    // A program change switch can be set up too, and sends nothing when changed while on.
    set_mode(5, PM_STOMP);
    mset(NP_TYPE + 5, MT_PROG);
    mset(NP_ON + 5, 7);
    mset(NP_OFF + 5, 8);
    ticks(1);
    clear_log();
    tap(sw_code(6));
    CHECK(midi_len == 2 && midi_log[1] == 7 && leds[5] == 1);
    mset(NP_ON + 5, 9);
    CHECK(midi_len == 2 && leds[5] == 0);

    // Relays: follow flags (0 / 1, flipping only on a change) and trigger values.
    reset_world();
    mset(NP_FOLLOW1 + 4, 1);
    mset(NP_FOLLOW1 + 4, 1);
    mset(NP_FOLLOW2 + 10, 1);
    mset(NP_FOLLOW1 + 5, 2);               // not 0 or 1: ignored
    mset(NP_TRIGGER + 3, 90);
    CHECK(sw_mask[0] == (1 << 4) && sw_mask[1] == 0 && sw_thr[3] == 90);
    mset(NP_FOLLOW1 + 4, 0);
    CHECK(sw_mask[0] == 0);

    // Channel: 1-16; afterwards a message for another device (channel) is ignored, one for
    // the new channel or for all (7F) taken.
    reset_world();
    mset(NP_CHANNEL, 0);
    mset(NP_CHANNEL, 17);
    CHECK(midi_channel == MIDI_CHANNEL);
    mset(NP_CHANNEL, (uint8_t)(MIDI_CHANNEL % 16 + 1));
    CHECK(midi_channel == MIDI_CHANNEL % 16 + 1);
    mset_on(MIDI_CHANNEL, NP_TRIGGER, 50);
    CHECK(sw_thr[0] == 0);
    mset(NP_TRIGGER, 50);
    CHECK(sw_thr[0] == 50);
    d[0] = NP_TRIGGER; d[1] = 51;
    sx_send(0x7F, SX_CMD_SET, d, 2);
    CHECK(sw_thr[0] == 51);

    // Read-only and unknown parameters cannot be set.
    reset_world();
    mset(88, 5);
    mset(NP_VERSION, 5);
    mset(5, 1);
    mset(NP_BANKS, 3);
    mset(NP_COUNT, 3);
    CHECK(heel[0] == 0 && midi_len == 0 && !sx_dump_due && hal_bank == 0);

    // Several pairs in one message, applied at its F7; anything not this protocol is
    // ignored: another header, a command not known, a message cut short by a status byte,
    // an odd count, too many pairs, a dump request with a bank out of range.
    reset_world();
    d[0] = NP_TRIGGER; d[1] = 20; d[2] = NP_TRIGGER + 1; d[3] = 21; d[4] = NP_TRIGGER + 2; d[5] = 22;
    sx_send(0x7F, SX_CMD_SET, d, 6);
    CHECK(sw_thr[0] == 20 && sw_thr[1] == 21 && sw_thr[2] == 22);
    ctl_midi_in(0xF0); ctl_midi_in(0x7D); ctl_midi_in(0x46); ctl_midi_in(0x44); ctl_midi_in(0x7F);
    ctl_midi_in(SX_CMD_SET); ctl_midi_in(NP_TRIGGER); ctl_midi_in(30); ctl_midi_in(0xF7);
    sx_send(0x7F, 0x09, d, 6);
    CHECK(sw_thr[0] == 20);
    ctl_midi_in(0xF0); ctl_midi_in(0x7D); ctl_midi_in(0x46); ctl_midi_in(0x43); ctl_midi_in(0x7F);
    ctl_midi_in(SX_CMD_SET); ctl_midi_in(NP_TRIGGER); ctl_midi_in(31); ctl_midi_in(NP_TRIGGER + 1);
    ctl_midi_in(32);
    CHECK(sw_thr[0] == 20 && sw_thr[1] == 21);   // nothing until the F7
    ctl_midi_in(CC_STATUS); ctl_midi_in(33); ctl_midi_in(0);
    CHECK(sw_thr[0] == 20 && sw_thr[1] == 21);   // cut short: nothing at all
    d[0] = NP_TRIGGER; d[1] = 34; d[2] = NP_TRIGGER + 1;
    sx_send(0x7F, SX_CMD_SET, d, 3);             // an odd count: nothing
    CHECK(sw_thr[0] == 20);
    {
        static uint8_t big[SX_SET_MAX + 2];

        for (i = 0; i < SX_SET_MAX + 2; i += 2)
        {
            big[i]     = NP_TRIGGER;
            big[i + 1] = 35;
        }

        sx_send(0x7F, SX_CMD_SET, big, SX_SET_MAX + 2);   // too long: nothing
        CHECK(sw_thr[0] == 20);
        sx_send(0x7F, SX_CMD_SET, big, SX_SET_MAX);       // the most: taken
        CHECK(sw_thr[0] == 35);
    }
    d[0] = BANK_COUNT + 1;
    sx_send(0x7F, SX_CMD_GET, d, 1);
    d[0] = 1; d[1] = 1;
    sx_send(0x7F, SX_CMD_GET, d, 2);
    CHECK(!sx_dump_due);
    // The identity request still works around them.
    sx_reply = 0;
    clear_log();
    ctl_midi_in(0xF0); ctl_midi_in(0x7E); ctl_midi_in(0x7F); ctl_midi_in(0x06); ctl_midi_in(0x01); ctl_midi_in(0xF7);
    CHECK(sx_reply);
    sx_reply = 0;

    // Configuration mode ignores it, and entering it drops a dump asked for.
    reset_world();
    sx_get(0);
    chord_long();
    CHECK(mode == MODE_CONFIG && !sx_dump_due);
    clear_log();
    mset(NP_TRIGGER, 33);
    sx_get(0);
    ticks(5);
    CHECK(sw_thr[0] == 0 && midi_len == 0 && !sx_dump_due);
    cancel_exit();

    // Leaving configuration mode from a later page starts no dump and sets nothing.
    reset_world();
    chord_long();
    tap(CTL_KEY_UP); tap(CTL_KEY_UP); tap(CTL_KEY_UP);
    tap(sw_code(4));
    CHECK(cfg_page == 3);
    cancel_exit();
    clear_log();
    ticks(20);
    CHECK(midi_len == 0 && !sx_dump_due && pedal_mode_get(3) == DEFAULT_PEDAL_MODE && sw_thr[0] == 0);

    // Save: at once, every record, as leaving configuration mode does; a dump asked for
    // straight after does not cancel it.
    reset_world();
    mset(NP_TRIGGER, 44);
    mset(NP_NUM + 9, 3);
    mset(NP_COMMAND, NC_SAVE);
    sx_get(0);
    CHECK(cfg_saved == 1 && sx_dump_due);
    dump_run(1000);
    CHECK(cfg_saved == 1 && cal_saved == 1 && bank_saved == 1 && msg_saved == 10 && !sx_dump_due);
    CHECK(saved_sw(0, 2) == 44 && saved_msg[9][1] == 3 && saved_cfg[0] == MIDI_CHANNEL);

    // Reload: back to what the EEPROM holds; switches to their resting states.
    reset_world();
    set_mode(0, PM_STOMP);
    tap(sw_code(1));
    mset(NP_TRIGGER, 44);
    mset(NP_ON + 2, 5);
    clear_log();
    mset(NP_COMMAND, NC_RELOAD);
    CHECK(sw_thr[0] == 0 && MSG_ON(2) == 127);
    ticks(2);
    CHECK(leds[0] == (DEFAULT_PEDAL_MODE == PM_MOM_INV));

    // Calibration cannot be set over MIDI (user's request): the old heel / toe commands
    // (4, 5) are ignored, and so is the old dump command (1).
    reset_world();
    pedal(0, 20); pedal(1, 30);
    mset(NP_COMMAND, 4);
    pedal(0, 200); pedal(1, 220);
    mset(NP_COMMAND, 5);
    mset(NP_COMMAND, 1);
    CHECK(heel[0] == 0 && heel[1] == 0 && toe[0] == 255 && toe[1] == 255 && !sx_dump_due);

    // The old NRPN messages do nothing now (user's request: CCs reached other software).
    reset_world();
    ctl_midi_in(CC_STATUS);
    ctl_midi_in(99); ctl_midi_in(0x46);
    ctl_midi_in(98); ctl_midi_in(NP_TRIGGER);
    ctl_midi_in(6);  ctl_midi_in(20);
    CHECK(sw_thr[0] == 0);
}

// Store footswitch sw's (0-9) message in bank b (0-based) of the fake EEPROM, with mode m.
static void store_bank_msg(int b, int sw, uint8_t type, uint8_t num, uint8_t on, uint8_t off, uint8_t m)
{
    int r = b * BANK_RECS + sw;

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
// Bank mode is the default (DIRECT SELECT dark: UP / DOWN choose banks, the footswitches
// are stomps). A short UP + DOWN toggles program mode (DIRECT SELECT lit; user's design):
// UP / DOWN step the program as before, and the footswitches choose a program by number
// (as the display shows it): 1-9 at once; 10 shows "--", then digits (10 = 0), sent once
// complete, with UP (ENTER) when shorter, or dropped with DOWN (ESCAPE).
#define SHOWN_PC(t) ((t) - DISPLAY_PROGRAM_OFFSET)
static void pc_digits(const char *d)
{
    for (; *d; d++)
    {
        tap(sw_code(*d == '0' ? 10 : *d - '0'));
    }
}

static void test_pc_mode(void)
{
    int i;

    printf("program mode: programs chosen with the footswitches\n");
    reset_world();
    for (i = 0; i < STOMP_COUNT; i++)
    {
        set_mode((uint8_t)i, PM_STOMP);
    }
    CHECK(!pc_mode && aux[HAL_LED_DIRECT_SELECT] == 0 && display == 1 && stomp_state == 0);
    clear_log();
    chord_short();
    CHECK(pc_mode && aux[HAL_LED_DIRECT_SELECT] == 1 && display == DISPLAY_PROGRAM_OFFSET && midi_len == 0);

    // Switches 1-9: that program at once; the stomps are untouched.
    tap(sw_code(3));
    CHECK(midi_len == 2 && midi_log[0] == PROG_CHG && midi_log[1] == SHOWN_PC(3) && display == 3 && program == SHOWN_PC(3));
    CHECK(stomp_state == 0 && leds[2] == 0);
    clear_log();
    tap(sw_code(9));
    CHECK(midi_len == 2 && midi_log[1] == SHOWN_PC(9) && display == 9);

    // Switch 10: "--", nothing sent; 1, 2 show 1, 12 (not complete); 3 makes 123, sent.
    clear_log();
    tap(sw_code(10));
    CHECK(pc_typing && display == SHOWN_TEXT(HAL_TEXT_DASHES) && midi_len == 0);
    // The right-hand dot flashes until the number is complete, starting lit.
    CHECK(dots == 0x01);
    ticks(CHAN_FLASH_TICKS - 1);
    CHECK(dots == 0x01);
    ticks(1);
    CHECK(dots == 0);
    ticks(CHAN_FLASH_TICKS);
    CHECK(dots == 0x01);
    pc_digits("12");
    CHECK(pc_typing && display == 12 && midi_len == 0 && program == SHOWN_PC(9) && dots == 0x01);
    ticks(CHAN_FLASH_TICKS);
    CHECK(dots == 0);
    ticks(CHAN_FLASH_TICKS);
    CHECK(dots == 0x01);
    pc_digits("3");
    CHECK(!pc_typing && midi_len == 2 && midi_log[1] == SHOWN_PC(123) && display == 123 && program == SHOWN_PC(123));
    // Complete: the dot goes out on the next tick and stays out.
    ticks(1);
    CHECK(dots == 0);
    ticks(CHAN_FLASH_TICKS * 3);
    CHECK(dots == 0);
    // 5, 6: 56 is complete (no third digit fits), sent at once.
    clear_log();
    pc_digits("056");
    CHECK(!pc_typing && midi_len == 2 && midi_log[1] == SHOWN_PC(56) && display == 56);
    // 1 then UP (ENTER): 1.
    clear_log();
    pc_digits("01");
    CHECK(pc_typing && midi_len == 0 && display == 1);
    tap(CTL_KEY_UP);
    CHECK(!pc_typing && midi_len == 2 && midi_log[1] == SHOWN_PC(1) && display == 1);
    // 4 then DOWN (ESCAPE): nothing sent, back to the current program.
    clear_log();
    pc_digits("04");
    tap(CTL_KEY_DOWN);
    CHECK(!pc_typing && midi_len == 0 && display == 1 && program == SHOWN_PC(1));
    ticks(1);
    CHECK(dots == 0);
    // UP with no digit typed: nothing sent, back to the current program.
    tap(sw_code(10));
    tap(CTL_KEY_UP);
    CHECK(!pc_typing && midi_len == 0 && display == 1);
#if DISPLAY_PROGRAM_OFFSET == 0
    // 0, 0 then UP: program 0. 1, 2, 8: 128 is too big, so 8 starts a new number.
    pc_digits("000");
    CHECK(pc_typing && display == 0 && midi_len == 0);
    tap(CTL_KEY_UP);
    CHECK(midi_len == 2 && midi_log[1] == 0 && display == 0);
    clear_log();
    pc_digits("0128");
    CHECK(pc_typing && display == 8 && midi_len == 0);
    tap(CTL_KEY_UP);
    CHECK(midi_len == 2 && midi_log[1] == 8);
    // 13 is complete.
    clear_log();
    pc_digits("013");
    CHECK(!pc_typing && midi_len == 2 && midi_log[1] == 13);
#endif

    // While typing UP does not step or repeat; it acts on release.
    clear_log();
    pc_digits("07");
    ctl_key_press(CTL_KEY_UP);
    ticks(REPEAT_DELAY_TICKS + 4 * REPEAT_PERIOD_TICKS);
    CHECK(pc_typing && display == 7 && midi_len == 0);
    ctl_key_release(CTL_KEY_UP);
    CHECK(!pc_typing && midi_len == 2 && midi_log[1] == SHOWN_PC(7));

    // UP / DOWN step the program as before, from the one chosen.
    clear_log();
    tap(CTL_KEY_UP);
    CHECK(midi_len == 2 && midi_log[1] == SHOWN_PC(8) && display == 8);

    // No room in MIDI OUT: the program goes when there is.
    clear_log();
    midi_space = 1;
    tap(sw_code(2));
    CHECK(midi_len == 0 && display == 2 && program == SHOWN_PC(2));
    midi_space = 15;
    ticks(1);
    CHECK(midi_len == 2 && midi_log[1] == SHOWN_PC(2));

    // A short UP + DOWN while typing drops the number and goes back to bank mode, where
    // the footswitches are stomps again; nothing is sent for either.
    clear_log();
    pc_digits("01");
    chord_short();
    CHECK(!pc_mode && !pc_typing && display == 1 && aux[HAL_LED_DIRECT_SELECT] == 0 && midi_len == 0);
    tap(0);
    CHECK(midi_len == 3 && cc_number(0) == stomp_cc[0] && leds[0] == 1);

    // Config mode drops a number being typed; leaving it keeps program mode.
    chord_short();
    pc_digits("05");
    chord_long();
    CHECK(mode == MODE_CONFIG && !pc_typing && dots == 0);
    cancel_exit();
    CHECK(mode == MODE_RUN && pc_mode && aux[HAL_LED_DIRECT_SELECT] == 1 && display == SHOWN_PC(2) + DISPLAY_PROGRAM_OFFSET);

    // Power-on is bank mode.
    power_on();
    boot_end();
    CHECK(!pc_mode && !pc_typing && aux[HAL_LED_DIRECT_SELECT] == 0 && display == 1);
}

// Page 5: each bank's program change, sent when the bank is chosen in bank mode (user's
// request). Footswitches type it, a pedal sets it, a short UP + DOWN turns it off ("--").
// Saved in the top nibbles of the bank's first two footswitch records.
static void test_bank_pc(void)
{
    int k, n;

    printf("footswitch banks: program change per bank\n");
    reset_world();
    stomp_banks(3);
    power_on();
    boot_end();
    CHECK(bank_pc == 0 && midi_len == 0);

    // Page 8 (UP from page 7): DIRECT SELECT lit, "--", footswitch LEDs dark.
    chord_long();
    for (k = 0; k < PAGE_BANK_PC; k++)
    {
        tap(CTL_KEY_UP);
    }
    CHECK(mode == MODE_CONFIG && cfg_page == PAGE_BANK_PC && aux[HAL_LED_DIRECT_SELECT] == 1 &&
          aux[HAL_LED_MIDI_FUNC] == 0 && aux[HAL_LED_EXP_1] == 0 && lamp == HAL_LAMP_NORMAL &&
          display == SHOWN_TEXT(HAL_TEXT_PROG) && stomp_state == 0);
    // "Pr" and then "--" (none), each for BANK_PC_LABEL_MS, in turn (user's request).
    ticks(VER_STEP_TICKS * BANK_LABEL_STEPS - 1);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_PROG));
    ticks(1);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_DASHES));
    ticks(VER_STEP_TICKS * BANK_LABEL_STEPS - 1);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_DASHES));
    ticks(1);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_PROG) && BANK_PC_LABEL_MS == VER_STEP_MS * BANK_LABEL_STEPS);
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_MISC);
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_BANK_PC && lamp == HAL_LAMP_NORMAL && display == SHOWN_TEXT(HAL_TEXT_PROG));

    // Digits as everywhere in config mode: 1, 2, 3 make 123; 0 (footswitch 10) is a
    // digit while a number can grow: 1, 0 make 10. A digit shows the program at once,
    // with the dot flashing while another digit can follow, and no "Pr" until it is
    // complete (user's request); then "Pr" comes back after BANK_PC_LABEL_MS.
    clear_log();
    tap(sw_code(1));
    CHECK(bank_pc == 2 && display == 1 && num_typing);
    ticks(1);
    CHECK(cfg_changed == 1 && dots == 1);
    ticks(VER_STEP_TICKS * BANK_LABEL_STEPS * 2);
    CHECK(display == 1 && num_typing);
    tap(sw_code(2));
    CHECK(bank_pc == 13 && display == 12 && num_typing && stomp_leds_are(0, digit_leds("01234567")));
    tap(sw_code(3));
    CHECK(bank_pc == 124 && display == 123 && !num_typing);
    ticks(1);
    CHECK(dots == 0);
    ticks(VER_STEP_TICKS * BANK_LABEL_STEPS);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_PROG));
    tap(sw_code(1));
    tap(sw_code(10));
    CHECK(bank_pc == 11 && display == 10 && num_typing && !pc_typing);
    // UP [ENTER] takes it as it is and stays on page 7 (user's request).
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_BANK_PC && bank_pc == 11 && !num_typing && display == 10);
    ticks(1);
    CHECK(dots == 0);

    // With no number being typed, footswitch 10 shows "--" with the right-hand dot
    // flashing, changing nothing yet.
    tap(sw_code(10));
    CHECK(pc_typing && bank_pc == 11 && display == SHOWN_TEXT(HAL_TEXT_DASHES) && dots == 0x01);
    ticks(CHAN_FLASH_TICKS);
    CHECK(dots == 0);
    ticks(CHAN_FLASH_TICKS);
    CHECK(dots == 0x01);
    // No "Pr" while "--" waits for UP / DOWN.
    ticks(VER_STEP_TICKS * BANK_LABEL_STEPS * 2);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_DASHES) && pc_typing);
    // A digit then starts a number: 5.
    tap(sw_code(5));
    CHECK(!pc_typing && bank_pc == 6 && display == 5 && midi_len == 0);
    // A number shown: its dot follows the blink (user's request: a dot flashes while a
    // number is entered).
    ticks(1);
    CHECK(dots == (blink_on ? 1 : 0));

    // 5, 7 make 57 (complete); then 10, 10: program 0.
    tap(sw_code(7));
    CHECK(bank_pc == 58);
    tap(sw_code(10));
    tap(sw_code(10));
    CHECK(bank_pc == 1 && display == 0);

    // 0, 4, 2 make 42; 10 then DOWN [ESCAPE]: back to showing it, still on page 7.
    tap(sw_code(4));
    tap(sw_code(2));
    tap(sw_code(10));
    tap(CTL_KEY_DOWN);
    CHECK(!pc_typing && bank_pc == 43 && display == 42 && cfg_page == PAGE_BANK_PC);

    // 10 then UP [ENTER]: none, still on page 7, the dot out.
    tap(sw_code(10));
    tap(CTL_KEY_UP);
    CHECK(bank_pc == 0 && display == SHOWN_TEXT(HAL_TEXT_DASHES) && mode == MODE_CONFIG && cfg_page == PAGE_BANK_PC);
    ticks(1);
    CHECK(dots == 0 && !pc_typing);

    // "--" starts when footswitch 10 is let go with UP and DOWN up: 10 pressed and let go
    // while UP is held, or UP tapped while 10 is held, only changes page.
    tap(sw_code(9));
    tap(sw_code(9));
    CHECK(bank_pc == 100 && !num_typing);
    ctl_key_press(CTL_KEY_UP);
    tap(sw_code(10));
    CHECK(!pc_typing && display == 99);
    ctl_key_release(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_MISC && bank_pc == 100);
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_BANK_PC);
    ctl_key_press(sw_code(10));
    CHECK(!pc_typing && display == SHOWN_TEXT(HAL_TEXT_PROG));
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_MISC);
    ctl_key_release(sw_code(10));
    CHECK(!pc_typing && cfg_page == PAGE_MISC && midi_channel == MIDI_CHANNEL);
    tap(CTL_KEY_DOWN);
    tap(sw_code(10));
    tap(CTL_KEY_UP);
    CHECK(bank_pc == 0 && cfg_page == PAGE_BANK_PC);

    // A long UP still saves (through the bank prompt) during a "--" entry, which is
    // dropped.
    tap(sw_code(10));
    long_nav(CTL_KEY_UP);
    CHECK(save_prompt && !pc_typing);
    tap(CTL_KEY_UP);
    CHECK(mode == MODE_RUN && !pc_typing && bank_pc == 0);
    ticks(1);
    CHECK(dots == 0);
    chord_long();
    for (k = 0; k < PAGE_BANK_PC; k++)
    {
        tap(CTL_KEY_UP);
    }
    CHECK(cfg_page == PAGE_BANK_PC);

    // A pedal sets nothing (user's request); a digit starts a new number.
    pedal(0, 0);
    pedal(0, 128);
    CHECK(bank_pc == 0);
    tap(sw_code(4));
    CHECK(bank_pc == 5 && display == 4);
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_BANK_PC);

    // UP takes it; to page 6 and back again: still 4. Then save: bank 1's first record holds the low
    // nibble of 5 (program 4 + 1), the second the high nibble, the rest nothing.
    tap(CTL_KEY_UP);
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_BANK_PC && bank_pc == 5 && display == SHOWN_TEXT(HAL_TEXT_PROG) && lamp == HAL_LAMP_NORMAL && dots == 0);
    long_press(CTL_KEY_UP);
    tap(CTL_KEY_UP);
    CHECK(mode == MODE_RUN && midi_len == 0 && aux[HAL_LED_DIRECT_SELECT] == 0);
    CHECK(saved_y(0, 0) == 5 && saved_y(0, 1) == bank_def[1] && saved_y(0, 2) == bank_def[2]);
    CHECK((saved_msg[0][0] & 0x3F) == (uint8_t)(MT_CC | (PM_STOMP << 2)));
    keep_saved_msgs();

    // Bank 2: program 100 (101 = 0x65); bank 3: none. Power-on (bank 1) sends nothing.
    store_y(1, 0, 0x65);
    power_on();
    boot_end();
    CHECK(bank_pc == 5 && midi_len == 0 && program == 0);

    // Config mode with nothing changed: no flash (the saved nibbles match).
    chord_long();
    CHECK(mode == MODE_CONFIG && cfg_changed == 0);
    long_press(CTL_KEY_DOWN);
    CHECK(mode == MODE_RUN && bank_pc == 5);

    // Bank mode (the default): UP to bank 2 sends program 100, which becomes the program;
    // the display keeps showing the bank. UP to bank 3 sends nothing.
    clear_log();
    tap(CTL_KEY_UP);
    ticks(1);
    CHECK(hal_bank == 1 && bank_pc == 101 && midi_len == 2 && midi_log[0] == PROG_CHG && midi_log[1] == 100);
    CHECK(program == 100 && display == 2);
    clear_log();
    tap(CTL_KEY_UP);
    ticks(1);
    CHECK(hal_bank == 2 && bank_pc == 0 && midi_len == 0 && program == 100 && display == 3);

    // Back down to bank 1 one step at a time: bank 2's program, then bank 1's (4).
    tap(CTL_KEY_DOWN);
    ticks(1);
    tap(CTL_KEY_DOWN);
    ticks(1);
    CHECK(hal_bank == 0 && midi_len == 4 && midi_log[1] == 100 && midi_log[2] == PROG_CHG && midi_log[3] == 4);

    // In program mode the display shows that program; UP steps on from it.
    chord_short();
    CHECK(pc_mode && program == 4 && display == 4 + DISPLAY_PROGRAM_OFFSET);
    clear_log();
    tap(CTL_KEY_UP);
    CHECK(midi_len == 2 && midi_log[1] == 5);

    // A bank chosen over MIDI (the bank parameter) sends its program change too.
    clear_log();
    mset(NP_BANK, 2);
    ticks(1);
    CHECK(hal_bank == 1 && midi_len == 2 && midi_log[0] == PROG_CHG && midi_log[1] == 100);

    // Over MIDI (parameters 92 and 93, the bank in use): the dump has program 100, on.
    clear_log();
    dump_run(1000);
    CHECK(parse_dump(0) > 0 && dump_values[NP_BANK_PC] == 100 && dump_values[NP_BANK_PC_ON] == 1);
    // Off: none, and the program then reads 127; an unchanged off does nothing.
    mset(NP_BANK_PC_ON, 0);
    CHECK(bank_pc == 0 && param_get(NP_BANK_PC) == 127 && param_get(NP_BANK_PC_ON) == 0);
    mset(NP_BANK_PC_ON, 0);
    CHECK(bank_pc == 0);
    // On again: the program it reads, 127. Values over 1 are ignored.
    mset(NP_BANK_PC_ON, 1);
    CHECK(bank_pc == 128 && param_get(NP_BANK_PC) == 127);
    mset(NP_BANK_PC_ON, 0);
    mset(NP_BANK_PC_ON, 2);
    CHECK(bank_pc == 0);
    // Setting the program turns it on, even to the 127 it reads while off.
    mset(NP_BANK_PC, 127);
    CHECK(bank_pc == 128);
    mset(NP_BANK_PC, 7);
    CHECK(bank_pc == 8 && param_get(NP_BANK_PC) == 7 && param_get(NP_BANK_PC_ON) == 1);
    // A dump fed back in is not a set: it changes nothing.
    clear_log();
    dump_run(1000);
    k = midi_len;
    bank_pc = 0;
    for (n = 0; n < k; n++)
    {
        ctl_midi_in(midi_log[n]);
    }
    CHECK(bank_pc == 0);
    bank_pc = 8;
    mset(NP_BANK_PC_ON, 0);
    CHECK(bank_pc == 0);
    // Nothing is sent for a set, and the save command stores it with the bank.
    clear_log();
    mset(NP_BANK_PC, 33);
    mset(NP_COMMAND, NC_SAVE);
    CHECK(midi_len == 0 && hal_bank == 1 && saved_y(1, 0) == 34);
}

// Banks NOTE_BANK_1 and NOTE_BANK_2, never saved, default to momentary notes: switch 1
// the first note, each next switch one higher, on velocity 127, off velocity 0. Other
// banks default to the CCs.
static void check_note_bank(int bank, int first)
{
    int i;

    while (hal_bank != bank - 1)
    {
        tap(CTL_KEY_UP);
        ticks(2);
    }

    for (i = 0; i < STOMP_COUNT; i++)
    {
        CHECK(msg_type(i) == (first ? MT_NOTE : MT_CC) && pedal_mode_get(i) == (first ? PM_MOMENTARY : DEFAULT_PEDAL_MODE));
        CHECK(MSG_NUM(i) == (first ? first + note_scale[i] : stomp_cc[i]) && MSG_ON(i) == 127 && MSG_OFF(i) == 0);
    }

    if (first)
    {
        clear_log();
        ctl_key_press(sw_code(10));
        CHECK(midi_len == 3 && midi_log[0] == NOTE_ON && midi_log[1] == first + note_scale[9] && midi_log[2] == 127 && leds[9] == 1);
        ctl_key_release(sw_code(10));
        CHECK(midi_len == 6 && midi_log[3] == NOTE_OFF && midi_log[4] == first + note_scale[9] && midi_log[5] == 0 && leds[9] == 0);
    }
}

static void test_note_banks(void)
{
    printf("footswitch banks: default note banks\n");
    reset_world();
    power_on();
    boot_end();
    check_note_bank(1, 0);
#if NOTE_BANK_1 > 0
    check_note_bank(NOTE_BANK_1, NOTE_BANK_1_FIRST);
#endif
#if NOTE_BANK_2 > 0
    check_note_bank(NOTE_BANK_2, NOTE_BANK_2_FIRST);
#endif
#if NOTE_BANK_2 < BANK_COUNT
    check_note_bank(NOTE_BANK_2 + 1, 0);
#endif
}

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
    power_on();
    boot_end();
    CHECK(hal_bank == 0 && bank_next == 0 && msg_type(0) == MT_CC && MSG_NUM(0) == cc[0] && pedal_mode_get(0) == PM_STOMP);

    // Bank mode is the default (DIRECT SELECT dark): the display shows bank 1. Switch 3 on.
    CHECK(!pc_mode && aux[HAL_LED_DIRECT_SELECT] == 0 && display == 1);
    tap(sw_code(3));
    CHECK(leds[2] == 1);
    clear_log();

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

    // Program mode (DIRECT SELECT lit): the display shows the program and UP sends a
    // program change; bank 2 stays.
    clear_log();
    chord_short();
    CHECK(pc_mode && aux[HAL_LED_DIRECT_SELECT] == 1 && display == DISPLAY_PROGRAM_OFFSET && hal_bank == 1 && midi_len == 0);
    tap(CTL_KEY_UP);
    CHECK(midi_len == 2 && midi_log[0] == PROG_CHG && midi_log[1] == 1 && display == 1 + DISPLAY_PROGRAM_OFFSET && hal_bank == 1);

    // Back to bank mode: the bank (2) is shown, not the program; the footswitches are
    // bank 2's stomps again. DOWN to bank 1, sending nothing. Switch 3's CC is still on;
    // switch 1 is a CC no switch of bank 2 has, off.
    clear_log();
    chord_short();
    CHECK(!pc_mode && aux[HAL_LED_DIRECT_SELECT] == 0 && display == 2 && program == 1);
    tap(sw_code(2));
    CHECK(midi_len == 3 && cc_number(0) == 70 && cc_value(0) == 10 && leds[1] == 0);
    clear_log();
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
    power_on();
    boot_end();
    for (i = 2; i < STOMP_COUNT; i++)
    {
        set_mode((uint8_t)i, PM_STOMP);
    }
    CHECK(msg_type(0) == MT_PROG && pedal_mode_get(1) == PM_MOMENTARY);
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
    CHECK(display == 5 + DISPLAY_PROGRAM_OFFSET);

    // Configuration mode edits the current bank: a save writes bank 2's records (with the
    // modes) and leaves bank 1's alone; the CONFIG LED compares with bank 2.
    reset_world();
    stomp_banks(2);
    store_bank_msg(1, 3, MT_CC, 44, 127, 0, PM_MOMENTARY);
    tap(CTL_KEY_UP);
    ticks(3);
    CHECK(hal_bank == 1 && pedal_mode_get(3) == PM_MOMENTARY);
    chord_long();
    CHECK(mode == MODE_CONFIG && status_led == 1 && status_toggles(CFG_FLASH_TICKS * 3) == 0);
    mode_tap(sw_code(4));
    mode_tap(sw_code(4));
    CHECK(pedal_mode_get(3) == PM_MOM_INV && status_led == 0);
    save_exit();
    CHECK(mode == MODE_RUN && hal_bank == 1 && display == 2 && aux[HAL_LED_DIRECT_SELECT] == 0);
    CHECK(msg_was_saved[BANK_RECS + 3] && saved_msg[BANK_RECS + 3][0] == (MT_CC | (PM_MOM_INV << 2)) &&
          saved_msg[BANK_RECS + 3][1] == 44);
    for (i = 0; i < STOMP_COUNT; i++)
    {
        CHECK(!msg_was_saved[i] && msg_was_saved[BANK_RECS + i]);
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
    mset(NP_BANK, 0);
    mset(NP_BANK, BANK_COUNT + 1);
    mset(NP_BANKS, 5);
    ticks(1);
    CHECK(hal_bank == 0 && !bank_busy);
    tap(sw_code(1));
    clear_log();
    mset(NP_BANK, 3);
    sx_get(0);
    CHECK(bank_busy && display == 3);
    midi_space = 128;
    ticks(1);
    midi_space = 15;
    // Switch 1 was on (a CC); bank 3's switch 1 is a note, so it is off. Nothing is sent
    // but the dump, which goes once the change is done, and describes the new bank.
    CHECK(hal_bank == 2 && msg_type(0) == MT_NOTE && MSG_NUM(0) == 61 && leds[0] == 0);
    CHECK(midi_len == SX_DUMP_LEN && parse_dump(0) == DUMP_PARAMS && dump_bank_of == 3);
    clear_log();
    dump_run(1000);
    parse_dump(0);
    CHECK(dump_values[NP_BANK] == 3 && dump_values[NP_BANKS] == BANK_COUNT && dump_values[NP_TYPE] == MT_NOTE &&
          dump_values[NP_NUM] == 61);
    // Saving over MIDI writes the current bank.
    mset(NP_COMMAND, NC_SAVE);
    CHECK(msg_was_saved[2 * BANK_RECS] && saved_msg[2 * BANK_RECS][1] == 61 && !msg_was_saved[0]);
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
    power_on();
    boot_end();
    ticks(2);

    // Switch 1 (CC 26) on lights switch 6 too (also CC 26 in bank 1).
    tap(sw_code(1));
    CHECK(leds[0] == 1 && leds[5] == 1);
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
    power_on();
    boot_end();
    for (i = 0; i < STOMP_COUNT; i++)
    {
        CHECK(leds[i] == (pedal_mode_get((uint8_t)i) == PM_MOM_INV));
    }
}

// A raw reading that scales to CC value v with the default calibration.
static uint8_t raw_for(uint8_t v)
{
    int r;

    for (r = 0; r < 256; r++)
    {
        if (ctl_scale((uint8_t)r, heel[0], toe[0]) == v)
        {
            return (uint8_t)r;
        }
    }

    return 0;
}

// EXP LED toggles of pedal ch over n ticks.
static int exp_toggles(uint8_t ch, int n)
{
    int t = 0;
    uint8_t was = aux[HAL_LED_EXP_1 + ch];

    while (n-- > 0)
    {
        ctl_tick();

        if (aux[HAL_LED_EXP_1 + ch] != was)
        {
            was = aux[HAL_LED_EXP_1 + ch];
            t++;
        }
    }

    return t;
}

static void midi_cc_in(uint8_t cc, uint8_t v)
{
    ctl_midi_in((uint8_t)(0xB0 | (midi_channel - 1)));
    ctl_midi_in(cc);
    ctl_midi_in(v);
}

// Page 5's pedal CC editor, the CCs per bank, and over MIDI (NRPN 96 / 97).
// The pedal ranges (user's request): VALUE 1 at the heel, VALUE 2 at the toe, set in the
// CC editor's second and third steps; the pedal's travel is scaled between them.
static int range_flickers(uint8_t led)
{
    int t, on = 0, off = 0;

    for (t = 0; t < 4 * PAR_TICKS; t++)
    {
        ticks(1);
        on  += aux[led] == 1;
        off += aux[led] == 0;
    }

    return on > 0 && off > 0;
}

static void test_pedal_range(void)
{
    uint8_t cc_a = bank_def[1] & 0x7F;

    printf("pedal range: VALUE 1 / VALUE 2, the CC editor's steps, scaling, saved per bank\n");
    reset_world();
    pk_mask = 0;
    CHECK(pedal_rng[0] == 0 && pedal_rng[1] == 0 && pedal_rng[2] == 127 && pedal_rng[3] == 127);
    // The default range sends the pedal's own value, as before.
    CHECK(pedal_map(0, 0) == 0 && pedal_map(0, 64) == 64 && pedal_map(0, 127) == 127);
    pedal_rng[0] = 10;
    pedal_rng[2] = 100;
    CHECK(pedal_map(0, 0) == 10 && pedal_map(0, 127) == 100 && pedal_map(0, 64) == 55);
    // Either way round.
    pedal_rng[0] = 100;
    pedal_rng[2] = 10;
    CHECK(pedal_map(0, 0) == 100 && pedal_map(0, 127) == 10 && pedal_map(0, 64) == 55);
    // Every value in range, and monotonic.
    {
        int v, ok = 1, last = 100;

        for (v = 0; v < 128; v++)
        {
            int m = pedal_map(0, (uint8_t)v);

            ok &= m <= 100 && m >= 10 && m <= last;
            last = m;
        }

        CHECK(ok);
    }

    // In run mode the pedal sends the scaled value: 10 at the heel, 100 at the toe.
    reset_world();
    pk_mask = 0;
    pedal_rng[0] = 10;
    pedal_rng[2] = 100;
    pedal(0, 128);
    clear_log();
    pedal(0, 0);
    CHECK(midi_len == 3 && cc_number(0) == cc_a && cc_value(0) == 10);
    clear_log();
    pedal(0, 255);
    CHECK(midi_len == 3 && cc_value(0) == 100 && sent[0] == 100);

    // The editor: footswitch 6 twice, the CC (NUMBER flickers), UP to VALUE 1 (VALUE 1
    // flickers, the heel value shown), typed; UP to VALUE 2, typed; UP accepts.
    reset_world();
    chord_long();
    to_page(PAGE_EXP_A);
    tap(sw_code(PED_CC + 1));
    CHECK(pcc_open && pcc_step == 0 && range_flickers(HAL_LED_NUMBER) && aux[HAL_LED_VALUE_1] == 0);
    tap(CTL_KEY_UP);
    CHECK(pcc_open && pcc_step == 1 && display == 0 && pedal_cc[0] == cc_a);
    CHECK(range_flickers(HAL_LED_VALUE_1) && aux[HAL_LED_NUMBER] == 0 && aux[HAL_LED_EXP_1] == 1 &&
          stomp_leds_are(0, digit_leds("0123456789")));
    tap(sw_code(1));
    tap(sw_code(10));
    CHECK(pedal_rng[0] == 10 && display == 10 && num_typing);
    tap(CTL_KEY_UP);          // ends the number (it could take a third digit)
    CHECK(pcc_open && pcc_step == 1 && !num_typing);
    tap(CTL_KEY_UP);
    CHECK(pcc_open && pcc_step == 2 && display == 127 && range_flickers(HAL_LED_VALUE_2));
    tap(sw_code(1));
    tap(sw_code(10));
    tap(sw_code(10));
    CHECK(pedal_rng[2] == 100 && !num_typing);
    // DOWN steps back without changing anything, UP forward again.
    tap(CTL_KEY_DOWN);
    CHECK(pcc_open && pcc_step == 1 && display == 10);
    tap(CTL_KEY_UP);
    tap(CTL_KEY_UP);
    // (The step shares ped_sel's byte: footswitch 6 is selected again.)
    CHECK(!pcc_open && pedal_rng[0] == 10 && pedal_rng[2] == 100 && pedal_rng[1] == 0 && pedal_rng[3] == 127 &&
          ped_sel == PED_CC && leds[PED_CC] == 1);
    ticks(1);
    CHECK(cfg_changed == 1);
    // DOWN on the first step puts all three back.
    // (5 and 2 could each take another digit: the first UP ends the number.)
    tap(sw_code(PED_CC + 1));
    tap(sw_code(5));
    tap(CTL_KEY_UP);
    tap(CTL_KEY_UP);
    tap(sw_code(2));
    tap(CTL_KEY_UP);
    tap(CTL_KEY_DOWN);
    CHECK(pcc_open && pcc_step == 0 && pedal_cc[0] == 5 && pedal_rng[0] == 2);
    tap(CTL_KEY_DOWN);
    CHECK(!pcc_open && pedal_cc[0] == cc_a && pedal_rng[0] == 10 && pedal_rng[2] == 100);
    // Saved with the bank (its range record), and loaded from it.
    chord_short();
    CHECK(mode == MODE_RUN && range_saved == 1 && saved_msg[HAL_REC_RANGE][0] == 10 &&
          saved_msg[HAL_REC_RANGE][2] == 100 && saved_msg[HAL_REC_RANGE][3] == 127);
    memcpy(stored_msg[HAL_REC_RANGE], saved_msg[HAL_REC_RANGE], 4);
    msg_stored_valid[HAL_REC_RANGE] = 1;
    pedal_rng[0] = 0;
    load_settings();
    CHECK(pedal_rng[0] == 10 && pedal_rng[2] == 100);
    // Another bank has its own (the defaults).
    mset(NP_BANK, 2);
    ticks(5);
    CHECK(hal_bank == 1 && pedal_rng[0] == 0 && pedal_rng[2] == 127);
    // An invalid record (a bit 7 set reads as 7 bits; a wrong checksum is the fake's
    // "not valid") gives the defaults.
    mset(NP_BANK, 1);
    ticks(5);
    CHECK(hal_bank == 0 && pedal_rng[0] == 10);
    msg_stored_valid[HAL_REC_RANGE] = 0;
    mset(NP_BANK, 2);
    ticks(5);
    mset(NP_BANK, 1);
    ticks(5);
    CHECK(pedal_rng[0] == 0 && pedal_rng[2] == 127);

    // Over MIDI: parameters 102-105, and the dump's last four bytes before F7.
    reset_world();
    mset(NP_RANGE + 1, 20);
    mset(NP_RANGE + 3, 90);
    CHECK(pedal_rng[1] == 20 && pedal_rng[3] == 90);
    clear_log();
    dump_run(1000);
    CHECK(parse_dump(0) == DUMP_PARAMS && dump_values[NP_RANGE] == 0 && dump_values[NP_RANGE + 2] == 127 &&
          dump_values[NP_RANGE + 1] == 20 && dump_values[NP_RANGE + 3] == 90);

    // The relay trigger set from a pedal on a SWITCH page is the CC value it sends there.
    reset_world();
    pedal_rng[0] = 10;
    pedal_rng[2] = 100;
    chord_long();
    to_page(PAGE_SW1);
    pedal(0, 128);
    pedal(0, 255);
    CHECK(sw_thr[0] == 100 && display == 100);
    pedal(0, 0);
    CHECK(sw_thr[0] == 0);
    cancel_exit();
    reset_world();
}

static void test_pedal_cc_config(void)
{
    uint8_t cc_a = bank_def[1] & 0x7F;
    uint8_t cc_b = bank_def[2] & 0x7F;

    uint8_t pk_def = (uint8_t)(PEDAL_PICKUP_A | (PEDAL_PICKUP_B << 1));

    printf("pedal CC: pedal page editor, per bank, NRPN\n");
    reset_world();
    CHECK(pedal_cc[0] == cc_a && pedal_cc[1] == cc_b && cc_a != 44 && cc_b != 44);
    chord_long();
    to_page(PAGE_EXP_A);
    CHECK(cfg_page == PAGE_EXP_A && !pcc_open);

    // The page opens on footswitch 6, showing pedal A's CC (user's request); pressed, it
    // opens its editor: EXP 1 and NUMBER lit, the CC shown, the digits that can start it
    // dim.
    CHECK(!pcc_open && display == cc_a && leds[PED_CC] == 1 && aux[HAL_LED_NUMBER] == 0);
    tap(sw_code(PED_CC + 1));
    CHECK(pcc_open && !pcc_b && pk_mask == pk_def && display == cc_a);
    CHECK(aux[HAL_LED_EXP_1] == 1 && aux[HAL_LED_EXP_2] == 0 && aux[HAL_LED_NUMBER] == 1 &&
          stomp_leds_are(0, digit_leds("0123456789")) && pcc_step == 0);
    // Digits as everywhere: 4, 4 make 44 (complete).
    tap(sw_code(4));
    CHECK(pedal_cc[0] == 4 && display == 4 && heel[0] == 0 && num_typing);
    ticks(1);
    CHECK(cfg_changed == 1);
    tap(sw_code(4));
    CHECK(pedal_cc[0] == 44 && display == 44 && !num_typing);
    // A short DOWN [ESCAPE] puts the old CC back, back on the pedal page (still showing
    // the CC, NUMBER dark).
    tap(CTL_KEY_DOWN);
    CHECK(mode == MODE_CONFIG && cfg_page == PAGE_EXP_A && !pcc_open && pedal_cc[0] == cc_a && display == cc_a);
    CHECK(aux[HAL_LED_NUMBER] == 0 && leds[PED_CC] == 1);
    ticks(1);
    CHECK(cfg_changed == 0);
    // Pedal B's, on page 4 with the same footswitch 6 (selected as the page opens): a
    // pedal sets nothing (user's request), typed it is; a short UP [ENTER] keeps it. A
    // long UP is a short one here: it does not save and leave.
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_EXP_B);
    tap(sw_code(PED_CC + 1));
    CHECK(pcc_open && pcc_b && aux[HAL_LED_EXP_2] == 1 && display == cc_b);
    pedal(0, 0);
    pedal(0, raw_for(100));
    CHECK(pedal_cc[1] == cc_b && display == cc_b);
    tap(sw_code(4));
    tap(sw_code(4));
    CHECK(pedal_cc[1] == 44 && display == 44 && midi_len == 0);
    long_nav(CTL_KEY_UP);
    // Then VALUE 1 and VALUE 2 (left as they are), and UP on VALUE 2 accepts.
    CHECK(mode == MODE_CONFIG && pcc_open && pcc_step == 1 && display == 0 && cfg_saved == 0);
    tap(CTL_KEY_UP);
    CHECK(pcc_open && pcc_step == 2 && display == 127);
    tap(CTL_KEY_UP);
    CHECK(mode == MODE_CONFIG && cfg_page == PAGE_EXP_B && !pcc_open && pedal_cc[1] == 44 && cfg_saved == 0);
    // Footswitch 8 shows pedal B's pickup, then toggles it.
    tap(sw_code(PED_PICKUP + 1));
    CHECK(pk_mask == pk_def && stomp_leds_are(1u << PED_PICKUP, PED_MASK));
    tap(sw_code(PED_PICKUP + 1));
    CHECK(pk_mask == (pk_def ^ 2) && leds[PED_PICKUP] == 1);
    tap(sw_code(PED_PICKUP + 1));
    CHECK(pk_mask == pk_def);
    // Saving stores it with the bank; run mode sends pedal B on CC 44.
    save_exit();
    CHECK(mode == MODE_RUN && (saved_y(0, 2) & 0x7F) == 44 && (saved_y(0, 1) & 0x7F) == cc_a);
    pk_mask = 0;
    pedal(1, 0);
    clear_log();
    pedal(1, 255);
    CHECK(midi_len == 3 && cc_number(0) == 44 && cc_value(0) == 127);

    // Bank 2 (never saved): the default CCs; bank 1 again: 44.
    keep_saved_msgs();
    tap(CTL_KEY_UP);
    ticks(1);
    CHECK(hal_bank == 1 && pedal_cc[1] == cc_b && pk_mask == pk_def);
    pk_mask = 0;
    clear_log();
    pedal(1, 0);
    CHECK(midi_len == 3 && cc_number(0) == cc_b && cc_value(0) == 0);
    tap(CTL_KEY_DOWN);
    ticks(1);
    CHECK(hal_bank == 0 && pedal_cc[1] == 44);

    // A pickup pedal changing CC with the bank: its new CC's value (here unknown, so 0) is
    // compared with where it is, so it waits until it gets there.
    pk_mask = 2;
    pedal(1, 255);
    clear_log();
    pedal(1, 254);
    CHECK(!pk_waiting(1));
    store_y(1, 2, (uint8_t)(cc_b | 0x80));
    tap(CTL_KEY_UP);
    ticks(1);
    CHECK(hal_bank == 1 && pedal_cc[1] == cc_b && (pk_mask & 2) && pk_waiting(1));
    clear_log();
    pedal(1, raw_for(100));
    CHECK(midi_len == 0);
    // At 0 (its value) it is picked up without sending it, then sends as it moves.
    pedal(1, 0);
    CHECK(midi_len == 0 && !pk_waiting(1));
    pedal(1, raw_for(30));
    CHECK(midi_len == 3 && cc_number(0) == cc_b && cc_value(0) == 30);

    // Over MIDI: parameters 96 / 97, the bank in use; the pedal sends on it from then on.
    reset_world();
    pk_mask = 0;
    mset(NP_COUNT, 20);
    CHECK(pedal_cc[0] != 20);
    mset(NP_PEDAL_CC, 20);
    CHECK(pedal_cc[0] == 20 && param_get(NP_PEDAL_CC) == 20 && param_get(NP_PEDAL_CC + 1) == cc_b);
    pedal(0, 0);
    clear_log();
    pedal(0, 255);
    CHECK(midi_len == 3 && cc_number(0) == 20);
}

// The relays' settings belong to the bank: bank 2's own follows and triggers.
static void test_bank_relays(void)
{
    printf("relays: per bank, moved only by their triggers\n");
    reset_world();
    store_relay(1, 0, 0x08, 0, 0);
    store_relay(1, 1, 0, 0, 100);
    // Switch 4 a stomp in both banks, whatever DEFAULT_PEDAL_MODE is.
    store_bank_mode(0, 3, PM_STOMP);
    store_bank_mode(1, 3, PM_STOMP);
    load_settings();
    pk_mask = 0;
    set_mode(2, PM_STOMP);
    // Off to start with (inverted momentary, the power-on mode in mode 3, rests on).
    if (stomp_on(3))
    {
        tap(3);
    }
    tap(2);
    ticks(1);
    CHECK(relays == 0 && sw_mask[0] == 0);
    tap(2);
    tap(CTL_KEY_UP);
    ticks(1);
    CHECK(hal_bank == 1 && sw_mask[0] == 0x08 && sw_mask[1] == 0 && sw_thr[3] == 100 && sw_thr[0] == 0);
    set_mode(3, PM_STOMP);
    tap(3);
    ticks(1);
    CHECK(relays == 1);
    pedal(1, 0);
    pedal(1, 255);
    ticks(1);
    CHECK(relays == 3);
    // A bank change does not move a relay (user's request): bank 1 has no triggers, so
    // both stay closed.
    tap(CTL_KEY_DOWN);
    ticks(1);
    CHECK(hal_bank == 0 && sw_mask[0] == 0 && sw_thr[3] == 0);
    ticks(1);
    CHECK(relays == 3 && aux[HAL_LED_SWITCH_1] == 1 && aux[HAL_LED_SWITCH_2] == 1);
    pedal(1, 0);
    tap(3);
    ticks(1);
    CHECK(relays == 3);
    // Back in bank 2 (switch 4 now off, pedal B at the heel): still closed; then each
    // opens when its trigger changes: pedal B crossing its threshold, switch 4 going on
    // and off.
    tap(CTL_KEY_UP);
    ticks(2);
    CHECK(hal_bank == 1 && relays == 3);
    pedal(1, 255);
    ticks(1);
    CHECK(relays == 3);
    pedal(1, 0);
    ticks(1);
    CHECK(relays == 1);
    tap(3);
    ticks(1);
    CHECK(relays == 1 && stomp_on(3));
    tap(3);
    ticks(1);
    CHECK(relays == 0);
    // A press straight after a bank change, before the next tick, still counts.
    pedal(1, 255);
    ticks(1);
    CHECK(relays == 2);
    tap(CTL_KEY_DOWN);
    ticks(1);
    tap(CTL_KEY_UP);
    ticks(1);
    CHECK(hal_bank == 1 && relays == 2);
    tap(3);
    ticks(1);
    CHECK(relays == 3);
}

static void test_pedal_pickup(void)
{
    const uint8_t pcc[2] = { PEDAL_CC_LIST };
    int k;

    printf("expression pedal pickup\n");

    // The default from config.h, and immediate pedals as before: the first reading is
    // only recorded, then every move is sent, with no flash and no distance shown.
    reset_world();
    CHECK(pk_mask == (PEDAL_PICKUP_A | (PEDAL_PICKUP_B << 1)));
    pk_mask = 0;
    pedal(0, raw_for(60));
    CHECK(midi_len == 0);
    pedal(0, raw_for(70));
    CHECK(cc_count() == 1 && cc_number(0) == pcc[0] && cc_value(0) == 70 && display == 1);
    CHECK(exp_toggles(0, 2000) == 0 && aux[HAL_LED_EXP_1] == 0);
    // Its sends are the CC's known value, so pickup mode turned on now is already picked up.
    pk_mask = 1;
    CHECK(!pk_waiting(0));
    clear_log();
    pedal(0, raw_for(75));
    CHECK(cc_count() == 1 && cc_value(0) == 75 && display == 1);

    // Pedal A in pickup mode at power-on: its CC counts as 0, so a pedal at 60 waits. The
    // first reading sends nothing and leaves the display alone; the EXP 1 LED flashes at
    // the half-period for 60 steps away.
    reset_world();
    pk_mask = 1;
    pedal(0, raw_for(60));
    CHECK(midi_len == 0 && display == 1 && pk_waiting(0) && !pk_waiting(1));
    CHECK(pk_period(0) == PICKUP_FLASH_TICKS + 60 + 30);
    k = PICKUP_FLASH_TICKS + 60 + 30;
    CHECK(exp_toggles(0, k * 6) == 6 && aux[HAL_LED_EXP_2] == 0);
    // Moved: nothing sent, the distance on the display (position minus the pickup value).
    pedal(0, raw_for(40));
    CHECK(midi_len == 0 && display == SHOWN_SIGNED(40));
    // Closer flashes faster.
    CHECK(pk_period(0) == PICKUP_FLASH_TICKS + 40 + 20);
    CHECK(exp_toggles(0, (PICKUP_FLASH_TICKS + 60) * 4) >= 4);
    // Wobble does not count as a move.
    pedal(0, (uint8_t)(raw_for(40) + 1));
    CHECK(midi_len == 0 && pk_pos[0] == 40);
    // Within PEDAL_PICKUP_WINDOW of the value: picked up, sent, the LED goes dark.
    pedal(0, raw_for(PEDAL_PICKUP_WINDOW));
    CHECK(cc_count() == (PEDAL_PICKUP_WINDOW ? 1 : 0) && !pk_waiting(0));
    CHECK(PEDAL_PICKUP_WINDOW == 0 || (cc_number(0) == pcc[0] && cc_value(0) == PEDAL_PICKUP_WINDOW));
    CHECK(display == SHOWN_SIGNED(PEDAL_PICKUP_WINDOW));
    ticks(1);
    CHECK(aux[HAL_LED_EXP_1] == 0 && exp_toggles(0, 1000) == 0);
    // Then it sends at once; the display goes back to the bank PEDAL_PICKUP_SHOW_MS after
    // the last distance shown.
    clear_log();
    pedal(0, raw_for(20));
    CHECK(cc_count() == 1 && cc_value(0) == 20);
    reset_world();
    pk_mask = 1;
    pedal(0, raw_for(60));
    pedal(0, raw_for(50));
    CHECK(display == SHOWN_SIGNED(50));
    ticks(PICKUP_SHOW_TICKS - 1);
    CHECK(display == SHOWN_SIGNED(50));
    ticks(1);
    CHECK(display == 1);
    ticks(PICKUP_SHOW_TICKS);
    CHECK(display == 1);

    // Passing the value without stopping near it picks up too: MIDI IN sets CC A to 100;
    // the pedal at 50 shows -50, at 90 -10, then a jump to 110 crosses 100 and is sent.
    midi_cc_in(pcc[0], 100);
    clear_log();
    pedal(0, raw_for(52));
    CHECK(midi_len == 0 && display == SHOWN_SIGNED(-48));
    pedal(0, raw_for(90));
    CHECK(midi_len == 0 && display == SHOWN_SIGNED(-10));
    pedal(0, raw_for(110));
    CHECK(cc_count() == 1 && cc_value(0) == 110 && !pk_waiting(0) && display == SHOWN_SIGNED(10));
    // A received value equal to the pedal's keeps it picked up; another one waits again.
    midi_cc_in(pcc[0], 110);
    CHECK(!pk_waiting(0));
    midi_cc_in(pcc[0], 30);
    CHECK(pk_waiting(0));
    // Reaching the value exactly sends nothing (the receiver has it already).
    clear_log();
    pedal(0, raw_for(30));
    CHECK(midi_len == 0 && !pk_waiting(0) && display == SHOWN_SIGNED(0));
    pedal(0, raw_for(35));
    CHECK(cc_count() == 1 && cc_value(0) == 35);

    // A footswitch sending the pedal's CC sets its value too.
    set_mode(0, PM_STOMP);
    msg[0] = pcc[0];
    msg[1] = 64;
    msg[2] = 0;
    clear_log();
    tap(sw_code(1));
    CHECK(cc_count() == 1 && cc_number(0) == pcc[0] && cc_value(0) == 64 && pk_waiting(0));

    // MIDI OUT full at the pickup: not picked up, retried at the next reading.
    reset_world();
    pk_mask = 1;
    pedal(0, raw_for(10));
    midi_space = 2;
    pedal(0, raw_for(1));
    CHECK(midi_len == 0 && pk_waiting(0) && pk_pos[0] == 10);
    midi_space = 15;
    pedal(0, raw_for(1));
    CHECK(cc_count() == (PEDAL_PICKUP_WINDOW ? 1 : 0) && !pk_waiting(0));

    // Typing a program number: the display keeps the number, the LED still flashes.
    reset_world();
    pk_mask = 2;
    program_mode();
    tap(sw_code(10));
    CHECK(pc_typing && display == SHOWN_TEXT(HAL_TEXT_DASHES));
    pedal(1, raw_for(80));
    pedal(1, raw_for(70));
    CHECK(midi_len == 0 && display == SHOWN_TEXT(HAL_TEXT_DASHES) && pk_waiting(1));
    CHECK(exp_toggles(1, 1000) > 0 && aux[HAL_LED_EXP_1] == 0);

    // Configuration mode: the LED shows the page's state instead; after it the pedal is
    // read afresh, and picked up again at once if it has not moved from its value.
    reset_world();
    pk_mask = 1;
    pedal(0, raw_for(1));
    CHECK(!pk_waiting(0) && cc_count() == (PEDAL_PICKUP_WINDOW ? 1 : 0));
    chord_long();
    pedal(0, raw_for(1));
    cancel_exit();
    CHECK(mode == MODE_RUN);
    pk_mask = 1;
    clear_log();
    pedal(0, raw_for(1));
    CHECK(midi_len == 0 && !pk_waiting(0));

    // The EXP pages: "Ab" immediate, "P.U." pickup (user's request: "PU" with the two
    // dots); footswitch 8 turns pickup on and off for the page's pedal (its EXP LED lit,
    // footswitch 8 lit and 6 / 9 dim); saved with the bank.
    reset_world();
    pk_mask = 0;
    chord_long();
    to_page(PAGE_EXP_A);
    CHECK(cfg_page == PAGE_EXP_A && display == pedal_cc[0] && dots == 0);
    CHECK(aux[HAL_LED_EXP_1] == 1 && aux[HAL_LED_EXP_2] == 0 && stomp_leds_are(1u << PED_CC, PED_MASK));
    // Page 3, EXP A: footswitch 8's first press shows pedal A's response, the next
    // toggles it.
    tap(sw_code(PED_PICKUP + 1));
    CHECK(pk_mask == 0 && display == SHOWN_TEXT(HAL_TEXT_ABS) && dots == 0 && stomp_leds_are(1u << PED_PICKUP, PED_MASK));
    tap(sw_code(PED_PICKUP + 1));
    ticks(1);
    CHECK(pk_mask == 1 && display == SHOWN_TEXT(HAL_TEXT_PICKUP) && dots == 3 && leds[PED_PICKUP] == 1);
    CHECK(cfg_changed == (pk_mask != (PEDAL_PICKUP_A | (PEDAL_PICKUP_B << 1))));
    // Another footswitch selected, then footswitch 8 again: the first press only shows
    // the response (user's request), the second toggles it.
    tap(sw_code(PED_CC + 1));
    CHECK(ped_sel == PED_CC && pk_mask == 1 && dots == 0 && stomp_leds_are(1u << PED_CC, PED_MASK));
    tap(sw_code(PED_PICKUP + 1));
    CHECK(pk_mask == 1 && leds[PED_PICKUP] == 1 && display == SHOWN_TEXT(HAL_TEXT_PICKUP) && dots == 3);
    tap(sw_code(PED_PICKUP + 1));
    ticks(1);
    CHECK(pk_mask == 0 && display == SHOWN_TEXT(HAL_TEXT_ABS) && dots == 0);
    // Page 4, EXP B, the same footswitch 8 for pedal B, its EXP 2 lit.
    tap(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_EXP_B && aux[HAL_LED_EXP_2] == 1 && aux[HAL_LED_EXP_1] == 0 && leds[PED_CC] == 1 &&
          display == pedal_cc[1]);
    tap(sw_code(PED_PICKUP + 1));
    CHECK(pk_mask == 0 && display == SHOWN_TEXT(HAL_TEXT_ABS));
    tap(sw_code(PED_PICKUP + 1));
    CHECK(pk_mask == 2 && display == SHOWN_TEXT(HAL_TEXT_PICKUP));
    // The pedals do nothing here, nor do the other footswitches.
    tap(sw_code(4));
    tap(sw_code(1));
    tap(sw_code(10));
    pedal(0, 200);
    pedal(0, 10);
    CHECK(pk_mask == 2 && midi_len == 0 && stomp_leds_are(1u << PED_PICKUP, PED_MASK));
    save_exit();
    CHECK(mode == MODE_RUN && cfg_saved == 1 && saved_cfg[0] == MIDI_CHANNEL && saved_cfg[1] == 0);
    CHECK(saved_cfg[2] == (LEVEL_VALID | LED_BRIGHTNESS) && saved_cfg[3] == MISC_DEFAULT);
    // Saved with the bank: bit 7 of the pedals' packed bytes.
    CHECK((saved_y(0, 1) >> 7) == 0 && (saved_y(0, 2) >> 7) == 1);
    CHECK((saved_y(0, 1) & 0x7F) == pedal_cc[0] && (saved_y(0, 2) & 0x7F) == pedal_cc[1]);
    // Loaded at power-on; the channel record's old pickup byte is ignored.
    keep_saved_msgs();
    stored_cfg[0] = MIDI_CHANNEL;
    stored_cfg[1] = 0xFD;
    stored_cfg[2] = 0;
    stored_cfg[3] = 0;
    cfg_stored_valid = 1;
    power_on();
    boot_end();
    CHECK(pk_mask == 2);
    cfg_stored_valid = 0;
    // Each bank has its own: bank 2 (never saved) has the defaults, bank 1 its own again.
    tap(CTL_KEY_UP);
    ticks(1);
    CHECK(hal_bank == 1 && pk_mask == (PEDAL_PICKUP_A | (PEDAL_PICKUP_B << 1)));
    tap(CTL_KEY_DOWN);
    ticks(1);
    CHECK(hal_bank == 0 && pk_mask == 2);

    // Over MIDI: NRPN 94 / 95 (0 or 1), the bank's pedal setting.
    reset_world();
    pk_mask = 0;
    mset(NP_PICKUP + 1, 1);
    CHECK(pk_mask == 2 && param_get(NP_PICKUP) == 0 && param_get(NP_PICKUP + 1) == 1);
    mset(NP_PICKUP, 2);
    CHECK(pk_mask == 2);
    mset(NP_PICKUP, 1);
    mset(NP_PICKUP, 1);
    CHECK(pk_mask == 3);
    mset(NP_PICKUP + 1, 0);
    CHECK(pk_mask == 1 && midi_len == 0);
    reset_world();
}

// A long UP asks which bank to save to (user's request): the bank in use first, typed
// on the footswitches, short UP saves there and leaves (in that bank), short DOWN goes
// back to the page. UP + DOWN still saves to the bank in use at once.
static void test_save_bank(void)
{
    uint8_t m;

    printf("config: a long UP asks which bank to save to\n");
    reset_world();
    chord_long();
    m = pedal_mode_get(0);
    mode_tap(sw_code(1));
    CHECK(pedal_mode_get(0) == ((m + 1) & 3));
    long_nav(CTL_KEY_UP);
    CHECK(save_prompt && mode == MODE_CONFIG && display == 1 && cfg_saved == 0 && msg_saved == 0);
    // Page LEDs dark; the digits that can start a bank (1-9, not 0) dim.
    CHECK(aux[HAL_LED_MIDI_FUNC] == 0 && stomp_leds_are(0, digit_leds((BANK_COUNT >= 9) ? "123456789" : "12345678")));
    // The number and the dot alternate (user's request): the number first, no dot;
    // then a blank display with the dot.
    ticks(1);
    CHECK(dots == 0 && display == 1 && blank == 0);
    ticks(CHAN_FLASH_TICKS);
    CHECK(dots == 0x01 && blank == 7);
    ticks(CHAN_FLASH_TICKS);
    CHECK(dots == 0 && display == 1 && blank == 0);
    ticks(CHAN_FLASH_TICKS);
    // A digit that completes a bank (4: no bank 40) shows it at once, alternating again.
    tap(sw_code(4));
    ticks(1);
    CHECK(display == 4 && blank == 0 && dots == 0 && save_bank == 4 && !num_typing);
    ticks(CHAN_FLASH_TICKS);
    CHECK(dots == 0x01 && blank == 7);
    // The pedals do nothing; a footswitch types a digit when let go.
    pedal(0, 0);
    pedal(0, 200);
    CHECK(save_bank == 4);
    ctl_key_press(sw_code(5));
    CHECK(save_bank == 4);
    ctl_key_release(sw_code(5));
    CHECK(save_bank == 5 && !num_typing);
#if BANK_COUNT >= 12
    // 1: 10-19 can follow, so the number stays and the dot flashes; 2 makes 12, complete.
    tap(sw_code(1));
    CHECK(save_bank == 1 && num_typing && display == 1 &&
          stomp_leds_are(0, digit_leds((BANK_COUNT >= 19) ? "0123456789" : "012")));
    ticks(1);
    CHECK(dots == 0x01 && blank == 0);
    ticks(CHAN_FLASH_TICKS);
    CHECK(dots == 0 && blank == 0 && display == 1);
    tap(sw_code(2));
    CHECK(display == 12 && save_bank == 12 && !num_typing);
    // 0 cannot start a bank.
    tap(sw_code(10));
    CHECK(display == 12);
    tap(sw_code(1));
    tap(sw_code(10));
    CHECK(display == 10 && !num_typing);
    // 1 then DOWN [ESCAPE]: back to 10, still in the prompt; 1 then UP [ENTER]: bank 1,
    // still in the prompt.
    tap(sw_code(1));
    tap(CTL_KEY_DOWN);
    CHECK(save_prompt && save_bank == 10 && display == 10 && !num_typing);
    tap(sw_code(1));
    tap(CTL_KEY_UP);
    CHECK(save_prompt && save_bank == 1 && display == 1 && !num_typing && mode == MODE_CONFIG);
#endif
    tap(sw_code(4));
    CHECK(display == 4);
    // With UP held, a footswitch types nothing (the simulator's row 2).
    ctl_key_press(CTL_KEY_UP);
    tap(sw_code(6));
    CHECK(display == 4 && save_prompt);
    // UP's release saves into bank 4 and leaves, in bank 4; bank 1 is not saved.
    ctl_key_release(CTL_KEY_UP);
    CHECK(mode == MODE_RUN && hal_bank == 3 && bank_next == 3);
    CHECK(msg_was_saved[3 * BANK_RECS + 0] && !msg_was_saved[0] && ((saved_msg[3 * BANK_RECS][0] >> 2) & 3) == ((m + 1) & 3));
    CHECK(saved_msg[3 * BANK_RECS][1] == stomp_cc[0] && display == 4 && !save_prompt);
    ticks(1);
    CHECK(dots == 0);

    // DOWN goes back; a long one too. Saving to the bank shown when nothing is typed.
    reset_world();
    chord_long();
    mode_tap(sw_code(1));
    long_nav(CTL_KEY_UP);
    tap(sw_code(4));
    tap(CTL_KEY_DOWN);
    CHECK(mode == MODE_CONFIG && !save_prompt && cfg_page == PAGE_MODES && pedal_mode_get(0) == ((m + 1) & 3));
    ticks(1);
    CHECK(dots == 0 && display == 1 && aux[HAL_LED_MIDI_FUNC] == 1 && msg_saved == 0);
    long_nav(CTL_KEY_UP);
    CHECK(save_prompt && display == 1);
    long_nav(CTL_KEY_DOWN);
    CHECK(mode == MODE_CONFIG && !save_prompt);
    long_nav(CTL_KEY_UP);
    long_nav(CTL_KEY_UP);
    CHECK(mode == MODE_RUN && hal_bank == 0 && msg_was_saved[0] && !msg_was_saved[BANK_RECS]);

    // UP + DOWN in the prompt saves to the bank in use at once.
    reset_world();
    chord_long();
    long_nav(CTL_KEY_UP);
    tap(sw_code(2));
    chord_down();
    ticks(1);
    chord_up();
    CHECK(mode == MODE_RUN && hal_bank == 0 && cfg_saved == 1 && !msg_was_saved[BANK_RECS] && !save_prompt);

    // From page 8: the lamp test and scroll stop; DOWN starts the page again.
    reset_world();
    chord_long();
    to_page(PAGE_VERSION);
    long_nav(CTL_KEY_UP);
    CHECK(save_prompt && lamp == HAL_LAMP_NORMAL && display == 1);
    ticks(VER_STEP_TICKS * 20);
    CHECK(display == 1 && lamp == HAL_LAMP_NORMAL);
    tap(CTL_KEY_DOWN);
    CHECK(cfg_page == PAGE_VERSION && lamp != HAL_LAMP_NORMAL && !save_prompt);
    cancel_exit();
}

// The right-hand dot flashes while a number being typed can take another digit (user's
// request), the same everywhere in config mode: the footswitch editor's number and
// values, the pedal CC editor, the channel; not the mode or type words, nor a number
// shown but not being typed.
static void test_entry_dot(void)
{
    const uint8_t pcc[2] = { PEDAL_CC_LIST };
    uint8_t d;

    printf("config: the right-hand dot flashes while a number is entered\n");
    reset_world();
    pk_mask = 0;
    chord_long();
    ticks(CHAN_FLASH_TICKS * 2 + 1);
    CHECK(dots == 0);
    long_press(sw_code(1));
    CHECK(edit_open && edit_param == EP_FUNC);
    ticks(CHAN_FLASH_TICKS * 2 + 1);
    CHECK(dots == 0);
    tap(CTL_KEY_UP);
    CHECK(edit_param == EP_TYPE);
    ticks(CHAN_FLASH_TICKS * 2 + 1);
    CHECK(dots == 0);
    tap(CTL_KEY_UP);
    CHECK(edit_param == EP_NUM);
    ticks(CHAN_FLASH_TICKS * 2 + 1);
    CHECK(dots == 0);
    tap(sw_code(6));
    ticks(1);
    d = dots;
    CHECK(d == 1 && MSG_NUM(0) == 6);
    ticks(CHAN_FLASH_TICKS);
    CHECK(dots == 0);
    ticks(CHAN_FLASH_TICKS);
    CHECK(dots == 1);
    // A third digit cannot follow 64: the dot goes out.
    tap(sw_code(4));
    ticks(1);
    CHECK(dots == 0 && MSG_NUM(0) == 64);
    ticks(CHAN_FLASH_TICKS);
    CHECK(dots == 0);
    edit_close();
    ticks(1);
    CHECK(!edit_open && dots == 0);

    // The pedal CC editor: footswitch 6 (selected as the page opens) pressed.
    to_page(PAGE_EXP_A);
    tap(sw_code(PED_CC + 1));
    CHECK(pcc_open);
    ticks(CHAN_FLASH_TICKS + 1);
    CHECK(dots == 0);
    tap(sw_code(1));
    ticks(1);
    CHECK(dots == 1 && num_typing);
    ticks(CHAN_FLASH_TICKS);
    CHECK(dots == 0);
    // DOWN ends the number (the CC back), still in the editor; DOWN again closes it.
    tap(CTL_KEY_DOWN);
    ticks(1);
    CHECK(pcc_open && !num_typing && dots == 0 && pedal_cc[0] == pcc[0]);
    tap(CTL_KEY_DOWN);
    ticks(1);
    CHECK(!pcc_open && dots == 0);

    // The channel: 1 can take 10-16, 6 then cannot.
    to_page(PAGE_CHANNEL);
    ticks(CHAN_FLASH_TICKS + 1);
    CHECK(dots == 0);
    tap(sw_code(1));
    ticks(1);
    CHECK(dots == 1 && midi_channel == 1);
    tap(sw_code(6));
    ticks(1);
    CHECK(dots == 0 && midi_channel == 16);
    ticks(CHAN_FLASH_TICKS);
    CHECK(dots == 0);
    cancel_exit();
    ticks(CHAN_FLASH_TICKS * 2);
    CHECK(dots == 0);
}

// n MIDI clocks (F8) on MIDI IN.
static void midi_clocks(int n)
{
    while (n-- > 0)
    {
        ctl_midi_in(0xF8);
    }
}

// Feed a SysEx message (F0 ... F7 included) to MIDI IN.
static void midi_in_bytes(const uint8_t *b, int n)
{
    int i;

    for (i = 0; i < n; i++)
    {
        ctl_midi_in(b[i]);
    }
}

// The reply to an Identity Request at midi_log[at]: F0 7E <channel - 1> 06 02, the
// non-commercial ID 7D, family "FC", member "10", the version (major, minor, build, bits
// 0-6) and their bit 7s, F7.
static int identity_reply_at(int at, uint8_t ch)
{
    static const uint8_t head[10] = { 0xF0, 0x7E, 0, 0x06, 0x02, 0x7D, 0x46, 0x43, 0x31, 0x30 };
    int i;

    if (midi_len < at + SX_REPLY_LEN)
    {
        return 0;
    }

    for (i = 0; i < 10; i++)
    {
        if (midi_log[at + i] != ((i == 2) ? (uint8_t)(ch - 1) : head[i]))
        {
            return 0;
        }
    }

    for (i = 0; i < 3; i++)
    {
        if (midi_log[at + 10 + i] != (fw_version[i] & 0x7F))
        {
            return 0;
        }
    }

    return midi_log[at + 13] == ((fw_version[0] >> 7) | ((fw_version[1] >> 7) << 1) | ((fw_version[2] >> 7) << 2)) &&
           midi_log[at + 14] == 0xF7;
}

// SysEx Identity Request (user's request): F0 7E <device> 06 01 F7, device 7F (all) or
// the pedal's channel - 1, answered once MIDI OUT has room, in any mode.
static void test_sysex_identity(void)
{
    const uint8_t all[6] = { 0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7 };
    uint8_t req[6] = { 0xF0, 0x7E, 0, 0x06, 0x01, 0xF7 };
    const uint8_t other[6] = { 0xF0, 0x7E, 0x7F, 0x06, 0x02, 0xF7 };
    const uint8_t longer[7] = { 0xF0, 0x7E, 0x7F, 0x06, 0x01, 0x00, 0xF7 };
    const uint8_t mfr[7] = { 0xF0, 0x7D, 0x7E, 0x7F, 0x06, 0x01, 0xF7 };

    printf("MIDI IN: SysEx Identity Request\n");
    reset_world();
    CHECK(SX_REPLY_LEN == 15);
    clear_log();
    midi_in_bytes(all, 6);
    CHECK(midi_len == 0);
    ticks(1);
    CHECK(identity_reply_at(0, MIDI_CHANNEL) && midi_len == SX_REPLY_LEN);
    ticks(5);
    CHECK(midi_len == SX_REPLY_LEN);

    // The pedal's own device number (channel - 1); another channel's is ignored.
    clear_log();
    req[2] = (uint8_t)(MIDI_CHANNEL - 1);
    midi_in_bytes(req, 6);
    ticks(1);
    CHECK(identity_reply_at(0, MIDI_CHANNEL));
    clear_log();
    req[2] = (uint8_t)(MIDI_CHANNEL % 16);
    midi_in_bytes(req, 6);
    ticks(1);
    CHECK(midi_len == 0);

    // Not an Identity Request: another sub-ID, an extra byte, another SysEx; a request
    // cut short by a status byte. None is answered.
    clear_log();
    midi_in_bytes(other, 6);
    midi_in_bytes(longer, 7);
    midi_in_bytes(mfr, 7);
    midi_in_bytes(all, 4);
    ctl_midi_in(0xB0);
    ctl_midi_in(0xF7);
    ticks(2);
    CHECK(midi_len == 0);

    // Real-time bytes inside it do not matter, and a CC after it works as usual.
    clear_log();
    ctl_midi_in(0xF0); ctl_midi_in(0xF8); ctl_midi_in(0x7E); ctl_midi_in(0x7F); ctl_midi_in(0xFE);
    ctl_midi_in(0x06); ctl_midi_in(0x01); ctl_midi_in(0xF8); ctl_midi_in(0xF7);
    set_mode(0, PM_STOMP);
    ctl_midi_in(CC_STATUS); ctl_midi_in(stomp_cc[0]); ctl_midi_in(127);
    CHECK(leds[0] == 1);
    ticks(1);
    CHECK(identity_reply_at(0, MIDI_CHANNEL) && midi_len == SX_REPLY_LEN);

    // It waits for room in MIDI OUT for the whole reply.
    clear_log();
    midi_space = SX_REPLY_LEN - 1;
    midi_in_bytes(all, 6);
    ticks(3);
    CHECK(midi_len == 0);
    midi_space = 15;
    ticks(1);
    CHECK(identity_reply_at(0, MIDI_CHANNEL));

    // In config mode too (the reply carries the channel in use).
    chord_long();
    clear_log();
    midi_in_bytes(all, 6);
    ticks(1);
    CHECK(mode == MODE_CONFIG && identity_reply_at(0, midi_channel));
    cancel_exit();
}

// Power-on sends the identity once (user's request), at the first tick with room.
static void test_identity_at_power_on(void)
{
    printf("start-up: SysEx identity sent\n");
    reset_world();
    clear_log();
    midi_space = SX_REPLY_LEN - 1;
    ctl_init();
    ticks(3);
    CHECK(midi_len == 0 && sx_reply);
    midi_space = 15;
    ticks(1);
    CHECK(identity_reply_at(0, MIDI_CHANNEL) && midi_len == SX_REPLY_LEN);
    ticks(5);
    CHECK(midi_len == SX_REPLY_LEN);
}

// Page 8, misc (user's request): footswitch 1 the MIDI clock display ("CL"), 9 running
// status ("rS"), 10 active sensing ("AS"). A press toggles at once and shows the name;
// each LED is lit while on and dim while off. Shared by every bank, saved in the channel
// record's byte 3 (bit 7 set).
static void test_misc_page(void)
{
    int k;

    printf("config: misc page (metronome, running status, active sensing)\n");
    reset_world();
    chord_long();
    to_page(PAGE_MISC);
    CHECK(cfg_page == PAGE_MISC && misc_sel == MISC_METRO && aux[HAL_LED_SELECT] == 1);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_CL) && stomp_leds_are(0, MISC_MASK) && !opt_metro && !opt_runstat && !opt_asense);
    // Each press toggles at once (on the press) and shows the name.
    ctl_key_press(sw_code(MISC_RUNSTAT + 1));
    CHECK(opt_runstat && display == SHOWN_TEXT(HAL_TEXT_RS) && stomp_leds_are(1u << MISC_RUNSTAT, MISC_MASK) && cfg_changed == 1);
    ctl_key_release(sw_code(MISC_RUNSTAT + 1));
    CHECK(opt_runstat && display == SHOWN_TEXT(HAL_TEXT_RS));
    tap(sw_code(MISC_ASENSE + 1));
    CHECK(opt_asense && display == SHOWN_TEXT(HAL_TEXT_AS) && stomp_leds_are((1u << MISC_RUNSTAT) | (1u << MISC_ASENSE), MISC_MASK));
    tap(sw_code(MISC_METRO + 1));
    CHECK(opt_metro && display == SHOWN_TEXT(HAL_TEXT_CL) && stomp_leds_are(MISC_MASK, 0));
    // Again: off, and back to the saved state.
    tap(sw_code(MISC_RUNSTAT + 1));
    tap(sw_code(MISC_ASENSE + 1));
    tap(sw_code(MISC_METRO + 1));
    CHECK(!opt_metro && !opt_runstat && !opt_asense && cfg_changed == 0 && stomp_leds_are(0, MISC_MASK));
    // The other footswitches do nothing, nor do footswitches with UP held (the
    // simulator's row 2): UP's release changes page.
    for (k = 1; k <= 10; k++)
    {
        if (k != MISC_METRO + 1 && k != MISC_RUNSTAT + 1 && k != MISC_ASENSE + 1)
        {
            tap(sw_code(k));
        }
    }

    CHECK(!opt_metro && !opt_runstat && !opt_asense && display == SHOWN_TEXT(HAL_TEXT_CL));
    tap(sw_code(MISC_METRO + 1));
    tap(sw_code(MISC_RUNSTAT + 1));
    ctl_key_press(CTL_KEY_UP);
    tap(sw_code(MISC_ASENSE + 1));
    ctl_key_release(CTL_KEY_UP);
    CHECK(cfg_page == PAGE_VERSION && opt_metro && opt_runstat && !opt_asense && midi_len == 0);
    // Saved in the channel record's byte 3; bytes 1 and 2 stay 0.
    save_exit();
    CHECK(cfg_saved == 1 && saved_cfg[1] == 0 && saved_cfg[2] == (LEVEL_VALID | LED_BRIGHTNESS) && saved_cfg[3] == (MISC_VALID | 3));
    // Loaded at power-on (bits 0-2, with bit 7).
    reset_world();
    cfg_stored_valid = 1;
    stored_cfg[0] = MIDI_CHANNEL;
    stored_cfg[3] = MISC_VALID | 7;
    power_on();
    CHECK(opt_metro && opt_runstat && opt_asense);
    stored_cfg[3] = MISC_VALID;
    power_on();
    CHECK(!opt_metro && !opt_runstat && !opt_asense);
    // A cancel puts them back as saved.
    chord_long();
    to_page(PAGE_MISC);
    tap(sw_code(MISC_ASENSE + 1));
    CHECK(opt_asense && cfg_changed == 1);
    cancel_exit();
    CHECK(mode == MODE_RUN && !opt_asense && cfg_saved == 0);
}

// Config indicators (user's request): "S.1" / "S.2" on the SWITCH pages (the dot after
// the S) until a pedal is moved; the misc page shows a setting's name ("CL", "r.S.",
// "A.S.") for CONFIG_SHOW_MS, then its value; page 1's and the editor's setting LED
// flickers, PARAM_FLASH_MS on and off.
static void test_config_indicators(void)
{
    int k, flips = 0, last;

    printf("config: S.1 / S.2, misc names then values, flickering setting LEDs\n");
    reset_world();
    chord_long();
    to_page(PAGE_SW1);
    ticks(1);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_S1) && dots == 2);
    pedal(0, 100);
    pedal(0, 200);
    ticks(1);
    CHECK(display == ctl_scale(200, 0, 255) && dots == 0);
    tap(CTL_KEY_UP);
    ticks(1);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_S2) && dots == 2);

    // Misc: the name first (CL has no dots), then the value.
    to_page(PAGE_MISC);
    ticks(1);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_CL) && dots == 0);
    ticks(CONFIG_SHOW_MS / TICK_MS - 2);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_CL));
    ticks(2);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_OFF) && dots == 0);
    tap(sw_code(MISC_RUNSTAT + 1));
    ticks(1);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_RS) && dots == 3 && opt_runstat);
    ticks(CONFIG_SHOW_MS / TICK_MS);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_ON) && dots == 0);
    tap(sw_code(MISC_ASENSE + 1));
    ticks(1);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_AS) && dots == 3);
    ticks(CONFIG_SHOW_MS / TICK_MS);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_ON));
    tap(sw_code(MISC_ASENSE + 1));
    ticks(CONFIG_SHOW_MS / TICK_MS + 1);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_OFF) && !opt_asense);
    // And so on, name and value in turn (user's request).
    ticks(CONFIG_SHOW_MS / TICK_MS);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_AS) && dots == 3);
    ticks(CONFIG_SHOW_MS / TICK_MS);
    CHECK(display == SHOWN_TEXT(HAL_TEXT_OFF) && dots == 0);
    tap(sw_code(MISC_RUNSTAT + 1));

    // Page 1's cycle: the setting's LED (SWITCH, for the pedal mode) steady (user's
    // request: only the editor's flickers).
    to_page(PAGE_MODES);
    ticks(CONFIG_SWITCH_SHOW_MS / TICK_MS);
    CHECK(aux[HAL_LED_SWITCH] == 1);
    last = aux[HAL_LED_SWITCH];

    for (k = 0; k < (PARAM_FLASH_MS / TICK_MS) * 4; k++)
    {
        ticks(1);
        flips += aux[HAL_LED_SWITCH] != last;
        last = aux[HAL_LED_SWITCH];
    }

    CHECK(flips == 0 && aux[HAL_LED_SWITCH] == 1 && aux[HAL_LED_MIDI_FUNC] == 1);
    // The editor's flickers, PARAM_FLASH_MS on and off.
    tap(sw_code(1));
    tap(CTL_KEY_UP);
    CHECK(edit_open && edit_param == EP_TYPE);
    flips = 0;
    last = aux[HAL_LED_SELECT];

    for (k = 0; k < (PARAM_FLASH_MS / TICK_MS) * 4; k++)
    {
        ticks(1);
        flips += aux[HAL_LED_SELECT] != last;
        last = aux[HAL_LED_SELECT];
    }

    CHECK(flips >= 3 && aux[HAL_LED_SWITCH] == 0);
    cancel_exit();
    reset_world();
}

// Reading another bank's saved settings over NRPN without changing bank (user's request):
// NP_READ_BANK starts a dump of that bank; the bank in use gives its live settings.
// What the fake EEPROM was sent becomes what it holds, as on the real pedal.
static void saves_to_stored(void)
{
    int r;

    for (r = 0; r < MSG_RECS; r++)
    {
        if (msg_was_saved[r])
        {
            memcpy(stored_msg[r], saved_msg[r], 4);
            msg_stored_valid[r] = 1;
        }
    }
}

// SX_CMD_WRITE: a bank's 48 bytes, as a dump gives them, stored into any bank without
// changing bank (user's request: a restore from a file must not step through the banks).
static void test_bank_write(void)
{
    uint8_t d[SX_WRITE_LEN + 1];
    int saves;

    printf("SysEx: writing any bank without changing bank\n");
    // Bank 1's settings, as a dump gives them.
    reset_world();
    set_mode(3, PM_MOM_INV);
    set_msg(4, MT_NOTE, 60, 100, 30);
    sw_mask[1] = (uint16_t)(1 << 2);
    sw_thr[1]  = 77;
    bank_pc    = 6;
    pk_mask    = 2;
    pedal_cc[0] = 20;
    clear_log();
    dump_run(1000);
    CHECK(parse_dump(0) == DUMP_PARAMS);
    memcpy(d + 1, midi_log + 16, SX_WRITE_LEN - 1);

    // Into bank 2, from bank 1: stored, nothing else changes, nothing is sent.
    reset_world();
    d[0] = 2;
    clear_log();
    saves = msg_saved;
    sx_send(0x7F, SX_CMD_WRITE, d, SX_WRITE_LEN);
    ticks(2);
    CHECK(msg_saved == saves + STOMP_COUNT && bank_saved == 1 && cfg_saved == 0 && midi_len == 0);
    CHECK(hal_bank == 0 && bank_next == 0 && !bank_busy && msg_type(4) == MT_CC && bank_pc == 0 &&
          pedal_cc[0] == 7 && pk_mask == ((PEDAL_PICKUP_A ? 1 : 0) | (PEDAL_PICKUP_B ? 2 : 0)));
    // Read back, bank 2 gives the same bytes.
    saves_to_stored();
    clear_log();
    dump_run_bank(2, 1000);
    CHECK(parse_dump(0) == DUMP_PARAMS && dump_bank_of == 2 && memcmp(midi_log + 16, d + 1, SX_WRITE_LEN - 1) == 0);
    CHECK(dump_values[NP_MODE + 3] == PM_MOM_INV && dump_values[NP_TYPE + 4] == MT_NOTE && dump_values[NP_FOLLOW2 + 2] == 1 &&
          dump_values[NP_TRIGGER + 1] == 77 && dump_values[NP_BANK_PC] == 5 && dump_values[NP_BANK_PC_ON] == 1 &&
          dump_values[NP_PICKUP + 1] == 1 && dump_values[NP_PEDAL_CC] == 20);
    // And choosing bank 2 loads it.
    mset(NP_BANK, 2);
    ticks(5);
    CHECK(hal_bank == 1 && msg_type(4) == MT_NOTE && sw_thr[1] == 77 && pedal_cc[0] == 20 && bank_pc == 6);

    // Into the bank in use: stored and loaded at once, each switch to its resting state.
    reset_world();
    d[0] = 1;
    eeprom_keeps = 1;
    sx_send(0x7F, SX_CMD_WRITE, d, SX_WRITE_LEN);
    eeprom_keeps = 0;
    CHECK(hal_bank == 0 && msg_type(4) == MT_NOTE && MSG_NUM(4) == 60 && pedal_mode_get(3) == PM_MOM_INV &&
          sw_mask[1] == (1 << 2) && bank_pc == 6 && pk_mask == 2 && pedal_cc[0] == 20 && cfg_dirty == ALL_SWITCHES);
    ticks(2);
    CHECK(cfg_dirty == 0 && leds[3] == 1);

    // Not whole, or no such bank: ignored.
    reset_world();
    saves = msg_saved;
    d[0] = 2;
    sx_send(0x7F, SX_CMD_WRITE, d, SX_WRITE_LEN - 1);
    d[0] = 0;
    sx_send(0x7F, SX_CMD_WRITE, d, SX_WRITE_LEN);
    d[0] = BANK_COUNT + 1;
    sx_send(0x7F, SX_CMD_WRITE, d, SX_WRITE_LEN);
    CHECK(msg_saved == saves && bank_saved == 0);
    reset_world();
}

static void test_read_bank(void)
{
    printf("SysEx: reading another bank without changing bank\n");
    reset_world();
    store_bank_msg(1, 3, MT_NOTE, 64, 90, 10, PM_MOMENTARY);
    store_relay(1, 0, 1u << 2, 33, 0);
    store_y(1, 0, 6);                       // bank 2's program change: program 5
    store_y(1, 1, (uint8_t)(0x80 | 20));    // pedal A: CC 20, pickup
    mset(NP_ON + 2, 5);                     // bank 1, live and not saved
    ticks(2);                               // (inverted momentary: the old off goes out)
    clear_log();
    dump_run_bank(2, 1000);
    CHECK(parse_dump(0) == DUMP_PARAMS && hal_bank == 0 && bank_next == 0 && !bank_busy && MSG_ON(2) == 5);
    CHECK(dump_bank_of == 2 && dump_values[NP_BANK] == 1);
    CHECK(dump_values[NP_TYPE + 3] == MT_NOTE && dump_values[NP_NUM + 3] == 64 && dump_values[NP_ON + 3] == 90 &&
          dump_values[NP_OFF + 3] == 10 && dump_values[NP_MODE + 3] == PM_MOMENTARY);
    CHECK(dump_values[NP_FOLLOW1 + 2] == 1 && dump_values[NP_FOLLOW1 + 3] == 0 && dump_values[NP_TRIGGER] == 33);
    CHECK(dump_values[NP_BANK_PC] == 5 && dump_values[NP_BANK_PC_ON] == 1);
    CHECK(dump_values[NP_PICKUP] == 1 && dump_values[NP_PEDAL_CC] == 20 && dump_values[NP_PICKUP + 1] == 0);
    CHECK(dump_values[NP_ON + 2] == 127);   // bank 2's switch 3: the default
    // A bank never saved: its defaults (the note banks' notes).
    clear_log();
    dump_run_bank(NOTE_BANK_1 ? NOTE_BANK_1 : 3, 1000);
    parse_dump(0);
    CHECK(dump_values[NP_TYPE] == (NOTE_BANK_1 ? MT_NOTE : MT_CC) && dump_values[NP_BANK_PC_ON] == 0 &&
          dump_values[NP_PEDAL_CC + 1] == 11);
    // The bank in use, by number or as 0: live.
    clear_log();
    dump_run_bank(1, 1000);
    parse_dump(0);
    CHECK(dump_values[NP_ON + 2] == 5 && dump_bank_of == 1);
    clear_log();
    dump_run_bank(0, 1000);
    parse_dump(0);
    CHECK(dump_values[NP_ON + 2] == 5 && dump_bank_of == 1);
    // Sets still go live.
    mset(NP_ON + 2, 6);
    CHECK(MSG_ON(2) == 6);
    reset_world();
}

// Reset to defaults (user's request): NRPN command 6, and power-on with footswitch 9
// held ("+INI"; UP resets, any other key cancels).
static void test_factory_reset(void)
{
    int n;

    printf("reset to defaults (NRPN, and footswitch 9 held at power-on)\n");
    reset_world();
    cal_stored_valid = 1;
    stored_heel[0] = 20; stored_toe[0] = 220; stored_heel[1] = 0; stored_toe[1] = 255;
    mset(NP_ON + 2, 5);
    mset(NP_LEVEL, 30);
    mset(NP_PEDAL_CC, 40);
    mset(NP_COMMAND, NC_DEFAULTS);
    CHECK(msg_saved == BANK_COUNT * STOMP_COUNT && bank_saved == BANK_COUNT && cfg_saved == 1 && cal_saved == 0);
    CHECK(saved_msg[2][2] == 127 && saved_cfg[0] == MIDI_CHANNEL && saved_cfg[2] == (LEVEL_VALID | LED_BRIGHTNESS));
#if NOTE_BANK_1 > 0
    CHECK((saved_msg[(NOTE_BANK_1 - 1) * BANK_RECS][0] & 3) == MT_NOTE);
#endif
    CHECK(MSG_ON(2) == 127 && led_level == LED_BRIGHTNESS && hw_level == LED_BRIGHTNESS && pedal_cc[0] == 7);
    CHECK(heel[0] == 20 && toe[0] == 220 && hal_bank == 0 && !rec_def);

    // Power-on with footswitch 9 held: "+INI" flashes; its release, holds and the pedals
    // do nothing; DOWN cancels.
    reset_world();
    power_on();
    ctl_settle_key(sw_code(9));
    CHECK(ini_prompt && !boot_anim && display == SHOWN_TEXT(HAL_TEXT_INI) && blank == 0);
    ticks(CHAN_FLASH_TICKS);
    CHECK(blank == 7);
    ticks(CHAN_FLASH_TICKS);
    CHECK(blank == 0 && display == SHOWN_TEXT(HAL_TEXT_INI));
    ctl_key_release(sw_code(9));
    ctl_key_hold(CTL_KEY_UP);
    pedal(0, 200);
    CHECK(ini_prompt && midi_len == 0);
    ctl_key_press(CTL_KEY_DOWN);
    ctl_key_release(CTL_KEY_DOWN);
    ticks(2);
    CHECK(!ini_prompt && msg_saved == 0 && cfg_saved == 0 && display == 1 && blank == 0 && hal_bank == 0 && mode == MODE_RUN);
    // UP resets.
    reset_world();
    power_on();
    ctl_settle_key(sw_code(9));
    ctl_key_press(CTL_KEY_UP);
    CHECK(!ini_prompt && msg_saved == BANK_COUNT * STOMP_COUNT && cfg_saved == 1 && display == 1 && blank == 0);
    ctl_key_release(CTL_KEY_UP);
    ticks(2);
    CHECK(hal_bank == 0 && bank_next == 0 && !pc_mode && mode == MODE_RUN);
    // Any other footswitch held at power-on: no prompt.
    reset_world();
    power_on();
    for (n = 1; n <= 10; n++)
    {
        if (n != 9) ctl_settle_key(sw_code(n));
    }
    CHECK(!ini_prompt && boot_anim);
    reset_world();
}

// Shared (global) settings (user's request): brightness, channel, clock display, running
// status and active sensing. Over NRPN (98-101, 0 the channel); and a long UP with only
// these changed saves and leaves without the bank prompt.
static void test_global_settings(void)
{
    int n;

    printf("config: global settings (NRPN, saving without the bank prompt)\n");
    reset_world();
    mset(NP_LEVEL, 30);
    CHECK(led_level == 30 && hw_level == 30);
    mset(NP_METRO, 1);
    mset(NP_RUNSTAT, 1);
    mset(NP_ASENSE, 1);
    CHECK(opt_metro && opt_runstat && opt_asense && midi_len == 0);
    mset(NP_ASENSE, 1);
    mset(NP_METRO, 2);
    CHECK(opt_asense && opt_metro);
    mset(NP_ASENSE, 0);
    CHECK(!opt_asense);
    // In a dump (running status off, so every message has its status byte here).
    mset(NP_RUNSTAT, 0);
    CHECK(!opt_runstat);
    clear_log();
    dump_run(1000);
    n = parse_dump(0);
    CHECK(n > 0 && dump_seen[NP_LEVEL] && dump_values[NP_LEVEL] == 30 && dump_values[NP_METRO] == 1 &&
          dump_values[NP_RUNSTAT] == 0 && dump_values[NP_ASENSE] == 0);
    // Saved by the save command, in the channel record.
    mset(NP_COMMAND, NC_SAVE);
    CHECK(saved_cfg[2] == (LEVEL_VALID | 30) && saved_cfg[3] == (MISC_VALID | 1));
    reset_world();

    // A long UP with only a shared setting changed (the channel) saves and leaves.
    chord_long();
    to_page(PAGE_CHANNEL);
    tap(sw_code(5));
    CHECK(midi_channel == 5);
    long_nav(CTL_KEY_UP);
    CHECK(!save_prompt && mode == MODE_CONFIG);
    ticks(1);
    CHECK(mode == MODE_RUN && cfg_saved == 1 && saved_cfg[0] == 5);
    // Nothing changed: the prompt (to copy the bank).
    reset_world();
    chord_long();
    long_nav(CTL_KEY_UP);
    CHECK(save_prompt);
    tap(CTL_KEY_DOWN);
    // A bank setting changed too: the prompt.
    to_page(PAGE_MISC);
    tap(sw_code(MISC_METRO + 1));
    to_page(PAGE_BANK_PC);
    tap(sw_code(3));
    long_nav(CTL_KEY_UP);
    CHECK(save_prompt && mode == MODE_CONFIG);
    tap(CTL_KEY_DOWN);
    cancel_exit();
    reset_world();
}

// LED brightness (user's request): pedal B on page 9 sets it, live there and in run
// mode; the other config pages are at full brightness. Saved in the channel record's
// byte 2 (bit 7 set; without it, LED_BRIGHTNESS).
static void test_led_brightness(void)
{
    printf("LEDs: brightness\n");
    reset_world();
    CHECK(led_level == LED_BRIGHTNESS && hw_level == LED_BRIGHTNESS);
    chord_long();
    CHECK(hw_level == 127);
    to_page(PAGE_VERSION);
    CHECK(hw_level == LED_BRIGHTNESS);
    // The first reading is only a reference; a move sets it.
    pedal(1, 128);
    CHECK(led_level == LED_BRIGHTNESS);
    pedal(1, 0);
    CHECK(led_level == 0 && hw_level == 0);
    pedal(1, 100);
    CHECK(led_level == ctl_scale(100, 0, 255) && hw_level == led_level);
    // Pedal A does nothing here.
    pedal(0, 0);
    pedal(0, 255);
    CHECK(led_level == ctl_scale(100, 0, 255) && midi_len == 0);
    pedal(1, 0);
    ticks(1);
    CHECK(cfg_changed == (LED_BRIGHTNESS != 0));
    // Other pages: full brightness; back on page 9, the setting again.
    tap(CTL_KEY_DOWN);
    CHECK(hw_level == 127 && led_level == 0);
    tap(CTL_KEY_UP);
    CHECK(hw_level == 0);
    // Saved, and used in run mode.
    save_exit();
    CHECK(mode == MODE_RUN && hw_level == 0 && saved_cfg[2] == LEVEL_VALID);
    // Loaded at power-on.
    reset_world();
    cfg_stored_valid = 1;
    stored_cfg[0] = MIDI_CHANNEL;
    stored_cfg[2] = LEVEL_VALID | 10;
    power_on();
    CHECK(led_level == 10 && hw_level == 10);
    // A record without bit 7 (older firmware saved 0): the default.
    stored_cfg[2] = 10;
    power_on();
    CHECK(led_level == LED_BRIGHTNESS && hw_level == LED_BRIGHTNESS);
    // A cancel puts it back.
    stored_cfg[2] = LEVEL_VALID | 50;
    power_on();
    chord_long();
    to_page(PAGE_VERSION);
    pedal(1, 255);
    pedal(1, 0);
    CHECK(led_level == 0);
    cancel_exit();
    CHECK(mode == MODE_RUN && led_level == 50 && hw_level == 50);
    reset_world();
}

// Active sensing (user's request): with it on, FE every ACTIVE_SENSE_MS, in run and
// config mode, waiting for room in MIDI OUT; it leaves running status alone.
static void test_active_sensing(void)
{
    int k, ok = 1;

    printf("MIDI OUT: active sensing\n");
    reset_world();
    ticks(AS_TICKS * 3);
    CHECK(midi_len == 0);
    set_opt(OPT_ASENSE, 1);
    as_ticks = 0;

    for (k = 1; k <= AS_TICKS * 3; k++)
    {
        ticks(1);
        ok = ok && midi_len == k / AS_TICKS && (midi_len == 0 || midi_log[midi_len - 1] == 0xFE);
    }

    CHECK(ok && AS_TICKS * TICK_MS == ACTIVE_SENSE_MS);
    // A full MIDI OUT: it waits, then goes.
    clear_log();
    midi_space = 0;
    ticks(AS_TICKS + 5);
    CHECK(midi_len == 0);
    midi_space = 15;
    ticks(1);
    CHECK(midi_len == 1 && midi_log[0] == 0xFE);
    // Running status is untouched: a CC after it can still leave out its status.
    set_opt(OPT_RUNSTAT, 1);
    set_mode(0, PM_STOMP);
    tap(0);
    as_ticks = 0;
    ticks(AS_TICKS);
    tap(0);
    CHECK(midi_len == 7 && midi_log[1] == CC_STATUS && midi_log[4] == 0xFE && midi_log[5] == stomp_cc[0]);
    // In config mode too.
    chord_long();
    clear_log();
    ticks(AS_TICKS);
    CHECK(mode == MODE_CONFIG && midi_len == 1 && midi_log[0] == 0xFE);
    cancel_exit();
    reset_world();
}

// The visual metronome (user's request): with it on, MIDI Start or Continue starts it;
// each beat (24 clocks) lights the "-" for 12 clocks and leaves it dark for 12; once the
// beat's phase is known (Start, or a Song Position Pointer) beat 1 shows "+" instead (4/4
// assumed). Stop ends it. Only in run mode.
static void test_metronome(void)
{
    int k, ok;

    printf("MIDI IN: visual metronome\n");
    reset_world();
    set_opt(OPT_METRO, 1);
    // Clocks alone do nothing.
    for (k = 0; k < 48; k++)
    {
        ctl_midi_in(0xF8);
    }

    CHECK(disp_sign == HAL_SIGN_NONE && sign_changes == 0);
    // Start: beat 1 is the next clock, "+" for 12 clocks, then dark for 12.
    ctl_midi_in(0xFA);
    CHECK(disp_sign == HAL_SIGN_NONE);

    ok = 1;

    for (k = 0; k < 12; k++)
    {
        ctl_midi_in(0xF8);
        ok = ok && disp_sign == HAL_SIGN_PLUS;
    }

    CHECK(ok);

    ctl_midi_in(0xF8);
    CHECK(disp_sign == HAL_SIGN_NONE);

    for (k = 0; k < 11; k++)
    {
        ctl_midi_in(0xF8);
    }

    // Beats 2-4: "-"; then beat 1 again.
    for (k = 0; k < 3; k++)
    {
        ctl_midi_in(0xF8);
        CHECK(disp_sign == HAL_SIGN_MINUS);
        midi_clocks(23);
        CHECK(disp_sign == HAL_SIGN_NONE);
    }

    ctl_midi_in(0xF8);
    CHECK(disp_sign == HAL_SIGN_PLUS && sign_changes == 9);
    // The display under it is untouched, nothing is sent, and a CC still works in
    // between (real-time bytes inside a message).
    set_mode(0, PM_STOMP);
    clear_log();
    ctl_midi_in(CC_STATUS); ctl_midi_in(0xF8); ctl_midi_in(stomp_cc[0]); ctl_midi_in(0xF8); ctl_midi_in(127);
    CHECK(leds[0] == 1 && midi_len == 0 && display == 1);
    // Stop: dark at once, clocks do nothing; Continue carries on from there.
    ctl_midi_in(0xFC);
    CHECK(disp_sign == HAL_SIGN_NONE);
    midi_clocks(30);
    CHECK(disp_sign == HAL_SIGN_NONE);
    ctl_midi_in(0xFB);
    ctl_midi_in(0xF8);
    CHECK(disp_sign == HAL_SIGN_PLUS);
    // A Song Position Pointer: 16th 6 is 2 16ths (12 clocks) into beat 2, so 12 clocks
    // later beat 3 ("-"), then beat 4, then beat 1 ("+").
    ctl_midi_in(0xFC);
    ctl_midi_in(0xF2); ctl_midi_in(6); ctl_midi_in(0);
    ctl_midi_in(0xFB);
    midi_clocks(12);
    CHECK(disp_sign == HAL_SIGN_NONE);
    ctl_midi_in(0xF8);
    CHECK(disp_sign == HAL_SIGN_MINUS);
    midi_clocks(47);
    ctl_midi_in(0xF8);
    CHECK(disp_sign == HAL_SIGN_PLUS);
    // The pointer's data bytes are not taken as a CC.
    CHECK(leds[0] == 1);

    // Config mode hides it; back in run mode the next clock shows it again.
    chord_long();
    CHECK(mode == MODE_CONFIG && disp_sign == HAL_SIGN_NONE);
    midi_clocks(24);
    CHECK(disp_sign == HAL_SIGN_NONE);
    cancel_exit();
    set_opt(OPT_METRO, 1);
    ctl_midi_in(0xF8);
    CHECK(disp_sign != HAL_SIGN_NONE);

    // Off: nothing shows. Continue without a known phase (after power-on): only "-".
    set_opt(OPT_METRO, 0);
    ctl_midi_in(0xF8);
    CHECK(disp_sign == HAL_SIGN_NONE);
    reset_world();
    set_opt(OPT_METRO, 1);
    ctl_midi_in(0xFB);
    ok = 1;

    for (k = 0; k < 96; k++)
    {
        ctl_midi_in(0xF8);
        ok = ok && disp_sign != HAL_SIGN_PLUS;
    }

    CHECK(ok);
    CHECK(sign_changes == 8);
    reset_world();
}

// Running status (user's request): with it on, a channel message with the same status
// byte as the last one sent leaves it out; a SysEx (the identity) sends it again.
static void test_running_status(void)
{
    printf("MIDI OUT: running status\n");
    reset_world();
    set_mode(0, PM_STOMP);
    set_mode(1, PM_STOMP);
    // Off: every message has its status.
    tap(0);
    tap(1);
    CHECK(midi_len == 6 && midi_log[0] == CC_STATUS && midi_log[3] == CC_STATUS);
    // On: neither CC has a status byte (the last one sent was the same).
    set_opt(OPT_RUNSTAT, 1);
    clear_log();
    tap(0);
    tap(1);
    CHECK(midi_len == 4 && midi_log[0] == stomp_cc[0] && midi_log[2] == stomp_cc[1]);
    // Turned on from the misc page, the first message sends it once.
    set_opt(OPT_RUNSTAT, 0);
    chord_long();
    to_page(PAGE_MISC);
    tap(sw_code(MISC_RUNSTAT + 1));
    CHECK(opt_runstat == !DEFAULT_RUNNING_STATUS);
    save_exit();
    set_opt(OPT_RUNSTAT, 1);
    clear_log();
    tap(0);
    tap(1);
    CHECK(midi_len == 5 && midi_log[0] == CC_STATUS && midi_log[1] == stomp_cc[0] && midi_log[3] == stomp_cc[1]);
    // A program change has its own status, a second one leaves it out, and the CC after
    // them sends 0xB0 again.
    clear_log();
    program_mode();
    tap(CTL_KEY_UP);
    tap(sw_code(2));
    chord_short();
    tap(0);
    CHECK(midi_len == 6 && midi_log[0] == (0xC0 | (MIDI_CHANNEL - 1)) && midi_log[2] < 0x80 && midi_log[3] == CC_STATUS);
    // After the identity reply (SysEx), the status again.
    clear_log();
    sx_reply = 1;
    ticks(1);
    CHECK(midi_len == SX_REPLY_LEN);
    tap(0);
    CHECK(midi_len == SX_REPLY_LEN + 3 && midi_log[SX_REPLY_LEN] == CC_STATUS);
    // A dump is SysEx: it ends running status, so the next CC sends its status again.
    tap(0);
    clear_log();
    dump_run(1000);
    tap(0);
    CHECK(parse_dump(0) == DUMP_PARAMS && midi_log[SX_DUMP_LEN] == CC_STATUS);
    reset_world();
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
    test_bank_pc();
    test_note_banks();
    test_midi_out_flash();
    test_save_bank();
    test_entry_dot();
    test_sysex_identity();
    test_identity_at_power_on();
    test_misc_page();
    test_metronome();
    test_running_status();
    test_active_sensing();
    test_config_indicators();
    test_led_brightness();
    test_global_settings();
    test_read_bank();
    test_bank_write();
    test_pedal_range();
    test_factory_reset();
    test_pc_mode();
    test_shared_state();
    test_cc_memory();
    test_scaling();
    test_pedal_cc();
    test_pedal_noise();
    test_pedal_pickup();
    test_pedal_cc_config();
    test_bank_relays();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
