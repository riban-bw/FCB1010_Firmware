#ifndef PLATFORM_H
#define PLATFORM_H

// Portable type and memory-space macros so the controller logic
// can be compiled for the 8051 (SDCC) and on a PC for unit tests.

#include <stdint.h>

#ifdef __SDCC
// IDATA is the upper 128 bytes of internal RAM (indirect addressing only).
#define IDATA __idata
// Single-bit variable in the 8051's bit-addressable RAM: one bit instead of one byte.
#define BIT_T __bit
// CODE keeps constant tables in ROM instead of copying them into RAM.
#define CODE  __code
#else
#define IDATA
#define CODE
#define BIT_T uint8_t
#endif

#endif
