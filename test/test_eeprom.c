// Host test for the settings EEPROM code (src/eeprom.inc, as hw.c includes it): its
// bit-banged I2C runs against a bit-level model of the 24C16, which acts as the real chip
// does where mistakes would hide: a write stays within its 16-byte page (the address wraps
// inside the page), a read runs on across pages and 256-byte blocks, and after a write the
// chip ignores its address for a while (the write cycle). Build and run with: make test

#include <stdio.h>
#include <string.h>

#include "platform.h"
#include "config.h"
#include "hal.h"

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

// ---- The 24C16 model ----

#define EE_SIZE  2048

static uint8_t ee[EE_SIZE];         // the memory
static uint8_t ee_touched[EE_SIZE]; // 1 where a write cycle stored a byte
static int     ee_cycles;           // write cycles
static int     ee_busy;             // device bytes still refused (the write cycle)

enum { PH_IDLE, PH_DEV, PH_WORD, PH_DATA, PH_READ };

static uint8_t  m_sda = 1, m_scl = 1, s_sda = 1;   // master's lines and the chip's SDA
static uint8_t  bus_sda = 1, bus_scl = 1;
static int      phase = PH_IDLE;
static int      nbits;          // bits of the byte being received or sent
static uint8_t  shift;          // byte being received
static int      acking;         // the chip pulls SDA low for this ninth clock
static uint8_t  block;          // block from the device byte
static uint16_t addr;           // the chip's address counter
static uint8_t  cur;            // byte being read out
static int      master_ack;     // the master's ACK after a byte read out
static uint8_t  page_buf[16];
static uint8_t  page_used[16];
static int      page_writes;    // data bytes in this write

static void ee_byte_received(void)
{
    if (phase == PH_DEV)
    {
        if ((shift & 0xF0) != 0xA0 || ee_busy > 0)
        {
            if (ee_busy > 0)
            {
                ee_busy--;
            }

            phase = PH_IDLE;
            return;
        }

        block = (uint8_t)((shift >> 1) & 7);
        acking = 1;

        if (shift & 1)
        {
            phase = PH_READ;
        }
        else
        {
            phase = PH_WORD;
        }

        return;
    }

    if (phase == PH_WORD)
    {
        addr = (uint16_t)((block << 8) | shift);
        memset(page_used, 0, sizeof(page_used));
        page_writes = 0;
        phase = PH_DATA;
        acking = 1;
        return;
    }

    if (phase == PH_DATA)
    {
        // The page buffer: the address wraps within the 16-byte page.
        page_buf[addr & 15] = shift;
        page_used[addr & 15] = 1;
        page_writes++;
        addr = (uint16_t)((addr & ~15u) | ((addr + 1) & 15));
        acking = 1;
    }
}

static void ee_start_read_byte(void)
{
    cur = ee[addr];
    nbits = 0;
    s_sda = (uint8_t)((cur >> 7) & 1);
}

static void ee_update(void)
{
    uint8_t sda = (uint8_t)(m_sda & s_sda);
    uint8_t scl = m_scl;
    int     i;

    if (scl && bus_scl && sda != bus_sda)
    {
        // SDA changing while SCL is high: START or STOP.
        if (!sda)
        {
            phase = PH_DEV;
            nbits = 0;
            shift = 0;
            acking = 0;
            s_sda = 1;
        }
        else
        {
            if (phase == PH_DATA && page_writes > 0)
            {
                for (i = 0; i < 16; i++)
                {
                    if (page_used[i])
                    {
                        ee[(addr & ~15u) | (unsigned)i] = page_buf[i];
                        ee_touched[(addr & ~15u) | (unsigned)i] = 1;
                    }
                }

                ee_cycles++;
                ee_busy = 3;
            }

            phase = PH_IDLE;
            s_sda = 1;
        }
    }
    else if (scl && !bus_scl)
    {
        // SCL rising: sample.
        if (phase == PH_READ)
        {
            if (nbits == 8)
            {
                master_ack = !sda;
            }
        }
        else if (phase != PH_IDLE && nbits < 8)
        {
            shift = (uint8_t)((shift << 1) | sda);
            nbits++;
        }
    }
    else if (!scl && bus_scl)
    {
        // SCL falling: move on.
        if (phase == PH_READ)
        {
            if (acking)
            {
                // The ACK of the device byte: then the first byte goes out.
                acking = 0;
                ee_start_read_byte();
            }
            else if (nbits < 8)
            {
                nbits++;
                s_sda = (nbits < 8) ? (uint8_t)((cur >> (7 - nbits)) & 1) : 1;
            }
            else if (master_ack)
            {
                addr = (uint16_t)((addr + 1) & (EE_SIZE - 1));
                ee_start_read_byte();
            }
            else
            {
                phase = PH_IDLE;
                s_sda = 1;
            }
        }
        else if (phase != PH_IDLE)
        {
            if (acking == 2)
            {
                acking = 0;
                s_sda = 1;
                nbits = 0;
                shift = 0;
            }
            else if (nbits == 8)
            {
                ee_byte_received();

                if (acking)
                {
                    s_sda = 0;
                    acking = (phase == PH_READ) ? 1 : 2;
                }
            }
        }
    }

    bus_sda = (uint8_t)(m_sda & s_sda);
    bus_scl = scl;
}

static void sda_set(uint8_t v) { m_sda = v ? 1 : 0; ee_update(); }
static void scl_set(uint8_t v) { m_scl = v ? 1 : 0; ee_update(); }

#define I2C_SDA_SET(v)  sda_set((uint8_t)(v))
#define I2C_SCL_SET(v)  scl_set((uint8_t)(v))
#define I2C_SDA_GET()   (bus_sda)
#define I2C_DELAY()     do { } while (0)
static int adc_deselected;
#define EEPROM_DESELECT_ADC()  (adc_deselected = 1)

#include "eeprom.inc"

// ---- Tests ----

static void erase(void)
{
    memset(ee, 0xFF, sizeof(ee));
    memset(ee_touched, 0, sizeof(ee_touched));
    ee_cycles = 0;
    ee_busy = 0;
}

static void fill(uint8_t rec, uint8_t bank)
{
    hal_bank = bank;
    hal_rec[0] = (uint8_t)((rec * 7 + bank) & 0x3F);
    hal_rec[1] = (uint8_t)((rec * 13 + bank * 3) & 0x7F);
    hal_rec[2] = (uint8_t)((rec * 29 + bank * 5 + 1) & 0x7F);
    hal_rec[3] = (uint8_t)((rec * 31 + bank * 11 + 2) & 0x7F);

    if (rec >= HAL_REC_BANK)
    {
        // The bank record and the shared ones hold full bytes (the bank record's bit 7s,
        // the calibration).
        hal_rec[1] |= 0x80;
        hal_rec[3] |= 0x80;
    }
}

static int matches(uint8_t rec, uint8_t bank)
{
    uint8_t want[4];

    fill(rec, bank);
    memcpy(want, hal_rec, 4);
    memset(hal_rec, 0x5A, 4);

    return hal_rec_load(rec) && memcmp(hal_rec, want, 4) == 0;
}

// Address of a per-bank record as the layout describes it: footswitch sw at 0x7AE in bank
// 1, 50 bytes lower per bank; the bank record (sw 10) at 0x0D3, 5 lower per bank.
static unsigned fs_addr(int bank, int sw)
{
    if (sw == HAL_REC_BANK)
    {
        return (unsigned)(0x0D3 - 5 * bank);
    }

    return (unsigned)(0x7E0 - 50 * (bank + 1) + 5 * sw);
}

int main(void)
{
    int b, r, n, ok, i, below;

    printf("settings EEPROM (24C16 model)\n");

    // Erased: nothing loads.
    erase();
    ok = 1;

    for (b = 0; b < BANK_COUNT; b++)
    {
        hal_bank = (uint8_t)b;

        for (r = 0; r < HAL_RECORDS; r++)
        {
            ok &= !hal_rec_load((uint8_t)r);
        }
    }

    CHECK(ok && ee_cycles == 0 && adc_deselected);

    // Every record of every bank saves (with its read-back) and loads, none overwriting
    // another; nothing below bank BANK_COUNT's records is written.
    for (b = 0; b < BANK_COUNT; b++)
    {
        for (r = 0; r < HAL_REC_CAL; r++)
        {
            fill((uint8_t)r, (uint8_t)b);
            CHECK(hal_rec_save((uint8_t)r));
        }
    }

    for (r = HAL_REC_CAL; r < HAL_RECORDS; r++)
    {
        fill((uint8_t)r, 0);
        CHECK(hal_rec_save((uint8_t)r));
    }

    ok = 1;

    for (b = 0; b < BANK_COUNT; b++)
    {
        for (r = 0; r < HAL_REC_CAL; r++)
        {
            ok &= matches((uint8_t)r, (uint8_t)b);
        }
    }

    CHECK(ok);

    for (r = HAL_REC_CAL; r < HAL_RECORDS; r++)
    {
        uint8_t want[4];

        CHECK(matches((uint8_t)r, 0));
        // The shared records are the same in every bank.
        memcpy(want, hal_rec, 4);
        hal_bank = (uint8_t)(BANK_COUNT - 1);
        CHECK(hal_rec_load((uint8_t)r) && memcmp(hal_rec, want, 4) == 0);
    }

    below = 0;

    for (i = 0; i < (int)fs_addr(BANK_COUNT - 1, HAL_REC_BANK); i++)
    {
        below |= ee_touched[i];
    }

    CHECK(!below);

    // The layout: bank 1's switch 1 at 0x7AE, five bytes each, the last bank 50 bytes per
    // bank lower; the bank records from 0x0D3 down; the shared records at 0x7F0 (magic
    // 0xC1 first) to 0x7FF, with 0x7E0-0x7EF (the old relay records) and 0x000-0x023 unused.
    hal_bank = 0;
    fill(0, 0);
    CHECK(ee[0x7AE] == hal_rec[0] && ee[0x7AF] == hal_rec[1] && ee[0x7B2] == (hal_rec[0] ^ hal_rec[1] ^ hal_rec[2] ^ hal_rec[3] ^ 0x96));
    fill(9, (uint8_t)(BANK_COUNT - 1));
    CHECK(ee[fs_addr(BANK_COUNT - 1, 9)] == hal_rec[0] && ee[fs_addr(BANK_COUNT - 1, 9) + 3] == hal_rec[3]);
    CHECK(ee[0x7F0] == 0xC1 && ee[0x7F8] == 0xC2);
    fill(HAL_REC_BANK, 0);
    CHECK(ee[0x0D3] == hal_rec[0] && ee[0x0D7] == (hal_rec[0] ^ hal_rec[1] ^ hal_rec[2] ^ hal_rec[3] ^ 0x4B));
    ok = 1;

    for (i = 0x7E0; i < 0x7F0; i++)
    {
        ok &= !ee_touched[i];
    }

    for (i = 0; i < 0x024; i++)
    {
        ok &= !ee_touched[i];
    }

    CHECK(ok);
#if BANK_COUNT == 36
    CHECK(fs_addr(35, 0) == 0x0D8 && fs_addr(35, HAL_REC_BANK) == 0x024);
#endif

    // A record crossing a page boundary takes two write cycles; one within a page takes one.
    erase();
    n = 0;

    for (b = 0; b < BANK_COUNT; b++)
    {
        for (r = 0; r < HAL_REC_CAL; r++)
        {
            unsigned a = fs_addr(b, r);
            int before = ee_cycles;

            fill((uint8_t)r, (uint8_t)b);
            CHECK(hal_rec_save((uint8_t)r));
            CHECK(ee_cycles - before == (((a & 15) > 11) ? 2 : 1));
            n += ((a & 15) > 11);
        }
    }

    CHECK(n > 0);

#define CB ((BANK_COUNT > 3) ? 2 : (BANK_COUNT > 1) ? BANK_COUNT - 2 : 0)
    // A changed byte, a set top bit or the record moved to another bank or switch: invalid
    // (banks CB and CB + 1, the last two when there are few).
    hal_bank = CB;
    CHECK(matches(4, CB));
    ee[fs_addr(CB, 4) + 2] ^= 0x01;
    CHECK(!hal_rec_load(4));
    ee[fs_addr(CB, 4) + 2] ^= 0x01;
    CHECK(matches(4, CB));
    ee[fs_addr(CB, 4) + 1] ^= 0x80;
    ee[fs_addr(CB, 4) + 4] ^= 0x80;
    CHECK(!hal_rec_load(4));
    ee[fs_addr(CB, 4) + 1] ^= 0x80;
    ee[fs_addr(CB, 4) + 4] ^= 0x80;
#if BANK_COUNT > 1
    memcpy(&ee[fs_addr(CB + 1, 4)], &ee[fs_addr(CB, 4)], 5);
    hal_bank = CB + 1;
    CHECK(!hal_rec_load(4));
#endif
    memcpy(&ee[fs_addr(CB, 5)], &ee[fs_addr(CB, 4)], 5);
    hal_bank = CB;
    CHECK(!hal_rec_load(5));
    // The bank record takes bit 7 in every byte, but not a changed byte or another bank's.
    CHECK(matches(HAL_REC_BANK, CB));
    ee[fs_addr(CB, HAL_REC_BANK) + 2] ^= 0x01;
    CHECK(!hal_rec_load(HAL_REC_BANK));
    ee[fs_addr(CB, HAL_REC_BANK) + 2] ^= 0x01;
#if BANK_COUNT > 1
    memcpy(&ee[fs_addr(CB + 1, HAL_REC_BANK)], &ee[fs_addr(CB, HAL_REC_BANK)], 5);
    hal_bank = CB + 1;
    CHECK(!hal_rec_load(HAL_REC_BANK));
#endif
    // An erased or zeroed bank record is invalid in every bank.
    ok = 1;

    for (b = 0; b < BANK_COUNT; b++)
    {
        hal_bank = (uint8_t)b;
        memset(&ee[fs_addr(b, HAL_REC_BANK)], 0xFF, 5);
        ok &= !hal_rec_load(HAL_REC_BANK);
        memset(&ee[fs_addr(b, HAL_REC_BANK)], 0x00, 5);
        ok &= !hal_rec_load(HAL_REC_BANK);
    }

    CHECK(ok);
    hal_bank = CB;
    ee[0x7F1] ^= 0x10;
    CHECK(!hal_rec_load(HAL_REC_CAL));

    // No EEPROM answering: loads and saves fail.
    {
        int keep = ee_busy;

        ee_busy = 1000000;
        CHECK(!hal_rec_load(0) && !hal_rec_save(0));
        ee_busy = keep;
    }

    printf("eeprom: %d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
