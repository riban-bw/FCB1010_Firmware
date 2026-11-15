// One ROM user plugin: lets the FCB1010 firmware (an 8051 running from the
// One ROM) switch the One ROM to another firmware image, such as the stock
// Behringer firmware, with no extra wiring. DRAFT: compiled, never run.
//
// Why not RBCP (the host-control plugin): an 8051 runs from the ROM it would
// be signalling to, so every data read (MOVC) has code fetches between it and
// the next one. RBCP's knock and its command mode take every read in turn, so
// they would swallow those fetches. This plugin only looks at reads in one
// reserved page the 8051 never runs code from, so the fetches are harmless.
//
// Sequence (see README.md and fcb_frame.h):
// 1. Wait for a frame: key, SWITCH, flash slot, and the address X the 8051
//    will spin at once it has sent the frame (three times).
// 2. Copy that flash slot into the RAM slot not being served.
// 3. Wait until the bus shows the CPU spinning at X..X+7 and nothing else.
// 4. In the copy, save the 9 bytes at X and overwrite them with NOPs and
//    LJMP 0000, then switch to the copy (atomic). The spinning CPU runs into
//    the LJMP and starts the new firmware from its reset vector.
// 5. Once the CPU has fetched from address 0, put the 9 saved bytes back, so
//    the new firmware runs unmodified.
// A power cycle brings back One ROM's boot image (this firmware).

#include <stdint.h>
#include "plugin.h"
#include "fcb_frame.h"

#define MAJOR_VERSION 0
#define MINOR_VERSION 1
#define PATCH_VERSION 0
#define BUILD_VERSION 0

// Ring of raw bus captures filled by DMA. 64 entries (256 bytes) is what
// host-control uses; a user plugin has 1KB for static RAM and stack together.
#define RING_LOG2   6u
#define RING_MASK   ((1u << RING_LOG2) - 1u)

// Non-zero passes check_control_pins to the demangle call for in-page
// captures, as host-control's ring_read_byte does, so captures taken while
// the chip is not selected are dropped. Whether this behaves with /CE tied
// low and only /OE (the 8051's /PSEN) toggling is unverified.
#ifndef FCB_CHECK_CONTROL_PINS
#define FCB_CHECK_CONTROL_PINS 1
#endif

// Captures in a row, all inside the spin loop, before switching (32us of
// bus time at 12MHz, two reads per machine cycle), and the time limits.
#define SPIN_CONFIRM      64u
#define SPIN_TIMEOUT_MS   300u
#define RESET_TIMEOUT_MS  50u

void fcb_main(ora_lookup_fn_t lookup, ora_plugin_type_t type, const ora_entry_args_t *args);

// Hand-written header like host-control's: the plugin never yields, since a
// pause could lose captures (properties1 = 0, no ORA_PROPERTY1_SUPPORTS_YIELD).
ORA_SECTION(".plugin_header")
const ora_plugin_header_t ora_plugin_header = {
    .magic = ORA_PLUGIN_MAGIC,
    .api_version = ORA_PLUGIN_VERSION_1,
    .major_version = MAJOR_VERSION,
    .minor_version = MINOR_VERSION,
    .patch_version = PATCH_VERSION,
    .build_version = BUILD_VERSION,
    .entry = fcb_main,
    .plugin_type = ORA_PLUGIN_TYPE_USER,
    .sam_usage = 255,
    .overrides1 = 0,
    .properties1 = 0,
    .min_fw_major_version = 0,
    .min_fw_minor_version = 8,
    .min_fw_patch_version = 1,
    .reserved = {0},
};

ORA_RING_BUF_DECLARE_32BIT(ring_buf, RING_LOG2);

static volatile uint32_t * volatile *s_write_pos;
static uint32_t s_rd;

static ora_log_fn_t s_log;
static ora_err_log_fn_t s_err;
static ora_demangle_observed_addr_fn_t s_demangle;
static ora_get_plugin_uptime_ms_fn_t s_uptime;
static ora_get_ram_slot_count_fn_t s_ram_count;
static ora_get_active_ram_slot_fn_t s_get_active;
static ora_set_active_ram_slot_fn_t s_set_active;
static ora_get_flash_slot_count_fn_t s_flash_count;
static ora_copy_flash_slot_to_ram_slot_fn_t s_copy;
static ora_read_ram_rom_slot_fn_t s_read_slot;
static ora_reprogram_ram_rom_slot_fn_t s_write_slot;

// Fast page test on raw captures: (capture & s_page_mask) == s_page_val
// means address lines A8 up to the chip's top line (A14 at most) are all 1.
// s_addr_mask holds the address bits the chip has, below A15 (0x7FFF on a
// 27C256 or 27C512, 0x3FFF on a 27C128). A15 is left out of every
// comparison because the 8051 code is linked below 32KB and the level the
// FCB1010 puts on a 27C512's pin 1 is unknown.
static uint32_t s_page_mask, s_page_val;
static uint32_t s_addr_mask;

static inline uint32_t ring_write_index(void) {
    return (uint32_t)(*s_write_pos - ring_buf) & RING_MASK;
}

static inline void ring_skip_to_now(void) {
    s_rd = ring_write_index();
}

// Next capture, or 0 with nothing written when the ring is empty.
static inline int ring_next(uint32_t *phys) {
    if (s_rd == ring_write_index())
        return 0;

    *phys = ring_buf[s_rd];
    s_rd = (s_rd + 1u) & RING_MASK;
    return 1;
}

static inline uint32_t elapsed_ms(uint32_t since) {
    return s_uptime() - since;
}

// Work out which capture bit carries each address line by demangling single
// bits (relative to an all-zero capture, so an inverted line works too), and
// build the page test from that. Only the demangle call is relied on; no
// assumption about the board's pin layout.
static int build_page_test(void) {
    // Static, so it is zeroed with .bss (called once): a zeroed local array
    // would need memset, and plugins link without a C library.
    static uint32_t line_bit[15];
    uint32_t base;

    if (s_demangle(0u, &base, 0) != ORA_RESULT_OK)
        return 0;

    for (uint32_t b = 0; b < 32u; b++) {
        uint32_t obs;

        if (s_demangle(1u << b, &obs, 0) != ORA_RESULT_OK)
            continue;

        uint32_t d = obs ^ base;

        if (d != 0u && (d & (d - 1u)) == 0u) {
            uint32_t line = (uint32_t)__builtin_ctz(d);

            if (line < 15u)
                line_bit[line] |= 1u << b;
        }
    }

    s_addr_mask = 0;
    s_page_mask = 0;
    s_page_val = 0;

    for (uint32_t line = 0; line < 15u; line++) {
        if (line_bit[line] == 0u)
            continue;

        s_addr_mask |= 1u << line;

        if (line >= 8u) {
            s_page_mask |= line_bit[line];

            // The capture bit must read so that the line demangles as 1.
            if (!(base & (1u << line)))
                s_page_val |= line_bit[line];
        }
    }

    // Lines A0-A13 are on every supported chip (16KB and up).
    if ((s_addr_mask & 0x3FFFu) != 0x3FFFu) {
        s_err("FCB: address lines not all found (mask 0x%x)", (unsigned)s_addr_mask);
        return 0;
    }

    return 1;
}

// Block until a valid frame has been read from the reserved page. The hot
// loop: at 12MHz the 8051 makes up to two million reads a second, and the
// ring holds 64, so non-page captures cost one AND and compare.
static void wait_for_frame(fcb_frame_t *f) {
    uint32_t phys, obs;

    fcb_reset(f);
    ring_skip_to_now();

    for (;;) {
        if (!ring_next(&phys))
            continue;

        if ((phys & s_page_mask) != s_page_val)
            continue;

        if (s_demangle(phys, &obs, FCB_CHECK_CONTROL_PINS) != ORA_RESULT_OK)
            continue;

        if (fcb_rx(f, (uint8_t)obs))
            return;
    }
}

// Wait until SPIN_CONFIRM captures in a row lie in the spin loop at x. Gives
// the A15 level seen (as an address bit) in *hi, or returns 0 on timeout.
static int wait_for_spin(uint32_t x, uint32_t *hi) {
    uint32_t phys, obs, run = 0, start = s_uptime();

    ring_skip_to_now();

    while (run < SPIN_CONFIRM) {
        if (!ring_next(&phys)) {
            if (elapsed_ms(start) > SPIN_TIMEOUT_MS)
                return 0;
            continue;
        }

        if (s_demangle(phys, &obs, FCB_CHECK_CONTROL_PINS) != ORA_RESULT_OK)
            continue;

        uint32_t a = obs & s_addr_mask;

        if (a >= x && a < x + FCB_SPIN_SPAN) {
            run++;
            *hi = obs & ~s_addr_mask;
        } else {
            run = 0;
        }
    }

    return 1;
}

// Wait for the CPU to fetch from 0-2 (the reset vector's LJMP) after the
// switch. Returns 0 on timeout.
static int wait_for_reset(void) {
    uint32_t phys, obs, start = s_uptime();

    ring_skip_to_now();

    for (;;) {
        if (!ring_next(&phys)) {
            if (elapsed_ms(start) > RESET_TIMEOUT_MS)
                return 0;
            continue;
        }

        if (s_demangle(phys, &obs, FCB_CHECK_CONTROL_PINS) == ORA_RESULT_OK &&
            (obs & s_addr_mask) < 3u)
            return 1;
    }
}

static void do_switch(uint8_t flash_slot, uint32_t x) {
    uint8_t active, target, saved[FCB_PATCH_LEN];
    uint32_t hi = 0, at;

    // Each check below just leaves the 8051 spinning; its own timeout (about
    // 1s) then restarts the FCB1010 firmware.
    if (x + FCB_PATCH_LEN - 1u > s_addr_mask) {
        s_err("FCB: spin address 0x%x is outside the chip", (unsigned)x);
        return;
    }

    if (flash_slot >= s_flash_count(ORA_FLASH_SLOT_FLAG_EXCLUDE_PLUGINS)) {
        s_err("FCB: no flash slot %u", (unsigned)flash_slot);
        return;
    }

    if (s_ram_count() < 2u || s_get_active(&active) != ORA_RESULT_OK) {
        s_err("FCB: needs two RAM slots");
        return;
    }

    // Always load into the slot not being served, even to restart the image
    // already running, so the switch is one atomic step.
    target = (active == 0u) ? 1u : 0u;

    if (s_copy(flash_slot, ORA_FLASH_SLOT_FLAG_EXCLUDE_PLUGINS, target, 0) != ORA_RESULT_OK) {
        s_err("FCB: copying flash slot %u to RAM slot %u failed", (unsigned)flash_slot, (unsigned)target);
        return;
    }

    if (!wait_for_spin(x, &hi)) {
        s_err("FCB: the CPU did not reach the spin loop at 0x%x", (unsigned)x);
        return;
    }

    at = hi | x;

    if (s_read_slot(target, at, saved, FCB_PATCH_LEN) != ORA_RESULT_OK ||
        s_write_slot(target, at, fcb_patch, FCB_PATCH_LEN, 0) != ORA_RESULT_OK) {
        s_err("FCB: patching RAM slot %u at 0x%x failed", (unsigned)target, (unsigned)at);
        return;
    }

    if (s_set_active(target) != ORA_RESULT_OK) {
        s_err("FCB: switching to RAM slot %u failed", (unsigned)target);
        return;
    }

    // Put the new image's own bytes back once its CPU has left the patch.
    // If the reset was not seen, the patch is left in place (harmless if
    // that area of the image is unused, which is likely, but logged).
    if (wait_for_reset())
        s_write_slot(target, at, saved, FCB_PATCH_LEN, 1);
    else
        s_err("FCB: no reset fetch seen; 9 bytes at 0x%x left patched", (unsigned)at);

    s_log("FCB: now serving flash slot %u from RAM slot %u", (unsigned)flash_slot, (unsigned)target);
}

void fcb_main(ora_lookup_fn_t lookup, ora_plugin_type_t type, const ora_entry_args_t *args) {
    (void)type;
    (void)args;

    ORA_PLUGIN_INIT_STATIC_RAM();

    s_log = lookup(ORA_ID_LOG);
    s_err = lookup(ORA_ID_ERR_LOG);
    s_demangle = lookup(ORA_ID_DEMANGLE_OBSERVED_ADDR);
    s_uptime = lookup(ORA_ID_GET_PLUGIN_UPTIME_MS);
    s_ram_count = lookup(ORA_ID_GET_RAM_SLOT_COUNT);
    s_get_active = lookup(ORA_ID_GET_ACTIVE_RAM_SLOT);
    s_set_active = lookup(ORA_ID_SET_ACTIVE_RAM_SLOT);
    s_flash_count = lookup(ORA_ID_GET_FLASH_SLOT_COUNT);
    s_copy = lookup(ORA_ID_COPY_FLASH_SLOT_TO_RAM_SLOT);
    s_read_slot = lookup(ORA_ID_READ_RAM_ROM_SLOT);
    s_write_slot = lookup(ORA_ID_REPROGRAM_RAM_ROM_SLOT);

    ora_setup_address_monitor_fn_t setup_monitor = lookup(ORA_ID_SETUP_ADDRESS_MONITOR);
    ora_start_address_monitor_fn_t start_monitor = lookup(ORA_ID_START_ADDRESS_MONITOR);
    ora_get_address_monitor_ring_write_pos_fn_t write_pos = lookup(ORA_ID_GET_ADDRESS_MONITOR_RING_WRITE_POS);
    ora_get_unobserved_addr_bits_fn_t unobserved = lookup(ORA_ID_GET_UNOBSERVED_ADDR_BITS);

    // The error log exists in every build; without it there is nowhere to
    // report anything, so stop.
    if (s_err == NULL)
        for (;;) {}

    if (!s_log || !s_demangle || !s_uptime || !s_ram_count || !s_get_active || !s_set_active ||
        !s_flash_count || !s_copy || !s_read_slot || !s_write_slot || !setup_monitor ||
        !start_monitor || !write_pos || !unobserved) {
        s_err("FCB: firmware lacks a needed API call");
        for (;;) {}
    }

    // The 28-pin board observes every address line; a 40-pin one would not
    // see A0, which carries data here.
    uint8_t skipped = 1;

    if (unobserved(&skipped) != ORA_RESULT_OK || skipped != 0u) {
        s_err("FCB: needs a board that observes every address line");
        for (;;) {}
    }

    // Control mode, as host-control uses for SWITCH_SLOT: the plugin changes
    // what is served.
    if (setup_monitor(ring_buf, RING_LOG2, ORA_MONITOR_MODE_CONTROL, 32u, NULL) != ORA_RESULT_OK ||
        (s_write_pos = write_pos()) == NULL || !build_page_test()) {
        s_err("FCB: address monitor setup failed");
        for (;;) {}
    }

    start_monitor();
    s_log("FCB: ready, page mask 0x%x, address mask 0x%x", (unsigned)s_page_mask, (unsigned)s_addr_mask);

    for (;;) {
        fcb_frame_t f;

        wait_for_frame(&f);

        if (fcb_frame_cmd(&f) == FCB_CMD_SWITCH)
            do_switch(fcb_frame_slot(&f), fcb_frame_spin(&f));
        else
            s_err("FCB: unknown command %u", (unsigned)fcb_frame_cmd(&f));
    }
}
