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
//                           held, the number auto-repeats on the display and is sent on release.
//                           With DIRECT SELECT lit (UP + DOWN tapped), the next / previous
//                           footswitch bank instead, chosen on release
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

// 30 banks at most (user's request): the EEPROM layout has room for 36, and banks 31-36
// stay spare for later settings, so a future layout need not move the user's banks.
#if BANK_COUNT < 1 || BANK_COUNT > 30
#error BANK_COUNT must be 1 to 30
#endif

#if NOTE_BANK_1 > BANK_COUNT || NOTE_BANK_2 > BANK_COUNT
#error NOTE_BANK_1 and NOTE_BANK_2 must be 0 (none) or a bank up to BANK_COUNT
#endif
#if NOTE_BANK_1_FIRST > 127 || NOTE_BANK_2_FIRST > 127
#error NOTE_BANK_1_FIRST and NOTE_BANK_2_FIRST must be notes (0 to 127)
#endif

// Configuration pages
// (user's order: modes, channel, EXP A, EXP B, SWITCH 1, SWITCH 2, program, version)
#define PAGE_MODES      0
#define PAGE_CHANNEL    1
#define PAGE_EXP_A      2
#define PAGE_EXP_B      3
#define PAGE_SW1        4
#define PAGE_SW2        5
#define PAGE_BANK_PC    6
#define PAGE_VERSION    7
#define PAGE_COUNT      8
// Pages 3 and 4, EXP A / EXP B, and pages 5 and 6, SWITCH 1 / 2 (one compare each: the
// subtraction wraps for the pages below).
#define IS_EXP_PAGE(p)  ((uint8_t)((p) - PAGE_EXP_A) < 2)
#define IS_SW_PAGE(p)   ((uint8_t)((p) - PAGE_SW1) < 2)
// The EXP pages' footswitches (user's layout, the same on both): 1 pickup, 2 CC, 3
// calibration; as footswitch indexes.
#define PED_PICKUP      0
#define PED_CC          1
#define PED_CAL         2

// A relay's follow mask: bits 0-9 are footswitches 1-10, this bit is bank mode (the UP +
// DOWN toggle, DIRECT SELECT lit).
// Every footswitch's bit.
#define ALL_SWITCHES    (((uint16_t)1 << STOMP_COUNT) - 1)
// On a SWITCH page a pedal must move this many CC steps (or reach 0 or 127) to set a
// new trigger value, so ADC noise does not keep changing it.
#define SW_PEDAL_STEP   2

// UP + DOWN chord: Not pressed together, pressed together and still undecided, or already
// acted on and waiting for both keys to be let go.
#define CHORD_NONE      0
#define CHORD_ACTIVE    1
#define CHORD_DONE      2

// The EXP pages' calibration footswitch (user's design), each press moving on: 0 the
// pedal's value shown, live; 1 "Lo" flashing, waiting for the heel reading; 2 heel taken
// (kept in cal_low), "Hi" flashing, waiting for the toe; 3 range too small, "Er" for
// CAL_ERROR_SHOW_MS, then the value again (the old calibration kept).
#define CAL_DONE        0
#define CAL_HEEL        1
#define CAL_TOE         2
#define CAL_ERROR       3
#if CAL_ERROR_SHOW_MS % CHANNEL_DIGIT_FLASH_MS || CAL_ERROR_SHOW_MS / CHANNEL_DIGIT_FLASH_MS < 1 || CAL_ERROR_SHOW_MS / CHANNEL_DIGIT_FLASH_MS > 255
#error "CAL_ERROR_SHOW_MS must be 1 to 255 times CHANNEL_DIGIT_FLASH_MS"
#endif
#define CAL_ERROR_BLINKS (CAL_ERROR_SHOW_MS / CHANNEL_DIGIT_FLASH_MS)

#define NAV_NONE        0xFF

#define KEYBIT_UP       0x01
#define KEYBIT_DOWN     0x02

// Footswitch message types (two bits, kept in bit 7 of its first two msg[] bytes).
#define MT_CC           0
#define MT_NOTE         1
#define MT_PROG         2
// Program change switches have two programs, in the on and off values (as a CC's two
// values): the on value is sent when the switch turns on and the off value when it turns
// off, so it toggles between them (following its pedal mode). The number is not used. With both the same it is a single program, sent on every press and
// nothing on release, whatever the pedal mode; its LED is lit only while held.

// Parameters in the footswitch editor, each marked by its panel LED: message type
// (SELECT), number (NUMBER: the CC, note or program number), on value (VALUE 1: the CC
// value or note velocity sent when on) and off value (VALUE 2: sent when off). A program
// change uses VALUE 1 and VALUE 2 for its two programs and skips NUMBER. The parameter is
// also the msg[] byte it edits, plus one.
#define EP_TYPE         0
#define EP_NUM          1
#define EP_ON           2
#define EP_OFF          3

static const CODE uint8_t stomp_cc[STOMP_COUNT] = { STOMP_CC_LIST };
// The note banks' notes above their first, footswitch 1 to 10 (NOTE_BANK_SCALE).
static const CODE uint8_t note_scale[STOMP_COUNT] = { NOTE_BANK_SCALE };
// Each bank's pedal settings until it is saved: no program change, then pedal A's and
// B's CC from PEDAL_CC_LIST with its pickup default in bit 7 (the packed form below).
#define PCC_A_(a, b)    (a)
#define PCC_B_(a, b)    (b)
#define PCC_A_OF(l)     PCC_A_(l)
#define PCC_B_OF(l)     PCC_B_(l)
static const CODE uint8_t bank_def[3] = {
    0, (uint8_t)(PCC_A_OF(PEDAL_CC_LIST) | (PEDAL_PICKUP_A << 7)), (uint8_t)(PCC_B_OF(PEDAL_CC_LIST) | (PEDAL_PICKUP_B << 7))
};
static const CODE uint8_t fw_version_dots[3]    = { 0x01, 0x05, 0x04 };
// The "riban" scroll moves the strip ". riban ." (user's design) one place left per frame
// across the display's three dots, left to right "+1.8.8.": bit 2 after the "1", bit 1
// between the digits, bit 0 after the units. The leading dot comes in alone (two lead-in
// frames, far right then between the digits), so " r" has it after the "1", and it is
// gone from "ri"; the trailing dot follows "n " at the far right, then runs off through
// the other two (two tail frames, display blank).
// The dot of each text frame:
static const CODE uint8_t scroll_dots[HAL_TEXT_SCROLL_FRAMES] = { 0x04, 0x00, 0x00, 0x00, 0x00, 0x01 };

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

// Scan ticks the MIDI FUNCTION LED stays lit after a message is sent.
#define TX_FLASH_TICKS       (MIDI_TX_FLASH_MS / TICK_MS)

#if TX_FLASH_TICKS < 1 || TX_FLASH_TICKS > 255
#error MIDI_TX_FLASH_MS is out of range
#endif

// Pedal pickup: the EXP LED's flash half-period at the pickup point, and how long the
// distance stays on the display after the last move.
#define PICKUP_FLASH_TICKS   (PEDAL_PICKUP_FLASH_MS / TICK_MS)
#define PICKUP_SHOW_TICKS    (PEDAL_PICKUP_SHOW_MS / TICK_MS)

#if PICKUP_FLASH_TICKS < 1 || PICKUP_FLASH_TICKS > 64 || PICKUP_SHOW_TICKS < 1 || PICKUP_SHOW_TICKS > 65535
#error PEDAL_PICKUP_FLASH_MS (2 to 128) or PEDAL_PICKUP_SHOW_MS is out of range
#endif
#if PEDAL_PICKUP_A > 1 || PEDAL_PICKUP_B > 1 || PEDAL_PICKUP_WINDOW > 127
#error PEDAL_PICKUP_A / _B must be 0 or 1, PEDAL_PICKUP_WINDOW 0 to 127
#endif

// Scan ticks per CONFIG LED flash half-period once a setting has changed.
#define CFG_FLASH_TICKS      (CONFIG_CHANGED_FLASH_MS / TICK_MS)

#if CFG_FLASH_TICKS < 1 || CFG_FLASH_TICKS > 255
#error CONFIG_CHANGED_FLASH_MS is out of range
#endif

// Page 8 runs in 50ms steps, in ten segments, one per footswitch (user's design), each
// shown from its start by a footswitch press:
//   0-4  the scroll frames " r", "ri", "ib", "ba", "an" (VER_SCROLL_STEPS each)
//   5    "n ", then the trailing dot running off (VER_TAIL_FRAMES more frames)
//   6-8  major, minor, build (VER_ITEM_STEPS each)
//   9    a blank (VER_ITEM_STEPS), then the lead-in (VER_LEAD_FRAMES frames)
// then the scroll again. Every LED is lit throughout.
#define VER_STEP_MS          50
#define VER_STEP_TICKS       (VER_STEP_MS / TICK_MS)
// The bank program page shows "Pr" and the program in turn, each for this many steps.
#define BANK_LABEL_STEPS     (BANK_PC_LABEL_MS / VER_STEP_MS)
#define BANK_LABEL_ON        0x80

#if BANK_PC_LABEL_MS % VER_STEP_MS != 0 || BANK_LABEL_STEPS < 1 || BANK_LABEL_STEPS > 127
#error BANK_PC_LABEL_MS must be a multiple of 50, from 50 to 6350
#endif
#define VER_ITEM_STEPS       (VERSION_SHOW_MS / VER_STEP_MS)
#define VER_SCROLL_STEPS     (SCROLL_FRAME_MS / VER_STEP_MS)
#define VER_TAIL_FRAMES      2
#define VER_LEAD_FRAMES      2
// Start of segment 5 ("n "), end of its tail (the scroll), start of the blank, the end.
#define VER_LAST_TEXT        ((HAL_TEXT_SCROLL_FRAMES - 1) * VER_SCROLL_STEPS)
#define VER_SCROLL_END       (VER_LAST_TEXT + (1 + VER_TAIL_FRAMES) * VER_SCROLL_STEPS)
#define VER_BLANK_START      (VER_SCROLL_END + 3 * VER_ITEM_STEPS)
#define VER_END_STEP         (VER_BLANK_START + VER_ITEM_STEPS + VER_LEAD_FRAMES * VER_SCROLL_STEPS)
// Power-on and page 8 start at the lead-in.
#define VER_START_STEP       (VER_END_STEP - VER_LEAD_FRAMES * VER_SCROLL_STEPS)
// At power-on only the lead-in and the scroll run, once, then run mode.
#define VER_BOOT_END_STEP    VER_SCROLL_END
// At power-on every LED is lit for the first VER_BOOT_LAMP_STEPS (from the lead-in, the
// settling included), then normal.
#define VER_BOOT_LAMP_STEPS  (BOOT_LAMP_MS / VER_STEP_MS)
// Footswitch N's LED is dark for the first VER_FLASH_STEPS of segment N.
#define VER_SEGMENTS         (HAL_TEXT_SCROLL_FRAMES + 4)
#define VER_FLASH_STEPS      ((VER_SCROLL_STEPS + 1) / 2)

#if VER_SEGMENTS != STOMP_COUNT
#error page 8 needs one segment per footswitch
#endif

#if VER_STEP_TICKS < 1 || VER_STEP_TICKS > 255 || VER_SCROLL_STEPS < 2 || \
    VERSION_SHOW_MS % VER_STEP_MS != 0 || SCROLL_FRAME_MS % VER_STEP_MS != 0 || \
    VER_END_STEP > 255 || STARTUP_TICKS / VER_STEP_TICKS >= VER_BOOT_END_STEP + VER_LEAD_FRAMES * VER_SCROLL_STEPS
#error VERSION_SHOW_MS or SCROLL_FRAME_MS is out of range (see config.h)
#endif

#if BOOT_LAMP_MS % VER_STEP_MS != 0 || VER_BOOT_LAMP_STEPS > 255 - VER_LEAD_FRAMES * VER_SCROLL_STEPS
#error BOOT_LAMP_MS is out of range (see config.h)
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
// Footswitch messages, three bytes per switch: msg[i * 3] is the number (CC or note;
// unused for a program change) with type bit 0 in bit 7, msg[i * 3 + 1] the on value (CC
// value, note velocity or program) with type bit 1 in bit 7, msg[i * 3 + 2] the off value. Each switch has its
// own EEPROM record (type, number, on value, off value).
#define MSG_BYTES       3
// Index of switch i's first msg[] byte, i * 3 as 8-bit shift and add: SDCC would compute
// i * MSG_BYTES with a call to its 16-bit multiply (a call level deeper, and slower).
#define MSG_AT(i)       ((uint8_t)((uint8_t)((uint8_t)(i) << 1) + (uint8_t)(i)))
static XDATA uint8_t  msg[STOMP_COUNT * MSG_BYTES];

// The last value of every CC and note, wherever it came from (a footswitch sending it,
// MIDI IN, or the state a switch showed when its bank was left): index the number, plus
// 0x80 for a note. A CC's entry is its 0-127 value, a note's 0x80 + its state (1 on),
// and 0xFF means never seen. A bank change gives each new switch the state its CC or
// note last had (remote_on), however many banks ago, so a CC that is on stays on. In
// the external SRAM (256 bytes).
static XDATA uint8_t  key_val[256];
#define KEY_UNKNOWN     0xFF

// The program change sent when the bank in use is chosen with UP / DOWN in bank mode (or
// over MIDI): 0 none, otherwise the program + 1 (only bits 0-6 of that program are used,
// so any byte is valid). Saved with the bank (see rec_pack).
static XDATA uint8_t  bank_pc;
#define BANK_PC_PROGRAM ((uint8_t)((bank_pc - 1) & 0x7F))
// Switch i's key_val index (not for a program change).

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
// EXP pages: the page's pedal (0 A, 1 B), and its CC editor open
// (footswitch 6 / 7). The CC when it opened is kept in edit_bak[0]; edit_typing is shared
// with the footswitch editor.
static BIT_T          pcc_b;
static BIT_T          pcc_open;

// Settings are loaded and saved through the HAL's one record buffer, hal_rec.

// Configuration over MIDI: NRPN numbers (described with the code, below).
#define NRPN_MSB        0x46
#define NRPN_NONE       0xFF

// Parameters (the NRPN LSB). Gaps are reserved and ignored.
#define NP_CHANNEL      0       // MIDI channel 1-16
#define NP_VERSION      1       // 1-3: firmware major, minor, build (read only)
#define NP_COMMAND      4       // a command (NC_*), never sent
#define NP_DUMP_END     5       // sent (value 0) after the last parameter of a dump
#define NP_BANK         6       // footswitch bank 1 to BANK_COUNT; setting it changes bank
#define NP_BANKS        7       // BANK_COUNT (read only)
#define NP_MODE         8       // + switch 0-9: pedal mode 0-3
#define NP_TYPE         20      // + switch: message type 0 CC, 1 note, 2 program change
#define NP_NUM          30      // + switch: number (CC or note; unused for a program change)
#define NP_ON           40      // + switch: on value (a program change's program sent on)
#define NP_OFF          50      // + switch: off value (a program change's program sent off)
#define NP_FOLLOW1      60      // + switch 0-9: SWITCH 1 follows it, 0 / 1 (70 was program mode, gone)
#define NP_FOLLOW2      72      // the same for SWITCH 2
#define NP_TRIGGER      84      // + relay * 2 + pedal: trigger value 1-127, 0 = off
#define NP_CAL          88      // heel A, toe A, heel B, toe B: raw readings 0-255 (read only)
#define NP_BANK_PC      92      // the bank's program change 0-127; setting it turns it on
#define NP_BANK_PC_ON   93      // 1 if the bank sends its program change, 0 none ("--")
#define NP_PICKUP       94      // + pedal 0-1: 1 pickup mode, 0 sends at once
#define NP_PEDAL_CC     96      // + pedal 0-1: the pedal's CC 0-127
#define NP_COUNT        98

// Commands (values of NP_COMMAND).
#define NC_DUMP         1       // send every parameter, then NP_DUMP_END
#define NC_SAVE         2       // store the settings, as leaving configuration mode does
#define NC_RELOAD       3       // drop unsaved changes: back to what the EEPROM holds
#define NC_HEEL         4       // the pedals' positions now become the heel ...
#define NC_TOE          5       // ... or the toe, if each is CAL_MIN_SPAN from the other end

// Parameter kinds.
#define NK_NONE         0
#define NK_SET          1
#define NK_READ         2

// A dump in progress (midi_job, sent by ctl_tick): 0 none, else the next parameter + 1, and
// NP_COUNT + 1 for the end marker.

// Configuration over MIDI (see "Configuration over MIDI" below): CC 99 selected this
// pedal's NRPN number. The parameter CC 98 selected (nrpn_lsb) and the dump position
// (midi_job) are only used in run mode, so they share the configuration page and
// selected switch (RAM): entering configuration mode sets those afresh, and leaving it
// clears these two.
static BIT_T          nrpn_msb_ok;
#define nrpn_lsb      cfg_sel
#define midi_job      cfg_page

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
// Program mode, toggled by a short UP + DOWN press in run mode (DIRECT SELECT lit; user's
// request). Off (the default): UP / DOWN choose a footswitch bank and the footswitches are
// stomps. On: UP / DOWN step the program and the footswitches choose one by number
// (pc_typing, pc_entry). Nothing is sent for the toggle.
static BIT_T          pc_mode;
// Program mode: footswitch 10 started typing a program number ("--" until a digit), and
// the number typed so far (PC_ENTRY_NONE before the first digit). Numbers are as shown
// on the display (the program + DISPLAY_PROGRAM_OFFSET).
static BIT_T          pc_typing;
static IDATA uint8_t  pc_entry;
// The right-hand dot flashes while a number is being typed (user's request); this is
// whether it is shown. Its timer is chan_flash_ticks, idle in run mode once power-on ends.
static BIT_T          pc_dot;
// Page 7: footswitch 10 pressed with no number being typed; its release starts "--".
static BIT_T          bank_none_ready;
// Config mode: a free-running blink, toggled every CHAN_FLASH_TICKS (blink_ticks), for
// the right-hand dot while a number is entered (user's request) and a flashing Lo / Hi.
static BIT_T          blink_on;
static IDATA uint8_t  blink_ticks;
// EXP pages: calibration waiting for a reading (cal_step CAL_HEEL or CAL_TOE), its Lo /
// Hi flashing; the heel reading taken, kept until the toe is good; and the blinks left
// of a shown "Er".
static BIT_T          cal_armed;
static XDATA uint8_t  cal_low;
static XDATA uint8_t  cal_err_left;
// A long UP asks which bank to save to (user's request): the bank shown (1 to
// BANK_COUNT), typed on the footswitches; short UP saves there, short DOWN goes back.
static BIT_T          save_prompt;
static XDATA uint8_t  save_bank;
#define PC_ENTRY_NONE 0xFF
#define PC_SHOWN_MAX  (127 + DISPLAY_PROGRAM_OFFSET)
// Whether a typed (shown) number is a program: with no offset every number is.
#if DISPLAY_PROGRAM_OFFSET
#define PC_IS_PROGRAM(n) ((n) >= DISPLAY_PROGRAM_OFFSET)
#else
#define PC_IS_PROGRAM(n) 1
#endif
// Footswitch banks: the bank UP / DOWN have chosen (0-based; the display shows it + 1 in
// bank mode), which is the loaded one (hal_bank) except while a change waits in bank_run,
// and whether one is waiting.
static IDATA uint8_t  bank_next;
static BIT_T          bank_busy;

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
// Page 8 reuses both: ticks into the current step, and the step number.
#define ver_ticks chan_flash_ticks
#define ver_step  cal_step
// The footswitch editor's parameter: only open on page 1, where neither is used.
#define edit_param chan_flash_ticks
// EXP pages: the footswitch last pressed, whose setting the display shows.
#define ped_sel chan_flash_ticks
// Page 7 shows "Pr" and the program in turn: steps into the current one (cal_step bits
// 0-6, with ticks in chan_flash_ticks), and BANK_LABEL_ON while "Pr" shows.
#define bank_label cal_step

// Calibrated pedal end points, indexed by pedal channel.
static IDATA uint8_t  heel[2];
static IDATA uint8_t  toe[2];

// Most recent averaged readings, last value sent, and the reading it was sent at.
static IDATA uint8_t  last_raw[2];
static IDATA uint8_t  sent[2];
static IDATA uint8_t  ref_raw[2];
// ref_raw is the run-mode pedal reference (set again from the first reading after
// configuration mode).

// The expression pedals' CCs (pedal A, B), set per bank (pages 3 / 4, NRPN 96 / 97).
static IDATA uint8_t  pedal_cc[2];

// Expression pedal pickup (user's request). pk_mask: bit 0 pedal A, bit 1 pedal B in
// pickup mode (per bank); the others send at once. A pickup
// pedal is picked up while its CC's last known value (key_val: what it sent, a footswitch
// sent or MIDI IN received; 0 if never seen) is what it last sent (sent[]), and waits
// otherwise, sending nothing until it reaches that value (within PEDAL_PICKUP_WINDOW) or
// passes it. pk_pos is its position at the last reading that counted as a move (every
// pedal, so a pedal picked up and then overridden starts from where it was), pk_flash the
// ticks left of the EXP LED's flash half-period, pk_led the LEDs lit (bit per pedal), and
// pk_show the ticks left until the display goes back from the distance to the program.
static IDATA uint8_t  pk_mask;
static IDATA uint8_t  pk_pos[2];
static IDATA uint8_t  pk_flash[2];
static IDATA uint8_t  pk_led;
static IDATA uint16_t pk_show;
// ctl_pedal_sample's pickup target and previous position, and pickup_tick's loop count:
// IDATA statics, so no fixed RAM (the directly addressed gap is full).
static IDATA uint8_t  pk_t;
static IDATA uint8_t  pk_p;
static IDATA uint8_t  pk_act;
static BIT_T          pk_wait;
static IDATA uint8_t  pk_i;
#define PK_MOVED        0
#define PK_KNOWN        1
#define PK_SEND         2

// Relay (SWITCH 1 / 2) triggers, saved per bank: follow masks (one bit per footswitch)
// and pedal trigger values (index relay * 2 + pedal, 0 = off). relay_out is what the
// relays are set to now (bit 0 = SWITCH 1). relay_lvl is what the triggers gave at the
// last tick: a relay only moves when its triggers' result changes (user's request), so a
// bank change, which takes relay_lvl afresh, leaves the relays as they were.
static IDATA uint16_t sw_mask[2];
static IDATA uint8_t  sw_thr[4];
static IDATA uint8_t  relay_out;
static IDATA uint8_t  relay_lvl;
static BIT_T          relay_resync;
// Scan ticks left of the MIDI CHAN flash for received MIDI (run mode only).
static IDATA uint8_t  rx_flash;
// Scan ticks left of the MIDI FUNCTION flash for sent MIDI (run mode only).
static IDATA uint8_t  tx_flash;
// Config mode: a setting has been changed since entry (the CONFIG LED flashes), and
// the LED is in its dark half. The flash timer reuses rx_flash, which is run mode only.
static BIT_T          cfg_changed;
static BIT_T          cfg_led_dark;
// The power-on lamp test and version are running (run mode; uses ver_step / ver_ticks).
static BIT_T          boot_anim;
// Page 8 is held on one segment while its footswitch is down.
static BIT_T          ver_paused;
#define cfg_flash_ticks rx_flash

// Mode of one footswitch, and set it.
// 1 << n as 16 bits: a call is far smaller than SDCC's inline 16-bit shift. A leaf.
static uint16_t bit16(uint8_t n)
{
    uint16_t b = 1;

    while (n--)
    {
        b <<= 1;
    }

    return b;
}

static uint8_t pedal_mode_get(uint8_t index)
{
    return (uint8_t)((pedal_modes[index >> 2] >> ((index & 3) * 2)) & 3);
}

static void pedal_mode_set(uint8_t index, uint8_t value)
{
    uint8_t shift = (uint8_t)((index & 3) * 2);

    pedal_modes[index >> 2] = (uint8_t)((pedal_modes[index >> 2] & ~(3 << shift)) | (value << shift));
}

// ---------------------------------------------------------------
// Saved settings: one way to pack, read back and unpack each record
// ---------------------------------------------------------------
// Every record (HAL_REC_*) has one saved form, which rec_pack makes from the settings.
// rec_read reads a record back in that same form, the way it is taken: the defaults if it
// is missing or invalid, and stray values put right (a footswitch's unused type 3 as a
// CC, bits above 7 cut, a channel outside 1-16 as the default, a trigger over 127 off,
// unused bits 0). rec_unpack applies that form to the settings. So saving is pack and
// store, loading is read and unpack, and the settings match the EEPROM if every record
// packs to what reads back. The record number is rec_n, not a parameter, so these cost
// no fixed RAM (the directly addressed gap is full).
//
// The per-bank records (of bank hal_bank), eleven of them, every bit used:
//   footswitch n (0-9): byte 0 the type (bits 0-1), pedal mode (bits 2-3), whether
//     SWITCH 1 / SWITCH 2 follow it (bits 4, 5), and bits 6-7 a 2-bit piece of the bank's
//     packed pedal settings (below); then the number, on value and off value (7 bits).
//   the bank record: the four relay triggers (relay * 2 + pedal, 7 bits), each with one
//     more bit of the packed pedal settings in bit 7.
// The packed pedal settings are three bytes: 0 bank_pc, 1 pedal A's CC with its pickup
// bit in bit 7, 2 pedal B's the same (BANK_Y). Footswitch n holds bits (n & 3) * 2 of byte
// n >> 2 (so byte 0 in records 0-3, byte 1 in 4-7, byte 2's bits 0-3 in 8-9), the bank
// record's bit 7 of trigger k bit 4 + k of byte 2. msg[] keeps the type in bit 7 of the
// number and the on value instead.
static IDATA uint8_t rec_n;
// The packed record while it is compared with the one read back.
static XDATA uint8_t rec_cmp[4];

#define BANK_Y(j)       ((uint8_t)(((j) == 0) ? bank_pc : \
                         (uint8_t)(pedal_cc[(uint8_t)((j) - 1)] | ((pk_mask & (j)) ? 0x80 : 0))))
#define BANK_Y_SET(j, v)                                                              \
    do {                                                                              \
        if ((j) == 0)                                                                 \
        {                                                                             \
            bank_pc = (v);                                                            \
        }                                                                             \
        else                                                                          \
        {                                                                             \
            pedal_cc[(uint8_t)((j) - 1)] = (uint8_t)((v) & 0x7F);                     \
            pk_mask = (uint8_t)(((v) & 0x80) ? (pk_mask | (j)) : (pk_mask & (uint8_t)~(j))); \
        }                                                                             \
    } while (0)

// The current settings of record rec_n into hal_rec. A leaf.
static void rec_pack(void)
{
    uint8_t  q;
    uint8_t  v;
    uint16_t w;

    if (rec_n < STOMP_COUNT)
    {
        q = MSG_AT(rec_n);
        v = (uint8_t)(rec_n >> 2);
        v = BANK_Y(v);
        w = (uint16_t)1 << rec_n;
        hal_rec[0] = (uint8_t)((msg[q] >> 7) | ((msg[q + 1] >> 6) & 2) |
                               (((pedal_modes[rec_n >> 2] >> ((rec_n & 3) * 2)) & 3) << 2) |
                               ((sw_mask[0] & w) ? 0x10 : 0) | ((sw_mask[1] & w) ? 0x20 : 0) |
                               (uint8_t)((uint8_t)(v >> ((rec_n & 3) << 1)) << 6));
        hal_rec[1] = (uint8_t)(msg[q] & 0x7F);
        hal_rec[2] = (uint8_t)(msg[q + 1] & 0x7F);
        hal_rec[3] = msg[q + 2];
    }
    else if (rec_n == HAL_REC_BANK)
    {
        v = BANK_Y(2);

        for (q = 0; q < 4; q++)
        {
            hal_rec[q] = (uint8_t)(sw_thr[q] | (uint8_t)((uint8_t)(v >> (4 + q)) << 7));
        }
    }
    else if (rec_n == HAL_REC_CAL)
    {
        hal_rec[0] = heel[0];
        hal_rec[1] = toe[0];
        hal_rec[2] = heel[1];
        hal_rec[3] = toe[1];
    }
    else
    {
        hal_rec[0] = midi_channel;
        hal_rec[1] = 0;
        hal_rec[2] = 0;
        hal_rec[3] = 0;
    }
}

// Record rec_n from the EEPROM into hal_rec in its saved form (see above).
static void rec_read(void)
{
    uint8_t q;

    if (!hal_rec_load(rec_n))
    {
        // The defaults: a CC from STOMP_CC_LIST (on 127, off 0) in DEFAULT_PEDAL_MODE,
        // followed by neither relay; the pedal settings of bank_def; no relay triggers;
        // the full pedal range; the default channel (set below).
        hal_rec[0] = 0;
        hal_rec[1] = 0;
        hal_rec[2] = 0;
        hal_rec[3] = 0;

        if (rec_n < STOMP_COUNT)
        {
            hal_rec[0] = (uint8_t)(MT_CC | (DEFAULT_PEDAL_MODE << 2));
            hal_rec[1] = stomp_cc[rec_n];
            hal_rec[2] = 127;
            // The note banks: momentary notes, NOTE_BANK_SCALE above the first.
#if NOTE_BANK_1 > 0
            if (hal_bank == NOTE_BANK_1 - 1)
            {
                hal_rec[0] = (uint8_t)(MT_NOTE | (PM_MOMENTARY << 2));
                hal_rec[1] = (uint8_t)((NOTE_BANK_1_FIRST + note_scale[rec_n]) & 0x7F);
            }
#endif
#if NOTE_BANK_2 > 0
            if (hal_bank == NOTE_BANK_2 - 1)
            {
                hal_rec[0] = (uint8_t)(MT_NOTE | (PM_MOMENTARY << 2));
                hal_rec[1] = (uint8_t)((NOTE_BANK_2_FIRST + note_scale[rec_n]) & 0x7F);
            }
#endif
            q = bank_def[rec_n >> 2];
            hal_rec[0] |= (uint8_t)((uint8_t)(q >> ((rec_n & 3) << 1)) << 6);
        }
        else if (rec_n == HAL_REC_BANK)
        {
            for (q = 0; q < 4; q++)
            {
                hal_rec[q] = (uint8_t)((uint8_t)(bank_def[2] >> (4 + q)) << 7);
            }
        }
        else if (rec_n == HAL_REC_CAL)
        {
            hal_rec[1] = 255;
            hal_rec[3] = 255;
        }
    }

    if (rec_n < STOMP_COUNT)
    {
        if ((hal_rec[0] & 3) == 3)
        {
            hal_rec[0] &= (uint8_t)~3;
        }

        hal_rec[1] &= 0x7F;
        hal_rec[2] &= 0x7F;
        hal_rec[3] &= 0x7F;
    }
    else if (rec_n == HAL_REC_CFG)
    {
        if (hal_rec[0] < 1 || hal_rec[0] > 16)
        {
            hal_rec[0] = MIDI_CHANNEL;
        }

        // Byte 1 held the pickup bits before they moved to the banks.
        hal_rec[1] = 0;
        hal_rec[2] = 0;
        hal_rec[3] = 0;
    }
}

// Record rec_n, read by rec_read, into the settings. The packed pedal settings come in
// pieces, so records 0-9 and the bank record are always unpacked together. A leaf.
static void rec_unpack(void)
{
    uint8_t  q;
    uint8_t  j;
    uint8_t  v;
    uint16_t w;

    if (rec_n < STOMP_COUNT)
    {
        q = MSG_AT(rec_n);
        msg[q]     = (uint8_t)(hal_rec[1] | ((hal_rec[0] & 1) << 7));
        msg[q + 1] = (uint8_t)(hal_rec[2] | ((hal_rec[0] & 2) << 6));
        msg[q + 2] = hal_rec[3];
        q = (uint8_t)((rec_n & 3) * 2);
        pedal_modes[rec_n >> 2] = (uint8_t)((pedal_modes[rec_n >> 2] & ~(3 << q)) |
                                            (((hal_rec[0] >> 2) & 3) << q));
        w = (uint16_t)1 << rec_n;
        sw_mask[0] = (uint16_t)((hal_rec[0] & 0x10) ? (sw_mask[0] | w) : (sw_mask[0] & ~w));
        sw_mask[1] = (uint16_t)((hal_rec[0] & 0x20) ? (sw_mask[1] | w) : (sw_mask[1] & ~w));
        j = (uint8_t)(rec_n >> 2);
        v = BANK_Y(j);
        v = (uint8_t)((v & (uint8_t)~(3 << q)) | (uint8_t)((hal_rec[0] >> 6) << q));
        BANK_Y_SET(j, v);
    }
    else if (rec_n == HAL_REC_BANK)
    {
        v = (uint8_t)(BANK_Y(2) & 0x0F);

        for (q = 0; q < 4; q++)
        {
            sw_thr[q] = (uint8_t)(hal_rec[q] & 0x7F);
            v |= (uint8_t)((uint8_t)(hal_rec[q] >> 7) << (4 + q));
        }

        // Byte 2 is pedal B's: its CC and pickup bit.
        pedal_cc[1] = (uint8_t)(v & 0x7F);
        pk_mask     = (uint8_t)((v & 0x80) ? (pk_mask | 2) : (pk_mask & (uint8_t)~2));
    }
    else if (rec_n == HAL_REC_CAL)
    {
        heel[0] = hal_rec[0];
        toe[0]  = hal_rec[1];
        heel[1] = hal_rec[2];
        toe[1]  = hal_rec[3];
    }
    else
    {
        midi_channel = hal_rec[0];
    }
}

// Records rec_n up to (not including) last from the EEPROM into the settings.
static void load_records(uint8_t last)
{
    for (; rec_n < last; rec_n++)
    {
        rec_read();
        rec_unpack();
    }
}

// Every setting and the calibration from the EEPROM (the defaults for any record missing
// or invalid), the footswitches of bank hal_bank. Used at start-up and to cancel changes.
static void load_settings(void)
{
    rec_n = 0;
    load_records(HAL_RECORDS);
}

// Store every setting and the calibration, the footswitches as bank hal_bank's. If a
// write fails the new settings still apply until power-off.
static void save_settings(void)
{
    for (rec_n = 0; rec_n < HAL_RECORDS; rec_n++)
    {
        rec_pack();
        hal_rec_save(rec_n);
    }
}

// 1 if every setting matches what the EEPROM holds, taken the way loading takes it. So the
// CONFIG LED flashes exactly while a save would change something, and cancel would
// restore what the EEPROM says.
static uint8_t settings_match_saved(void)
{
    for (rec_n = 0; rec_n < HAL_RECORDS; rec_n++)
    {
        rec_pack();
        rec_cmp[0] = hal_rec[0];
        rec_cmp[1] = hal_rec[1];
        rec_cmp[2] = hal_rec[2];
        rec_cmp[3] = hal_rec[3];
        rec_read();

        if (rec_cmp[0] != hal_rec[0] || rec_cmp[1] != hal_rec[1] ||
            rec_cmp[2] != hal_rec[2] || rec_cmp[3] != hal_rec[3])
        {
            return 0;
        }
    }

    return 1;
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
    tx_flash = TX_FLASH_TICKS;
    hal_midi_tx((uint8_t)(0xB0 | (midi_channel - 1)));
    hal_midi_tx((uint8_t)(cc & 0x7F));
    hal_midi_tx((uint8_t)(value & 0x7F));
}

static void send_program(uint8_t value)
{
    tx_flash = TX_FLASH_TICKS;
    hal_midi_tx((uint8_t)(0xC0 | (midi_channel - 1)));
    hal_midi_tx((uint8_t)(value & 0x7F));
}

// Byte k (0 number, 1 on value, 2 off value) of footswitch i's message, without the type
// bit. A leaf.
static uint8_t msg_field(uint8_t i, uint8_t k)
{
    return (uint8_t)(msg[(uint8_t)(MSG_AT(i) + k)] & 0x7F);
}

// Footswitch i's message type set to t (MT_*): bit 0 in bit 7 of the number, bit 1 in bit
// 7 of the on value. A leaf.
static void msg_set_type(uint8_t i, uint8_t t)
{
    i = MSG_AT(i);
    msg[i]     = (uint8_t)((msg[i] & 0x7F) | ((t & 1) << 7));
    msg[i + 1] = (uint8_t)((msg[i + 1] & 0x7F) | ((t & 2) << 6));
}

// Type of footswitch index's message (MT_*).
static uint8_t msg_type(uint8_t index)
{
    return (uint8_t)((msg[MSG_AT(index)] >> 7) | ((msg[MSG_AT(index) + 1] >> 6) & 2));
}

// 1 if footswitch index is a single-program switch: a program change (type bits: bit 7
// clear in its first byte, set in its second) whose two programs, the on and off values,
// are the same. A leaf function, so it costs no fixed RAM.
static uint8_t prog_single(uint8_t index)
{
    index = MSG_AT(index);

    return (uint8_t)((!(msg[index] & 0x80) && (msg[index + 1] & 0x80) &&
                      (uint8_t)(msg[index + 1] & 0x7F) == msg[index + 2]) ? 1 : 0);
}

// The run-mode number: the bank from 1, or in program mode (DIRECT SELECT lit) the program.
static void show_program(void)
{
    hal_display(pc_mode ? (uint8_t)(program + DISPLAY_PROGRAM_OFFSET) : (uint8_t)(bank_next + 1));
}

// The key_val index of footswitch i's CC or note: its number, + 0x80 for a note. A leaf.
static uint8_t switch_key(uint8_t i)
{
    i = MSG_AT(i);

    return (uint8_t)((msg[i] & 0x7F) | (((msg[i] & 0x80) && !(msg[i + 1] & 0x80)) ? 0x80 : 0));
}

// What footswitch i's CC or note says in state on, as remote_cc takes it: a CC's on or off
// value, or for a note 0x80 + on. A leaf.
static uint8_t switch_value(uint8_t i, uint8_t on)
{
    i = MSG_AT(i);

    if ((msg[i] & 0x80) && !(msg[i + 1] & 0x80))
    {
        return (uint8_t)(0x80 | on);
    }

    return (uint8_t)(msg[(uint8_t)(i + (on ? 1 : 2))] & 0x7F);
}

// Footswitch i's state set to on (stomp_state only). A leaf.
static void stomp_put(uint8_t i, uint8_t on)
{
    if (on)
    {
        stomp_state |= (uint16_t)((uint16_t)1 << i);
    }
    else
    {
        stomp_state &= (uint16_t)~((uint16_t)1 << i);
    }
}

// Footswitch i's state (stomp_state bit i). A leaf.
static uint8_t stomp_on(uint8_t i)
{
    return (uint8_t)((stomp_state >> i) & 1);
}

// 1 if footswitch i rests on: inverted momentary, unless it is a single program.
static uint8_t rests_on(uint8_t i)
{
    return (uint8_t)(pedal_mode_get(i) == PM_MOM_INV && !prog_single(i));
}

// Send footswitch index's message for the state on: a CC with the on or off value, a note
// on with the on value as velocity (on) or a note off with the off value (off), or a
// program change, the on value (on) or the off value (off; a single-program switch sends
// nothing), like a CC's two values; its number is not used. A program sent becomes the
// current program, so UP / DOWN step from it and the run-mode display shows it. The caller has checked there is room for three bytes.
static void remote_cc(uint8_t cc, uint8_t value);

static void send_switch(uint8_t index, uint8_t on)
{
    if (msg_type(index) == MT_PROG)
    {
        if (on || !prog_single(index))
        {
            program = msg_field(index, (uint8_t)(on ? 1 : 2));
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

    tx_flash = TX_FLASH_TICKS;
    hal_midi_tx((uint8_t)(((msg_type(index) == MT_NOTE) ? (on ? 0x90 : 0x80) : 0xB0) | (midi_channel - 1)));
    hal_midi_tx(msg_field(index, 0));
    hal_midi_tx(msg_field(index, (uint8_t)(on ? 1 : 2)));

    // A CC or note has one state wherever it is used: every switch with it follows, as
    // for one received on MIDI IN (remote_cc; not in configuration mode, where the LEDs
    // show the pages). Last, so SDCC jumps to it and the stack goes no deeper (this is
    // the main line's deepest path when a revert is flushed).
    on = switch_value(index, on);
    remote_cc(msg_field(index, 0), on);
}


static uint8_t span_of(uint8_t a, uint8_t b);

// Forget the last sent pedal positions so the next reading is adopted
// silently instead of being sent as a controller change.
static void resync_pedals(void)
{
    sent[0] = NO_VALUE;
    sent[1] = NO_VALUE;
    pk_pos[0] = NO_VALUE;
    pk_pos[1] = NO_VALUE;
}

// A pedal's CC or response may have changed (a bank change, or over MIDI): its value now
// counts as where it is, so a pickup pedal compares its new CC's last value with its
// position (pk_pos, which is also where an immediate pedal last sent from). A leaf.
static void pedals_rebase(void)
{
    sent[0] = pk_pos[0];
    sent[1] = pk_pos[1];
}

// The value pedal ch must reach to be picked up: its CC's last known value, 0 if it has
// none yet. A leaf.
static uint8_t pk_target(uint8_t ch)
{
    ch = key_val[pedal_cc[ch]];

    return (uint8_t)((ch > 127) ? 0 : ch);
}

// 1 if pedal ch is in pickup mode and not picked up (see pk_mask). A leaf.
static uint8_t pk_waiting(uint8_t ch)
{
    // ch + 1 is the pedal's pk_mask bit (1 A, 2 B), with no variable shift.
    return (uint8_t)(((pk_mask & (uint8_t)(ch + 1)) &&
                      (sent[ch] == NO_VALUE || key_val[pedal_cc[ch]] != sent[ch])) ? 1 : 0);
}

// The EXP LED's flash half-period, in ticks, for a waiting pedal ch: PICKUP_FLASH_TICKS
// at the pickup point, 3ms more per CC step away from it (about 0.4s at the far end), and
// the slowest before the first reading. A leaf.
static uint8_t pk_period(uint8_t ch)
{
    uint8_t d = 127;

    if (pk_pos[ch] != NO_VALUE)
    {
        d = span_of(pk_pos[ch], pk_target(ch));
    }

    return (uint8_t)(PICKUP_FLASH_TICKS + d + (d >> 1));
}

// Run mode, every tick: each waiting pickup pedal's EXP LED flashes, faster the closer
// the pedal is to its pickup point; otherwise it is dark. Then, once the distance has been
// shown for PEDAL_PICKUP_SHOW_MS with no move, the display shows the program (or bank)
// again, unless a program number is being typed.
static void pickup_tick(void)
{
    // pk_i + 1 is the pedal's pk_led bit.
    for (pk_i = 0; pk_i < 2; pk_i++)
    {
        if (pk_waiting(pk_i))
        {
            if (pk_flash[pk_i] == 0 || --pk_flash[pk_i] == 0)
            {
                pk_led ^= (uint8_t)(pk_i + 1);
                pk_flash[pk_i] = pk_period(pk_i);
            }
        }
        else
        {
            pk_led &= (uint8_t)~(pk_i + 1);
            pk_flash[pk_i] = 0;
        }

        hal_aux_led((uint8_t)(HAL_LED_EXP_1 + pk_i), (uint8_t)((pk_led & (uint8_t)(pk_i + 1)) ? 1 : 0));
    }

    if (pk_show && --pk_show == 0 && !pc_typing && !boot_anim)
    {
        show_program();
    }
}

// ---------------------------------------------------------------
// Program change
// ---------------------------------------------------------------

// Bank mode: the next / previous bank for UP / DOWN, stopping at the ends like the program
// number (or wrapping with PROGRAM_WRAP). A leaf function.
static void bank_step(uint8_t code)
{
    if (code == CTL_KEY_UP)
    {
        if ((uint8_t)(bank_next + 1) < BANK_COUNT)
        {
            bank_next++;
        }
#if PROGRAM_WRAP
        else
        {
            bank_next = 0;
        }
#endif
    }
    else if (bank_next != 0)
    {
        bank_next--;
    }
#if PROGRAM_WRAP
    else
    {
        bank_next = BANK_COUNT - 1;
    }
#endif
}

// Move the bank (or in program mode the program number) one step for UP or DOWN and show
// it. Nothing is sent.
static void program_step(uint8_t code)
{
    if (!pc_mode)
    {
        bank_step(code);
    }
    else if (code == CTL_KEY_UP)
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

// Send the program number now, or as soon as there is room in the MIDI OUT queue. Out of
// program mode, change to the chosen bank if it is not the one loaded (bank_run does it).
static void commit_program(void)
{
    if (!pc_mode)
    {
        if (bank_next != hal_bank)
        {
            bank_busy = 1;
        }

        return;
    }

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

// Every panel LED (bit n of led_aux is HAL_LED n) and every footswitch LED (bit n of
// led_stomp is switch n) at once, for a page or a mode change; both masks are used up.
// The loop count and masks are not locals or parameters, so they cost no directly
// addressed RAM.
#define LED_BIT(n) ((uint16_t)((uint16_t)1 << (n)))
static IDATA uint16_t led_aux;
static IDATA uint16_t led_stomp;
static IDATA uint8_t  led_i;

static void show_stomp_leds(void)
{
    for (led_i = 0; led_i < STOMP_COUNT; led_i++)
    {
        hal_stomp_led(led_i, (uint8_t)(led_stomp & 1));
        led_stomp >>= 1;
    }
}

static void show_leds(void)
{
    for (led_i = 0; led_i < HAL_LED_COUNT; led_i++)
    {
        hal_aux_led(led_i, (uint8_t)(led_aux & 1));
        led_aux >>= 1;
    }

    show_stomp_leds();
}

// Page 7: the bank's program change, or "--" for none, for BANK_LABEL_STEPS before "Pr"
// shows again (user's request).
static void show_bank_pc(void)
{
    chan_flash_ticks = 0;
    bank_label       = 0;

    if (bank_pc != 0)
    {
        hal_display(BANK_PC_PROGRAM);
    }
    else
    {
        hal_display_text(HAL_TEXT_DASHES);
    }
}

// Page 7, each tick while no "--" entry is open: "Pr" and the program in turn.
static void bank_label_tick(void)
{
    if (++chan_flash_ticks < VER_STEP_TICKS)
    {
        return;
    }

    chan_flash_ticks = 0;

    if ((uint8_t)(++bank_label & ~BANK_LABEL_ON) < BANK_LABEL_STEPS)
    {
        return;
    }

    if (bank_label & BANK_LABEL_ON)
    {
        show_bank_pc();
    }
    else
    {
        bank_label = BANK_LABEL_ON;
        hal_display_text(HAL_TEXT_PROG);
    }
}

// A footswitch on page 7 types a digit of the program (switches 1-9 type 1-9, switch 10
// types 0), as in the footswitch editor: added to the number being typed while that
// stays within 127, otherwise starting a new one. A leaf, so its locals cost no fixed RAM.
static void bank_pc_digit(uint8_t index)
{
    uint8_t v = BANK_PC_PROGRAM;

    index = (uint8_t)((index == STOMP_COUNT - 1) ? 0 : index + 1);

    if (edit_typing && bank_pc != 0 && (v < 12 || (v == 12 && index <= 7)))
    {
        index = (uint8_t)(v * 10 + index);
    }

    edit_typing = (index < 13) ? 1 : 0;
    bank_pc = (uint8_t)(index + 1);
}

// v (0-99) as two BCD digits, for hal_display_hex. A leaf.
static uint8_t bcd_of(uint8_t v)
{
    uint8_t t = 0;

    while (v >= 10)
    {
        v = (uint8_t)(v - 10);
        t = (uint8_t)(t + 0x10);
    }

    return (uint8_t)(t | v);
}

// Page 8 (and power-on), the current step: every LED lit, and the "riban" frame or
// version part that starts at this step (other steps leave the display as it is).
static void version_show(void)
{
    uint8_t s = ver_step;
    uint8_t g;

    // Page 8: every LED stays lit throughout, the footswitch LEDs marking the segments.
    // Power-on: every LED lit for VER_BOOT_LAMP_STEPS from the lead-in (it starts at
    // VER_START_STEP and wraps to 0), then the normal LEDs (run mode's).
    if (!boot_anim)
    {
        hal_lamp_test(HAL_LAMP_LEDS_STOMPS);
    }
    else if ((s >= VER_START_STEP) ? (uint8_t)(s - VER_START_STEP) < VER_BOOT_LAMP_STEPS :
             (uint8_t)(s + VER_LEAD_FRAMES * VER_SCROLL_STEPS) < VER_BOOT_LAMP_STEPS)
    {
        hal_lamp_test(HAL_LAMP_LEDS_ON);
    }
    else
    {
        hal_lamp_test(HAL_LAMP_NORMAL);
    }

    // g = the segment (0-9), s = the step within it.
    if (s < VER_LAST_TEXT)
    {
        g = (uint8_t)(s / VER_SCROLL_STEPS);
        s = (uint8_t)(s % VER_SCROLL_STEPS);
    }
    else if (s < VER_SCROLL_END)
    {
        g = HAL_TEXT_SCROLL_FRAMES - 1;
        s = (uint8_t)(s - VER_LAST_TEXT);
    }
    else if (s < VER_BLANK_START)
    {
        s = (uint8_t)(s - VER_SCROLL_END);
        g = (uint8_t)(HAL_TEXT_SCROLL_FRAMES + s / VER_ITEM_STEPS);
        s = (uint8_t)(s % VER_ITEM_STEPS);
    }
    else
    {
        g = VER_SEGMENTS - 1;
        s = (uint8_t)(s - VER_BLANK_START);
    }

    // Page 8 (not power-on): segment g's footswitch LED goes dark at its start (the one
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

    // The trailing dot running off after "n " (display blank), one frame each.
    if (g == HAL_TEXT_SCROLL_FRAMES - 1 && s >= VER_SCROLL_STEPS)
    {
        if (s % VER_SCROLL_STEPS == 0)
        {
            hal_display(0);
            hal_display_blank(7);
            hal_display_dots((uint8_t)(1 << (s / VER_SCROLL_STEPS)));
        }

        return;
    }

    // The lead-in after the blank: the leading dot far right, then between the digits.
    if (g == VER_SEGMENTS - 1 && s >= VER_ITEM_STEPS)
    {
        s = (uint8_t)(s - VER_ITEM_STEPS);

        if (s % VER_SCROLL_STEPS == 0)
        {
            hal_display(0);
            hal_display_blank(7);
            hal_display_dots((uint8_t)(1 << (s / VER_SCROLL_STEPS)));
        }

        return;
    }

    if (s != 0)
    {
        return;
    }

    // The text frames, each with its dot (after the text, which clears the left position).
    if (g < HAL_TEXT_SCROLL_FRAMES)
    {
        hal_display_text((uint8_t)(HAL_TEXT_SCROLL + g));
        hal_display_dots(scroll_dots[g]);
        return;
    }

    {
        s = (uint8_t)(g - HAL_TEXT_SCROLL_FRAMES);

        // Step 3 is the blank between build and the lead-in.
        if (s == 3)
        {
            hal_display_dots(0);
            hal_display(0);
            hal_display_blank(7);
        }
        else
        {
            // Major and minor in decimal, the build in hex (user's request).
            hal_display_hex((s < 2) ? bcd_of(fw_version[s]) : fw_version[s]);
            hal_display_dots(fw_version_dots[s]);
        }
    }
}

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

// Page 8: footswitch index (0-9) jumps to its segment, held there until released.
// Every footswitch LED lights; version_show then darkens this one.
static void version_jump(uint8_t index)
{
    led_stomp = LED_BIT(STOMP_COUNT) - 1;
    show_stomp_leds();

    // Segments 0-5 start a scroll frame apart; 6-9 a version part apart from the scroll's end.
    ver_step   = (uint8_t)((index < HAL_TEXT_SCROLL_FRAMES) ? index * VER_SCROLL_STEPS :
                           VER_SCROLL_END + (index - HAL_TEXT_SCROLL_FRAMES) * VER_ITEM_STEPS);
    ver_ticks  = VER_STEP_TICKS;
    ver_paused = 1;
    version_show();
}

// Every scan tick on page 8: move to the next step, looping over the version parts
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

    // At power-on, stop once the trailing dot has gone and go to run mode (it starts at the
    // lead-in, VER_START_STEP, before wrapping to 0).
    if (boot_anim && ver_step >= VER_BOOT_END_STEP && ver_step < VER_START_STEP)
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
    // Opened from page 1, so MIDI FUNCTION stays lit.
    led_aux   = (uint16_t)(LED_BIT(HAL_LED_MIDI_FUNC) | bit16((uint8_t)(HAL_LED_SELECT + edit_param)));
    led_stomp = bit16(cfg_sel);
    show_leds();

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
// Restart the config-mode blink lit (a new flashing thing starts lit).
static void blink_restart(void)
{
    blink_ticks = 0;
    blink_on    = 1;
}

// Whether a number is being entered (its right-hand dot flashes, user's request): the
// bank to save to, the pedal CC editor, the footswitch editor's number and values, and
// page 7's program while it shows. A leaf.
static uint8_t num_entry(void)
{
    if (save_prompt || pcc_open)
    {
        return 1;
    }

    if (edit_open)
    {
        return (uint8_t)(edit_param != EP_TYPE);
    }

    return (uint8_t)(cfg_page == PAGE_BANK_PC && !(bank_label & BANK_LABEL_ON) && bank_pc != 0);
}

static void config_show(void)
{
    uint8_t r;

    // Saving: only the bank number (its dot flashing, from ctl_tick), page LEDs dark.
    // The bank number and the dot alternate (user's request): the number while
    // blink_on, then a blank display with the dot (from ctl_tick).
    if (save_prompt)
    {
        led_aux   = 0;
        led_stomp = 0;
        show_leds();
        hal_display(save_bank);

        if (!blink_on)
        {
            hal_display_blank(7);
        }

        return;
    }

    if (edit_open)
    {
        edit_show();
        return;
    }

    led_aux   = 0;
    led_stomp = 0;

    // Page 8: no page LED; footswitch LEDs lit (the first segment's, switch 1, then goes
    // dark), display blank under the version, with every LED lit.
    if (cfg_page == PAGE_VERSION)
    {
        led_stomp = LED_BIT(STOMP_COUNT) - 1;
        show_leds();
        hal_display(0);
        hal_display_blank(7);
        version_show();
        return;
    }

    // Pages 3 and 4, EXP A / EXP B: the page's EXP LED (NUMBER too in the CC editor),
    // the footswitch last pressed, and its setting: "Pu" pickup or "Ab" immediate
    // (footswitch 1), the CC (2), or the calibration (3): the pedal's value, a flashing
    // Lo / Hi, or Er.
    if (IS_EXP_PAGE(cfg_page))
    {
        led_aux = bit16((uint8_t)(HAL_LED_EXP_1 + pcc_b));

        if (pcc_open)
        {
            led_aux |= LED_BIT(HAL_LED_NUMBER);
        }

        led_stomp = bit16(ped_sel);
        show_leds();

        if (ped_sel == PED_PICKUP)
        {
            hal_display_text((uint8_t)(((pk_mask >> pcc_b) & 1) ? HAL_TEXT_MODE + PM_MOMENTARY : HAL_TEXT_ABS));
        }
        else if (ped_sel == PED_CC)
        {
            hal_display(pedal_cc[pcc_b]);
        }
        else if (cal_step == CAL_DONE)
        {
            hal_display(ctl_scale(last_raw[pcc_b], heel[pcc_b], toe[pcc_b]));
        }
        // Armed: Lo / Hi flashing (blank half the time), or Er.
        else if (cal_armed && !blink_on)
        {
            hal_display(0);
            hal_display_blank(7);
        }
        else
        {
            hal_display_text((uint8_t)((cal_step == CAL_HEEL) ? HAL_TEXT_LO :
                                       (cal_step == CAL_TOE) ? HAL_TEXT_HI : HAL_TEXT_ER));
        }

        return;
    }

    // Page 7, the bank's program change: DIRECT SELECT lit, "Pr" and the program or "--"
    // (none) in turn.
    if (cfg_page == PAGE_BANK_PC)
    {
        led_aux = LED_BIT(HAL_LED_DIRECT_SELECT);
        show_leds();

        if (bank_label & BANK_LABEL_ON)
        {
            hal_display_text(HAL_TEXT_PROG);
        }
        else
        {
            show_bank_pc();
        }

        return;
    }

    // A SWITCH page (its SWITCH LED lit) shows that relay's triggers: footswitch LEDs for
    // the switches it follows, EXP 1 / EXP 2 for a pedal
    // trigger value. The display is blank until a pedal is moved, then shows the value.
    if (IS_SW_PAGE(cfg_page))
    {
        r = (uint8_t)(cfg_page - PAGE_SW1);
        led_aux = bit16((uint8_t)(HAL_LED_SWITCH_1 + r));

        if (sw_thr[r * 2])
        {
            led_aux |= LED_BIT(HAL_LED_EXP_1);
        }

        if (sw_thr[r * 2 + 1])
        {
            led_aux |= LED_BIT(HAL_LED_EXP_2);
        }

        led_stomp = sw_mask[r];
        show_leds();
        hal_display(0);
        hal_display_blank(7);
        return;
    }

    // Page 1: MIDI FUNCTION (the selected footswitch's LED lit too); page 2: MIDI CHANNEL.
    if (cfg_page == PAGE_MODES)
    {
        led_aux   = LED_BIT(HAL_LED_MIDI_FUNC);
        led_stomp = bit16(cfg_sel);
        show_leds();
        // The selected footswitch's mode: "St" stomp, "+St" stomp with momentary, "Pu"
        // momentary (push), "-Pu" inverted momentary (user's request).
        hal_display_text((uint8_t)(HAL_TEXT_MODE + pedal_mode_get(cfg_sel)));
    }
    else
    {
        led_aux = LED_BIT(HAL_LED_MIDI_CHAN);
        show_leds();
        hal_display(midi_channel);
    }
}

static void config_goto_page(uint8_t page)
{
    cfg_page = page;
    chan_digit_pending = 0;
    ver_paused = 0;
    mode_press_live = 0;
    mode_cycle_pending = 0;
    // Leaving page 8 (or restarting it): normal LEDs again and no dots.
    hal_lamp_test(HAL_LAMP_NORMAL);
    hal_display_dots(0);

    // The pedal page starts on footswitch 1, pedal A's response (user's request).
    if (IS_EXP_PAGE(page))
    {
        ped_sel   = PED_PICKUP;
        pcc_b     = (uint8_t)(page - PAGE_EXP_A);
        cal_step  = CAL_DONE;
        cal_armed = 0;
    }
    // A SWITCH page takes the next pedal readings as the reference, so only a pedal that
    // is then moved sets a trigger; page 7's first digit starts a new number, and "Pr"
    // shows first.
    else if (IS_SW_PAGE(page) || page == PAGE_BANK_PC)
    {
        resync_pedals();
        edit_typing = 0;
        pc_typing   = 0;
        bank_label  = BANK_LABEL_ON;
        chan_flash_ticks = 0;
    }
    // Page 8 starts with the "riban" scroll's lead-in.
    else if (page == PAGE_VERSION)
    {
        ver_step  = VER_START_STEP;
        ver_ticks = VER_STEP_TICKS;
    }

    config_show();
}

static uint8_t span_of(uint8_t a, uint8_t b)
{
    return (uint8_t)((a > b) ? a - b : b - a);
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
    cfg_dirty |= bit16(index);
    config_changed();
    config_show();
}

// The editor's copy of the edited switch's three msg[] bytes, taken when it opens.
static XDATA uint8_t edit_bak[MSG_BYTES];

// Copy (EK_COPY), swap with (EK_SWAP) or compare with (EK_SAME: 1 if equal) the edited
// switch's three bytes and the saved ones. A leaf.
#define EK_COPY 0
#define EK_SWAP 1
#define EK_SAME 2

static uint8_t edit_bytes(uint8_t how)
{
    XDATA uint8_t *m = &msg[MSG_AT(cfg_sel)];
    uint8_t k;
    uint8_t t;

    for (k = 0; k < MSG_BYTES; k++)
    {
        t = m[k];

        if (how == EK_SAME)
        {
            if (t != edit_bak[k])
            {
                return 0;
            }
        }
        else
        {
            if (how == EK_SWAP)
            {
                m[k] = edit_bak[k];
            }

            edit_bak[k] = t;
        }
    }

    return 1;
}

#define edit_copy() edit_bytes(EK_COPY)
#define edit_swap() edit_bytes(EK_SWAP)

// Open the editor for footswitch cfg_sel (a long press on the modes page), on its type.
static void edit_begin(void)
{
    edit_open   = 1;
    edit_param  = EP_TYPE;
    edit_typing = 0;
    edit_copy();
    resync_pedals();
    edit_show();
}

// Short UP [ENTER] or DOWN [ESCAPE] in the editor (user's request: short presses, not
// long). DOWN puts the shown parameter back as it was when it was shown and closes the
// editor. UP keeps it and goes on to the next parameter (a program change has no number:
// its programs are the on and off values); after the off value it closes the editor.
// Leaving configuration mode with a save keeps everything (edit_param set to EP_OFF
// first). Like a mode change, a changed message puts the switch in its mode's resting
// state when configuration mode is saved (cfg_dirty). If the switch is on, the old
// message's "off" (its CC off value or note off) is sent now, while the old message is
// still known, and the switch is off (a program change sends nothing).
static void edit_end(uint8_t accept)
{
    if (!accept)
    {
        edit_swap();
        cfg_check_due = 1;
    }
    else if (!edit_bytes(EK_SAME))
    {
        cfg_dirty |= bit16(cfg_sel);

        if (stomp_on(cfg_sel))
        {
            // Not for a program change: nothing is left sounding, and sending its "off"
            // program would change the receiver's program for no reason.
            edit_swap();

            if (hal_midi_space() >= 3 && msg_type(cfg_sel) != MT_PROG)
            {
                send_switch(cfg_sel, 0);
            }

            edit_swap();
            stomp_put(cfg_sel, 0);
        }
    }

    edit_typing = 0;
    resync_pedals();

    if (accept && edit_param != EP_OFF)
    {
        edit_copy();
        edit_param++;

        if (edit_param == EP_NUM && msg_type(cfg_sel) == MT_PROG)
        {
            edit_param = EP_ON;
        }

        edit_show();
        return;
    }

    edit_open = 0;
    config_show();
}

static void edit_set_type(uint8_t t)
{
    msg_set_type(cfg_sel, t);
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

// An EXP page's pedal CC editor (footswitch 2, pressed twice): footswitches type the CC
// as the footswitch editor types a value, a short UP [ENTER] accepts and a short DOWN
// [ESCAPE] puts the old CC back, both back to the pedal page.
static void pcc_begin(void)
{
    pcc_open    = 1;
    edit_typing = 0;
    edit_bak[0] = pedal_cc[pcc_b];
    config_show();
}

static void pcc_end(uint8_t accept)
{
    if (!accept)
    {
        pedal_cc[pcc_b] = edit_bak[0];
        cfg_check_due   = 1;
    }

    pcc_open    = 0;
    edit_typing = 0;
    config_show();
}

static void pcc_set(uint8_t v)
{
    pedal_cc[pcc_b] = v;
    cfg_check_due   = 1;
    config_show();
}

// A footswitch let go on an EXP page (the CC editor closed), the same on both pages
// (user's layout): 1 pickup, 2 CC, 3 calibration; the others do nothing. A footswitch not
// yet selected only shows its setting (user's request): pickup or immediate, the CC, or
// the pedal's value. Pressed again, 1 toggles pickup and 2 opens the CC editor; 3 steps
// through the calibration (user's design): "Lo" flashing, then the heel reading with
// "Hi" flashing, then the toe reading and the value again. The toe must be CAL_MIN_SPAN
// from the heel, or "Er" shows for CAL_ERROR_SHOW_MS and the calibration is unchanged.
static void ped_press(uint8_t index)
{
    if (index > PED_CAL)
    {
        return;
    }

    if (index != ped_sel)
    {
        ped_sel  = index;
        cal_step = CAL_DONE;
    }
    else if (index == PED_PICKUP)
    {
        pk_mask ^= (uint8_t)(pcc_b ? 2 : 1);
        config_changed();
    }
    else if (index == PED_CC)
    {
        pcc_begin();
        return;
    }
    else if (cal_step == CAL_HEEL)
    {
        cal_low  = last_raw[pcc_b];
        cal_step = CAL_TOE;
        blink_restart();
    }
    else if (cal_step == CAL_TOE)
    {
        if (span_of(last_raw[pcc_b], cal_low) < CAL_MIN_SPAN)
        {
            cal_step     = CAL_ERROR;
            cal_err_left = CAL_ERROR_BLINKS;
            blink_restart();
        }
        else
        {
            heel[pcc_b] = cal_low;
            toe[pcc_b]  = last_raw[pcc_b];
            cal_step    = CAL_DONE;
            config_changed();
        }
    }
    else
    {
        cal_step = CAL_HEEL;
        blink_restart();
    }

    cal_armed = (uint8_t)(cal_step == CAL_HEEL || cal_step == CAL_TOE);
    config_show();
}

static void pcc_digit(uint8_t index)
{
    uint8_t v = pedal_cc[pcc_b];

    index = (uint8_t)((index == STOMP_COUNT - 1) ? 0 : index + 1);

    if (edit_typing && (v < 12 || (v == 12 && index <= 7)))
    {
        index = (uint8_t)(v * 10 + index);
    }

    edit_typing = (index < 13) ? 1 : 0;
    pcc_set(index);
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
    // A program number being typed is dropped.
    pc_typing = 0;

    // A stomp being held (momentary, or past the long-press time) is treated as released so
    // the synth is not left with it on.
    stomp_revert |= (uint16_t)(stomp_down & stomp_held);
    stomp_down = 0;
    stomp_held = 0;

    cfg_dirty = 0;
    // This also drops a dump over MIDI in progress (midi_job is cfg_page) and the
    // selected NRPN parameter (nrpn_lsb is cfg_sel).
    cfg_sel   = 0;
    nrpn_msb_ok = 0;
    edit_open = 0;
    pcc_open  = 0;
    tx_flash  = 0;
    cfg_check_due = 0;
    rx_flash  = 0;
    cfg_changed  = 0;
    cfg_led_dark = 0;
    pk_show   = 0;

    hal_status_led(1);
    config_goto_page(PAGE_MODES);
    config_changed();
}

// Every switch whose mode or message changed (cfg_dirty) goes to its new mode's resting
// state: on for inverted momentary, off for the others and for a single program. If that
// changes its state its message is sent (through the revert queue, so as soon as MIDI OUT
// has room, and the LED follows); if it is already there nothing is sent. Used when
// configuration mode is left and after a change over MIDI.
static void settle_dirty(void)
{
    uint8_t  i;
    uint16_t bit;

    for (i = 0; i < STOMP_COUNT; i++)
    {
        bit = bit16(i);

        if (cfg_dirty & bit)
        {
            if (rests_on(i))
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

    cfg_dirty = 0;
}

// Leave configuration mode. save = 1 (the save prompt's UP, or UP + DOWN) stores the
// settings and calibration in bank hal_bank;
// save = 0 (long DOWN) drops every change by reloading what the EEPROM holds.
static void exit_config(uint8_t save)
{
    // An open footswitch editor keeps its changes (UP + DOWN held saves and leaves), as
    // does the pedal CC editor.
    if (edit_open)
    {
        edit_param = EP_OFF;
        edit_end(1);
    }

    pcc_open    = 0;
    save_prompt = 0;

    // Stop a two-digit channel entry that could still take a second digit, and page 7's
    // "--" entry (its dot goes out on the next tick).
    chan_digit_pending = 0;
    pc_typing          = 0;
    // Leaving from page 8 ends the lamp test and its dots.
    hal_lamp_test(HAL_LAMP_NORMAL);
    hal_display_dots(0);

    if (save)
    {
        save_settings();
    }
    else
    {
        load_settings();
        cfg_dirty = 0;
    }

    settle_dirty();

    mode = MODE_RUN;
    // Stop the CONFIG LED flash; its timer goes back to the MIDI IN flash.
    cfg_changed  = 0;
    cfg_led_dark = 0;
    cfg_flash_ticks = 0;
    hal_status_led(0);
    // Run mode's LEDs: the relays, DIRECT SELECT in program mode, the stomp states.
    led_aux = (uint16_t)((uint16_t)(relay_out & 3) << HAL_LED_SWITCH_1);

    if (pc_mode)
    {
        led_aux |= LED_BIT(HAL_LED_DIRECT_SELECT);
    }

    led_stomp = stomp_state;
    show_leds();

    resync_pedals();
    show_program();
    // cfg_page and cfg_sel become midi_job and nrpn_lsb again: no dump, no parameter.
    midi_job = 0;
    nrpn_lsb = NRPN_NONE;
}

// A long UP: ask which bank to save to, starting at the bank in use (user's request).
// Whatever the page was doing stops; DOWN goes back to the page from its start.
static void save_begin(void)
{
    save_prompt        = 1;
    save_bank          = (uint8_t)(hal_bank + 1);
    edit_typing        = 0;
    chan_digit_pending = 0;
    ver_paused         = 0;
    bank_none_ready    = 0;
    pc_typing          = 0;
    pc_dot             = 0;
    mode_press_live    = 0;
    mode_cycle_pending = 0;
    hal_lamp_test(HAL_LAMP_NORMAL);
    blink_restart();
    config_show();
}

// A footswitch in the save prompt types the bank (1-9, 10 = 0): added to the number while
// that stays a bank, otherwise starting a new one; a 0 cannot start one.
static void save_digit(uint8_t index)
{
    uint8_t d = (uint8_t)((index == STOMP_COUNT - 1) ? 0 : index + 1);

    if (edit_typing && save_bank * 10 + d <= BANK_COUNT)
    {
        save_bank   = (uint8_t)(save_bank * 10 + d);
        edit_typing = 0;
    }
    else if (d != 0 && d <= BANK_COUNT)
    {
        save_bank   = d;
        edit_typing = (d * 10 <= BANK_COUNT) ? 1 : 0;
    }

    blink_restart();
    config_show();
}

// Short UP saves into the bank shown and leaves, in that bank (another bank: the bank in
// use keeps what it had saved); short DOWN goes back to the page.
static void save_end(uint8_t save)
{
    save_prompt = 0;

    if (save)
    {
        hal_bank  = (uint8_t)(save_bank - 1);
        bank_next = hal_bank;
        exit_config(1);
    }
    else
    {
        edit_typing = 0;
        config_goto_page(cfg_page);
    }
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
    // The save prompt and the editors: UP [ENTER] accepts, DOWN [ESCAPE] undoes.
    else if (save_prompt)
    {
        save_end((uint8_t)(code == CTL_KEY_UP));
    }
    else if (edit_open)
    {
        edit_end((uint8_t)(code == CTL_KEY_UP));
    }
    else if (pcc_open)
    {
        pcc_end((uint8_t)(code == CTL_KEY_UP));
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

// Short UP + DOWN in run mode: toggle program mode and show it on the DIRECT SELECT LED.
// Nothing is sent; a number being typed is dropped.
static void pc_mode_toggle(void)
{
    pc_mode   = !pc_mode;
    pc_typing = 0;
    hal_aux_led(HAL_LED_DIRECT_SELECT, pc_mode ? 1 : 0);
    // The display shows the program in program mode, the bank otherwise.
    show_program();
}

// Program mode: send the program chosen with the footswitches (in program) now, or as soon
// as MIDI OUT has room, and show it; typing ends.
static void pc_choose(void)
{
    pc_typing     = 0;
    program_moved = 0;
    show_program();

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
            // In configuration mode it saves and leaves at once: on the next tick, from
            // ctl_tick's chord timer (not this key handler's deeper stack).
            nav_ticks = (mode == MODE_CONFIG) ? CHORD_TICKS - 1 : 0;
            nav_cancel();
        }

        return;
    }

    if (chord_state != CHORD_NONE)
    {
        return;
    }

    // Typing a program number: UP / DOWN only act on release (enter / cancel), no repeat.
    if (pc_typing && mode == MODE_RUN)
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
            // Let go before the hold time (run mode only: in configuration mode the
            // chord has already saved and left): toggles program mode.
            chord_state = CHORD_DONE;
            pc_mode_toggle();
        }

        if (keys_down == 0)
        {
            chord_state = CHORD_NONE;
        }

        return;
    }

    // Page 7 after footswitch 10 ("--"): UP (ENTER) turns the bank's program change off,
    // DOWN (ESCAPE) goes back to showing it; neither changes page. Not after a hold.
    if (pc_typing && mode == MODE_CONFIG)
    {
        if (code == nav_code)
        {
            nav_code      = NAV_NONE;
            nav_repeating = 0;
            pc_typing     = 0;

            if (code == CTL_KEY_UP)
            {
                bank_pc = 0;
                config_changed();
            }

            show_bank_pc();
        }

        return;
    }

    // Typing a program number: UP (ENTER) sends the number typed, if it is a program;
    // DOWN (ESCAPE), or UP with no number, goes back to the current program.
    if (pc_typing && mode == MODE_RUN)
    {
        if (code == CTL_KEY_UP && pc_entry != PC_ENTRY_NONE && PC_IS_PROGRAM(pc_entry))
        {
            program = (uint8_t)(pc_entry - DISPLAY_PROGRAM_OFFSET);
            pc_choose();
        }
        else
        {
            pc_typing = 0;
            show_program();
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
    uint8_t span;
    uint8_t d;

    // d is the distance from the heel towards the toe; a pedal wired the other way round
    // just has toe below heel. Unsigned 8-bit, so no 16-bit signed library routines.
    if (t >= h)
    {
        span = (uint8_t)(t - h);
        d    = (raw > h) ? (uint8_t)(raw - h) : 0;
    }
    else
    {
        span = (uint8_t)(h - t);
        d    = (raw < h) ? (uint8_t)(h - raw) : 0;
    }

    if (span == 0 || d <= PEDAL_END_SNAP)
    {
        return 0;
    }

    if (span <= PEDAL_END_SNAP || d >= (uint8_t)(span - PEDAL_END_SNAP))
    {
        return 127;
    }

    // d < span, so the rounded result is below 127.5; d * 127 as a shift, no multiply.
    return (uint8_t)(((((uint16_t)d << 7) - d) + (span >> 1)) / span);
}

void ctl_init(void)
{
    uint8_t i;

    // Power-on is always in bank 1, with no CC or note value known.
    hal_bank  = 0;
    bank_next = 0;
    load_settings();

    i = 0;
    do
    {
        key_val[i] = KEY_UNKNOWN;
    } while (++i != 0);

    mode          = MODE_RUN;
    rx_status     = 0;
    rx_have_data1 = 0;
    cfg_dirty     = 0;
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
    pc_mode       = 0;
    pc_typing     = 0;
    pc_dot        = 0;
    save_prompt   = 0;
    cal_armed     = 0;
    bank_busy     = 0;
    nav_ticks   = 0;
    cfg_page      = PAGE_MODES;
    cfg_sel       = 0;
    // cfg_page and cfg_sel are midi_job and nrpn_lsb in run mode.
    midi_job      = 0;
    nrpn_lsb      = NRPN_NONE;
    nrpn_msb_ok   = 0;
    cal_step      = CAL_DONE;
    cfg_dirty     = 0;
    chan_digit_pending = 0;
    chan_flash_ticks = 0;
    relay_out     = 0;
    relay_lvl     = 0;
    relay_resync  = 0;
    rx_flash      = 0;
    tx_flash      = 0;
    cfg_changed   = 0;
    cfg_led_dark  = 0;
    edit_open     = 0;
    pcc_open      = 0;
    edit_typing   = 0;
    cfg_check_due = 0;
    mode_press_live    = 0;
    mode_cycle_pending = 0;
    last_raw[0]   = 0;
    last_raw[1]   = 0;
    pk_led        = 0;
    pk_show       = 0;
    pk_flash[0]   = 0;
    pk_flash[1]   = 0;
    resync_pedals();

    for (i = 0; i < STOMP_COUNT; i++)
    {
        stomp_put(i, rests_on(i));
    }

    hal_status_led(0);
    led_aux   = 0;
    led_stomp = stomp_state;
    show_leds();
    hal_relays(0);
    show_program();

    // Power-on: every LED lit while "riban" scrolls once (as page 8 starts), then run
    // mode. A key press ends it early and then acts as usual.
    boot_anim = 1;
    ver_step  = VER_START_STEP;
    version_show();

    // The scroll keeps time through the start-up settling (ctl_settle_tick), so power-on
    // takes ten frames in all, the settling included.
    ver_ticks = VER_STEP_TICKS;
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
    uint16_t bit = bit16(index);
    uint8_t  on  = (stomp_prev & bit) ? 1 : 0;

    if (on)
    {
        stomp_state |= bit;
    }
    else
    {
        stomp_state &= (uint16_t)~bit;
    }

    // In configuration mode the footswitch LEDs belong to the settings pages.
    if (mode == MODE_RUN)
    {
        hal_stomp_led(index, on);
    }

    // Last, so nothing is kept on the stack across it: this is the main line's deepest
    // path when a revert is flushed from ctl_tick.
    send_switch(index, on);
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

            stomp_revert &= (uint16_t)~bit16(i);
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
        // In the footswitch and pedal CC editors a long press is a short one: it acts
        // on release.
        // A long UP asks which bank to save to; a long DOWN cancels and leaves. In the
        // save prompt, as in the editors, a long press acts as a short one.
        if (mode == MODE_CONFIG && chord_state == CHORD_NONE && nav_code == code && !edit_open && !pcc_open &&
            !save_prompt)
        {
            nav_code      = NAV_NONE;
            nav_repeating = 0;

            if (code == CTL_KEY_UP)
            {
                save_begin();
            }
            else
            {
                exit_config(0);
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
        stomp_held |= bit16(index);
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

    // Page 8: a footswitch release carries on from the segment it held.
    if (ver_paused && mode == MODE_CONFIG)
    {
        ver_paused = 0;
        return;
    }

    if (mode == MODE_CONFIG)
    {
        // The save prompt: a footswitch types a bank digit when let go, if UP and DOWN
        // were up at both ends (the simulator presses switches 6-10 with UP).
        if (save_prompt)
        {
            if (mode_press_live && keys_down == 0)
            {
                save_digit(index);
            }

            mode_press_live = 0;
            return;
        }

        // Page 7: footswitch 10 let go with UP and DOWN up (so "10, then UP", not the two
        // together, which only changes page) shows "--", the right-hand dot flashing: UP
        // then turns the bank's program change off.
        if (bank_none_ready && index == STOMP_COUNT - 1)
        {
            bank_none_ready = 0;

            if (keys_down == 0 && cfg_page == PAGE_BANK_PC)
            {
                pc_typing        = 1;
                pc_dot           = 1;
                chan_flash_ticks = 0;
                hal_display_text(HAL_TEXT_DASHES);
                hal_display_dots(1);
            }

            return;
        }

        // EXP pages: a footswitch pressed and let go with UP and DOWN up (so not the
        // simulator's UP with switches 6-10, which only changes page).
        if (IS_EXP_PAGE(cfg_page))
        {
            if (mode_press_live && keys_down == 0)
            {
                mode_press_live = 0;
                ped_press(index);
            }

            mode_press_live = 0;
            return;
        }

        // Modes page: releasing the selected switch after a short press changes its mode.
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

    bit = bit16(index);

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
        // The save prompt: the digit is typed on release.
        if (save_prompt)
        {
            mode_press_live = (keys_down == 0) ? 1 : 0;
        }
        // Channel page: types a digit. Modes page: selects a switch, or changes the
        // selected one's mode.
        else if (cfg_page == PAGE_CHANNEL)
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
            bit   = bit16(index);
            sw_mask[cfg_page - PAGE_SW1] ^= bit;
            config_changed();
            hal_stomp_led(index, (sw_mask[cfg_page - PAGE_SW1] & bit) ? 1 : 0);
        }
        // EXP pages: a digit in the CC editor; otherwise the footswitch acts on its release
        // (ped_press), if UP and DOWN are up at both ends.
        else if (IS_EXP_PAGE(cfg_page))
        {
            index = stomp_index(code);

            if (pcc_open)
            {
                pcc_digit(index);
            }
            else
            {
                mode_press_live = (keys_down == 0) ? 1 : 0;
            }
        }
        // Page 8: a footswitch shows its segment and holds it until released.
        else if (cfg_page == PAGE_VERSION)
        {
            version_jump(stomp_index(code));
        }
        // Page 7: types a digit of the bank's program change. Footswitch 10 with no number
        // being typed shows "--" on its release (below); a digit then starts a number (10
        // again is 0).
        else
        {
            index = stomp_index(code);
            bank_none_ready = 0;

            if (index == STOMP_COUNT - 1 && !edit_typing && !pc_typing)
            {
                bank_none_ready = 1;
            }
            else
            {
                pc_typing = 0;
                bank_pc_digit(index);
                config_changed();
                show_bank_pc();
            }
        }

        return;
    }

    // Program mode (DIRECT SELECT lit): the footswitches choose a program by number, as
    // shown on the display (user's design). Switches 1-9 send that program at once.
    // Switch 10 shows "--" and starts typing a number: switches 1-9 type 1-9 and 10 types
    // 0, each digit added while the number stays a program; a number no further digit
    // fits (13 and up) is sent at once, a shorter one with UP (ENTER), or DOWN (ESCAPE)
    // goes back to the current program. index and on are the digit work space.
    if (pc_mode)
    {
        index = stomp_index(code);
        on    = (uint8_t)((index == STOMP_COUNT - 1) ? 0 : index + 1);

        if (!pc_typing)
        {
            if (index == STOMP_COUNT - 1)
            {
                pc_typing = 1;
                pc_entry  = PC_ENTRY_NONE;
                hal_display_text(HAL_TEXT_DASHES);
                pc_dot = 1;
                chan_flash_ticks = 0;
                hal_display_dots(1);
            }
            else if (PC_IS_PROGRAM(on))
            {
                program = (uint8_t)(on - DISPLAY_PROGRAM_OFFSET);
                pc_choose();
            }

            return;
        }

        if (pc_entry != PC_ENTRY_NONE &&
            (pc_entry < PC_SHOWN_MAX / 10 || (pc_entry == PC_SHOWN_MAX / 10 && on <= PC_SHOWN_MAX % 10)))
        {
            on = (uint8_t)(pc_entry * 10 + on);
        }

        pc_entry = on;

        if (on > PC_SHOWN_MAX / 10 && PC_IS_PROGRAM(on))
        {
            program = (uint8_t)(on - DISPLAY_PROGRAM_OFFSET);
            pc_choose();
        }
        else
        {
            hal_display(on);
        }

        return;
    }

    // Stomps: If MIDI OUT is backed up, ignore the press entirely so the LEDs and
    // toggle states never drift from what the synth received. Also while a bank change
    // waits (one tick), so the press uses the new bank.
    if (!bank_busy && hal_midi_space() >= 3)
    {
        index = stomp_index(code);
        bit   = bit16(index);

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
    uint8_t q;

    if (value & 0x80)
    {
        return (uint8_t)(value & 1);
    }

    q = MSG_AT(i);

    if (value == (uint8_t)(msg[q + 1] & 0x7F))
    {
        // A CC whose on and off values are both this value cannot say which it means:
        // the switch keeps its state (so the switch that sent it does too).
        if (value == msg[q + 2])
        {
            return (uint8_t)((stomp_state >> i) & 1);
        }

        return 1;
    }

    if (value == msg[q + 2])
    {
        return 0;
    }

    return (uint8_t)((value >= 64) ? 1 : 0);
}

// Apply an incoming control change or note (rx_status) to every footswitch sending that
// CC or note (remote_on). No locals beyond the loop count:
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

    // Kept for every CC and note, used by a switch of this bank or not (bank changes);
    // cc is now that key (+ 0x80 for a note), as switch_key gives it.
    cc = (uint8_t)(cc | (value & 0x80));
    key_val[cc] = value;

    for (i = 0; i < STOMP_COUNT; i++)
    {
        if (switch_key(i) == cc && msg_type(i) != MT_PROG)
        {
            stomp_put(i, remote_on(i, value));
            hal_stomp_led(i, stomp_on(i));
        }
    }
}

// ---------------------------------------------------------------
// Configuration over MIDI: NRPN on the pedal's channel
// ---------------------------------------------------------------
// CC 99 (NRPN MSB) = NRPN_MSB and CC 98 (NRPN LSB) = a parameter (either order) select it;
// then CC 6 (data entry) sets it. CC 99 with any other value, or an RPN select (CC 101 or
// 100), deselects, so other NRPN traffic on the channel is ignored. Run mode only, like the
// other received messages. A set changes the setting at once (unsaved, as in configuration
// mode); the save command stores everything. The pedal never answers a set, so a MIDI loop
// cannot make it talk to itself: it sends parameters only for the dump command, each as
// CC 99, 98 and 6 with running status. Read-only parameters that can be over 127 (version
// and calibration) add CC 38: CC 6 holds bits 7 and up, CC 38 bits 0-6. A settable value
// is always sent in CC 6 alone, so a dump fed back in changes nothing.
// The parameter numbers and commands (NP_*, NC_*) are defined near the top.

// The state (nrpn_msb_ok, nrpn_lsb, midi_job) is declared with the other state above.

static uint8_t np_kind(uint8_t p)
{
    if (p == NP_CHANNEL || p == NP_BANK || (p >= NP_MODE && p < NP_MODE + STOMP_COUNT) ||
        (p >= NP_TYPE && p < NP_OFF + STOMP_COUNT) || (p >= NP_FOLLOW1 && p < NP_FOLLOW1 + STOMP_COUNT) ||
        (p >= NP_FOLLOW2 && p < NP_FOLLOW2 + STOMP_COUNT) || (p >= NP_TRIGGER && p < NP_TRIGGER + 4) ||
        p == NP_BANK_PC || p == NP_BANK_PC_ON || (p >= NP_PICKUP && p < NP_PEDAL_CC + 2))
    {
        return NK_SET;
    }

    if ((p >= NP_VERSION && p < NP_VERSION + 3) || p == NP_BANKS || (p >= NP_CAL && p < NP_CAL + 4))
    {
        return NK_READ;
    }

    return NK_NONE;
}

// The current value of known parameter p.
static uint8_t nrpn_get(uint8_t p)
{
    if (p == NP_CHANNEL)
    {
        return midi_channel;
    }

    if (p == NP_BANK)
    {
        return (uint8_t)(bank_next + 1);
    }

    if (p == NP_BANKS)
    {
        return BANK_COUNT;
    }

    // The bank's program change (127 while it is off: what turning it on gives) and whether
    // it is on.
    if (p == NP_BANK_PC)
    {
        return BANK_PC_PROGRAM;
    }

    if (p == NP_BANK_PC_ON)
    {
        return (uint8_t)(bank_pc != 0);
    }

    if (p >= NP_PEDAL_CC)
    {
        return pedal_cc[(uint8_t)(p - NP_PEDAL_CC)];
    }

    if (p >= NP_PICKUP)
    {
        return (uint8_t)((pk_mask >> (p - NP_PICKUP)) & 1);
    }

    if (p < NP_MODE)
    {
        return fw_version[(uint8_t)(p - NP_VERSION)];
    }

    if (p < NP_TYPE)
    {
        return pedal_mode_get((uint8_t)(p - NP_MODE));
    }

    if (p < NP_NUM)
    {
        return msg_type((uint8_t)(p - NP_TYPE));
    }

    if (p < NP_FOLLOW1)
    {
        // Number, on value or off value: msg[] byte 0, 1 or 2 of switch (p - NP_TYPE) % 10.
        p = (uint8_t)(p - NP_TYPE);
        return msg_field((uint8_t)(p % (uint8_t)STOMP_COUNT), (uint8_t)(p / (uint8_t)STOMP_COUNT - 1));
    }

    if (p < NP_TRIGGER)
    {
        // A follow flag: relay 0 from NP_FOLLOW1, relay 1 from NP_FOLLOW2.
        return (uint8_t)((p < NP_FOLLOW2) ? (bit16((uint8_t)(p - NP_FOLLOW1)) & sw_mask[0]) != 0 :
                                            (bit16((uint8_t)(p - NP_FOLLOW2)) & sw_mask[1]) != 0);
    }

    if (p < NP_CAL)
    {
        return sw_thr[(uint8_t)(p - NP_TRIGGER)];
    }

    p = (uint8_t)(p - NP_CAL);
    return (p & 1) ? toe[p >> 1] : heel[p >> 1];
}

// Send the dump's next parameter (midi_job - 1), or its end marker once midi_job is past
// the last. Reads midi_job instead of taking a parameter: no fixed RAM.
static void nrpn_send(void)
{
    tx_flash = TX_FLASH_TICKS;
    hal_midi_tx((uint8_t)(0xB0 | (midi_channel - 1)));
    hal_midi_tx(99);
    hal_midi_tx(NRPN_MSB);
    hal_midi_tx(98);
    hal_midi_tx((midi_job > NP_COUNT) ? NP_DUMP_END : (uint8_t)(midi_job - 1));
    hal_midi_tx(6);

    if (midi_job > NP_COUNT)
    {
        hal_midi_tx(0);
    }
    else if (np_kind((uint8_t)(midi_job - 1)) == NK_READ)
    {
        hal_midi_tx((uint8_t)(nrpn_get((uint8_t)(midi_job - 1)) >> 7));
        hal_midi_tx(38);
        hal_midi_tx((uint8_t)(nrpn_get((uint8_t)(midi_job - 1)) & 0x7F));
    }
    else
    {
        hal_midi_tx(nrpn_get((uint8_t)(midi_job - 1)));
    }
}

// ctl_tick's share of MIDI configuration (run mode): the next step of a dump, one
// parameter per tick once MIDI OUT has room for it.
static void midi_job_run(void)
{
    // At most 9 bytes: CC 99, 98 and 6 with running status (7), plus CC 38 (2).
    if (hal_midi_space() < 9)
    {
        return;
    }

    while (midi_job <= NP_COUNT && np_kind((uint8_t)(midi_job - 1)) == NK_NONE)
    {
        midi_job++;
    }

    nrpn_send();
    midi_job = (midi_job > NP_COUNT) ? 0 : (uint8_t)(midi_job + 1);
}

// A bank change (bank_busy, run mode), once any undo waiting in the revert queue has gone
// out. Nothing is sent. First the old bank's states go into key_val as the values they
// stand for (on: the on value, off: the off value; a note 0x80 + state), so a state shown
// but never sent (a rest state, say) carries too. Then each switch of the new bank starts
// in its power-on state (on only for inverted momentary, not a single program) and, if
// its CC or note has a value, takes the state that value gives it (remote_on, as for MIDI
// IN). No press is kept across the change, so a held switch does nothing on release. The
// new bank's pedal CCs, responses and relay settings apply from the next reading or tick.
static uint8_t relays_compute(void);

static void bank_run(void)
{
    // Static and IDATA: kept out of the directly addressed RAM, and not pushed around each
    // EEPROM read (the stack).
    static IDATA uint8_t i;
    static IDATA uint8_t v;

    // Room for the new bank's program change, if it has one.
    if (stomp_revert || hal_midi_space() < 2)
    {
        return;
    }

    for (i = 0; i < STOMP_COUNT; i++)
    {
        if (msg_type(i) != MT_PROG)
        {
            key_val[switch_key(i)] = switch_value(i, stomp_on(i));
        }
    }

    hal_bank    = bank_next;
    bank_busy   = 0;
    rec_n       = 0;
    load_records(HAL_REC_CAL);
    pedals_rebase();
    stomp_state = 0;
    stomp_down  = 0;
    stomp_held  = 0;

    for (i = 0; i < STOMP_COUNT; i++)
    {
        stomp_put(i, rests_on(i));

        if (msg_type(i) != MT_PROG)
        {
            v = key_val[switch_key(i)];

            if (v != KEY_UNKNOWN)
            {
                stomp_put(i, remote_on(i, v));
            }
        }
    }

    led_stomp = stomp_state;
    show_stomp_leds();

    // The new bank's program change, if set: it becomes the current program, as a
    // program sent by a footswitch does (the display keeps showing the bank in bank mode).
    if (bank_pc != 0)
    {
        program = BANK_PC_PROGRAM;
        send_program(program);
        show_program();
    }

    // The new bank's relay triggers are only taken: the relays stay as they were.
    relay_resync = 1;
    relays_compute();
}

// A command (in rx_data1). Save and reload run at once, so a command that follows (a dump,
// say) cannot overtake them; the EEPROM access is still less deep on the stack than from
// the key handlers. A dump starts on the next tick (and a new one restarts it).
static void nrpn_command(void)
{
    if (rx_data1 == NC_DUMP)
    {
        midi_job = 1;
    }
    else if (rx_data1 == NC_SAVE)
    {
        save_settings();
    }
    else if (rx_data1 == NC_RELOAD)
    {
        load_settings();
        // Every switch goes to its resting state under the reloaded settings (next tick).
        cfg_dirty = ALL_SWITCHES;
        resync_pedals();
    }
    else if (rx_data1 == NC_HEEL || rx_data1 == NC_TOE)
    {
        // Both pedals must be CAL_MIN_SPAN from the other end, as on the calibration pages.
        if (span_of(last_raw[0], (rx_data1 == NC_HEEL) ? toe[0] : heel[0]) < CAL_MIN_SPAN ||
            span_of(last_raw[1], (rx_data1 == NC_HEEL) ? toe[1] : heel[1]) < CAL_MIN_SPAN)
        {
            return;
        }

        if (rx_data1 == NC_HEEL)
        {
            heel[0] = last_raw[0];
            heel[1] = last_raw[1];
        }
        else
        {
            toe[0] = last_raw[0];
            toe[1] = last_raw[1];
        }

        resync_pedals();
    }
}

// Write rx_data1 into footswitch i's message field for parameter nrpn_lsb (a leaf).
static void msg_write(uint8_t i)
{
    uint8_t q = MSG_AT(i);

    if (nrpn_lsb < NP_NUM)
    {
        msg[q]     = (uint8_t)((msg[q] & 0x7F) | ((rx_data1 & 1) << 7));
        msg[q + 1] = (uint8_t)((msg[q + 1] & 0x7F) | ((rx_data1 & 2) << 6));
        return;
    }

    q = (uint8_t)(q + (uint8_t)(nrpn_lsb - NP_TYPE) / (uint8_t)STOMP_COUNT - 1);
    msg[q] = (uint8_t)((msg[q] & 0x80) | rx_data1);
}

// Set parameter nrpn_lsb to rx_data1 (the CC 6 value, put there by nrpn_cc, so this takes
// no parameter). Values out of range, read-only parameters and unchanged values are
// ignored. A changed pedal mode or message puts the switch in its resting state on the next
// tick (cfg_dirty, settle_dirty), as leaving configuration mode does; a message change to
// a switch that is on first sends the old message's off and turns it off, as the editor does.
static void nrpn_set(void)
{
    uint8_t i;

    if (nrpn_lsb == NP_COMMAND)
    {
        nrpn_command();
        return;
    }

    // The bank's program change: setting the program turns it on, even to the program it
    // already shows while off (so this comes before the unchanged check).
    if (nrpn_lsb == NP_BANK_PC)
    {
        bank_pc = (uint8_t)(rx_data1 + 1);
        return;
    }

    if (np_kind(nrpn_lsb) != NK_SET || nrpn_get(nrpn_lsb) == rx_data1)
    {
        return;
    }

    if (nrpn_lsb == NP_CHANNEL)
    {
        if (rx_data1 >= 1 && rx_data1 <= 16)
        {
            midi_channel = rx_data1;
        }

        return;
    }

    // On / off (the value differs, so it is 0 or 1 here only if it changes): on keeps the
    // program parameter 92 reads (127 if it was never set).
    if (nrpn_lsb == NP_BANK_PC_ON)
    {
        if (rx_data1 <= 1)
        {
            bank_pc = rx_data1 ? (uint8_t)(BANK_PC_PROGRAM + 1) : 0;
        }

        return;
    }

    // A pedal's CC (a data byte, so always 0-127), or its pickup mode on / off (the value
    // differs, so a 0 or 1 flips it). The pedal's value then counts as where it is.
    if (nrpn_lsb >= NP_PICKUP)
    {
        if (nrpn_lsb >= NP_PEDAL_CC)
        {
            pedal_cc[(uint8_t)(nrpn_lsb - NP_PEDAL_CC)] = rx_data1;
        }
        else if (rx_data1 <= 1)
        {
            pk_mask ^= (uint8_t)(1 << (nrpn_lsb - NP_PICKUP));
        }

        pedals_rebase();
        return;
    }

    // A bank change, as UP / DOWN make one in bank mode (bank_run). Unsaved changes to
    // the old bank's footswitches are dropped.
    if (nrpn_lsb == NP_BANK)
    {
        if (rx_data1 >= 1 && rx_data1 <= BANK_COUNT)
        {
            bank_next = (uint8_t)(rx_data1 - 1);
            bank_busy = 1;

            if (!pc_mode && !boot_anim)
            {
                show_program();
            }
        }

        return;
    }

    if (nrpn_lsb < NP_TYPE)
    {
        if (rx_data1 < PM_COUNT)
        {
            i = (uint8_t)(nrpn_lsb - NP_MODE);
            pedal_mode_set(i, rx_data1);
            cfg_dirty |= bit16(i);
        }

        return;
    }

    if (nrpn_lsb < NP_FOLLOW1)
    {
        if (nrpn_lsb < NP_NUM && rx_data1 > MT_PROG)
        {
            return;
        }

        i = (uint8_t)((uint8_t)(nrpn_lsb - NP_TYPE) % (uint8_t)STOMP_COUNT);

        if (stomp_on(i))
        {
            if (hal_midi_space() >= 3 && msg_type(i) != MT_PROG)
            {
                send_switch(i, 0);
            }

            stomp_state &= (uint16_t)~bit16(i);
            hal_stomp_led(i, 0);
        }

        msg_write(i);
        cfg_dirty |= bit16(i);
        return;
    }

    if (nrpn_lsb < NP_TRIGGER)
    {
        // A follow flag: the value differs from the current one, so a 0 or 1 flips it.
        if (rx_data1 <= 1)
        {
            if (nrpn_lsb < NP_FOLLOW2)
            {
                sw_mask[0] ^= bit16((uint8_t)(nrpn_lsb - NP_FOLLOW1));
            }
            else
            {
                sw_mask[1] ^= bit16((uint8_t)(nrpn_lsb - NP_FOLLOW2));
            }
        }

        return;
    }

    sw_thr[(uint8_t)(nrpn_lsb - NP_TRIGGER)] = rx_data1;
}

// A received CC on the pedal's channel in run mode (number in rx_data1): the NRPN selects
// and data entry.
static void nrpn_cc(uint8_t value)
{
    if (rx_data1 == 99)
    {
        nrpn_msb_ok = (value == NRPN_MSB) ? 1 : 0;
    }
    else if (rx_data1 == 98)
    {
        nrpn_lsb = value;
    }
    else if (rx_data1 == 101 || rx_data1 == 100)
    {
        nrpn_msb_ok = 0;
    }
    else if (rx_data1 == 6 && nrpn_msb_ok && nrpn_lsb < NP_COUNT)
    {
        rx_data1 = value;
        nrpn_set();
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

    // A note goes to remote_cc as 0x80 + its state (note on with a velocity above 0).
    if ((rx_status & 0xF0) != 0xB0)
    {
        byte = (uint8_t)(((rx_status & 0xF0) == 0x90 && byte != 0) ? 0x81 : 0x80);
    }

    remote_cc(rx_data1, byte);

    // Configuration over MIDI (after remote_cc: nrpn_cc may reuse rx_data1).
    if ((rx_status & 0xF0) == 0xB0 && mode == MODE_RUN)
    {
        nrpn_cc(byte);
    }
}

void ctl_pedal_sample(uint8_t channel, uint8_t raw)
{
    uint8_t v;

    last_raw[channel] = raw;

    flush_reverts();

    if (mode != MODE_RUN)
    {
        if (save_prompt)
        {
            return;
        }

        // On a SWITCH page a pedal sets that relay's trigger value (it sets no other
        // value, user's request); on an EXP page with the calibration selected, the
        // page's pedal shows its value. Nothing is sent.
        if (IS_SW_PAGE(cfg_page))
        {
            sw_pedal(channel);
        }
        else if (IS_EXP_PAGE(cfg_page) && channel == pcc_b && ped_sel == PED_CAL &&
                 cal_step == CAL_DONE && !pcc_open)
        {
            hal_display(ctl_scale(raw, heel[channel], toe[channel]));
        }

        return;
    }

    v = ctl_scale(raw, heel[channel], toe[channel]);

    // A pickup pedal not yet picked up (pk_wait) sends nothing until it reaches its CC's
    // last known value or passes it; its moves are measured from pk_pos. Any other pedal's
    // are measured from the value it last sent.
    pk_wait = pk_waiting(channel);
    pk_p    = pk_wait ? pk_pos[channel] : sent[channel];

    // Ignore small wobble, but always let the pedal reach 0 and 127. The first reading
    // (after power-up or configuration mode) only records the position.
    if (pk_p != NO_VALUE &&
        (v == pk_p || (span_of(raw, ref_raw[channel]) < PEDAL_HYST && v != 0 && v != 127)))
    {
        return;
    }

    // What this reading does: PK_MOVED only records the position, PK_KNOWN also makes it
    // the pedal's value without sending it (the CC already has it), PK_SEND sends it.
    pk_act = PK_SEND;

    if (pk_wait)
    {
        pk_t   = pk_target(channel);
        pk_act = PK_MOVED;

        // Picked up: within PEDAL_PICKUP_WINDOW of the value, or past it since the last move.
        if (span_of(v, pk_t) <= PEDAL_PICKUP_WINDOW ||
            (pk_p != NO_VALUE && (pk_p < pk_t) != (v < pk_t)))
        {
            pk_act = (v == pk_t) ? PK_KNOWN : PK_SEND;
        }

        // Once moved, the display shows the distance (position minus the value).
        if (pk_p != NO_VALUE && !pc_typing && !boot_anim)
        {
            hal_display_signed((uint8_t)(v - pk_t));
            pk_show = PICKUP_SHOW_TICKS;
        }
    }
    else if (pk_p == NO_VALUE)
    {
        sent[channel] = v;
        pk_act = PK_MOVED;
    }

    if (pk_act == PK_SEND)
    {
        // Not enough room now: leave state alone so it is retried next reading.
        if (hal_midi_space() < 3)
        {
            return;
        }

        send_cc(pedal_cc[channel], v);
    }

    // The CC's value is now known (a pickup pedal stays picked up while it is this).
    if (pk_act != PK_MOVED)
    {
        sent[channel] = v;
        key_val[pedal_cc[channel]] = v;
    }

    pk_pos[channel]  = v;
    ref_raw[channel] = raw;
}

// Run mode, every tick: each relay's triggers give on if any is on (logical OR): a
// followed footswitch that is on, or a pedal at or above its trigger value (the last CC
// value sent); the relay moves only when that result changes. Until a pedal has a value
// again (after start-up or configuration mode), its trigger keeps the result as it was.
// The SWITCH LEDs follow.
// The relay outputs (bit 0 SWITCH 1, bit 1 SWITCH 2) for relays_update. A leaf function,
// so its locals stay in registers instead of the directly addressed RAM.
static uint8_t relays_compute(void)
{
    uint16_t on = stomp_state;
    uint8_t  out = 0;
    uint8_t  i;
    uint8_t  bit;

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
            // No pedal value yet (after start-up or configuration mode): as it was.
            if (sent[i & 1] == NO_VALUE)
            {
                out |= (uint8_t)(relay_lvl & bit);
            }
            else if (sent[i & 1] >= sw_thr[i])
            {
                out |= bit;
            }
        }
    }

    // Only the relays whose triggers' result changed move (user's request); after a bank
    // change the new bank's result is only taken (relay_resync), moving nothing.
    if (relay_resync)
    {
        relay_resync = 0;
        relay_lvl    = out;

        return relay_out;
    }

    bit       = (uint8_t)((relay_out ^ out) & (relay_lvl ^ out));
    relay_lvl = out;

    return (uint8_t)(relay_out ^ bit);
}

static void relays_update(void)
{
    uint8_t out = relays_compute();

    if (out != relay_out)
    {
        relay_out = out;
        hal_relays(relay_out);
        hal_aux_led(HAL_LED_SWITCH_1, relay_out & 1);
        hal_aux_led(HAL_LED_SWITCH_2, (relay_out >> 1) & 1);
    }
}

// During the start-up settling (no ctl_tick yet): only the power-on scroll moves on.
void ctl_settle_tick(void)
{
    if (boot_anim)
    {
        version_tick();
    }
}

void ctl_tick(void)
{
    flush_reverts();
    flush_program();

    if (mode == MODE_RUN)
    {
        relays_update();

        // A bank change; a dump waits for it. Then any switch whose mode or message
        // changed over MIDI goes to its resting state.
        if (bank_busy)
        {
            bank_run();
        }

        if (midi_job && !bank_busy)
        {
            midi_job_run();
        }

        if (cfg_dirty)
        {
            settle_dirty();
        }
    }

    if (mode == MODE_RUN)
    {
        if (rx_flash && --rx_flash == 0)
        {
            hal_aux_led(HAL_LED_MIDI_CHAN, 0);
        }

        // MIDI OUT: MIDI FUNCTION lit for TX_FLASH_TICKS after each message (user's
        // request).
        if (tx_flash)
        {
            hal_aux_led(HAL_LED_MIDI_FUNC, --tx_flash != 0);
        }

        pickup_tick();
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

    // Typing a program number: the right-hand dot flashes; once typing ends (the number
    // sent or dropped, by any route) it goes out on the next tick.
    if (pc_typing)
    {
        if (++chan_flash_ticks >= CHAN_FLASH_TICKS)
        {
            chan_flash_ticks = 0;
            pc_dot = !pc_dot;
            hal_display_dots(pc_dot ? 1 : 0);
        }
    }
    else if (pc_dot)
    {
        pc_dot = 0;
        hal_display_dots(0);
    }

    // Only set on the channel page in configuration mode.
    if (chan_digit_pending)
    {
        channel_digit_tick();
    }
    else if (mode == MODE_CONFIG && cfg_page == PAGE_BANK_PC && !pc_typing && !save_prompt)
    {
        bank_label_tick();
    }

    // Config mode: the blink. A number being entered flashes the right-hand dot (set
    // every tick, so a redraw cannot leave it wrong), and an armed heel / toe its Lo / Hi.
    // Not on page 8, whose dots are the version's, nor during page 7's "--" (pc_dot).
    if (mode == MODE_CONFIG)
    {
        if (++blink_ticks >= CHAN_FLASH_TICKS)
        {
            blink_ticks = 0;
            blink_on    = !blink_on;

            // An "Er" from the calibration shows for CAL_ERROR_SHOW_MS, then the value.
            if (IS_EXP_PAGE(cfg_page) && cal_step == CAL_ERROR && !pcc_open && --cal_err_left == 0)
            {
                cal_step = CAL_DONE;
                config_show();
            }

            if ((save_prompt || (IS_EXP_PAGE(cfg_page) && cal_armed && !pcc_open)))
            {
                config_show();
            }
        }

        if ((save_prompt || cfg_page != PAGE_VERSION) && !pc_typing)
        {
            hal_display_dots((save_prompt ? !blink_on : (num_entry() && blink_on)) ? 1 : 0);
        }
    }

    if (boot_anim || (mode == MODE_CONFIG && cfg_page == PAGE_VERSION && !save_prompt))
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
// Read-only view of the controller state for the browser editor's labels and hints.
// Only compiled when CTL_INSPECT is defined (the editor build), never into the firmware.
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
    case CTL_INSPECT_BANK:          return hal_bank;
    case CTL_INSPECT_BANK_PC:       return bank_pc;
    case CTL_INSPECT_PC_MODE:       return (uint8_t)(pc_mode | (pc_typing << 1));
    case CTL_INSPECT_PICKUP:        return (uint8_t)(pk_mask | (pk_waiting(0) << 2) | (pk_waiting(1) << 3));
    case CTL_INSPECT_PEDAL_CC:      return pedal_cc[0];
    case CTL_INSPECT_PEDAL_CC + 1:  return pedal_cc[1];
    case CTL_INSPECT_PCC:           return (uint8_t)(pcc_open | (pcc_b << 1));
    case CTL_INSPECT_PED_SEL:       return ped_sel;
    case CTL_INSPECT_SAVE:          return save_prompt ? save_bank : 0;
    case CTL_INSPECT_CAL_ARMED:     return cal_armed;
    default:                        return 0;
    }
}
#endif
