#include "platform.h"
#include "config.h"
#include "hal.h"
#include "ctl.h"

// Controller behaviour:
//   footswitches 1..10  -> stomp switches, each with a mode chosen in configuration mode:
//                           stomp, stomp with momentary on a long press, or momentary
//   expression pedal A/B -> continuous CC 0..127
//   UP / DOWN            -> MIDI program change +1 / -1, sent when the key is released;
//                           held, the number auto-repeats on the display and is sent on release
//   UP + DOWN held 1s   -> configuration mode (MIDI channel, pedal modes, calibration)

#define NO_VALUE        0xFF
#define DISPLAY_ERROR   88

// Overall mode.
#define MODE_RUN        0
#define MODE_CONFIG     1

// Pedal modes (stored as two bits per footswitch).
#define PM_STOMP        0
#define PM_STOMP_MOM    1
#define PM_MOMENTARY    2
#define PM_COUNT        3

// Configuration pages, in the order a short UP + DOWN press steps through them.
#define PAGE_CHANNEL    0
#define PAGE_MODES      1
#define PAGE_CAL        2
#define PAGE_COUNT      3

// UP + DOWN chord: Not pressed together, pressed together and still undecided, or already
// acted on and waiting for both keys to be let go.
#define CHORD_NONE      0
#define CHORD_ACTIVE    1
#define CHORD_DONE      2

// Calibration steps on the calibration page: 0 = done, 1 = set heel, 2 = set toe.
#define CAL_DONE        0
#define CAL_HEEL        1
#define CAL_TOE         2

#define NAV_NONE        0xFF

#define KEYBIT_UP       0x01
#define KEYBIT_DOWN     0x02

static const CODE uint8_t stomp_cc[STOMP_COUNT] = { STOMP_CC_LIST };
static const CODE uint8_t pedal_cc[2]           = { PEDAL_CC_LIST };

// Scan ticks for the timed gestures.
#define REPEAT_DELAY_TICKS   (PROGRAM_REPEAT_DELAY_MS / TICK_MS)
#define REPEAT_PERIOD_TICKS  (1000 / PROGRAM_REPEAT_HZ / TICK_MS)
#define CHORD_TICKS          (CONFIG_HOLD_MS / TICK_MS)

#if REPEAT_DELAY_TICKS < 1 || REPEAT_PERIOD_TICKS < 1 || CHORD_TICKS < 1
#error The repeat and configuration times must each be at least one tick
#endif

static IDATA uint8_t  mode;

// MIDI IN parser state: Current (running) status byte and a pending first data byte.
static IDATA uint8_t  rx_status;
static IDATA uint8_t  rx_data1;
static BIT_T    rx_have_data1;
static IDATA uint8_t  program;
static IDATA uint16_t stomp_state;

// Settings that can be changed in configuration mode and are saved in the EEPROM.
static IDATA uint8_t  midi_channel;
static IDATA uint8_t  pedal_modes[3];

// Scratch record for loading and saving the settings (a shared buffer saves RAM).
static IDATA uint8_t  cfg_buf[4];

// Per stomp (one bit each): down = a press is in progress, held = it will be undone on
// release (a long press, or a momentary pedal), revert = an undo is waiting for MIDI room,
// prev = the state before the press.
static IDATA uint16_t stomp_down;
static IDATA uint16_t stomp_held;
static IDATA uint16_t stomp_revert;
static IDATA uint16_t stomp_prev;

// UP / DOWN: The key being tracked for program change (NAV_NONE if none), scan ticks since
// it was pressed or since the last repeat step, whether auto-repeat has started, and
// whether the final program change is still waiting for room in the MIDI OUT queue.
static IDATA uint8_t  nav_code;
static IDATA uint16_t nav_ticks;
static BIT_T          nav_repeating;
static BIT_T          program_pending;

// Which of UP / DOWN are down, and the state of an UP + DOWN chord.
static IDATA uint8_t  keys_down;
static IDATA uint8_t  chord_state;

// Configuration mode state: Open page, footswitch being edited on the modes page, calibration
// step, and which pedals had their mode changed.
static IDATA uint8_t  cfg_page;
static IDATA uint8_t  cfg_sel;
static IDATA uint8_t  cal_step;
static IDATA uint16_t cfg_dirty;

// Calibrated pedal end points, indexed by pedal channel.
static IDATA uint8_t  heel[2];
static IDATA uint8_t  toe[2];

// Most recent averaged readings, last value sent, and the reading it was sent at.
static IDATA uint8_t  last_raw[2];
static IDATA uint8_t  sent[2];
static IDATA uint8_t  ref_raw[2];

static void send_cc(uint8_t cc, uint8_t value)
{
    hal_midi_tx((uint8_t)(0xB0 | (midi_channel - 1)));
    hal_midi_tx((uint8_t)(cc & 0x7F));
    hal_midi_tx((uint8_t)(value & 0x7F));
}

static void send_program(uint8_t value)
{
    hal_midi_tx((uint8_t)(0xC0 | (midi_channel - 1)));
    hal_midi_tx((uint8_t)(value & 0x7F));
}

static void show_program(void)
{
    hal_display((uint8_t)(program + DISPLAY_PROGRAM_OFFSET));
}

// Mode of one footswitch, and set it.
static uint8_t pedal_mode_get(uint8_t index)
{
    return (uint8_t)((pedal_modes[index >> 2] >> ((index & 3) * 2)) & 3);
}

static void pedal_mode_set(uint8_t index, uint8_t value)
{
    uint8_t shift = (uint8_t)((index & 3) * 2);

    pedal_modes[index >> 2] = (uint8_t)((pedal_modes[index >> 2] & ~(3 << shift)) | (value << shift));
}

// Forget the last sent pedal positions so the next reading is adopted
// silently instead of being sent as a controller change.
static void resync_pedals(void)
{
    sent[0] = NO_VALUE;
    sent[1] = NO_VALUE;
}

// ---------------------------------------------------------------
// Program change
// ---------------------------------------------------------------

// Move the program number one step for UP or DOWN and show it. Nothing is sent.
static void program_step(uint8_t code)
{
    if (code == CTL_KEY_UP)
    {
        if (program >= 127)
        {
#if PROGRAM_WRAP
            program = 0;
#endif
        }
        else
        {
            program++;
        }
    }
    else
    {
        if (program == 0)
        {
#if PROGRAM_WRAP
            program = 127;
#endif
        }
        else
        {
            program--;
        }
    }

    show_program();
}

// Send the program number now, or as soon as there is room in the MIDI OUT queue.
static void commit_program(void)
{
    if (hal_midi_space() >= 2)
    {
        program_pending = 0;
        send_program(program);
    }
    else
    {
        program_pending = 1;
    }
}

// Send a program change that had to wait for room. Never while a key is being tracked,
// so nothing goes out during auto-repeat; the release sends the final number.
static void flush_program(void)
{
    if (program_pending && nav_code == NAV_NONE && hal_midi_space() >= 2)
    {
        program_pending = 0;
        send_program(program);
    }
}

// ---------------------------------------------------------------
// Configuration mode
// ---------------------------------------------------------------

// Redraw the display and panel LEDs for the open page.
static void config_show(void)
{
    uint8_t i;

    hal_aux_led(HAL_LED_MIDI_CHAN, cfg_page == PAGE_CHANNEL);
    hal_aux_led(HAL_LED_MIDI_FUNC, cfg_page == PAGE_MODES);
    hal_aux_led(HAL_LED_EXP_1,     cfg_page == PAGE_CAL);
    hal_aux_led(HAL_LED_EXP_2,     cfg_page == PAGE_CAL);

    for (i = 0; i < STOMP_COUNT; i++)
    {
        hal_stomp_led(i, (cfg_page == PAGE_MODES && i == cfg_sel) ? 1 : 0);
    }

    if (cfg_page == PAGE_CHANNEL)
    {
        hal_display(midi_channel);
    }
    else if (cfg_page == PAGE_MODES)
    {
        // Footswitch number, then its mode as the last digit: 1 stomp, 2 stomp with
        // momentary, 3 momentary. For example 32 is footswitch 3 in mode 2.
        hal_display((uint8_t)((cfg_sel + 1) * 10 + pedal_mode_get(cfg_sel) + 1));
    }
    else
    {
        hal_display(cal_step);
    }
}

static void config_goto_page(uint8_t page)
{
    cfg_page = page;

    if (page == PAGE_CAL)
    {
        cal_step = CAL_HEEL;
    }

    config_show();
}

static int16_t span_of(uint8_t a, uint8_t b)
{
    int16_t s = (int16_t)a - (int16_t)b;

    return (s < 0) ? -s : s;
}

// UP on the calibration page: Take the heel reading, then the toe reading and apply them.
static void calibration_next(void)
{
    if (cal_step == CAL_HEEL)
    {
        ref_raw[0] = last_raw[0];
        ref_raw[1] = last_raw[1];
        cal_step = CAL_TOE;
        hal_display(CAL_TOE);
    }
    else if (cal_step == CAL_TOE)
    {
        if (span_of(last_raw[0], ref_raw[0]) < CAL_MIN_SPAN ||
            span_of(last_raw[1], ref_raw[1]) < CAL_MIN_SPAN)
        {
            // A pedal barely moved: Show 88 and start again at the heel step.
            cal_step = CAL_HEEL;
            hal_display(DISPLAY_ERROR);
            return;
        }

        heel[0] = ref_raw[0];
        heel[1] = ref_raw[1];
        toe[0]  = last_raw[0];
        toe[1]  = last_raw[1];
        cal_step = CAL_DONE;
        hal_display(CAL_DONE);
    }
    else
    {
        cal_step = CAL_HEEL;
        hal_display(CAL_HEEL);
    }
}

// UP or DOWN on the open page changes its value by one step.
static void config_step(uint8_t code)
{
    uint8_t m;

    if (cfg_page == PAGE_CHANNEL)
    {
        if (code == CTL_KEY_UP)
        {
            midi_channel = (midi_channel >= 16) ? 1 : (uint8_t)(midi_channel + 1);
        }
        else
        {
            midi_channel = (midi_channel <= 1) ? 16 : (uint8_t)(midi_channel - 1);
        }
    }
    else if (cfg_page == PAGE_MODES)
    {
        m = pedal_mode_get(cfg_sel);
        m = (code == CTL_KEY_UP) ? (uint8_t)((m + 1) % PM_COUNT) : (uint8_t)((m + PM_COUNT - 1) % PM_COUNT);

        pedal_mode_set(cfg_sel, m);
        cfg_dirty |= (uint16_t)((uint16_t)1 << cfg_sel);
    }
    else
    {
        if (code == CTL_KEY_UP)
        {
            calibration_next();
        }
        else
        {
            // DOWN starts the calibration again from the heel step.
            cal_step = CAL_HEEL;
        }

        if (cal_step != CAL_TOE && !(cal_step == CAL_HEEL && code == CTL_KEY_UP))
        {
            config_show();
        }

        return;
    }

    config_show();
}

static void enter_config(void)
{
    mode = MODE_CONFIG;

    // A stomp being held (momentary, or past the long-press time) is treated as released so
    // the synth is not left with it on.
    stomp_revert |= (uint16_t)(stomp_down & stomp_held);
    stomp_down = 0;
    stomp_held = 0;

    cfg_dirty = 0;
    cfg_sel   = 0;

    hal_status_led(1);
    config_goto_page(PAGE_CHANNEL);
}

// Take settings from four saved bytes, keeping the defaults for anything out of range.
static void settings_unpack(const uint8_t *data)
{
    uint8_t i;
    uint8_t m;

    if (data[0] >= 1 && data[0] <= 16)
    {
        midi_channel = data[0];
    }

    for (i = 0; i < STOMP_COUNT; i++)
    {
        m = (uint8_t)((data[1 + (i >> 2)] >> ((i & 3) * 2)) & 3);

        if (m < PM_COUNT)
        {
            pedal_mode_set(i, m);
        }
    }
}

static void exit_config(void)
{
    uint8_t  i;
    uint16_t bit;

    cfg_buf[0] = midi_channel;
    cfg_buf[1] = pedal_modes[0];
    cfg_buf[2] = pedal_modes[1];
    cfg_buf[3] = pedal_modes[2];

    // If a write fails the new settings still apply until power-off.
    hal_cfg_save(cfg_buf);
    hal_cal_save(heel, toe);

    // A stomp whose mode was changed while on is turned off, so it does not carry a stale
    // state into its new mode. The synth is told once there is room.
    for (i = 0; i < STOMP_COUNT; i++)
    {
        bit = (uint16_t)((uint16_t)1 << i);

        if ((cfg_dirty & bit) && (stomp_state & bit))
        {
            stomp_prev &= (uint16_t)~bit;
            stomp_revert |= bit;
        }
    }

    mode = MODE_RUN;
    hal_status_led(0);
    hal_aux_led(HAL_LED_MIDI_CHAN, 0);
    hal_aux_led(HAL_LED_MIDI_FUNC, 0);
    hal_aux_led(HAL_LED_EXP_1, 0);
    hal_aux_led(HAL_LED_EXP_2, 0);

    for (i = 0; i < STOMP_COUNT; i++)
    {
        hal_stomp_led(i, (stomp_state >> i) & 1);
    }

    resync_pedals();
    show_program();
}

// ---------------------------------------------------------------
// UP / DOWN keys: Program change, auto-repeat, and the UP + DOWN chord
// ---------------------------------------------------------------

// One step for the tracked key: The program in run mode, the open page's value in config mode.
static void nav_step(uint8_t code)
{
    if (mode == MODE_RUN)
    {
        program_step(code);
    }
    else
    {
        config_step(code);
    }
}

// UP or DOWN released on its own: A tap makes one step, a hold has already stepped by repeating.
// In run mode the program number is then sent, once.
static void nav_release(uint8_t code)
{
    if (code != nav_code)
    {
        return;
    }

    if (!nav_repeating)
    {
        nav_step(code);
    }

    nav_code      = NAV_NONE;
    nav_repeating = 0;

    if (mode == MODE_RUN)
    {
        commit_program();
    }
}

// Stop tracking the held key because it has become part of a chord. If it had already been
// repeating, the program on the display is sent so the synth agrees with it.
static void nav_cancel(void)
{
    if (nav_code != NAV_NONE && nav_repeating && mode == MODE_RUN)
    {
        commit_program();
    }

    nav_code      = NAV_NONE;
    nav_repeating = 0;
}

static void nav_key_press(uint8_t code)
{
    keys_down |= (code == CTL_KEY_UP) ? KEYBIT_UP : KEYBIT_DOWN;

    if (keys_down == (KEYBIT_UP | KEYBIT_DOWN))
    {
        if (chord_state == CHORD_NONE)
        {
            chord_state = CHORD_ACTIVE;
            nav_ticks = 0;
            nav_cancel();
        }

        return;
    }

    if (chord_state != CHORD_NONE)
    {
        return;
    }

    if (nav_code == NAV_NONE)
    {
        nav_code      = code;
        nav_ticks     = 0;
        nav_repeating = 0;
    }
}

static void nav_key_release(uint8_t code)
{
    keys_down &= (uint8_t)~((code == CTL_KEY_UP) ? KEYBIT_UP : KEYBIT_DOWN);

    if (chord_state != CHORD_NONE)
    {
        if (chord_state == CHORD_ACTIVE)
        {
            // Let go before the hold time: in configuration mode that means the next page.
            chord_state = CHORD_DONE;

            if (mode == MODE_CONFIG)
            {
                config_goto_page((uint8_t)((cfg_page + 1) % PAGE_COUNT));
            }
        }

        if (keys_down == 0)
        {
            chord_state = CHORD_NONE;
        }

        return;
    }

    nav_release(code);
}

// ---------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------

uint8_t ctl_scale(uint8_t raw, uint8_t h, uint8_t t)
{
    int16_t span = (int16_t)t - (int16_t)h;
    int16_t d    = (int16_t)raw - (int16_t)h;
    int16_t v;

    // A pedal wired the other way round just has toe below heel.
    if (span < 0)
    {
        span = -span;
        d = -d;
    }

    if (span == 0)
    {
        return 0;
    }

    if (d <= PEDAL_END_SNAP)
    {
        return 0;
    }

    if (d >= span - PEDAL_END_SNAP)
    {
        return 127;
    }

    // d < span <= 255 so d * 127 stays below 32767 even with 16-bit ints.
    v = (int16_t)((d * 127 + span / 2) / span);

    if (v < 0)
    {
        v = 0;
    }

    if (v > 127)
    {
        v = 127;
    }

    return (uint8_t)v;
}

void ctl_init(void)
{
    uint8_t i;

    // Defaults used for anything without a valid saved value.
    heel[0] = 0;
    heel[1] = 0;
    toe[0]  = 255;
    toe[1]  = 255;

    midi_channel = MIDI_CHANNEL;
    pedal_modes[0] = 0;
    pedal_modes[1] = 0;
    pedal_modes[2] = 0;

    for (i = 0; i < STOMP_COUNT; i++)
    {
        pedal_mode_set(i, DEFAULT_PEDAL_MODE);
    }

    hal_cal_load(heel, toe);

    if (hal_cfg_load(cfg_buf))
    {
        settings_unpack(cfg_buf);
    }

    mode          = MODE_RUN;
    rx_status     = 0;
    rx_have_data1 = 0;
    program       = 0;
    stomp_state   = 0;
    stomp_down    = 0;
    stomp_held    = 0;
    stomp_revert  = 0;
    stomp_prev    = 0;
    nav_code      = NAV_NONE;
    nav_ticks     = 0;
    nav_repeating = 0;
    program_pending = 0;
    keys_down     = 0;
    chord_state   = CHORD_NONE;
    nav_ticks   = 0;
    cfg_page      = PAGE_CHANNEL;
    cfg_sel       = 0;
    cal_step      = CAL_DONE;
    cfg_dirty     = 0;
    last_raw[0]   = 0;
    last_raw[1]   = 0;
    resync_pedals();

    for (i = 0; i < STOMP_COUNT; i++)
    {
        hal_stomp_led(i, 0);
    }

    hal_status_led(0);
    hal_aux_led(HAL_LED_MIDI_CHAN, 0);
    hal_aux_led(HAL_LED_MIDI_FUNC, 0);
    hal_aux_led(HAL_LED_EXP_1, 0);
    hal_aux_led(HAL_LED_EXP_2, 0);
    show_program();
}

// Stomp number 0..9 for a key code, or 0xFF for UP / DOWN / invalid codes.
static uint8_t stomp_index(uint8_t code)
{
    if (code == CTL_KEY_DOWN || code > CTL_KEY_UP)
    {
        return 0xFF;
    }

    // Codes 0..4 are switches 1..5 and 6..10 are switches 6..10.
    return (code < CTL_KEY_DOWN) ? code : (uint8_t)(code - 1);
}

// Put one stomp back to its pre-press state and tell the synth.
static void apply_revert(uint8_t index)
{
    uint16_t bit = (uint16_t)((uint16_t)1 << index);
    uint8_t  on  = (stomp_prev & bit) ? 1 : 0;

    if (on)
    {
        stomp_state |= bit;
    }
    else
    {
        stomp_state &= (uint16_t)~bit;
    }

    send_cc(stomp_cc[index], on ? 127 : 0);

    // In configuration mode the footswitch LEDs belong to the settings pages.
    if (mode == MODE_RUN)
    {
        hal_stomp_led(index, on);
    }
}

// Finish any revert that was held back because MIDI OUT was full at release time.
static void flush_reverts(void)
{
    uint8_t i;

    if (stomp_revert == 0)
    {
        return;
    }

    for (i = 0; i < STOMP_COUNT; i++)
    {
        if ((stomp_revert >> i) & 1)
        {
            if (hal_midi_space() < 3)
            {
                return;
            }

            stomp_revert &= (uint16_t)~((uint16_t)1 << i);
            apply_revert(i);
        }
    }
}

void ctl_key_hold(uint8_t code)
{
    uint8_t index = stomp_index(code);

    if (mode != MODE_RUN || index == 0xFF)
    {
        return;
    }

    // Only a press that was actually acted on, in the stomp-with-momentary mode, can
    // become a momentary hold.
    if (((stomp_down >> index) & 1) && pedal_mode_get(index) == PM_STOMP_MOM)
    {
        stomp_held |= (uint16_t)((uint16_t)1 << index);
    }
}

void ctl_key_release(uint8_t code)
{
    uint8_t  index;
    uint16_t bit;

    if (code == CTL_KEY_UP || code == CTL_KEY_DOWN)
    {
        nav_key_release(code);
        return;
    }

    index = stomp_index(code);

    if (index == 0xFF)
    {
        return;
    }

    bit = (uint16_t)((uint16_t)1 << index);

    if ((stomp_down & bit) == 0)
    {
        return;
    }

    stomp_down &= (uint16_t)~bit;

    if ((stomp_held & bit) == 0 || mode != MODE_RUN)
    {
        // Short press: The toggle stays latched.
        stomp_held &= (uint16_t)~bit;
        return;
    }

    stomp_held &= (uint16_t)~bit;

    // Long press or momentary pedal: Undo the press, now or as soon as there is room.
    if (hal_midi_space() < 3)
    {
        stomp_revert |= bit;
    }
    else
    {
        apply_revert(index);
    }
}

void ctl_key_press(uint8_t code)
{
    uint8_t  index;
    uint8_t  on;
    uint16_t bit;

    flush_reverts();

    if (code > CTL_KEY_UP)
    {
        return;
    }

    // UP / DOWN: Nothing happens on press. The key is tracked so that its release (or the
    // auto-repeat timer) can act, and so that pressing both can be told from pressing one.
    if (code == CTL_KEY_UP || code == CTL_KEY_DOWN)
    {
        nav_key_press(code);
        return;
    }

    if (mode != MODE_RUN)
    {
        // On the modes page a footswitch chooses which one is being edited.
        if (cfg_page == PAGE_MODES)
        {
            cfg_sel = stomp_index(code);
            config_show();
        }

        return;
    }

    // Stomps: If MIDI OUT is backed up, ignore the press entirely so the LEDs and
    // toggle states never drift from what the synth received.
    if (hal_midi_space() >= 3)
    {
        index = stomp_index(code);
        bit   = (uint16_t)((uint16_t)1 << index);

        // Remember what the state was, in case this press is undone on release.
        if (stomp_state & bit)
        {
            stomp_prev |= bit;
        }
        else
        {
            stomp_prev &= (uint16_t)~bit;
        }

        stomp_down |= bit;
        stomp_held &= (uint16_t)~bit;

        if (pedal_mode_get(index) == PM_MOMENTARY)
        {
            // Momentary: On while pressed whatever it was before, off again on release.
            stomp_state |= bit;
            stomp_prev  &= (uint16_t)~bit;
            stomp_held  |= bit;
            on = 1;
        }
        else
        {
            stomp_state ^= bit;
            on = (stomp_state & bit) ? 1 : 0;
        }

        send_cc(stomp_cc[index], on ? 127 : 0);
        hal_stomp_led(index, on);
    }
}

// Apply a remote controller value to every stomp switch assigned to that CC.
static void remote_cc(uint8_t cc, uint8_t value)
{
    uint8_t  i;
    uint8_t  on = (value >= 64) ? 1 : 0;
    uint16_t bit;

    // In configuration mode the LEDs show the settings, so remote changes wait.
    if (mode != MODE_RUN)
    {
        return;
    }

    for (i = 0; i < STOMP_COUNT; i++)
    {
        if (stomp_cc[i] == cc)
        {
            bit = (uint16_t)((uint16_t)1 << i);

            if (on)
            {
                stomp_state |= bit;
            }
            else
            {
                stomp_state &= (uint16_t)~bit;
            }

            hal_stomp_led(i, on);
        }
    }
}

void ctl_midi_in(uint8_t byte)
{
    // Real-time bytes (clock, start, stop...) can appear anywhere: skip them
    // without disturbing a message in progress.
    if (byte >= 0xF8)
    {
        return;
    }

    if (byte >= 0x80)
    {
        // System messages (including SysEx start and end) cancel running status.
        rx_status     = (byte >= 0xF0) ? 0 : byte;
        rx_have_data1 = 0;
        return;
    }

    // Only control changes on our channel are of interest; everything else,
    // including SysEx data and other channels, is ignored.
    if (rx_status != (uint8_t)(0xB0 | (midi_channel - 1)))
    {
        return;
    }

    if (!rx_have_data1)
    {
        rx_data1      = byte;
        rx_have_data1 = 1;
        return;
    }

    // Running status: The next pair of data bytes is another control change.
    rx_have_data1 = 0;
    remote_cc(rx_data1, byte);
}

void ctl_pedal_sample(uint8_t channel, uint8_t raw)
{
    uint8_t v;
    int16_t diff;

    last_raw[channel] = raw;

    flush_reverts();

    if (mode != MODE_RUN)
    {
        return;
    }

    v = ctl_scale(raw, heel[channel], toe[channel]);

    // The first reading after power-up only records the position.
    if (sent[channel] == NO_VALUE)
    {
        sent[channel]    = v;
        ref_raw[channel] = raw;
        return;
    }

    if (v == sent[channel])
    {
        return;
    }

    diff = (int16_t)raw - (int16_t)ref_raw[channel];

    if (diff < 0)
    {
        diff = -diff;
    }

    // Ignore small wobble, but always let the pedal reach 0 and 127.
    if (diff < PEDAL_HYST && v != 0 && v != 127)
    {
        return;
    }

    // Not enough room now: leave state alone so it is retried next reading.
    if (hal_midi_space() < 3)
    {
        return;
    }

    send_cc(pedal_cc[channel], v);
    sent[channel]    = v;
    ref_raw[channel] = raw;
}

void ctl_tick(void)
{
    flush_reverts();
    flush_program();

    // UP and DOWN held together: After the hold time, open or close configuration mode.
    if (chord_state == CHORD_ACTIVE)
    {
        nav_ticks++;

        if (nav_ticks >= CHORD_TICKS)
        {
            chord_state = CHORD_DONE;

            if (mode == MODE_RUN)
            {
                enter_config();
            }
            else
            {
                exit_config();
            }
        }

        return;
    }

    if (nav_code == NAV_NONE)
    {
        return;
    }

    // The calibration page uses UP and DOWN as single presses, never auto-repeated.
    if (mode == MODE_CONFIG && cfg_page == PAGE_CAL)
    {
        return;
    }

    nav_ticks++;

    if (!nav_repeating)
    {
        // First repeat step once the key has been held for the delay.
        if (nav_ticks >= REPEAT_DELAY_TICKS)
        {
            nav_repeating = 1;
            nav_ticks     = 0;
            nav_step(nav_code);
        }
    }
    else if (nav_ticks >= REPEAT_PERIOD_TICKS)
    {
        nav_ticks = 0;
        nav_step(nav_code);
    }
}

#ifdef CTL_INSPECT
// Read-only view of the controller state for the browser emulator's labels and hints.
// Only compiled when CTL_INSPECT is defined (the emulator build), never into the firmware.
uint8_t ctl_inspect(uint8_t what)
{
    if (what >= CTL_INSPECT_PEDAL_MODE && what < CTL_INSPECT_PEDAL_MODE + STOMP_COUNT)
    {
        return pedal_mode_get((uint8_t)(what - CTL_INSPECT_PEDAL_MODE));
    }

    switch (what)
    {
    case CTL_INSPECT_MODE:          return mode;
    case CTL_INSPECT_PROGRAM:       return program;
    case CTL_INSPECT_CHANNEL:       return midi_channel;
    case CTL_INSPECT_PAGE:          return cfg_page;
    case CTL_INSPECT_SELECTED:      return cfg_sel;
    case CTL_INSPECT_CAL_STEP:      return cal_step;
    case CTL_INSPECT_HEEL_A:        return heel[0];
    case CTL_INSPECT_HEEL_B:        return heel[1];
    case CTL_INSPECT_TOE_A:         return toe[0];
    case CTL_INSPECT_TOE_B:         return toe[1];
    case CTL_INSPECT_SENT_A:        return sent[0];
    case CTL_INSPECT_SENT_B:        return sent[1];
    case CTL_INSPECT_DISPLAY_ERROR: return DISPLAY_ERROR;
    case CTL_INSPECT_CAL_MIN_SPAN:  return CAL_MIN_SPAN;
    default:                        return 0;
    }
}
#endif
