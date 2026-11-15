#include "platform.h"
#include "config.h"
#include "hal.h"
#include "ctl.h"
#include "version.h"

// Controller behaviour:
//   footswitches 1..10  -> stomp switches, each with a mode chosen in configuration mode:
//                           stomp, stomp with momentary on a long press, or momentary,
//                           and a message (CC, note or program change) set in its editor
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

// Footswitch message types (two bits, kept in bit 7 of its first two msg[] bytes).
#define MT_CC           0
#define MT_NOTE         1
#define MT_PROG         2
// Program change switches have two programs: the number, sent when the switch turns on,
// and the on value, sent when it turns off, so it toggles between them (following its
// pedal mode). With both the same it is a single program, sent on every press and
// nothing on release, whatever the pedal mode; its LED is lit only while held.

// Parameters in the footswitch editor, each marked by its panel LED: message type
// (SELECT), number (NUMBER: the CC, note or program number), on value (VALUE 1: the CC
// value or note velocity sent when on) and off value (VALUE 2: sent when off). A program
// change has two: the number (sent on) and the on value (sent off). The parameter is
// also the msg[] byte it edits, plus one.
#define EP_TYPE         0
#define EP_NUM          1
#define EP_ON           2
#define EP_OFF          3

// A pedal in the editor chooses the type by thirds of its travel.
#define EDIT_TYPE_NOTE_FROM  43
#define EDIT_TYPE_PROG_FROM  86

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

// Page 7 runs in 100 ms steps: the "riban" scroll (HAL_TEXT_SCROLL_FRAMES frames of
// VER_SCROLL_STEPS, twice the version speed), then VER_ITEM_STEPS steps for each of major,
// minor, build and a blank display, then the scroll again. Every LED is lit throughout.
#define VER_STEP_MS          100
#define VER_STEP_TICKS       (VER_STEP_MS / TICK_MS)
#define VER_ITEM_STEPS       (VERSION_SHOW_MS / VER_STEP_MS)
#define VER_SCROLL_STEPS     (VER_ITEM_STEPS / 2)
#define VER_SCROLL_END       (HAL_TEXT_SCROLL_FRAMES * VER_SCROLL_STEPS)
#define VER_END_STEP         (VER_SCROLL_END + 4 * VER_ITEM_STEPS)
// At power-on only the scroll runs, once, then run mode.
#define VER_BOOT_END_STEP    VER_SCROLL_END
// Page 7 has one segment per footswitch: the six scroll frames, then major, minor, build
// and the blank. Footswitch N's LED is dark for the first VER_FLASH_STEPS of segment N.
#define VER_SEGMENTS         (HAL_TEXT_SCROLL_FRAMES + 4)
#define VER_FLASH_STEPS      ((VER_SCROLL_STEPS + 1) / 2)

#if VER_SEGMENTS != STOMP_COUNT
#error page 7 needs one segment per footswitch
#endif

#if VER_STEP_TICKS < 1 || VER_STEP_TICKS > 255 || VER_SCROLL_STEPS < 1 || \
    VERSION_SHOW_MS % (2 * VER_STEP_MS) != 0 || VER_END_STEP > 255
#error VERSION_SHOW_MS is out of range (a multiple of 200, at most 3600)
#endif

#if REPEAT_DELAY_TICKS < 1 || REPEAT_PERIOD_TICKS < 1 || CHORD_TICKS < 1
#error The repeat and configuration times must each be at least one tick
#endif

// A few of the most used bytes are directly addressed (no IDATA): they fill a gap below
// the bit area that IDATA cannot use, so they cost no stack room, and the code is smaller.
static uint8_t        mode;

// MIDI IN parser state: Current (running) status byte and a pending first data byte.
static IDATA uint8_t  rx_status;
static IDATA uint8_t  rx_data1;
static BIT_T    rx_have_data1;
static uint8_t        program;
static IDATA uint16_t stomp_state;

// Settings that can be changed in configuration mode and are saved in the EEPROM.
static uint8_t        midi_channel;
static IDATA uint8_t  pedal_modes[3];
// Footswitch messages, three bytes per switch: msg[i * 3] is the number (CC, note or
// program) with type bit 0 in bit 7, msg[i * 3 + 1] the on value (CC value or note
// velocity) with type bit 1 in bit 7, msg[i * 3 + 2] the off value. Each switch has its
// own EEPROM record (type, number, on value, off value).
#define MSG_BYTES       3
// Index of switch i's first msg[] byte, i * 3 as 8-bit shift and add: SDCC would compute
// i * MSG_BYTES with a call to its 16-bit multiply (a call level deeper, and slower).
#define MSG_AT(i)       ((uint8_t)((uint8_t)((uint8_t)(i) << 1) + (uint8_t)(i)))
static IDATA uint8_t  msg[STOMP_COUNT * MSG_BYTES];

// The footswitch editor (configuration page 1, a long footswitch press): open, which
// parameter, the switch's three bytes when it was opened (DOWN [ESCAPE] puts them back), and
// whether a number is being typed (the next digit adds to it instead of starting anew).
// The switch being edited is cfg_sel. The parameter and the three bytes share RAM with
// state that is idle while the editor is open (defined below with that state).
static BIT_T          edit_open;
static BIT_T          edit_typing;
// An editor change waits for the next tick to compare the settings with the EEPROM
// (config_changed), so that deep EEPROM read runs from ctl_tick, not from under a key or
// pedal handler.
static BIT_T          cfg_check_due;
// Page 1: a footswitch press there is in progress (its hold opens the editor), and it
// was a press of the selected switch, which changes its mode on release.
static BIT_T          mode_press_live;
static BIT_T          mode_cycle_pending;

// Settings are loaded and saved through the HAL's one record buffer, hal_rec.

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
static uint8_t        cfg_page;
static uint8_t        cfg_sel;
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
// The footswitch editor's parameter: only open on page 1, where neither is used.
#define edit_param chan_flash_ticks
// A heel reading was taken on page 3 (in ref_raw) and waits for a toe reading.
static BIT_T          cal_heel_taken;

// Calibrated pedal end points, indexed by pedal channel.
static IDATA uint8_t  heel[2];
static IDATA uint8_t  toe[2];

// Most recent averaged readings, last value sent, and the reading it was sent at.
static IDATA uint8_t  last_raw[2];
static IDATA uint8_t  sent[2];
static IDATA uint8_t  ref_raw[2];
// The footswitch editor's saved bytes. ref_raw is the run-mode pedal reference (set again
// from the first reading after configuration mode) and the calibration heel reading kept
// from page 3 for page 4, which opening the editor discards (cal_heel_taken = 0). The
// third byte is nav_steps, which only counts UP / DOWN auto-repeat steps in run mode
// (reset when a repeat starts).
#define edit_bak     ref_raw
#define edit_bak_off nav_steps

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
// The power-on lamp test and version are running (run mode; uses ver_step / ver_ticks).
static BIT_T          boot_anim;
// Page 7 is held on one segment while its footswitch is down.
static BIT_T          ver_paused;
#define cfg_flash_ticks rx_flash

// Footswitch i's message record into hal_rec[1..3] in msg[]'s form: the defaults (a CC
// from STOMP_CC_LIST, on value 127, off value 0) if it is missing or invalid, the unused
// type 3 as a CC, and each value cut to 7 bits. A macro, not a function: the EEPROM read
// under it is the deepest stack use, and a call level would add two bytes to it.
#define MSG_REC_LOAD(i)                                                         \
    do {                                                                        \
        if (!hal_rec_load((uint8_t)(HAL_REC_MSG0 + (i))))                       \
        {                                                                       \
            hal_rec[0] = MT_CC;                                                 \
            hal_rec[1] = stomp_cc[i];                                           \
            hal_rec[2] = 127;                                                   \
            hal_rec[3] = 0;                                                     \
        }                                                                       \
        if ((hal_rec[0] & 3) == 3)                                              \
        {                                                                       \
            hal_rec[0] = MT_CC;                                                 \
        }                                                                       \
        hal_rec[1] = (uint8_t)((hal_rec[1] & 0x7F) | ((hal_rec[0] & 1) << 7));  \
        hal_rec[2] = (uint8_t)((hal_rec[2] & 0x7F) | ((hal_rec[0] & 2) << 6));  \
        hal_rec[3] &= 0x7F;                                                     \
    } while (0)

// 1 if every setting matches what the EEPROM holds, read back the way load_settings()
// takes it (a record that is missing or invalid counts as the defaults; a stored
// channel outside 1-16 counts as the default channel). So the CONFIG LED flashes exactly
// while a save would change something, and cancel would restore what the EEPROM says.
// Each record is read into hal_rec in turn; no other RAM is used.
static uint8_t settings_match_saved(void)
{
    uint8_t i;

    // Footswitch messages.
    for (i = 0; i < STOMP_COUNT; i++)
    {
        MSG_REC_LOAD(i);

        if (hal_rec[1] != msg[MSG_AT(i)] || hal_rec[2] != msg[MSG_AT(i) + 1] ||
            hal_rec[3] != msg[MSG_AT(i) + 2])
        {
            return 0;
        }
    }

    // Calibration (heel A, toe A, heel B, toe B), or the defaults.
    if (!hal_rec_load(HAL_REC_CAL))
    {
        hal_rec[0] = 0;
        hal_rec[1] = 255;
        hal_rec[2] = 0;
        hal_rec[3] = 255;
    }

    if (hal_rec[0] != heel[0] || hal_rec[1] != toe[0] || hal_rec[2] != heel[1] || hal_rec[3] != toe[1])
    {
        return 0;
    }

    // Relay triggers: none by default; a stored value over 127 is off.
    for (i = 0; i < 2; i++)
    {
        if (!hal_rec_load((uint8_t)(HAL_REC_SW1 + i)))
        {
            hal_rec[0] = 0;
            hal_rec[1] = 0;
            hal_rec[2] = 0;
            hal_rec[3] = 0;
        }

        if (hal_rec[0] != (uint8_t)sw_mask[i] || (hal_rec[1] & 0x07) != (uint8_t)(sw_mask[i] >> 8) ||
            ((hal_rec[2] <= 127) ? hal_rec[2] : 0) != sw_thr[i * 2] ||
            ((hal_rec[3] <= 127) ? hal_rec[3] : 0) != sw_thr[i * 2 + 1])
        {
            return 0;
        }
    }

    // Channel and pedal modes.
    if (!hal_rec_load(HAL_REC_CFG))
    {
        hal_rec[0] = MIDI_CHANNEL;
        hal_rec[1] = (uint8_t)(DEFAULT_PEDAL_MODE * 0x55);
        hal_rec[2] = (uint8_t)(DEFAULT_PEDAL_MODE * 0x55);
        hal_rec[3] = (uint8_t)(DEFAULT_PEDAL_MODE * 0x05);
    }

    if (hal_rec[0] < 1 || hal_rec[0] > 16)
    {
        hal_rec[0] = MIDI_CHANNEL;
    }

    return (uint8_t)(hal_rec[0] == midi_channel && hal_rec[1] == pedal_modes[0] &&
                     hal_rec[2] == pedal_modes[1] && (uint8_t)(hal_rec[3] & 0x0F) == pedal_modes[2]);
}

// A setting was edited in config mode (or config mode was just entered). If the settings
// now differ from the EEPROM, flash the CONFIG LED (dark first, so the change shows at
// once); if they match it, stop and light it steadily.
static void config_changed(void)
{
    if (settings_match_saved())
    {
        cfg_changed  = 0;
        cfg_led_dark = 0;
        hal_status_led(1);
    }
    else if (!cfg_changed)
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

// Type of footswitch index's message (MT_*).
static uint8_t msg_type(uint8_t index)
{
    return (uint8_t)((msg[MSG_AT(index)] >> 7) | ((msg[MSG_AT(index) + 1] >> 6) & 2));
}

// 1 if footswitch index is a single-program switch: a program change whose two programs
// are the same (type bits: bit 7 clear in its first byte, set in its second). A leaf
// function, so it costs no fixed RAM.
static uint8_t prog_single(uint8_t index)
{
    index = MSG_AT(index);

    return (uint8_t)(((uint8_t)(msg[index] ^ msg[index + 1]) == 0x80 && (msg[index + 1] & 0x80)) ? 1 : 0);
}

static void show_program(void)
{
    hal_display((uint8_t)(program + DISPLAY_PROGRAM_OFFSET));
}

// Send footswitch index's message for the state on: a CC with the on or off value, a note
// on with the on value as velocity (on) or a note off with the off value (off), or a
// program change, the number (on) or the on value (off; a single-program switch sends
// nothing). A program sent becomes the current program, so UP / DOWN step from it and the
// run-mode display shows it. The caller has checked there is room for three bytes.
static void send_switch(uint8_t index, uint8_t on)
{
    if (msg_type(index) == MT_PROG)
    {
        if (on || !prog_single(index))
        {
            program = (uint8_t)(msg[(uint8_t)(MSG_AT(index) + (on ? 0 : 1))] & 0x7F);
            send_program(program);
            program_moved   = 0;
            program_pending = 0;

            if (mode == MODE_RUN)
            {
                show_program();
            }
        }

        return;
    }

    hal_midi_tx((uint8_t)(((msg_type(index) == MT_NOTE) ? (on ? 0x90 : 0x80) : 0xB0) | (midi_channel - 1)));
    hal_midi_tx((uint8_t)(msg[MSG_AT(index)] & 0x7F));
    hal_midi_tx((uint8_t)(msg[(uint8_t)(MSG_AT(index) + (on ? 1 : 2))] & 0x7F));
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

// Page 7 (and power-on), the current step: every LED lit, and the "riban" frame or
// version part that starts at this step (other steps leave the display as it is).
static void version_show(void)
{
    uint8_t s = ver_step;
    uint8_t g;

    // Every LED stays lit throughout; on page 7 the footswitch LEDs mark the segments.
    hal_lamp_test(boot_anim ? HAL_LAMP_LEDS_ON : HAL_LAMP_LEDS_STOMPS);

    // g = the segment (0-9), s = the step within it.
    if (s < VER_SCROLL_END)
    {
        g = (uint8_t)(s / VER_SCROLL_STEPS);
        s = (uint8_t)(s % VER_SCROLL_STEPS);
    }
    else
    {
        s = (uint8_t)(s - VER_SCROLL_END);
        g = (uint8_t)(HAL_TEXT_SCROLL_FRAMES + s / VER_ITEM_STEPS);
        s = (uint8_t)(s % VER_ITEM_STEPS);
    }

    // Page 7 (not power-on): segment g's footswitch LED goes dark at its start (the one
    // before lights again), then lights after VER_FLASH_STEPS.
    if (!boot_anim)
    {
        if (s == 0)
        {
            hal_stomp_led((uint8_t)((g == 0) ? VER_SEGMENTS - 1 : g - 1), 1);
            hal_stomp_led(g, 0);
        }
        else if (s == VER_FLASH_STEPS)
        {
            hal_stomp_led(g, 1);
        }
    }

    if (s != 0)
    {
        return;
    }

    // First the scroll, no dots.
    if (g < HAL_TEXT_SCROLL_FRAMES)
    {
        hal_display_dots(0);
        hal_display_text((uint8_t)(HAL_TEXT_SCROLL + g));
        return;
    }

    {
        s = (uint8_t)(g - HAL_TEXT_SCROLL_FRAMES);

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

// End the power-on lamp test and version: normal LEDs, no dots, the program number.
// ver_step / ver_ticks go back to their idle values (cal_step / chan_flash_ticks).
static void boot_end(void)
{
    boot_anim = 0;
    ver_step  = CAL_DONE;
    ver_ticks = 0;
    hal_lamp_test(HAL_LAMP_NORMAL);
    hal_display_dots(0);
    show_program();
}

// Page 7: footswitch index (0-9) jumps to its segment, held there until released.
// Every footswitch LED lights; version_show then darkens this one.
static void version_jump(uint8_t index)
{
    uint8_t i;

    for (i = 0; i < STOMP_COUNT; i++)
    {
        hal_stomp_led(i, 1);
    }

    ver_step   = (uint8_t)((index < HAL_TEXT_SCROLL_FRAMES) ? index * VER_SCROLL_STEPS :
                           VER_SCROLL_END + (index - HAL_TEXT_SCROLL_FRAMES) * VER_ITEM_STEPS);
    ver_ticks  = VER_STEP_TICKS;
    ver_paused = 1;
    version_show();
}

// Every scan tick on page 7: move to the next step, looping over the version parts
static void version_tick(void)
{
    // Held by a footswitch: no steps until it is released.
    if (ver_paused)
    {
        return;
    }

    if (--ver_ticks != 0)
    {
        return;
    }

    ver_ticks = VER_STEP_TICKS;
    ver_step++;

    // At power-on, stop after the build number and go to run mode.
    if (boot_anim && ver_step >= VER_BOOT_END_STEP)
    {
        boot_end();
        return;
    }

    if (ver_step >= VER_END_STEP)
    {
        ver_step = 0;
    }

    version_show();
}

// The msg[] byte of the open editor parameter (EP_NUM, EP_ON or EP_OFF) of switch cfg_sel;
// in the first two, bit 7 is a type bit and the rest the value.
#define EDIT_BYTE msg[(uint8_t)(MSG_AT(cfg_sel) + edit_param - 1)]

// The footswitch editor: SELECT / NUMBER / VALUE 1 / VALUE 2 for the parameter (type,
// number, on value, off value), the edited switch's LED lit, and the type ("CC", "nt",
// "Pr") or the value on the display.
static void edit_show(void)
{
    uint8_t i;

    hal_aux_led(HAL_LED_SELECT, edit_param == EP_TYPE);
    hal_aux_led(HAL_LED_NUMBER, edit_param == EP_NUM);
    hal_aux_led(HAL_LED_VALUE_1, edit_param == EP_ON);
    hal_aux_led(HAL_LED_VALUE_2, edit_param == EP_OFF);

    for (i = 0; i < STOMP_COUNT; i++)
    {
        hal_stomp_led(i, i == cfg_sel);
    }

    if (edit_param == EP_TYPE)
    {
        hal_display_text((uint8_t)(HAL_TEXT_CC + msg_type(cfg_sel)));
    }
    else
    {
        hal_display((uint8_t)(EDIT_BYTE & 0x7F));
    }
}

// Redraw the display and panel LEDs for the open page.
static void config_show(void)
{
    uint8_t i;
    uint8_t r;

    if (edit_open)
    {
        edit_show();
        return;
    }

    hal_aux_led(HAL_LED_SELECT, 0);
    hal_aux_led(HAL_LED_NUMBER, 0);
    hal_aux_led(HAL_LED_VALUE_1, 0);
    hal_aux_led(HAL_LED_VALUE_2, 0);

    // SWITCH 1 / SWITCH 2 mark pages 5 and 6.
    hal_aux_led(HAL_LED_SWITCH_1, cfg_page == PAGE_SW1);
    hal_aux_led(HAL_LED_SWITCH_2, cfg_page == PAGE_SW2);

    // Page 7: no page LED (everything blinks); stomp LEDs off, display blank under the
    // version, with every LED lit.
    if (cfg_page == PAGE_VERSION)
    {
        hal_aux_led(HAL_LED_MIDI_CHAN, 0);
        hal_aux_led(HAL_LED_MIDI_FUNC, 0);
        hal_aux_led(HAL_LED_EXP_1, 0);
        hal_aux_led(HAL_LED_EXP_2, 0);
        hal_aux_led(HAL_LED_DIRECT_SELECT, 0);

        // Footswitch LEDs lit; the first segment's (switch 1) then goes dark.
        for (i = 0; i < STOMP_COUNT; i++)
        {
            hal_stomp_led(i, 1);
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
    ver_paused = 0;
    mode_press_live = 0;
    mode_cycle_pending = 0;
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
    // Page 7 starts with the "riban" scroll.
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

// A pedal reading in configuration mode: 1 once its CC value has moved SW_PEDAL_STEP from
// the last one (or reached 0 or 127), and the new value is then in sent[ch]. The first
// reading after resync_pedals() only records the value, so a pedal sets nothing until it
// is moved.
static uint8_t pedal_moved(uint8_t ch)
{
    uint8_t v = ctl_scale(last_raw[ch], heel[ch], toe[ch]);
    uint8_t d;

    if (sent[ch] == NO_VALUE)
    {
        sent[ch] = v;
        return 0;
    }

    d = (uint8_t)((v > sent[ch]) ? v - sent[ch] : sent[ch] - v);

    if (d == 0 || (d < SW_PEDAL_STEP && v != 0 && v != 127))
    {
        return 0;
    }

    sent[ch] = v;
    return 1;
}

// A pedal reading on a SWITCH page: once it has moved, its CC value becomes the relay's
// trigger value for that pedal (0 turns the trigger off). The display shows it and the
// EXP LED shows whether it is on.
static void sw_pedal(uint8_t ch)
{
    if (!pedal_moved(ch))
    {
        return;
    }

    sw_thr[(uint8_t)((cfg_page - PAGE_SW1) * 2 + ch)] = sent[ch];
    config_changed();
    hal_display(sent[ch]);
    hal_aux_led((uint8_t)(HAL_LED_EXP_1 + ch), sent[ch] != 0);
}

// Footswitch pressed on the modes page: A switch that is not selected is selected and its
// mode shown. The selected switch moves to its next mode when it is released (mode_cycle),
// unless it was held long enough to open its editor instead.
static void mode_press(uint8_t index)
{
    mode_press_live = 1;

    if (index == cfg_sel)
    {
        mode_cycle_pending = 1;
        return;
    }

    mode_cycle_pending = 0;
    cfg_sel = index;
    config_show();
}

// The selected switch released after a short press on the modes page: its next mode.
static void mode_cycle(uint8_t index)
{
    uint8_t m = (uint8_t)((pedal_mode_get(index) + 1) & (PM_COUNT - 1));

    pedal_mode_set(index, m);
    cfg_dirty |= (uint16_t)((uint16_t)1 << index);
    config_changed();
    config_show();
}

// Open the editor for footswitch cfg_sel (a long press on the modes page), on its type.
static void edit_begin(void)
{
    cal_heel_taken = 0;
    edit_open   = 1;
    edit_param  = EP_TYPE;
    edit_typing = 0;
    edit_bak[0]  = msg[MSG_AT(cfg_sel)];
    edit_bak[1]  = msg[MSG_AT(cfg_sel) + 1];
    edit_bak_off = msg[MSG_AT(cfg_sel) + 2];
    resync_pedals();
    edit_show();
}

// Swap the edited switch's three bytes with the saved ones.
static void edit_swap(void)
{
    uint8_t t = msg[MSG_AT(cfg_sel)];

    msg[MSG_AT(cfg_sel)] = edit_bak[0];
    edit_bak[0] = t;
    t = msg[MSG_AT(cfg_sel) + 1];
    msg[MSG_AT(cfg_sel) + 1] = edit_bak[1];
    edit_bak[1] = t;
    t = msg[MSG_AT(cfg_sel) + 2];
    msg[MSG_AT(cfg_sel) + 2] = edit_bak_off;
    edit_bak_off = t;
}

// Close the editor: accept = 1 (a long UP [ENTER], or leaving configuration mode with a
// save) keeps the new message, 0 (a long DOWN [ESCAPE]) puts the old one back. Like a mode
// change, a changed message puts the switch in its mode's resting state when configuration
// mode is saved (cfg_dirty). If the switch is on, the old message's "off" (its CC off
// value or note off) is sent now, while the old message is still known, and the switch is
// off (a program change sends nothing).
static void edit_end(uint8_t accept)
{
    if (!accept)
    {
        msg[MSG_AT(cfg_sel)]     = edit_bak[0];
        msg[MSG_AT(cfg_sel) + 1] = edit_bak[1];
        msg[MSG_AT(cfg_sel) + 2] = edit_bak_off;
        cfg_check_due = 1;
    }
    else if (msg[MSG_AT(cfg_sel)] != edit_bak[0] || msg[MSG_AT(cfg_sel) + 1] != edit_bak[1] ||
             msg[MSG_AT(cfg_sel) + 2] != edit_bak_off)
    {
        cfg_dirty |= (uint16_t)((uint16_t)1 << cfg_sel);

        if ((stomp_state >> cfg_sel) & 1)
        {
            // Not for a program change: nothing is left sounding, and sending its "off"
            // program would change the receiver's program for no reason.
            edit_swap();

            if (hal_midi_space() >= 3 && msg_type(cfg_sel) != MT_PROG)
            {
                send_switch(cfg_sel, 0);
            }

            edit_swap();

            stomp_state &= (uint16_t)~((uint16_t)1 << cfg_sel);
        }
    }

    edit_open   = 0;
    edit_typing = 0;
    resync_pedals();
    config_show();
}

// Short UP / DOWN in the editor: the next / previous parameter, not wrapping (a program
// change stops at its number).
static void edit_step(uint8_t code)
{
    if (code == CTL_KEY_UP)
    {
        if (edit_param >= ((msg_type(cfg_sel) == MT_PROG) ? EP_ON : EP_OFF))
        {
            return;
        }

        edit_param++;
    }
    else
    {
        if (edit_param == EP_TYPE)
        {
            return;
        }

        edit_param--;
    }

    edit_typing = 0;
    resync_pedals();
    edit_show();
}

static void edit_set_type(uint8_t t)
{
    msg[MSG_AT(cfg_sel)]     = (uint8_t)((msg[MSG_AT(cfg_sel)] & 0x7F) | ((t & 1) << 7));
    msg[MSG_AT(cfg_sel) + 1] = (uint8_t)((msg[MSG_AT(cfg_sel) + 1] & 0x7F) | ((t & 2) << 6));
    cfg_check_due = 1;
    edit_show();
}

static void edit_set_value(uint8_t v)
{
    EDIT_BYTE = (uint8_t)((EDIT_BYTE & 0x80) | v);
    cfg_check_due = 1;
    edit_show();
}

// Footswitch in the editor. Type: switch 1 CC, 2 note, 3 program change (others do
// nothing). Values: switches 1-9 type digits 1-9 and switch 10 types 0; each digit is
// added to the number being typed while that stays within 127 (1, 2, 7 make 127),
// otherwise it starts a new one. The number ends once no further digit would fit (13 and
// up), so the next digit starts a new one; a pedal or a parameter change ends it too.
static void edit_digit(uint8_t index)
{
    uint8_t v;

    if (edit_param == EP_TYPE)
    {
        if (index <= MT_PROG)
        {
            edit_set_type(index);
        }

        return;
    }

    index = (uint8_t)((index == STOMP_COUNT - 1) ? 0 : index + 1);
    v = (uint8_t)(EDIT_BYTE & 0x7F);

    if (edit_typing && (v < 12 || (v == 12 && index <= 7)))
    {
        index = (uint8_t)(v * 10 + index);
    }

    edit_typing = (index < 13) ? 1 : 0;
    edit_set_value(index);
}

// A pedal in the editor, once moved: its CC value sets the value, or for the type, the
// first third of its travel chooses CC, the middle note and the last program change.
static void edit_pedal(uint8_t ch)
{
    if (!pedal_moved(ch))
    {
        return;
    }

    edit_typing = 0;

    if (edit_param == EP_TYPE)
    {
        ch = (uint8_t)((sent[ch] >= EDIT_TYPE_PROG_FROM) ? MT_PROG :
                       (sent[ch] >= EDIT_TYPE_NOTE_FROM) ? MT_NOTE : MT_CC);

        if (ch != msg_type(cfg_sel))
        {
            edit_set_type(ch);
        }

        return;
    }

    edit_set_value(sent[ch]);
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

    midi_channel = d;
    config_changed();

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
    edit_open = 0;
    cfg_check_due = 0;
    rx_flash  = 0;
    cfg_changed  = 0;
    cfg_led_dark = 0;

    hal_status_led(1);
    hal_aux_led(HAL_LED_DIRECT_SELECT, 0);
    config_goto_page(PAGE_MODES);
    config_changed();
}

// Take the settings from the four bytes loaded into hal_rec, keeping the default
// channel if the saved one is out of range. Every 2-bit pedal mode value is valid.
static void settings_unpack(void)
{
    if (hal_rec[0] >= 1 && hal_rec[0] <= 16)
    {
        midi_channel = hal_rec[0];
    }

    pedal_modes[0] = hal_rec[1];
    pedal_modes[1] = hal_rec[2];
    pedal_modes[2] = (uint8_t)(hal_rec[3] & 0x0F);
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

    if (hal_rec_load(HAL_REC_CAL))
    {
        heel[0] = hal_rec[0];
        toe[0]  = hal_rec[1];
        heel[1] = hal_rec[2];
        toe[1]  = hal_rec[3];
    }

    // Footswitch messages: a CC from STOMP_CC_LIST (on 127, off 0) unless saved.
    for (i = 0; i < STOMP_COUNT; i++)
    {
        MSG_REC_LOAD(i);
        msg[MSG_AT(i)]     = hal_rec[1];
        msg[MSG_AT(i) + 1] = hal_rec[2];
        msg[MSG_AT(i) + 2] = hal_rec[3];
    }

    // Relay triggers: none by default. A saved value over 127 is ignored (off).
    for (i = 0; i < 2; i++)
    {
        sw_mask[i]        = 0;
        sw_thr[i * 2]     = 0;
        sw_thr[i * 2 + 1] = 0;

        if (hal_rec_load((uint8_t)(HAL_REC_SW1 + i)))
        {
            sw_mask[i]        = (uint16_t)(hal_rec[0] | ((uint16_t)(hal_rec[1] & 0x07) << 8));
            sw_thr[i * 2]     = (hal_rec[2] <= 127) ? hal_rec[2] : 0;
            sw_thr[i * 2 + 1] = (hal_rec[3] <= 127) ? hal_rec[3] : 0;
        }
    }

    if (hal_rec_load(HAL_REC_CFG))
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

    // An open footswitch editor keeps its changes (UP + DOWN held saves and leaves).
    if (edit_open)
    {
        edit_end(1);
    }

    // Stop a two-digit channel entry that could still take a second digit.
    chan_digit_pending = 0;
    // Leaving from page 7 ends the lamp test and its dots.
    hal_lamp_test(HAL_LAMP_NORMAL);
    hal_display_dots(0);

    if (save)
    {
        hal_rec[0] = midi_channel;
        hal_rec[1] = pedal_modes[0];
        hal_rec[2] = pedal_modes[1];
        hal_rec[3] = pedal_modes[2];

        // If a write fails the new settings still apply until power-off.
        hal_rec_save(HAL_REC_CFG);

        hal_rec[0] = heel[0];
        hal_rec[1] = toe[0];
        hal_rec[2] = heel[1];
        hal_rec[3] = toe[1];
        hal_rec_save(HAL_REC_CAL);

        for (i = 0; i < 2; i++)
        {
            hal_rec[0] = (uint8_t)sw_mask[i];
            hal_rec[1] = (uint8_t)(sw_mask[i] >> 8);
            hal_rec[2] = sw_thr[i * 2];
            hal_rec[3] = sw_thr[i * 2 + 1];
            hal_rec_save((uint8_t)(HAL_REC_SW1 + i));
        }

        for (i = 0; i < STOMP_COUNT; i++)
        {
            hal_rec[0] = msg_type(i);
            hal_rec[1] = (uint8_t)(msg[MSG_AT(i)] & 0x7F);
            hal_rec[2] = (uint8_t)(msg[MSG_AT(i) + 1] & 0x7F);
            hal_rec[3] = msg[MSG_AT(i) + 2];
            hal_rec_save((uint8_t)(HAL_REC_MSG0 + i));
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

        // A switch whose mode or message changed goes to its new mode's resting state: on
        // for inverted momentary, off for the others and for a single program. If that
        // changes its state its message is sent (through the revert queue, so as soon as
        // MIDI OUT has room, and the LED follows); if it is already there nothing is sent.
        if (cfg_dirty & bit)
        {
            if (pedal_mode_get(i) == PM_MOM_INV && !prog_single(i))
            {
                stomp_prev |= bit;
            }
            else
            {
                stomp_prev &= (uint16_t)~bit;
            }

            if ((stomp_state ^ stomp_prev) & bit)
            {
                stomp_revert |= bit;
            }
            else
            {
                stomp_revert &= (uint16_t)~bit;
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
    hal_aux_led(HAL_LED_SELECT, 0);
    hal_aux_led(HAL_LED_NUMBER, 0);
    hal_aux_led(HAL_LED_VALUE_1, 0);
    hal_aux_led(HAL_LED_VALUE_2, 0);

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
    else if (edit_open)
    {
        edit_step(code);
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
    edit_open     = 0;
    edit_typing   = 0;
    cfg_check_due = 0;
    mode_press_live    = 0;
    mode_cycle_pending = 0;
    last_raw[0]   = 0;
    last_raw[1]   = 0;
    resync_pedals();

    for (i = 0; i < STOMP_COUNT; i++)
    {
        if (pedal_mode_get(i) == PM_MOM_INV && !prog_single(i))
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
    hal_aux_led(HAL_LED_SELECT, 0);
    hal_aux_led(HAL_LED_NUMBER, 0);
    hal_aux_led(HAL_LED_VALUE_1, 0);
    hal_aux_led(HAL_LED_VALUE_2, 0);
    hal_relays(0);
    show_program();

    // Power-on: every LED lit while "riban" scrolls once (as page 7 starts), then run
    // mode. A key press ends it early and then acts as usual.
    boot_anim = 1;
    ver_step  = 0;
    ver_ticks = VER_STEP_TICKS;
    version_show();
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

    send_switch(index, on);

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

            // In the footswitch editor they accept or undo its changes instead.
            if (edit_open)
            {
                edit_end((code == CTL_KEY_UP) ? 1 : 0);
            }
            else
            {
                exit_config((code == CTL_KEY_UP) ? 1 : 0);
            }
        }

        return;
    }

    // Modes page: a long press of a footswitch opens its editor (the press has selected
    // it, and its release then changes nothing).
    if (mode == MODE_CONFIG)
    {
        if (cfg_page == PAGE_MODES && !edit_open && mode_press_live && index == cfg_sel && keys_down == 0)
        {
            mode_press_live    = 0;
            mode_cycle_pending = 0;
            edit_begin();
        }

        return;
    }

    if (index == 0xFF)
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

    // Page 7: a footswitch release carries on from the segment it held.
    if (ver_paused && mode == MODE_CONFIG)
    {
        ver_paused = 0;
        return;
    }

    // Modes page: releasing the selected switch after a short press changes its mode.
    if (mode == MODE_CONFIG)
    {
        if (mode_press_live && index == cfg_sel)
        {
            mode_press_live = 0;

            if (mode_cycle_pending)
            {
                mode_cycle_pending = 0;
                mode_cycle(index);
            }
        }

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

    // Any key ends the power-on animation, then is handled as usual.
    if (boot_anim)
    {
        boot_end();
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
            if (edit_open)
            {
                edit_digit(stomp_index(code));
            }
            else
            {
                mode_press(stomp_index(code));
            }
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
        // Page 7: a footswitch shows its segment and holds it until released.
        else if (cfg_page == PAGE_VERSION)
        {
            version_jump(stomp_index(code));
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

        // A single-program switch acts as momentary whatever its mode: lit while held,
        // its program sent on the press and nothing on the release.
        on = prog_single(index) ? PM_MOMENTARY : pedal_mode_get(index);

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

        send_switch(index, on);
        hal_stomp_led(index, on);
    }
}

// Whether an incoming CC or note (rx_status, value) turns footswitch i on. A CC value equal
// to the switch's on value is on, equal to its off value off, and any other value is on
// from 64 up; a note on with a velocity above 0 is on, a note off or a note on with
// velocity 0 is off. A leaf function, so its locals cost no fixed RAM.
static uint8_t remote_on(uint8_t i, uint8_t value)
{
    i = (uint8_t)(MSG_AT(i));

    if ((rx_status & 0xF0) != 0xB0)
    {
        return (uint8_t)(((rx_status & 0xF0) == 0x90 && value != 0) ? 1 : 0);
    }

    if (value == (msg[i + 1] & 0x7F))
    {
        return 1;
    }

    if (value == msg[i + 2])
    {
        return 0;
    }

    return (uint8_t)((value >= 64) ? 1 : 0);
}

// Apply an incoming control change or note (rx_status) to every footswitch sending that
// CC or note (remote_on). CHORD_CC is on from 64 up. No locals beyond the loop count:
// ctl.c's directly addressed variables must stay small enough for the gap below the bit
// area (0x08-0x1F), or the linker moves them all above it and the stack starts higher.
static void remote_cc(uint8_t cc, uint8_t value)
{
    uint8_t i;

    // In configuration mode the LEDs show the settings, so remote changes wait.
    if (mode != MODE_RUN)
    {
        return;
    }

    // 0xB0 (CC) has bit 5 set, 0x90 and 0x80 (notes) do not.
    for (i = 0; i < STOMP_COUNT; i++)
    {
        if ((msg[MSG_AT(i)] & 0x7F) == cc && msg_type(i) == ((rx_status & 0x20) ? MT_CC : MT_NOTE))
        {
            if (remote_on(i, value))
            {
                stomp_state |= (uint16_t)((uint16_t)1 << i);
            }
            else
            {
                stomp_state &= (uint16_t)~((uint16_t)1 << i);
            }

            hal_stomp_led(i, (uint8_t)((stomp_state >> i) & 1));
        }
    }

    // CHORD_CC sets the DIRECT SELECT state and LED the same way (relays that follow it
    // update on the next tick). Nothing is sent back.
    if ((rx_status & 0x20) && cc == CHORD_CC)
    {
        chord_cc_on = (value >= 64) ? 1 : 0;
        hal_aux_led(HAL_LED_DIRECT_SELECT, chord_cc_on);
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

    // Only control changes and notes on our channel are of interest; everything else,
    // including program changes, SysEx data and other channels, is ignored.
    if ((uint8_t)(rx_status & 0x0F) != (uint8_t)(midi_channel - 1) ||
        ((rx_status & 0xF0) != 0xB0 && (rx_status & 0xF0) != 0x90 && (rx_status & 0xF0) != 0x80))
    {
        return;
    }

    if (!rx_have_data1)
    {
        rx_data1      = byte;
        rx_have_data1 = 1;
        return;
    }

    // Running status: The next pair of data bytes is another message of the same kind.
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
        // In the footswitch editor a pedal sets the parameter; on a SWITCH page it sets
        // that relay's trigger value. Nothing is sent.
        if (edit_open)
        {
            edit_pedal(channel);
        }
        else if (IS_SW_PAGE(cfg_page))
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
    // Config mode: an editor change is compared with the EEPROM, then with changes the
    // CONFIG LED flashes.
    else if (cfg_check_due)
    {
        cfg_check_due = 0;
        config_changed();
    }
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

    if (boot_anim || (mode == MODE_CONFIG && cfg_page == PAGE_VERSION))
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

    if (what >= CTL_INSPECT_MSG && what < CTL_INSPECT_MSG + STOMP_COUNT * MSG_BYTES)
    {
        return msg[what - CTL_INSPECT_MSG];
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
    case CTL_INSPECT_EDIT:          return edit_open ? (uint8_t)(edit_param + 1) : 0;
    default:                        return 0;
    }
}
#endif
