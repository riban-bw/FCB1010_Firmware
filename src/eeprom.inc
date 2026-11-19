// I2C (bit-banged) and the 24C16 settings EEPROM: the settings records. Included by hw.c,
// and by test/test_eeprom.c, which runs it against a model of the 24C16. The includer
// defines I2C_SCL_SET(v), I2C_SDA_SET(v), I2C_SDA_GET(), I2C_DELAY() and
// EEPROM_DESELECT_ADC() (the ADC shares the I2C lines; its chip select must be high).

static void i2c_start(void)
{
    I2C_SDA_SET(1);
    I2C_SCL_SET(1);
    I2C_DELAY();
    I2C_SDA_SET(0);
    I2C_DELAY();
    I2C_SCL_SET(0);
    I2C_DELAY();
}

static void i2c_stop(void)
{
    I2C_SDA_SET(0);
    I2C_DELAY();
    I2C_SCL_SET(1);
    I2C_DELAY();
    I2C_SDA_SET(1);
    I2C_DELAY();
}

// Returns 1 if the device acknowledged.
static uint8_t i2c_write(uint8_t data)
{
    uint8_t i;
    uint8_t ack;

    for (i = 0; i < 8; i++)
    {
        I2C_SDA_SET((data & 0x80) ? 1 : 0);
        I2C_DELAY();
        I2C_SCL_SET(1);
        I2C_DELAY();
        I2C_SCL_SET(0);
        data = (uint8_t)(data << 1);
    }

    I2C_SDA_SET(1);
    I2C_DELAY();
    I2C_SCL_SET(1);
    I2C_DELAY();
    ack = I2C_SDA_GET() ? 0 : 1;
    I2C_SCL_SET(0);
    I2C_DELAY();

    return ack;
}

static uint8_t i2c_read(uint8_t send_nak)
{
    uint8_t i;
    uint8_t data = 0;

    I2C_SDA_SET(1);

    for (i = 0; i < 8; i++)
    {
        I2C_DELAY();
        I2C_SCL_SET(1);
        I2C_DELAY();
        data = (uint8_t)(data << 1);

        if (I2C_SDA_GET())
        {
            data |= 1;
        }

        I2C_SCL_SET(0);
    }

    I2C_SDA_SET(send_nak ? 1 : 0);
    I2C_DELAY();
    I2C_SCL_SET(1);
    I2C_DELAY();
    I2C_SCL_SET(0);
    I2C_SDA_SET(1);
    I2C_DELAY();

    return data;
}

#define EEPROM_DEVICE  0xA0

// Settings records in the 2KB 24C16. The two shared records are 6 bytes in 8-byte slots
// at the top (calibration 0x7F0, settings 0x7F8): magic, the four bytes of hal_rec,
// checksum (the XOR of the first five bytes and the record's salt); 0x7E0-0x7EF held the
// relay records, which are now per bank. The per-bank records are 5 bytes, packed: the
// four bytes of hal_rec, then a checksum (their XOR, the record's salt and the bank
// number). A bank's ten footswitch records are 50 bytes: bank 1 at 0x7AE-0x7DF, each
// further bank 50 bytes lower (bank 36 at 0x0D8-0x109). In them bytes 1-3 hold 7-bit
// values, so a set top bit (erased EEPROM reads 0xFF) also makes a record invalid. Each
// bank's own record (HAL_REC_BANK) is below them, 5 bytes per bank: bank 1 at 0x0D3, each
// further bank 5 lower (bank 36 at 0x024); all its bits are used, and its salt keeps an
// erased record invalid (salt ^ bank never 0xFF). 0x000-0x023 is spare. Behringer's
// SysEx documentation places the stock preset data from 0x000, so banks that are saved
// can overwrite it: putting the original program chip back would then need the presets
// restored. A bank that has never been saved holds no valid records and loads as the
// defaults.
#define EEPROM_SHARED  0x7F0
#define FS_LENGTH      5
#define BANK_BYTES     (STOMP_COUNT * FS_LENGTH)
#define BANK_MAX       36
#define FS_BOTTOM      (0x7E0 - BANK_MAX * BANK_BYTES)
#define BANK_REC_TOP   (FS_BOTTOM - FS_LENGTH)

#if EEPROM_SHARED + (HAL_RECORDS - HAL_REC_CAL) * 8 != 0x800 || HAL_REC_BANK != STOMP_COUNT || \
    HAL_REC_CAL != HAL_REC_BANK + 1 || STOMP_COUNT != 10
#error EEPROM record layout assumptions broken
#endif

#if BANK_COUNT < 1 || BANK_COUNT > BANK_MAX || BANK_REC_TOP - (BANK_MAX - 1) * FS_LENGTH < 0x020
#error BANK_COUNT must be 1 to 36 (eleven 5-byte records per bank below 0x7E0)
#endif

// Magics of the shared records; salts of the ten footswitch records and the bank record
// (every bank uses the same, with the bank number added; the bank record's top two bits
// are 01, so with a bank number below 64 its checksum of an erased record is never 0xFF)
// and the shared ones.
static const CODE uint8_t rec_magic[HAL_RECORDS - HAL_REC_CAL] = { 0xC1, 0xC2 };
static const CODE uint8_t rec_salt[HAL_RECORDS] = {
    0x96, 0x69, 0x55, 0xAA, 0x33, 0xCC, 0x0F, 0xF0, 0x5F, 0xF5, 0x4B, 0x5A, 0xA5
};

// The one record buffer and the bank the footswitch records belong to, shared with ctl.c
// (loads and saves never overlap), and the result of the last EEPROM access as a bit.
NEAR uint8_t hal_rec[4];
NEAR uint8_t hal_bank;
static BIT_T  eeprom_ok;

// The record being accessed (rec_locate): byte i of the stored form (0 magic, 1-4 hal_rec,
// 5 checksum) is at rec_addr + i, from byte rec_first (1 for a footswitch record, which
// has no magic). rec_control is the device address byte (write) of the last transfer
// started (rec_start).
#define REC_LAST  5
static uint16_t rec_addr;
static uint8_t  rec_first;
static uint8_t  rec_control;

// Find record rec. Footswitch records move down 50 bytes per bank, the bank record 5 (a
// loop, not SDCC's multiply call). A leaf, so it costs no fixed RAM.
static void rec_locate(uint8_t rec)
{
    uint8_t b;

    if (rec < HAL_REC_CAL)
    {
        rec_first = 1;
        rec_addr  = (uint16_t)(BANK_REC_TOP - 1);
        b         = FS_LENGTH;

        if (rec < HAL_REC_BANK)
        {
            rec_addr = (uint16_t)(0x7E0 - BANK_BYTES - 1 + (uint8_t)((uint8_t)(rec << 2) + rec));
            b        = BANK_BYTES;
        }

        for (rec = hal_bank; rec != 0; rec--)
        {
            rec_addr -= b;
        }
    }
    else
    {
        rec_addr  = (uint16_t)(EEPROM_SHARED + (uint8_t)((uint8_t)(rec - HAL_REC_CAL) << 3));
        rec_first = 0;
    }
}

// Byte i of record rec as stored: magic (shared records only), hal_rec[0..3], checksum
// (the XOR of the data, the record's salt, and the magic or, for a footswitch, the bank).
static uint8_t rec_byte(uint8_t rec, uint8_t i)
{
    uint8_t x;

    if (i != 0 && i != REC_LAST)
    {
        return hal_rec[i - 1];
    }

    x = (rec < HAL_REC_CAL) ? hal_bank : rec_magic[rec - HAL_REC_CAL];

    if (i == 0)
    {
        return x;
    }

    return (uint8_t)(x ^ hal_rec[0] ^ hal_rec[1] ^ hal_rec[2] ^ hal_rec[3] ^ rec_salt[rec]);
}

// Start a transfer at address a; eeprom_ok = 1 if the EEPROM acknowledged. The 24C16 takes
// the 256-byte block (address bits 8-10) in the device address byte, the low eight bits as
// the word address.
static void rec_start(uint16_t a)
{
    rec_control = (uint8_t)(EEPROM_DEVICE | ((uint8_t)(a >> 7) & 0x0E));
    EEPROM_DESELECT_ADC();
    i2c_start();
    eeprom_ok = (i2c_write(rec_control) && i2c_write((uint8_t)a)) ? 1 : 0;
}

// Wait for the internal write cycle (about 5ms) by polling for an ACK; eeprom_ok = 1 once
// it answers.
static void rec_wait(void)
{
    uint16_t tries;

    for (tries = 0; tries < 5000; tries++)
    {
        i2c_start();
        eeprom_ok = i2c_write(rec_control) ? 1 : 0;
        i2c_stop();

        if (eeprom_ok)
        {
            return;
        }
    }
}

// Set by hal_rec_save for its read-back: hal_rec_load then compares every byte with what
// was written instead of loading it.
static BIT_T rec_verify;

// Read record rec into hal_rec in one sequential read (it may cross pages and blocks). 1
// if every byte was read and the magic and checksum match (or, for a read-back, every
// byte matches).
uint8_t hal_rec_load(uint8_t rec)
{
    uint8_t i;
    uint8_t b;

    eeprom_ok = 0;

    if (rec < HAL_RECORDS)
    {
        rec_locate(rec);
        rec_start((uint16_t)(rec_addr + rec_first));

        if (eeprom_ok)
        {
            i2c_start();
            eeprom_ok = i2c_write((uint8_t)(rec_control | 1)) ? 1 : 0;
        }
    }

    if (eeprom_ok)
    {
        for (i = rec_first; i <= REC_LAST; i++)
        {
            // Acknowledge every byte but the last, which ends the read.
            b = i2c_read((uint8_t)(i == REC_LAST));

            // The data into hal_rec (a footswitch record's 7-bit bytes with bit 7 clear),
            // then the magic and checksum against what was read.
            if (!rec_verify && i != 0 && i != REC_LAST)
            {
                hal_rec[i - 1] = b;

                if (rec < HAL_REC_BANK && i != 1 && (b & 0x80))
                {
                    eeprom_ok = 0;
                }
            }
            else if (b != rec_byte(rec, i))
            {
                eeprom_ok = 0;
            }
        }
    }

    i2c_stop();
    rec_verify = 0;

    return eeprom_ok;
}

// Write hal_rec as record rec (adding the magic and checksum), then read it back. A write
// stops at each 16-byte page boundary and goes on in a new write cycle (packed records
// can cross one; the shared ones are 8-byte aligned and never do).
uint8_t hal_rec_save(uint8_t rec)
{
    uint8_t i;

    if (rec >= HAL_RECORDS)
    {
        return 0;
    }

    rec_locate(rec);
    eeprom_ok = 1;

    for (i = rec_first; i <= REC_LAST && eeprom_ok; i++)
    {
        if (i == rec_first || ((uint8_t)(rec_addr + i) & 15) == 0)
        {
            if (i != rec_first)
            {
                i2c_stop();
                rec_wait();

                if (!eeprom_ok)
                {
                    return 0;
                }
            }

            rec_start((uint16_t)(rec_addr + i));

            if (!eeprom_ok)
            {
                break;
            }
        }

        eeprom_ok = i2c_write(rec_byte(rec, i)) ? 1 : 0;
    }

    i2c_stop();

    if (!eeprom_ok)
    {
        return 0;
    }

    rec_wait();

    if (!eeprom_ok)
    {
        return 0;
    }

    rec_verify = 1;
    return hal_rec_load(rec);
}

