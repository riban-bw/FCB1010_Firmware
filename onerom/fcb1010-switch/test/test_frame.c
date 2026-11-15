// Host test for the frame decoder in fcb_frame.h (used unchanged by the
// plugin). Optional arguments: the 15 frame bytes the 8051 sender built in
// the simulator, then the spin address and slot it should decode to (see
// test/run.sh). Build: cc -I.. test_frame.c
#include <stdio.h>
#include <stdlib.h>
#include "fcb_frame.h"

static int checks, failures;

static void check(int ok, const char *what) {
    checks++;

    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

// Mirror of what host_8051.c builds (cross-checked against the simulator).
static void make_frame(uint8_t *out, uint8_t slot, uint16_t x) {
    for (unsigned i = 0; i < FCB_KEY_LEN; i++)
        out[i] = fcb_key[i];

    out[7] = 0xC0u | FCB_CMD_SWITCH;
    out[8] = slot;
    out[9] = (uint8_t)~slot;
    out[10] = (uint8_t)(0x50u | (x >> 12));
    out[11] = (uint8_t)(0x60u | ((x >> 8) & 0x0Fu));
    out[12] = (uint8_t)(0x70u | ((x >> 4) & 0x0Fu));
    out[13] = (uint8_t)(0x80u | (x & 0x0Fu));
    out[14] = (uint8_t)(0x90u | fcb_check(FCB_CMD_SWITCH, slot, x));
}

// Feed bytes; returns how many frames completed, the last in *f.
static int feed_all(fcb_frame_t *f, const uint8_t *b, unsigned n, unsigned repeat) {
    int frames = 0;

    for (unsigned i = 0; i < n; i++)
        for (unsigned r = 0; r < repeat; r++)
            frames += fcb_rx(f, b[i]);

    return frames;
}

int main(int argc, char **argv) {
    fcb_frame_t f;
    uint8_t fr[FCB_FRAME_LEN], twice[2 * FCB_FRAME_LEN];

    // The frame the real 8051 code built.
    if (argc == 18) {
        uint8_t sim[FCB_FRAME_LEN];
        uint16_t x = (uint16_t)strtoul(argv[16], NULL, 16);
        uint8_t slot = (uint8_t)strtoul(argv[17], NULL, 0);

        for (int i = 0; i < (int)FCB_FRAME_LEN; i++)
            sim[i] = (uint8_t)strtoul(argv[1 + i], NULL, 16);

        make_frame(fr, slot, x);
        for (unsigned i = 0; i < FCB_FRAME_LEN; i++)
            check(sim[i] == fr[i], "simulator frame byte matches the C model");

        fcb_reset(&f);
        check(feed_all(&f, sim, FCB_FRAME_LEN, 1) == 1, "simulator frame decodes");
        check(fcb_frame_cmd(&f) == FCB_CMD_SWITCH && fcb_frame_slot(&f) == slot &&
              fcb_frame_spin(&f) == x, "simulator frame fields");
    }

    // Every slot and many spin addresses, sent once, with every byte
    // captured twice, and three times back to back (as the sender does).
    for (unsigned slot = 0; slot <= FCB_MAX_SLOT; slot++) {
        for (uint32_t x = 0; x < 0x8000u; x += 0x13u) {
            make_frame(fr, (uint8_t)slot, (uint16_t)x);

            for (unsigned i = 1; i < FCB_FRAME_LEN; i++)
                if (fr[i] == fr[i - 1])
                    check(0, "no two consecutive frame bytes are equal");

            check(fr[FCB_FRAME_LEN - 1] != fr[0], "the frame end differs from its start");

            fcb_reset(&f);
            check(feed_all(&f, fr, FCB_FRAME_LEN, 1) == 1 && fcb_frame_slot(&f) == slot &&
                  fcb_frame_spin(&f) == x, "frame decodes");

            fcb_reset(&f);
            check(feed_all(&f, fr, FCB_FRAME_LEN, 2) == 1, "duplicated captures decode once");

            fcb_reset(&f);
            int n = feed_all(&f, fr, FCB_FRAME_LEN, 1);
            n += feed_all(&f, fr, FCB_FRAME_LEN, 1);
            n += feed_all(&f, fr, FCB_FRAME_LEN, 1);
            check(n == 3, "three frames back to back give three");
        }
    }

    // Junk first, including a broken-off key, then the frame.
    {
        static const uint8_t junk[] = { 'F', 'C', 'B', '1', 'F', 'C', 0x00, 0xC1, 'F' };

        make_frame(fr, 9, 0x1234);
        fcb_reset(&f);
        check(feed_all(&f, junk, sizeof junk, 1) == 0, "junk alone is no frame");
        check(feed_all(&f, fr, FCB_FRAME_LEN, 1) == 1 && fcb_frame_slot(&f) == 9 &&
              fcb_frame_spin(&f) == 0x1234, "frame after junk decodes");
    }

    // Any single changed bit in any byte rejects the frame, and the matcher
    // then picks up the next good one.
    for (unsigned i = 0; i < FCB_FRAME_LEN; i++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            make_frame(twice, 33, 0x0ABC);
            make_frame(twice + FCB_FRAME_LEN, 33, 0x0ABC);
            twice[i] ^= (uint8_t)(1u << bit);
            fcb_reset(&f);
            int n = feed_all(&f, twice, 2 * FCB_FRAME_LEN, 1);
            check(n == 1 && fcb_frame_spin(&f) == 0x0ABC, "a corrupted frame is rejected, the next accepted");
        }
    }

    // Random in-page reads (as if another firmware ran code in the page)
    // never make a frame.
    {
        unsigned long seed = 12345;
        int n = 0;

        fcb_reset(&f);

        for (long i = 0; i < 20000000L; i++) {
            seed = seed * 1103515245ul + 12345ul;
            n += fcb_rx(&f, (uint8_t)(seed >> 16));
        }

        check(n == 0, "20M random bytes make no frame");
    }

    printf("frame: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
