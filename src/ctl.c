#include "platform.h"
#include "config.h"
#include "hal.h"
#include "ctl.h"
#include "version.h"

// Controller behaviour:
//   footswitches 1..10  -> stomp switches, each with a mode chosen in configuration mode:
//                           stomp, stomp with momentary on a long press, or momentary
//   expression pedal A/B -> continuous CC 0..127
//   UP / DOWN            -> MIDI program change +1 / -1, sent when the key is released;
//                           held, the number auto-repeats on the display and is sent on release
//   UP + DOWN held 1s   -> configuration mode (MIDI channel, pedal modes, calibration)

#define NO_VALUE        0xFF

// Overall mode.
#define MODE_RUN        0
#define MODE_CONFIG     1

// Pedal modes (stored as two bits per footswitch).
#define PM_STOMP        0
#define PM_STOMP_MOM    1
#define PM_MOMENTARY    2
#define PM_MOM_INV      3
#define PM_COUNT        4

#if DEFAULT_PEDAL_MODE < 0 || DEFAULT_PEDAL_MODE >= PM_COUNT
#error DEFAULT_PEDAL_MODE must be 0 to 3
#endif

// Configuration pages
#define PAGE_MODES      0
#define PAGE_CHANNEL    1
#define PAGE_HEEL       2
#define PAGE_TOE        3
#define PAGE_SW1        4
#define PAGE_SW2        5
#define PAGE_VERSION    6
#define PAGE_COUNT      7
// Pages 5 and 6 (one compare: the subtraction wraps for the pages below).
#define IS_SW_PAGE(p)   ((uint8_t)((p) - PAGE_SW1) < 2)

// A relay's follow mask: bits 0-9 are footswitches 1-10, this bit is CHORD_CC.
#define SW_CC_BIT       ((uint16_t)1 << STOMP_COUNT)
// On a SWITCH page a pedal must move this many CC steps (or reach 0 or 127) to set a
// new trigger value, so ADC noise does not keep changing it.
#define SW_PEDAL_STEP   2

// UP + DOWN chord: Not pressed together, pressed together and still undecided, or already
// acted on and waiting for both keys to be let go.
#define CHORD_NONE      0
#define CHORD_ACTIVE    1
#define CHORD_DONE      2

// What a calibration page shows: 0 = reading taken, 1 = waiting for the heel
// reading ("Lo"), 2 = waiting for the toe reading ("Hi"), 3 = range too small ("Er").
#define CAL_DONE        0
#define CAL_HEEL        1
#define CAL_TOE         2
#define CAL_ERROR       3

#define NAV_NONE        0xFF

#define KEYBIT_UP       0x01
#define KEYBIT_DOWN     0x02

static const CODE uint8_t stomp_cc[STOMP_COUNT] = { STOMP_CC_LIST };
static const CODE uint8_t pedal_cc[2]           = { PEDAL_CC_LIST };
static const CODE uint8_t fw_version_dots[3]    = { 0x01, 0x05, 0x04 };

// Scan ticks for the timed gestures.
#define REPEAT_DELAY_TICKS   (PROGRAM_REPEAT_DELAY_MS / TICK_MS)
#define REPEAT_PERIOD_TICKS  (1000 / PROGRAM_REPEAT_HZ / TICK_MS)
// Fast repeat: its period, and the repeat step (counting the first, at the delay) from
// which it applies: the first step held at least PROGRAM_REPEAT_FAST_AFTER_MS. Step k
// (from 1) comes at DELAY + (k - 1) * PERIOD.
#define REPEAT_PERIOD_MS     (1000 / PROGRAM_REPEAT_HZ)
#define REPEAT_FAST_TICKS    (1000 / PROGRAM_REPEAT_FAST_HZ / TICK_MS)
#define REPEAT_FAST_STEPS    ((PROGRAM_REPEAT_FAST_AFTER_MS - PROGRAM_REPEAT_DELAY_MS + REPEAT_PERIOD_MS - 1) \
                              / REPEAT_PERIOD_MS + 1)

#if REPEAT_FAST_TICKS < 1 || PROGRAM_REPEAT_FAST_AFTER_MS < PROGRAM_REPEAT_DELAY_MS || REPEAT_FAST_STEPS > 255
#error PROGRAM_REPEAT_FAST_AFTER_MS or PROGRAM_REPEAT_FAST_HZ is out of range
#endif
#define CHORD_TICKS          (CONFIG_HOLD_MS / TICK_MS)
// Channel entry: scan ticks per flash half-period, and half-periods until timeout.
#define CHAN_FLASH_TICKS     (CHANNEL_DIGIT_FLASH_MS / TICK_MS)
#define CHAN_TIMEOUT_FLASHES (CHANNEL_DIGIT_TIMEOUT_MS / CHANNEL_DIGIT_FLASH_MS)

#if CHAN_FLASH_TICKS < 1 || CHAN_FLASH_TICKS > 255 || CHAN_TIMEOUT_FLASHES < 1 || CHAN_TIMEOUT_FLASHES > 255
#error CHANNEL_DIGIT_FLASH_MS and CHANNEL_DIGIT_TIMEOUT_MS are out of range
#endif

// Scan ticks the MIDI CHAN LED stays lit after a received byte.
#define RX_FLASH_TICKS       (MIDI_RX_FLASH_MS / TICK_MS)

#if RX_FLASH_TICKS < 1 || RX_FLASH_TICKS > 255
#error MIDI_RX_FLASH_MS is out of range
#endif

// Scan ticks per CONFIG LED flash half-period once a setting has changed.
#define CFG_FLASH_TICKS      (CONFIG_CHANGED_FLASH_MS / TICK_MS)

#if CFG_FLASH_TICKS < 1 || CFG_FLASH_TICKS > 255
#error CONFIG_CHANGED_FLASH_MS is out of range
#endif

// Page 7 runs in steps of LAMP_TEST_BLINK_MS: LAMP_TEST_BLINKS on / off pairs, then
// VER_ITEM_STEPS steps for each of major, minor, build and a blank display, then major again.
#define VER_STEP_TICKS       (LAMP_TEST_BLINK_MS / TICK_MS)
#define VER_BLINK_STEPS      (LAMP_TEST_BLINKS * 2)
#define VER_ITEM_STEPS       (VERSION_SHOW_MS / LAMP_TEST_BLINK_MS)
#define VER_END_STEP         (VER_BLINK_STEPS + 4 * VER_ITEM_STEPS)

#if VER_STEP_TICKS < 1 || VER_STEP_TICKS > 255 || VER_ITEM_STEPS < 1 || \
    VERSION_SHOW_MS % LAMP_TEST_BLINK_MS != 0 || VER_END_STEP > 255
#error LAMP_TEST_BLINK_MS, LAMP_TEST_BLINKS or VERSION_SHOW_MS is out of range
#endif

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
// Repeat steps so far in this hold (stops counting at REPEAT_FAST_STEPS: fast repeat).
static IDATA uint8_t  nav_steps;
static BIT_T          program_pending;
// The program number changed since it was last sent (a step at 0 or 127 without wrap
// does not change it, so nothing is sent).
static BIT_T          program_moved;

// Which of UP / DOWN are down, and the state of an UP + DOWN chord.
static IDATA uint8_t  keys_down;
static IDATA uint8_t  chord_state;
// State of CHORD_CC, toggled by a short UP + DOWN press in run mode.
static BIT_T          chord_cc_on;

// Configuration mode state: Open page, footswitch being edited on the modes page, calibration
// step, and which pedals had their mode changed.
static IDATA uint8_t  cfg_page;
static IDATA uint8_t  cfg_sel;
static IDATA uint8_t  cal_step;
static IDATA uint16_t cfg_dirty;
// Switch 10 was pressed on the channel page and a second digit can follow; scan ticks
// into the current flash half-period, and half-periods so far.
static BIT_T          chan_digit_pending;
static IDATA uint8_t  chan_flash_ticks;
// The flash count lives in cal_step: one is only used on the channel page and the
// other only on the calibration pages, so they share a byte.
#define chan_flash_count cal_step
// Page 7 reuses both: ticks into the current step, and the step number.
#define ver_ticks chan_flash_ticks
#define ver_step  cal_step
// A heel reading was taken on page 3 (in ref_raw) and waits for a toe reading.
static BIT_T          cal_heel_taken;

// Calibrated pedal end points, indexed by pedal channel.
static IDATA uint8_t  heel[2];
static IDATA uint8_t  toe[2];

// Most recent averaged readings, last value sent, and the reading it was sent at.
static IDATA uint8_t  last_raw[2];
static IDATA uint8_t  sent[2];
static IDATA uint8_t  ref_raw[2];

// Relay (SWITCH 1 / 2) triggers, saved: follow masks (SW_CC_BIT and footswitches) and
// pedal trigger values (index relay * 2 + pedal, 0 = off). relay_out is what the
// relays are set to now (bit 0 = SWITCH 1).
static IDATA uint16_t sw_mask[2];
static IDATA uint8_t  sw_thr[4];
static IDATA uint8_t  relay_out;
// Scan ticks left of the MIDI CHAN flash for received MIDI (run mode only).
static IDATA uint8_t  rx_flash;
// Config mode: a setting has been changed since entry (the CONFIG LED flashes), and
// the LED is in its dark half. The flash timer reuses rx_flash, which is run mode only.
static BIT_T          cfg_changed;
static BIT_T          cfg_led_dark;
#define cfg_flash_ticks rx_flash

// A setting was changed in config mode: start flashing the CONFIG LED (dark first, so
// the change shows at once).
static void config_changed(void)
{
    if (!cfg_changed)
    {
        cfg_changed     = 1;
        cfg_led_dark    = 1;
        cfg_flash_ticks = 0;
        hal_status_led(0);
    }
}

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
            program_moved = 1;
#endif
        }
        else
        {
            program++;
            program_moved = 1;
        }
    }
    else
    {
        if (program == 0)
        {
#if PROGRAM_WRAP
            program = 127;
            program_moved = 1;
#endif
        }
        else
        {
            program--;
            program_moved = 1;
        }
    }

    show_program();
}

// Send the program number now, or as soon as there is room in the MIDI OUT queue.
static void commit_program(void)
{
    if (!program_moved)
    {
        return;
    }

    program_moved = 0;

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

// Calibration page on the display: "Lo" heel, "Hi" toe, 0 taken, "Er" error.
static void show_cal_step(void)
{
    if (cal_step == CAL_HEEL)
    {
        hal_display_text(HAL_TEXT_LO);
    }
    else if (cal_step == CAL_TOE)
    {
        hal_display_text(HAL_TEXT_HI);
    }
    else if (cal_step == CAL_ERROR)
    {
        hal_display_text(HAL_TEXT_ER);
    }
    else
    {
        hal_display(0);
    }
}

// Page 7, the current step: lamp test on / off, or the version part that starts at
// this step (other steps leave the display as it is).
static void version_show(void)
{
    uint8_t s = ver_step;

    if (s < VER_BLINK_STEPS)
    {
        hal_lamp_test((s & 1) ? HAL_LAMP_ALL_OFF : HAL_LAMP_ALL_ON);
        return;
    }

    // After the blinks every LED stays lit while the version shows.
    hal_lamp_test(HAL_LAMP_LEDS_ON);
    s = (uint8_t)(s - VER_BLINK_STEPS);

    if (s % VER_ITEM_STEPS == 0)
    {
        s = (uint8_t)(s / VER_ITEM_STEPS);

        // Step 3 is the blank between build and the major
        if (s == 3)
        {
            hal_display_dots(0);
            hal_display(0);
            hal_display_blank(7);
        }
        else
        {
            hal_display_hex(fw_version[s]);
            hal_display_dots(fw_version_dots[s]);
        }
    }
}

// Every scan tick on page 7: move to the next step, looping over the version parts
static void version_tick(void)
{
    if (--ver_ticks != 0)
    {
        return;
    }

    ver_ticks = VER_STEP_TICKS;
    ver_step++;

    if (ver_step >= VER_END_STEP)
    {
        ver_step = VER_BLINK_STEPS;
    }

    version_show();
}

// Redraw the display and panel LEDs for the open page.
static void config_show(void)
{
    uint8_t i;
    uint8_t r;

    // SWITCH 1 / SWITCH 2 mark pages 5 and 6.
    hal_aux_led(HAL_LED_SWITCH_1, cfg_page == PAGE_SW1);
    hal_aux_led(HAL_LED_SWITCH_2, cfg_page == PAGE_SW2);

    // Page 7: no page LED (everything blinks); stomp LEDs off, display blank under the
    // lamp test, then the version.
    if (cfg_page == PAGE_VERSION)
    {
        hal_aux_led(HAL_LED_MIDI_CHAN, 0);
        hal_aux_led(HAL_LED_MIDI_FUNC, 0);
        hal_aux_led(HAL_LED_EXP_1, 0);
        hal_aux_led(HAL_LED_EXP_2, 0);
        hal_aux_led(HAL_LED_DIRECT_SELECT, 0);

        for (i = 0; i < STOMP_COUNT; i++)
        {
            hal_stomp_led(i, 0);
        }

        hal_display(0);
        hal_display_blank(7);
        version_show();
        return;
    }

    // A SWITCH page shows that relay's triggers: footswitch LEDs for the switches it
    // follows, DIRECT SELECT for CHORD_CC, EXP 1 / EXP 2 for a pedal trigger value. The
    // display is blank until a pedal is moved, then shows the value it set.
    if (IS_SW_PAGE(cfg_page))
    {
        r = (uint8_t)(cfg_page - PAGE_SW1);
        hal_aux_led(HAL_LED_MIDI_CHAN, 0);
        hal_aux_led(HAL_LED_MIDI_FUNC, 0);
        hal_aux_led(HAL_LED_EXP_1, sw_thr[r * 2] != 0);
        hal_aux_led(HAL_LED_EXP_2, sw_thr[r * 2 + 1] != 0);
        hal_aux_led(HAL_LED_DIRECT_SELECT, (sw_mask[r] & SW_CC_BIT) != 0);

        for (i = 0; i < STOMP_COUNT; i++)
        {
            hal_stomp_led(i, (uint8_t)((sw_mask[r] >> i) & 1));
        }

        hal_display(0);
        hal_display_blank(7);
        return;
    }

    hal_aux_led(HAL_LED_DIRECT_SELECT, 0);

    // Panel LEDs per page: MIDI CHANNEL, MIDI FUNCTION, then both EXP pedal LEDs on
    // the two calibration pages.
    hal_aux_led(HAL_LED_MIDI_CHAN, cfg_page == PAGE_CHANNEL);
    hal_aux_led(HAL_LED_MIDI_FUNC, cfg_page == PAGE_MODES);
    hal_aux_led(HAL_LED_EXP_1,     cfg_page >= PAGE_HEEL);
    hal_aux_led(HAL_LED_EXP_2,     cfg_page >= PAGE_HEEL);

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
        // Only the selected footswitch's mode: 1 stomp, 2 stomp with momentary,
        // 3 momentary. The footswitch's own LED shows which one is selected.
        hal_display((uint8_t)(pedal_mode_get(cfg_sel) + 1));
    }
    else
    {
        show_cal_step();
    }
}

static void config_goto_page(uint8_t page)
{
    cfg_page = page;
    chan_digit_pending = 0;
    // Leaving page 7 (or restarting it): normal LEDs again and no dots.
    hal_lamp_test(HAL_LAMP_NORMAL);
    hal_display_dots(0);

    if (page == PAGE_HEEL)
    {
        cal_step = CAL_HEEL;
    }
    else if (page == PAGE_TOE)
    {
        cal_step = CAL_TOE;
    }
    // A SWITCH page takes the next pedal readings as the reference, so only a pedal
    // that is then moved sets a trigger value.
    else if (IS_SW_PAGE(page))
    {
        resync_pedals();
    }
    // Page 7 starts with the first lamp test step.
    else if (page == PAGE_VERSION)
    {
        ver_step  = 0;
        ver_ticks = VER_STEP_TICKS;
    }

    config_show();
}

static int16_t span_of(uint8_t a, uint8_t b)
{
    int16_t s = (int16_t)a - (int16_t)b;

    return (s < 0) ? -s : s;
}

// Footswitch on a calibration page. Page 3 stores the heel reading. Page 4 takes the
// toe reading and checks it against that heel (or the current heel if page 3 was not
// used): both pedals must move CAL_MIN_SPAN, or "Er" shows and nothing changes. A
// heel reading with no toe reading is discarded.
static void calibration_take(void)
{
    if (cfg_page == PAGE_HEEL)
    {
        ref_raw[0] = last_raw[0];
        ref_raw[1] = last_raw[1];
        cal_heel_taken = 1;
        cal_step = CAL_DONE;
    }
    else
    {
        if (!cal_heel_taken)
        {
            ref_raw[0] = heel[0];
            ref_raw[1] = heel[1];
        }

        if (span_of(last_raw[0], ref_raw[0]) < CAL_MIN_SPAN ||
            span_of(last_raw[1], ref_raw[1]) < CAL_MIN_SPAN)
        {
            cal_step = CAL_ERROR;
        }
        else
        {
            heel[0] = ref_raw[0];
            heel[1] = ref_raw[1];
            toe[0]  = last_raw[0];
            toe[1]  = last_raw[1];
            cal_heel_taken = 0;
            cal_step = CAL_DONE;
            config_changed();
        }
    }

    show_cal_step();
}

// Short UP or DOWN in configuration mode: The next or previous page, not wrapping.
static void config_step(uint8_t code)
{
    if (code == CTL_KEY_UP)
    {
        if (cfg_page < PAGE_COUNT - 1)
        {
            config_goto_page((uint8_t)(cfg_page + 1));
        }
    }
    else if (cfg_page > 0)
    {
        config_goto_page((uint8_t)(cfg_page - 1));
    }
}

// A pedal reading on a SWITCH page: once it has moved SW_PEDAL_STEP from the last value
// (or reached 0 or 127), its CC value becomes the relay's trigger value for that pedal
// (0 turns the trigger off). The display shows it and the EXP LED shows whether it is on.
static void sw_pedal(uint8_t ch)
{
    uint8_t v = ctl_scale(last_raw[ch], heel[ch], toe[ch]);
    uint8_t d;

    if (sent[ch] == NO_VALUE)
    {
        sent[ch] = v;
        return;
    }

    d = (uint8_t)((v > sent[ch]) ? v - sent[ch] : sent[ch] - v);

    if (d == 0 || (d < SW_PEDAL_STEP && v != 0 && v != 127))
    {
        return;
    }

    sent[ch] = v;
    sw_thr[(uint8_t)((cfg_page - PAGE_SW1) * 2 + ch)] = v;
    config_changed();
    hal_display(v);
    hal_aux_led((uint8_t)(HAL_LED_EXP_1 + ch), v != 0);
}

// Footswitch on the modes page: A switch that is not selected is selected and its mode
// shown. Pressing the selected switch again moves it to the next mode.
static void mode_press(uint8_t index)
{
    uint8_t m;

    if (index == cfg_sel)
    {
        m = (uint8_t)((pedal_mode_get(index) + 1) & (PM_COUNT - 1));
        pedal_mode_set(index, m);
        cfg_dirty |= (uint16_t)((uint16_t)1 << index);
        config_changed();
    }
    else
    {
        cfg_sel = index;
    }

    config_show();
}

// End a two-digit channel entry: stop the flashing and show the channel.
static void channel_entry_end(void)
{
    chan_digit_pending = 0;
    config_show();
}

// Footswitch on the channel page. Switches 1-9 set channels 1-9 at once. Switch 10
// sets channel 10 and shows "10" with the 0 flashing while a second digit can follow:
// switches 1-6 make 11-16, switch 10 (as 0) keeps 10, 7-9 set channels 7-9. After
// CHANNEL_DIGIT_TIMEOUT_MS, a page change or leaving, the channel simply stays 10.
static void channel_digit(uint8_t index)
{
    uint8_t d = (uint8_t)(index + 1);

    if (chan_digit_pending)
    {
        d = (d == 10) ? 10 : (d <= 6) ? (uint8_t)(10 + d) : d;
    }

    if (d != midi_channel)
    {
        midi_channel = d;
        config_changed();
    }

    if (chan_digit_pending)
    {
        channel_entry_end();
        return;
    }

    if (d == 10)
    {
        chan_digit_pending = 1;
        chan_flash_ticks   = 0;
        chan_flash_count   = 0;
    }

    config_show();
}

// Each scan tick while a second digit can follow: flash the 0, and time out.
static void channel_digit_tick(void)
{
    if (++chan_flash_ticks < CHAN_FLASH_TICKS)
    {
        return;
    }

    chan_flash_ticks = 0;

    if (++chan_flash_count >= CHAN_TIMEOUT_FLASHES)
    {
        channel_entry_end();
        return;
    }

    if (chan_flash_count & 1)
    {
        hal_display_blank(1);
    }
    else
    {
        hal_display(midi_channel);
    }
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
    cal_heel_taken = 0;
    rx_flash  = 0;
    cfg_changed  = 0;
    cfg_led_dark = 0;

    hal_status_led(1);
    hal_aux_led(HAL_LED_DIRECT_SELECT, 0);
    config_goto_page(PAGE_MODES);
}

// Take the settings from the four bytes loaded into cfg_buf, keeping the default
// channel if the saved one is out of range. Every 2-bit pedal mode value is valid.
static void settings_unpack(void)
{
    if (cfg_buf[0] >= 1 && cfg_buf[0] <= 16)
    {
        midi_channel = cfg_buf[0];
    }

    pedal_modes[0] = cfg_buf[1];
    pedal_modes[1] = cfg_buf[2];
    pedal_modes[2] = (uint8_t)(cfg_buf[3] & 0x0F);
}

// Defaults, then whatever valid settings and calibration the EEPROM holds. Used at
// start-up and to cancel configuration changes.
static void load_settings(void)
{
    uint8_t i;

    // Defaults used for anything without a valid saved value.
    heel[0] = 0;
    heel[1] = 0;
    toe[0]  = 255;
    toe[1]  = 255;

    midi_channel = MIDI_CHANNEL;
    // DEFAULT_PEDAL_MODE in every 2-bit field
    pedal_modes[0] = (uint8_t)(DEFAULT_PEDAL_MODE * 0x55);
    pedal_modes[1] = (uint8_t)(DEFAULT_PEDAL_MODE * 0x55);
    pedal_modes[2] = (uint8_t)(DEFAULT_PEDAL_MODE * 0x05);

    hal_cal_load(heel, toe);

    // Relay triggers: none by default. A saved value over 127 is ignored (off).
    for (i = 0; i < 2; i++)
    {
        sw_mask[i]        = 0;
        sw_thr[i * 2]     = 0;
        sw_thr[i * 2 + 1] = 0;

        if (hal_sw_load(i, cfg_buf))
        {
            sw_mask[i]        = (uint16_t)(cfg_buf[0] | ((uint16_t)(cfg_buf[1] & 0x07) << 8));
            sw_thr[i * 2]     = (cfg_buf[2] <= 127) ? cfg_buf[2] : 0;
            sw_thr[i * 2 + 1] = (cfg_buf[3] <= 127) ? cfg_buf[3] : 0;
        }
    }

    if (hal_cfg_load(cfg_buf))
    {
        settings_unpack();
    }
}

// Leave configuration mode. save = 1 (long UP) stores the settings and calibration;
// save = 0 (long DOWN) drops every change by reloading what the EEPROM holds.
static void exit_config(uint8_t save)
{
    uint8_t  i;
    uint16_t bit;

    // Stop a two-digit channel entry that could still take a second digit.
    chan_digit_pending = 0;
    // Leaving from page 7 ends the lamp test and its dots.
    hal_lamp_test(HAL_LAMP_NORMAL);
    hal_display_dots(0);

    if (save)
    {
        cfg_buf[0] = midi_channel;
        cfg_buf[1] = pedal_modes[0];
        cfg_buf[2] = pedal_modes[1];
        cfg_buf[3] = pedal_modes[2];

        // If a write fails the new settings still apply until power-off.
        hal_cfg_save(cfg_buf);
        hal_cal_save(heel, toe);

        for (i = 0; i < 2; i++)
        {
            cfg_buf[0] = (uint8_t)sw_mask[i];
            cfg_buf[1] = (uint8_t)(sw_mask[i] >> 8);
            cfg_buf[2] = sw_thr[i * 2];
            cfg_buf[3] = sw_thr[i * 2 + 1];
            hal_sw_save(i, cfg_buf);
        }
    }
    else
    {
        load_settings();
        cfg_dirty = 0;
    }

    for (i = 0; i < STOMP_COUNT; i++)
    {
        bit = (uint16_t)((uint16_t)1 << i);

        if (cfg_dirty & bit)
        {
            if (pedal_mode_get(i) == PM_MOM_INV)
            {
                stomp_state  |= bit;
                stomp_prev   |= bit;
                stomp_revert &= (uint16_t)~bit;
            }
            else if (stomp_state & bit)
            {
                stomp_prev &= (uint16_t)~bit;
                stomp_revert |= bit;
            }
        }
    }

    mode = MODE_RUN;
    // Stop the CONFIG LED flash; its timer goes back to the MIDI IN flash.
    cfg_changed  = 0;
    cfg_led_dark = 0;
    cfg_flash_ticks = 0;
    hal_status_led(0);
    hal_aux_led(HAL_LED_MIDI_CHAN, 0);
    hal_aux_led(HAL_LED_MIDI_FUNC, 0);
    hal_aux_led(HAL_LED_EXP_1, 0);
    hal_aux_led(HAL_LED_EXP_2, 0);
    hal_aux_led(HAL_LED_DIRECT_SELECT, chord_cc_on ? 1 : 0);
    hal_aux_led(HAL_LED_SWITCH_1, relay_out & 1);
    hal_aux_led(HAL_LED_SWITCH_2, (relay_out >> 1) & 1);

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

// Short UP + DOWN in run mode: Toggle CHORD_CC and show it on the DIRECT SELECT LED.
// Like a stomp press, it is ignored if MIDI OUT has no room for the message.
static void chord_cc_toggle(void)
{
    if (hal_midi_space() < 3)
    {
        return;
    }

    chord_cc_on = !chord_cc_on;
    send_cc(CHORD_CC, chord_cc_on ? 127 : 0);
    hal_aux_led(HAL_LED_DIRECT_SELECT, chord_cc_on ? 1 : 0);
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
            // Let go before the hold time: in run mode it toggles CHORD_CC. In
            // configuration mode UP + DOWN does nothing.
            chord_state = CHORD_DONE;

            if (mode == MODE_RUN)
            {
                chord_cc_toggle();
            }
            // On a SWITCH page it toggles whether that relay follows CHORD_CC.
            else if (IS_SW_PAGE(cfg_page))
            {
                sw_mask[cfg_page - PAGE_SW1] ^= SW_CC_BIT;
                config_changed();
                hal_aux_led(HAL_LED_DIRECT_SELECT, (sw_mask[cfg_page - PAGE_SW1] & SW_CC_BIT) != 0);
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

    load_settings();

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
    program_moved   = 0;
    keys_down     = 0;
    chord_state   = CHORD_NONE;
    chord_cc_on   = 0;
    nav_ticks   = 0;
    cfg_page      = PAGE_MODES;
    cfg_sel       = 0;
    cal_step      = CAL_DONE;
    cfg_dirty     = 0;
    chan_digit_pending = 0;
    chan_flash_ticks = 0;
    cal_heel_taken = 0;
    relay_out     = 0;
    rx_flash      = 0;
    cfg_changed   = 0;
    cfg_led_dark  = 0;
    last_raw[0]   = 0;
    last_raw[1]   = 0;
    resync_pedals();

    for (i = 0; i < STOMP_COUNT; i++)
    {
        if (pedal_mode_get(i) == PM_MOM_INV)
        {
            stomp_state |= (uint16_t)((uint16_t)1 << i);
            hal_stomp_led(i, 1);
        }
        else
        {
            hal_stomp_led(i, 0);
        }
    }

    hal_status_led(0);
    hal_aux_led(HAL_LED_MIDI_CHAN, 0);
    hal_aux_led(HAL_LED_MIDI_FUNC, 0);
    hal_aux_led(HAL_LED_EXP_1, 0);
    hal_aux_led(HAL_LED_EXP_2, 0);
    hal_aux_led(HAL_LED_DIRECT_SELECT, 0);
    hal_aux_led(HAL_LED_SWITCH_1, 0);
    hal_aux_led(HAL_LED_SWITCH_2, 0);
    hal_relays(0);
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

    // In configuration mode a long UP saves and leaves, a long DOWN cancels and
    // leaves. Only for a key pressed on its own (not part of, or left over from, an
    // UP + DOWN chord). Its release then does nothing.
    if (code == CTL_KEY_UP || code == CTL_KEY_DOWN)
    {
        if (mode == MODE_CONFIG && chord_state == CHORD_NONE && nav_code == code)
        {
            nav_code      = NAV_NONE;
            nav_repeating = 0;
            exit_config((code == CTL_KEY_UP) ? 1 : 0);
        }

        return;
    }

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
        // Channel page: types a digit. Modes page: selects a switch, or changes the
        // selected one's mode. Calibration pages: takes the reading.
        if (cfg_page == PAGE_CHANNEL)
        {
            channel_digit(stomp_index(code));
        }
        else if (cfg_page == PAGE_MODES)
        {
            mode_press(stomp_index(code));
        }
        // SWITCH page: toggles whether the relay follows this footswitch (LED lit).
        else if (IS_SW_PAGE(cfg_page))
        {
            index = stomp_index(code);
            bit   = (uint16_t)((uint16_t)1 << index);
            sw_mask[cfg_page - PAGE_SW1] ^= bit;
            config_changed();
            hal_stomp_led(index, (sw_mask[cfg_page - PAGE_SW1] & bit) ? 1 : 0);
        }
        // Page 7: any footswitch runs the lamp test and version again.
        else if (cfg_page == PAGE_VERSION)
        {
            config_goto_page(PAGE_VERSION);
        }
        else
        {
            calibration_take();
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

        on = pedal_mode_get(index);

        if (on >= PM_MOMENTARY)
        {
            // Momentary: On while pressed whatever it was before, off again on release.
            // Inverted momentary is the opposite: off while pressed, on on release.
            // (on holds the mode until it is turned into the value to send.)
            on = (on == PM_MOMENTARY) ? 1 : 0;

            if (on)
            {
                stomp_state |= bit;
                stomp_prev  &= (uint16_t)~bit;
            }
            else
            {
                stomp_state &= (uint16_t)~bit;
                stomp_prev  |= bit;
            }

            stomp_held |= bit;
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

// Apply an incoming MIDI CC value to every stomp switch assigned to that CC.
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

    // CHORD_CC sets the DIRECT SELECT state and LED the same way (relays that follow it
    // update on the next tick). Nothing is sent back.
    if (cc == CHORD_CC)
    {
        chord_cc_on = on;
        hal_aux_led(HAL_LED_DIRECT_SELECT, on);
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

    // Run mode: flash the MIDI CHAN LED for any other received byte.
    if (mode == MODE_RUN)
    {
        if (rx_flash == 0)
        {
            hal_aux_led(HAL_LED_MIDI_CHAN, 1);
        }

        rx_flash = RX_FLASH_TICKS;
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
        // On a SWITCH page a pedal sets that relay's trigger value; nothing is sent.
        if (IS_SW_PAGE(cfg_page))
        {
            sw_pedal(channel);
        }

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

// Run mode, every tick: each relay is on if any of its triggers is on (logical OR): a
// followed footswitch that is on, CHORD_CC on if followed, or a pedal at or above its
// trigger value (the last CC value sent). Until a pedal has a value again (after start-up
// or configuration mode), its trigger keeps the relay as it was. The SWITCH LEDs follow.
static void relays_update(void)
{
    uint16_t on = stomp_state;
    uint8_t  out = 0;
    uint8_t  i;
    uint8_t  bit;

    if (chord_cc_on)
    {
        on |= SW_CC_BIT;
    }

    // i = relay * 2 + pedal.
    for (i = 0; i < 4; i++)
    {
        bit = (uint8_t)(1 << (i >> 1));

        if ((i & 1) == 0 && (sw_mask[i >> 1] & on))
        {
            out |= bit;
        }

        if (sw_thr[i])
        {
            if (sent[i & 1] == NO_VALUE)
            {
                out |= (uint8_t)(relay_out & bit);
            }
            else if (sent[i & 1] >= sw_thr[i])
            {
                out |= bit;
            }
        }
    }

    if (out != relay_out)
    {
        relay_out = out;
        hal_relays(out);
        hal_aux_led(HAL_LED_SWITCH_1, out & 1);
        hal_aux_led(HAL_LED_SWITCH_2, (out >> 1) & 1);
    }
}

void ctl_tick(void)
{
    flush_reverts();
    flush_program();

    if (mode == MODE_RUN)
    {
        relays_update();
    }

    if (mode == MODE_RUN)
    {
        if (rx_flash && --rx_flash == 0)
        {
            hal_aux_led(HAL_LED_MIDI_CHAN, 0);
        }
    }
    // Config mode with changes: flash the CONFIG LED.
    else if (cfg_changed && ++cfg_flash_ticks >= CFG_FLASH_TICKS)
    {
        cfg_flash_ticks = 0;
        cfg_led_dark    = !cfg_led_dark;
        hal_status_led(cfg_led_dark ? 0 : 1);
    }

    // Only set on the channel page in configuration mode.
    if (chan_digit_pending)
    {
        channel_digit_tick();
    }

    if (mode == MODE_CONFIG && cfg_page == PAGE_VERSION)
    {
        version_tick();
    }

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
                exit_config(1);
            }
        }

        return;
    }

    if (nav_code == NAV_NONE)
    {
        return;
    }

    // In configuration mode UP and DOWN are single presses (pages), never repeated.
    if (mode == MODE_CONFIG)
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
            nav_steps     = 1;
            nav_step(nav_code);
        }
    }
    else if (nav_ticks >= ((nav_steps >= REPEAT_FAST_STEPS) ? REPEAT_FAST_TICKS : REPEAT_PERIOD_TICKS))
    {
        nav_ticks = 0;

        if (nav_steps < REPEAT_FAST_STEPS)
        {
            nav_steps++;
        }

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

    if (what >= CTL_INSPECT_SW_THR && what < CTL_INSPECT_SW_THR + 4)
    {
        return sw_thr[what - CTL_INSPECT_SW_THR];
    }

    if (what >= CTL_INSPECT_SW_MASK && what < CTL_INSPECT_SW_MASK + 4)
    {
        what = (uint8_t)(what - CTL_INSPECT_SW_MASK);
        return (uint8_t)(sw_mask[what >> 1] >> ((what & 1) * 8));
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
    case CTL_INSPECT_CAL_MIN_SPAN:  return CAL_MIN_SPAN;
    case CTL_INSPECT_RELAYS:        return relay_out;
    default:                        return 0;
    }
}
#endif
