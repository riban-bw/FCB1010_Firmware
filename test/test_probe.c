// Host test for the SRAM probe's search (probe/sram_probe.c): the probe runs against a model
// of the external data bus with RAM placed and mirrored in different ways, and with a
// floating bus everywhere else (a read returns the last value driven, the trap a naive
// write-then-read test falls into). Build and run with: make test

#include <stdio.h>
#include <stdint.h>

#define PROBE_HOST
#define IDATA
#define CODE const
#define __xdata
#define TICK_MS 2

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

// The bus: RAM of ram_size bytes from ram_base, with address bits outside ram_mask not
// decoded (so it repeats); anything else floats.
static uint8_t  mem[65536];
static uint8_t  bus;
static uint32_t ram_base, ram_size, ram_mask;
static int      latch_touched;
static uint8_t  EA;

static uint8_t *cell(uint16_t a)
{
    static uint8_t floating;

    if (a >= ram_base && a < ram_base + ram_size)
    {
        return &mem[ram_base + ((a - ram_base) & ram_mask)];
    }

    floating = bus;
    return &floating;
}

static uint8_t bus_rd(uint16_t a)
{
    bus = *cell(a);
    return bus;
}

static void bus_wr(uint16_t a, uint8_t v)
{
    // Below 0x8000 a write reaches the latch A0-A2 selects (74HC139, gated by A15 and /WR):
    // 0 and 1 are the display commons and segments; 5 the key / relay column and 6, 7 the
    // resets, which the probe must never write.
    if (a < 0x8000 && (a & 7) >= 5)
    {
        latch_touched++;
    }

    bus = v;
    *cell(a) = v;
}

#define XMEM_RD(a)     bus_rd((uint16_t)(a))
#define XMEM_WR(a, v)  bus_wr((uint16_t)(a), (uint8_t)(v))

// The display and MIDI functions the probe's reporting code calls (not exercised here).
static uint8_t hal_midi_space(void) { return 1; }
static void hal_midi_tx(uint8_t b) { (void)b; }
static void hal_stomp_led(uint8_t i, uint8_t on) { (void)i; (void)on; }
static void hal_aux_led(uint8_t w, uint8_t on) { (void)w; (void)on; }
static void hal_display(uint8_t v) { (void)v; }
static void hal_display_hex(uint8_t v) { (void)v; }
static void hal_display_dots(uint8_t m) { (void)m; }
static void hal_display_blank(uint8_t m) { (void)m; }
#define HAL_LED_SWITCH_1 5
#define HAL_LED_SWITCH_2 6

#include "../probe/sram_probe.c"

static void run(uint32_t base, uint32_t size, uint32_t mask)
{
    int i;

    ram_base = base;
    ram_size = size;
    ram_mask = mask;

    for (i = 0; i < 32; i++)
    {
        ram_map[i] = 0;
    }

    probe();
}

#define RUN_IS(r, s, e, b)  (run_start[r] == (s) && run_end[r] == (e) && run_base[r] == (b))

int main(void)
{
    // Nothing there: the floating bus reads back what was last driven, which the
    // complement written to the neighbouring cell defeats.
    run(0, 0, 0);
    CHECK(run_count == 0 && distinct_lo == 0 && distinct_hi == 0 && value_count() == 1);

    run(0x0000, 0x8000, 0x7FFF);
    CHECK(run_count == 1 && RUN_IS(0, 0x00, 0x7F, 0x00) && distinct_lo == 0x80 && distinct_hi == 0);
    CHECK(value_count() == 4 && value_get(1) == 0x00 && value_get(2) == 0x7F && value_lo == 0xFF);

    run(0x8000, 0x8000, 0x7FFF);
    CHECK(run_count == 1 && RUN_IS(0, 0x80, 0xFF, 0x80) && distinct_lo == 0x80);

    // A15 not decoded: the top half is a mirror of the bottom.
    run(0x0000, 0x10000, 0x7FFF);
    CHECK(run_count == 2 && RUN_IS(0, 0x00, 0x7F, 0x00) && RUN_IS(1, 0x80, 0xFF, 0x00) && distinct_lo == 0x80);
    CHECK(value_get(6) == 0x00 && value_lo == 0x00);

    // Not aligned to a 32KB boundary.
    run(0x2000, 0x8000, 0x7FFF);
    CHECK(run_count == 1 && RUN_IS(0, 0x20, 0x9F, 0x20) && distinct_lo == 0x80);

    // An 8KB part repeated four times over the top half.
    run(0x8000, 0x8000, 0x1FFF);
    CHECK(run_count == 4 && RUN_IS(0, 0x80, 0x9F, 0x80) && RUN_IS(3, 0xE0, 0xFF, 0x80) && distinct_lo == 0x20);

    // All 64KB (as the simulator has): 0x0100 distinct pages.
    run(0x0000, 0x10000, 0xFFFF);
    CHECK(run_count == 1 && RUN_IS(0, 0x00, 0xFF, 0x00) && distinct_lo == 0 && distinct_hi == 1);

    // No write in any of the runs selects the key / relay latch or a reset latch.
    CHECK(latch_touched == 0);

    printf("\nprobe: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
