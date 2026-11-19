#ifndef PLATFORM_H
#define PLATFORM_H

// Portable type and memory-space macros so the controller logic
// can be compiled for the 8051 (SDCC) and on a PC for unit tests.

#include <stdint.h>

#ifdef __SDCC
// IDATA is the upper 128 bytes of internal RAM (indirect addressing only).
#define IDATA __idata
// NEAR is the lower 128 (direct addressing): a byte less, and simpler code, at every use.
// ctl.c's own directly addressed data must fit the gap below the bit area, so NEAR
// variables live in another module (hw.c's data, or ctl_ram.c), placed above the bits.
#define NEAR  __data
// Single-bit variable in the 8051's bit-addressable RAM: one bit instead of one byte.
#define BIT_T __bit
// CODE keeps constant tables in ROM instead of copying them into RAM.
#define CODE  __code
// XDATA is the board's 32KB SRAM (IC14), at 0x8000-0xFFFF: its chip select is A15
// through an inverter (factory schematic). The Makefile links external data there
// (--xram-loc 0x8000); below 0x8000 every write reaches a display / key latch.
// NO_XDATA keeps everything internal (the SRAM probe's copy of hw.c).
#ifndef NO_XDATA
#define XDATA __xdata
#else
#define XDATA
#endif
#else
#define IDATA
#define NEAR
#define CODE
#define BIT_T uint8_t
#define XDATA
#endif

#endif
