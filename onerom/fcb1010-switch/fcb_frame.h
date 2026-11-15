// FCB1010 slot-switch frame: the encoding shared by the One ROM plugin
// (plugin_main.c), the host test (test_frame.c) and the 8051 sender
// (host_8051.c). See README.md for the protocol.
//
// The 8051 signals by reading ROM addresses in one reserved page (the top
// 256 bytes the chip can address: 0x7Fxx on a 32KB chip). Only the low
// address byte carries data. Reads outside the page (the 8051's own code
// fetches, which come between every data read) are ignored, and a read
// that repeats the previous in-page byte is dropped (in case one bus cycle
// is captured twice), so no two consecutive frame bytes are equal.
//
// Frame, 15 bytes:
//   "FCB1010"            key (0x46 0x43 0x42 0x31 0x30 0x31 0x30)
//   0xC0 | cmd           command (FCB_CMD_SWITCH = 1)
//   slot                 flash slot to serve, 0-63
//   ~slot                check
//   0x50 | x[15:12]      the address X the 8051 then spins at,
//   0x60 | x[11:8]       one nibble per byte
//   0x70 | x[7:4]
//   0x80 | x[3:0]
//   0x90 | check         low nibble of t ^ (t >> 4), t = cmd ^ slot ^ x_hi ^ x_lo
#ifndef FCB_FRAME_H
#define FCB_FRAME_H

#include <stdint.h>

#define FCB_KEY_LEN     7u
#define FCB_FRAME_LEN   15u
#define FCB_BODY_LEN    (FCB_FRAME_LEN - FCB_KEY_LEN)
#define FCB_CMD_SWITCH  1u
#define FCB_MAX_SLOT    0x3Fu

// The spin loop the 8051 runs at X after sending (see host_8051.c), and the
// bytes the plugin writes over it in the target image before switching:
// NOPs, then LJMP 0000. Wherever the switch lands in the loop (an opcode or
// a DJNZ offset fetched from the new image), the CPU reaches the LJMP.
#define FCB_SPIN_SPAN   8u
#define FCB_PATCH_LEN   9u

static const uint8_t fcb_key[FCB_KEY_LEN] = { 'F', 'C', 'B', '1', '0', '1', '0' };
static const uint8_t fcb_patch[FCB_PATCH_LEN] = { 0, 0, 0, 0, 0, 0, 0x02, 0x00, 0x00 };

typedef struct {
    uint16_t last;             // previous in-page byte, 0x100 = none
    uint8_t pos;               // bytes matched so far
    uint8_t body[FCB_BODY_LEN];
} fcb_frame_t;

static inline void fcb_reset(fcb_frame_t *f) {
    f->last = 0x100u;
    f->pos = 0;
}

static inline uint8_t fcb_check(uint8_t cmd, uint8_t slot, uint16_t x) {
    uint8_t t = (uint8_t)(cmd ^ slot ^ (x >> 8) ^ x);

    return (uint8_t)((t ^ (t >> 4)) & 0x0Fu);
}

// Whether byte b is valid at body position i, given the bytes before it.
static inline int fcb_body_ok(const fcb_frame_t *f, uint8_t i, uint8_t b) {
    switch (i) {
        case 0: return (b & 0xF0u) == 0xC0u;
        case 1: return b <= FCB_MAX_SLOT;
        case 2: return (uint8_t)(b ^ f->body[1]) == 0xFFu;
        case 3: case 4: case 5: case 6:
            return (b & 0xF0u) == (uint8_t)(0x50u + 0x10u * (i - 3u));
        default: {
            uint16_t x = (uint16_t)(((f->body[3] & 0x0Fu) << 12) | ((f->body[4] & 0x0Fu) << 8) |
                                    ((f->body[5] & 0x0Fu) << 4) | (f->body[6] & 0x0Fu));

            return b == (uint8_t)(0x90u | fcb_check(f->body[0] & 0x0Fu, f->body[1], x));
        }
    }
}

// Feed one in-page byte (duplicates already dropped). Returns 1 when a
// complete, valid frame has been received (f->body holds it, and the matcher
// is reset for the next one). A byte that does not fit restarts the match,
// counting it as the first key byte if it is one ('F' occurs nowhere else
// in the frame, so this never misses a frame).
static inline int fcb_feed(fcb_frame_t *f, uint8_t b) {
    if (f->pos < FCB_KEY_LEN) {
        f->pos = (b == fcb_key[f->pos]) ? (uint8_t)(f->pos + 1u) : (uint8_t)(b == fcb_key[0]);
        return 0;
    }

    uint8_t i = (uint8_t)(f->pos - FCB_KEY_LEN);

    if (!fcb_body_ok(f, i, b)) {
        f->pos = (uint8_t)(b == fcb_key[0]);
        return 0;
    }

    f->body[i] = b;

    if (++f->pos < FCB_FRAME_LEN)
        return 0;

    f->pos = 0;
    return 1;
}

// Take one in-page capture's low address byte: drops a repeat of the
// previous one, then feeds it. Returns 1 on a complete frame.
static inline int fcb_rx(fcb_frame_t *f, uint8_t b) {
    if (b == f->last)
        return 0;

    f->last = b;
    return fcb_feed(f, b);
}

static inline uint8_t fcb_frame_cmd(const fcb_frame_t *f) { return f->body[0] & 0x0Fu; }
static inline uint8_t fcb_frame_slot(const fcb_frame_t *f) { return f->body[1]; }

static inline uint16_t fcb_frame_spin(const fcb_frame_t *f) {
    return (uint16_t)(((f->body[3] & 0x0Fu) << 12) | ((f->body[4] & 0x0Fu) << 8) |
                      ((f->body[5] & 0x0Fu) << 4) | (f->body[6] & 0x0Fu));
}

#endif
