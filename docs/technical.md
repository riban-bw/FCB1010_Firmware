# Technical implementation

[Back to the README](../README.md)

How the firmware works inside: its structure, how a key press or a MIDI byte travels through it, the state it keeps, its timing, memory and EEPROM layout. For building it see [Building](building.md); for what it sends and receives see [MIDI implementation](midi.md).

## Structure

The firmware is C for SDCC (8051, small memory model), in three layers:

| Layer | Files | Job |
|---|---|---|
| Hardware | `src/hw.c`, `src/hw.h`, `src/keys.h` | The FCB1010's circuits: the LED, display and key multiplex, the MIDI UART, the pedal ADC (an ADC0832, bit-banged), the 24C16 EEPROM (bit-banged I2C) and the relays |
| Interface | `src/hal.h` | The only functions the controller logic may call: send a MIDI byte, set an LED, show a number or text, load or save a settings record, set the relays |
| Controller logic | `src/ctl.c`, `src/ctl.h` | Everything the pedal does: modes, messages, banks, configuration pages, MIDI IN, relays. Portable C with no hardware access |

`src/main.c` starts the hardware, gives the key debouncer and pedal averager 0.2 seconds to settle (`STARTUP_TICKS`), discards anything pressed meanwhile, then loops forever. Each pass it:

1. calls `ctl_tick()` once per 2ms timer tick, and passes each new pedal reading to `ctl_pedal_sample()`;
2. passes every byte waiting in the MIDI IN queue to `ctl_midi_in()`;
3. takes one key event from the key queue and passes it to `ctl_key_press()`, `ctl_key_hold()` or `ctl_key_release()`.

Because the logic sees only `hal.h`, the same `ctl.c` runs in three other places: the host tests (`test/test_ctl.c`, against a fake hardware layer that records everything), the browser editor (`ctl.c` and `keys.c` compiled to WebAssembly, with `editor/emu.c` standing in for `hw.c` and `main.c`), and the 8051 simulator (the real image).

## Interrupts

Two interrupts do the time-critical work; everything else runs in the main loop.

- **Timer 0, every 2ms** (low priority). It drives one row of the LED and display multiplex (for the footswitch LED rows, first with the dim LEDs lit as well, then, after the key scan, without them, so a dim LED is lit for only a small part of the row's time; unverified on hardware), reads one column of the key matrix (two keys, one from each row), runs the debounce and hold detection for those keys and queues their events, writes the relay bits with the key column, and counts the tick for the main loop. The key detection is written as macros (`KEY_SCAN` in `src/keys.h`, expanded once and run for both keys of the column) so the interrupt calls no function, which would make SDCC save every register on the stack. `src/keys.c` wraps the same macros as functions for the tests and the editor.
- **Serial** (high priority, so it can interrupt the timer). It moves a received byte into the MIDI IN queue and the next waiting byte from the MIDI OUT queue to the UART.

The queues are ring buffers with 8-bit head and tail indices, each one written by only one side, so they need no locking except that `hal_midi_tx` briefly masks the serial interrupt while it adds a byte and starts the transmitter.

## Keys

Each key is sampled every 12ms (six columns, one per tick). A press counts once `DEBOUNCE_SAMPLES` samples in a row read pressed, and a release once as many read released. A key held for `MOMENTARY_HOLD_MS` after its press gives one hold event. A key's state changes only once its event is in the queue: if the queue is full the event is tried again on the key's next sample, so a burst (all twelve keys at once) is delayed, never lost.

Key codes: 0-4 are footswitches 1-5, 5 is DOWN, 6-10 are footswitches 6-10, 11 is UP. `ctl.c` tracks which keys are down, recognises UP + DOWN as a chord (tap, or held for `CONFIG_HOLD_MS`), and runs the program repeat from `ctl_tick()`.

## From a press to MIDI OUT

A footswitch press in run mode:

1. `ctl_key_press()` ignores it if MIDI OUT does not have room for the message, so the pedal never shows a state the receiver did not hear about.
2. The switch's pedal mode decides its new state (toggle, or on while held, or inverted) and the switch's message is sent by `send_switch()`: a CC with its ON or OFF value, a note on or off, or a program change.
3. `send_switch()` ends by passing what it sent to `remote_cc()`, the same function MIDI IN uses. That records the value in the CC and note table and sets every other footswitch with the same CC or note by the MIDI IN rule, so switches that share a controller share its state.
4. On release, a momentary switch sends its other value, and a stomp with momentary switch held past the hold time is undone. If MIDI OUT is full at that moment the undo waits in a revert queue (`stomp_revert`) and goes out from `ctl_tick()` as soon as there is room; the LED changes when it is sent.

## MIDI IN

`ctl_midi_in()` is a byte-at-a-time parser with running status. It ignores real-time bytes, other channels and anything but CC, note on and note off on the pedal's channel. A CC or note goes to `remote_cc()` (note states as 0x80 + on, so CCs and notes share one table without clashing), which sets every matching footswitch: a CC equal to the switch's ON value turns it on, equal to its OFF value off, and otherwise on from 64. A switch whose ON and OFF values are the same keeps its state. Nothing is echoed. Every CC then also goes to the NRPN configuration parser (`nrpn_cc()`), which acts only on CCs 99, 98, 6, 101 and 100, in run mode. SysEx is matched against the Identity Request as it arrives (`sx_pos`, in any mode, against the `sx_request[]` table; any other byte, or a status byte, ends the match), and a complete one sets `sx_reply`; `ctl_tick()` sends the 15-byte reply (`sysex_identity()`) once MIDI OUT has room for all of it.

## Expression pedals

`ctl_pedal_sample()` scales each averaged reading to 0-127 with the calibration and ignores moves under `PEDAL_HYST` ADC counts (except to reach 0 and 127). An immediate pedal sends every move. A pickup pedal (`pk_mask`, saved with the bank) is picked up while its controller's last known value (`key_val[]`, which the pedal's own sends, footswitches and MIDI IN all update; never seen counts as 0) equals what the pedal last sent (`sent[]`), and waits otherwise. While it waits, a move only records its position (`pk_pos[]`), shows the distance (position minus the value) with `hal_display_signed()`, and restarts a `PEDAL_PICKUP_SHOW_MS` timer after which `ctl_tick()` shows the bank or program again. It picks up when it comes within `PEDAL_PICKUP_WINDOW` of the value, or when the value lies between its last position and this one; the reading is sent then, unless it equals the value. `pickup_tick()`, every tick in run mode, flashes a waiting pedal's EXP LED with a half-period of `PEDAL_PICKUP_FLASH_MS` plus 3ms per CC step from the value, and keeps it dark otherwise. After configuration mode every pedal's first reading is taken afresh, so a pedal moved meanwhile has to pick up again. Each bank has its own pedal controllers (`pedal_cc[]`) and responses; when a bank change or an NRPN set changes them, `pedals_rebase()` takes each pedal's last position as its last sent value, so a pickup pedal then waits for its new controller's value.

## State

| State | Where | What |
|---|---|---|
| Footswitch messages | `msg[]`, 3 bytes per switch (SRAM) | Number with type bit 0 in bit 7; ON value with type bit 1 in bit 7; OFF value |
| Pedal modes | `pedal_modes[]`, 2 bits per switch | 0 stomp, 1 stomp with momentary, 2 momentary, 3 inverted momentary |
| Switch states | `stomp_state`, one bit per switch | What the LEDs show and the receiver last heard |
| CC and note values | `key_val[256]` (SRAM) | The last value of each CC 0-127 and the state of each note (index 128 + note), 0xFF if not seen since power-on |
| Bank | `hal_bank` (in use), `bank_next` (chosen) | 0 to `BANK_COUNT` - 1 |
| Relays | `sw_mask[]`, `sw_thr[]` | Followed switches and pedal trigger values (of the bank in use) |
| Pedal controllers | `pedal_cc[]` | The CC each pedal sends (of the bank in use) |
| Pedal pickup | `pk_mask` (bit 0 pedal A, bit 1 pedal B; of the bank in use), `pk_pos[]`, `pk_flash[]`, `pk_led`, `pk_show` | Which pedals are in pickup mode, each pedal's last position, the EXP LED flash, and the distance display timer |
| Calibration, channel | `heel[]`, `toe[]`, `midi_channel` | Raw ADC heel and toe readings; MIDI channel |

Configuration mode reuses run-mode bytes where the two never overlap (the editor's backup shares the pedal reference readings, the page number shares the NRPN dump counter, and so on), which was needed when everything had to fit in the processor's own RAM.

## Banks

Each bank is ten footswitch records and a bank record in the EEPROM (footswitch messages and modes, relay settings, pedal controllers and responses, program change). Choosing a bank sets `bank_next`; the change itself (`bank_run()`, from `ctl_tick()`) waits until no undo is waiting in the revert queue. It then:

1. writes each old switch's shown state into the CC and note table (its ON or OFF value, or the note state), so a resting state that was never sent carries too;
2. loads the new bank's eleven records (its relays and pedal settings with the footswitches);
3. gives each new switch its power-on state (on only for inverted momentary), then, if the table knows its CC or note, the state that value gives it by the MIDI IN rule;
4. takes the new bank's relay trigger result (`relay_lvl`, `relay_resync`) without moving the relays: a relay moves only when its triggers' result changes.

It then sends the new bank's program change, if it has one (`bank_pc`, set on configuration page 7), and makes it the current program; `bank_run()` waits for room for it in MIDI OUT. Nothing else is sent, and a switch held through the change does nothing on release. Because the table keeps every CC and note, a state survives any number of bank changes, including through banks that do not use it.

## Configuration mode

Configuration mode is a set of pages (`cfg_page`), each a small state machine in the key handlers, plus the footswitch editor on page 1 (`edit_open`) and the pedal CC editor on pages 3 and 4 (`pcc_open`, opened by footswitch 6's second press). Page 1 selects a footswitch (`cfg_sel`) and shows its settings in turn (`cyc_elem`, stepped by `cyc_tick()` every `CONFIG_SHOW_MS`, the number skipped for a program change); the selected footswitch pressed again (`edit_pending`, acting on its release with UP and DOWN up) or any held one opens the editor. The editor steps `edit_param` through the pedal mode (`EP_FUNC`, MIDI FUNCTION, footswitches 1-4), the type (`EP_TYPE`, SELECT, footswitches 1 / 6 / 10 through `sw_type[]`) and the three message bytes (`PARAM_BYTE()`); its changes apply at once, and `edit_bak` / `edit_mode_bak` keep the message and mode as it opened, for DOWN on the pedal mode to put back. The choice made is lit and the others that can be chosen dim (`led_stomp`, `led_dim`, `hal_stomp_led()` with `HAL_STOMP_DIM`). Pages 3 and 4 (EXP A and EXP B, `IS_EXP_PAGE()`, `pcc_b` the page's pedal; `ped_press()`, the same footswitches on both: 6 CC, 8 pickup, 9 calibration) hold that pedal's settings: the footswitch last let go (`ped_sel`) picks what the display shows; footswitches act on release, and only with UP and DOWN up. Footswitch 9 steps `cal_step` through the pedal's value (shown live from `ctl_pedal_sample`), `Lo` flashing, the heel reading (`last_raw[]`, kept in `cal_low`) with `Hi` flashing, and the toe reading: both are stored only if they are `CAL_MIN_SPAN` apart, otherwise `Er` shows for `CAL_ERROR_SHOW_MS` (`cal_err_left`, counted in blinks). Pedals set no values in configuration mode except the SWITCH pages' triggers. Every number (channel, the editor's bytes, a pedal's CC, the bank's program, the bank to save to) goes through one entry engine: `num_begin(hi, lo1)` sets the range, `num_digit()` takes a footswitch's digit only if `num_avail()` (the mask of digits that can come next, shown dim) allows it and UP and DOWN are up, and stores it at once through `num_put()`; `num_typing` stays set while another digit would fit, which flashes the right-hand dot (`cfg_dots()`), and while it is set UP ends the number and DOWN puts back `num_old` (`num_cancel()`), both without changing page or setting. Hold events in the editors are ignored, so a long press acts as a tap on release. Page 7 shows `Pr` and the bank's program in turn (`bank_label_tick()`, `BANK_PC_LABEL_MS`), but not while a number is typed. Page 8 lights every LED for `LAMP_TEST_MS` (`ver_lamp`), then chases the panel LEDs (`chase_led[]`, `LAMP_CHASE_MS` each); footswitch 10 there lights everything (`HAL_LAMP_ALL_ON`) while held. A free-running config-mode blink (`blink_on`, every `CHANNEL_DIGIT_FLASH_MS`) drives the flashing dot, `Lo` / `Hi` and the save prompt. A long UP opens the save prompt (`save_prompt`, `save_bank`; the number and the dot alternate, with the same blink, the number steady while it can take more digits): footswitches type the bank on release (only with UP and DOWN up), short UP sets `hal_bank` to it and saves (`exit_config(1)`, so `hal_rec_save` writes that bank's records), short DOWN goes back to the page. Edits change the live settings at once; saving writes every record, and cancelling reloads them from the EEPROM, so no copy of the settings is kept. The CONFIG LED flashes while `config_changed()` finds any difference between the live settings and what the EEPROM holds, read back record by record and compared the way loading would take them.

## Timing details

- **Long press.** The threshold is `MOMENTARY_HOLD_MS` in `src/config.h` (currently 600). The key scanner checks each switch every 12ms and starts timing once the press has been debounced (`DEBOUNCE_SAMPLES`), so the real threshold is about 0.64s. The same hold saves (UP) or cancels (DOWN) in configuration mode. If a CC arrives over MIDI IN during the hold, the release still goes back to the state before the press. A switch already down at power-on does nothing when released. UP and DOWN are not affected in run mode.
- **Program repeat.** Held: after 400ms the number auto-repeats at 4Hz on the display only, speeding up to about 25Hz once the key has been held for `PROGRAM_REPEAT_FAST_AFTER_MS` (2.7s), and one program change is sent on release.
- **Configuration.** UP + DOWN held 1s opens configuration mode (`CONFIG_HOLD_MS`); in configuration mode UP + DOWN saves and leaves at once (on the next tick, from `ctl_tick`'s chord timer, so the save runs on the main loop's stack, not under the key handler). A short UP + DOWN toggles program mode (`pc_mode`, DIRECT SELECT lit; bank mode is the default) in run mode.
- **Program entry.** In program mode a footswitch press (not its release) chooses a program: 1-9 at once, 10 starts typing (`pc_typing`, the number in `pc_entry`, shown as on the display, so with `DISPLAY_PROGRAM_OFFSET`; the right-hand dot flashes every `CHANNEL_DIGIT_FLASH_MS` meanwhile). A digit is appended while the number stays within the highest shown program, and the number is sent once it is over a tenth of that (13 and up), otherwise on UP's release; DOWN's release, or UP with nothing typed, goes back to the current program. While typing, UP and DOWN neither step nor repeat. A short UP + DOWN or entering configuration mode drops the number. The program goes out when MIDI OUT has room for it (`program_pending`). The stomp states are left alone, and their LEDs keep showing them.
- **Bank change.** In bank mode (the default) UP / DOWN repeat at the program rates; the bank is loaded on release, on the next 2ms tick (later only if a stomp with momentary revert is waiting for room in MIDI OUT). Loading reads the new bank's eleven records from the EEPROM. The last value of every CC (0-127) and the state of every note is kept in a 256-byte table in the external SRAM, set whenever one is sent or received in run mode and, at a bank change, from the old bank's switch states; the new bank's switches take their state from it. It is cleared at power-on.
- **Page 8 and power-on.** Each scroll frame shows for `SCROLL_FRAME_MS` (200ms), and the scroll moves the strip ". riban ." one place left per frame across the display's three dots ("+1.8.8."). It starts with a lead-in of two frames, the display blank and the leading dot at the far right, then between the digits; then " r" (dot after the "1"), "ri", "ib", "ba", "an" (no dot), "n " (dot at the far right), and two blank frames as the trailing dot runs off (between the digits, then after the "1"). Power-on is those ten frames, 2 seconds at 200ms: the scroll keeps time through the 0.2 seconds of start-up settling (`main()` calls `ctl_settle_tick` then, not `ctl_tick`), so the settling counts towards it. Each version part shows as two digits (major and minor in decimal, the build in hex), one part per second, then a blank of the same length followed by the lead-in again. The ten steps, one per footswitch: " r" to "an", "n " with the trailing dot, major, minor, build, and the blank with the lead-in. Power-on and page 7 start at the lead-in. During each step its footswitch LED is dark for about the first half of a scroll frame. Power-on shows the scroll only, once, with every LED lit for its first `BOOT_LAMP_MS` (1 second, from power-on), then the normal LEDs (the stomp states and so on) while the scroll finishes. Page 7 keeps them lit throughout.
- All timing comes from a 2ms timer tick and assumes the FCB1010's 12MHz crystal, as does the 31250 baud MIDI rate.

## Memory

The 8032 has 256 bytes of internal RAM, which holds the stack, the flags and the small state. The larger buffers and tables live in the board's 32KB SRAM, which the factory schematic places at 0x8000-0xFFFF in the external data space (its chip select is A15 through an inverter; below 0x8000 every write reaches the display and key latches, so nothing is placed there). This is from the schematic only: it has not been checked on a real board, and the [probe image](sram-probe.md) is the way to check it before trusting the firmware. In the SRAM: the MIDI OUT queue (128 bytes, about 40ms of messages), the MIDI IN queue (128 bytes, enough to ride over an EEPROM save), the key event queue (16), the key debounce and hold counters, the footswitch messages of the bank in use and the CC and note state table (256 bytes), about 590 bytes in all. The stack starts at 0x90 with 112 bytes free, and peaks at 0xb1 in the simulator (with both interrupts nested on top). If you add features, check `build/fcb1010.mem` for the stack headroom and run `test/sim_smoke.sh`, which fails if the stack gets too close to the end.

## Settings storage (EEPROM)

Settings are stored in the 24C16 EEPROM (2KB, addresses 0x000-0x7FF). The two shared records are at the top (0x7F0-0x7FF), 6 bytes each in 8-byte slots; 0x7E0-0x7EF held the relay records before they moved into the banks, and is now unused. Each bank has eleven 5-byte records, packed with no gaps: ten footswitch records (50 bytes a bank: bank 1 at 0x7AE-0x7DF and each further bank 50 bytes (0x32) lower, so bank b starts at 0x7E0 - 50 × b; bank 2 at 0x77C, bank 36 at 0x0D8) and the bank's own record (bank 1 at 0x0D3, each further bank 5 bytes lower, bank 36 at 0x024). That is 36 banks; 0x000-0x023 is spare. A record can cross one of the EEPROM's 16-byte write pages, so saving it then takes two write cycles. The layout has room for 36 banks, but `BANK_COUNT` is limited to 30, so the room of banks 31 to 36 stays free for later settings without moving anyone's banks. The footswitch records are where the 40-bank builds kept them, so banks 1 to 30 keep their footswitches; those builds' banks 31 to 40 can no longer be chosen, their shared relay records are lost, and a bank program change saved by them reads wrongly (its bits now hold other settings), so set the relays, pedals and bank program changes again. Builds before 40 banks kept each footswitch record in an 8-byte slot with a magic byte (bank 1 at 0x790, 25 banks); those read as invalid, so a pedal moving from such a build starts with the default footswitches.

The stock firmware keeps its presets from 0x000 upwards, so saving any bank can overwrite them (the bank records are low in the EEPROM; bank 1's footswitch records and the shared records are where the stock presets are least likely to be). A bank that has never been saved still holds whatever was there before, which fails the checksum test and loads as the defaults. To go back to the stock firmware, restore its presets from a SysEx backup or reset them to the factory settings.

| Address | Record | Byte 0 | Byte 1 | Byte 2 | Byte 3 | Byte 4 | Byte 5 |
|---|---|---|---|---|---|---|---|
| bank start + 5 × (n - 1) | Footswitch n (1-10) of a bank (bank 1: 0x7AE, 0x7B3 ... 0x7DB) | bits 0-1: message type; bits 2-3: pedal mode; bit 4: SWITCH 1 follows it; bit 5: SWITCH 2 follows it; bits 6-7: two bits of the bank's pedal settings (below) | number (0-127; bit 7 must be 0) | ON value (0-127; bit 7 must be 0) | OFF value (0-127; bit 7 must be 0) | checksum: the XOR of bytes 0-3, the bank number less 1 and the switch's salt (0x96, 0x69, 0x55, 0xAA, 0x33, 0xCC, 0x0F, 0xF0, 0x5F, 0xF5 for switches 1-10) | (no byte 5) |
| 0x0D3 - 5 × (bank - 1) | Bank record | SWITCH 1 trigger for EXP 1 (bits 0-6: 1-127, 0 = off); bit 7 a bit of the pedal settings | SWITCH 1 trigger for EXP 2, the same | SWITCH 2 trigger for EXP 1, the same | SWITCH 2 trigger for EXP 2, the same | checksum: the XOR of bytes 0-3, the bank number less 1 and the salt 0x4B | (no byte 5) |
| 0x7F0 | Pedal calibration | magic 0xC1 | EXP 1 heel reading | EXP 1 toe reading | EXP 2 heel reading | EXP 2 toe reading | checksum, salt 0x5A |
| 0x7F8 | Channel | magic 0xC2 | MIDI channel (1-16) | unused (saved as 0; held the pickup bits before they moved into the banks, now ignored) | unused (saved as 0) | unused (saved as 0) | checksum, salt 0xA5 |

- **Checksum:** for a shared record, the XOR of bytes 0-4 and the record's salt; for a footswitch or bank record, as in its row. Erased EEPROM (0xFF) fails all of them: a shared record by its magic, a footswitch record by bit 7 of bytes 1-3, a bank record by its checksum (the salt's top two bits are 01, so with the bank number it is never 0xFF; an all-zero record fails too).
- **Pedal modes:** in bits 2-3 of each footswitch record's type byte: 0 stomp, 1 stomp with momentary, 2 momentary and 3 inverted momentary. (Earlier builds kept every mode in the channel record; their footswitch records read as stomp mode, and their stored modes are ignored, so set the modes again.)
- **Calibration:** the readings are the raw 8-bit ADC values.
- **Footswitch messages:** the type is 0 CC, 1 note, 2 program change; type 3 is unused and loads as CC with the same values. Values over 127 lose bit 7. A program change sends the ON value as the program when ON and the OFF value when OFF (both the same: a single program, sent on every press); it ignores the number. (Builds before this change used the number and the ON value; a program change record saved by one now sends its old second program when on and its old OFF value, normally 0, when off, so set it again.)
- **Bank pedal settings:** three bytes, packed into the spare bits: byte 0 the bank's program change (0 for none, otherwise the program + 1; only the program's low 7 bits are used, so every value is valid), byte 1 pedal A's controller number in bits 0-6 and its pickup mode in bit 7, byte 2 the same for pedal B. Footswitch n's bits 6-7 hold bits 2 × ((n - 1) mod 4) and up of byte (n - 1) div 4, so footswitches 1-4 hold byte 0, 5-8 byte 1 and 9-10 bits 0-3 of byte 2; bit 7 of the bank record's byte k (0-3) holds bit 4 + k of byte 2. Every bit of the eleven records is used.
- **Missing or invalid records:** a record with the wrong magic or checksum loads as the defaults:
  - footswitch messages are CC `STOMP_CC_LIST` with ON value 127 and OFF value 0, followed by neither relay;
  - a bank has no program change, the pedal controllers `PEDAL_CC_LIST` and the pedal responses `PEDAL_PICKUP_A` / `PEDAL_PICKUP_B` (each footswitch record and the bank record carries its own part, so one invalid record defaults only that part);
  - relays have no triggers;
  - calibration is heel 0, toe 255;
  - the channel and pedal modes come from `src/config.h` (a footswitch record that is missing or invalid has `DEFAULT_PEDAL_MODE`).
- **Out-of-range values:** a stored channel outside 1-16 loads as the default channel.
- **Saving:** a save writes each record in one page write, or two when a packed record crosses a 16-byte page boundary (about one in four), and reads it back in one sequential read. Loading reads a record the same way.
- **CONFIG LED:** it compares the settings being edited with what is read back from these records (the bank in use and the shared ones).

## Hardware notes

The hardware map (latch addresses, LED and key wiring, ADC and EEPROM pins) comes from jspyve's public-domain reverse-engineering library at github.com/jspyve/FCB1010-lib, not from Behringer documents or from my own measurements. If the key polarity or an LED is wrong on your unit, `KEY_ACTIVE_LEVEL` in `src/config.h` or the LED tables in `src/hw.c` are where to look. The display has three dots ("+1.8.8."): one after each position (bit 0 of its latch byte, the left position's after its "1"). The panel has a hole between the "+" and the "1" but no LED behind it.
