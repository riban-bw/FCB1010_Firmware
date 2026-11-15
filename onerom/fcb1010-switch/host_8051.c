// 8051 side of the One ROM slot switch (SDCC). DRAFT: not yet part of the
// firmware build; test/run_host_8051.sh runs it in the simulator.
//
// fcb_onerom_switch(slot) never returns. It puts the CPU's registers back to
// their reset values, sends the switch frame (fcb_frame.h) three times by
// reading ROM addresses 0x7F00 + byte, then spins in a 3-instruction loop
// whose address is in the frame. The plugin swaps the image under the spin
// loop; the new firmware then starts from its reset vector as if powered on
// (only the RAM contents and the latched LED / relay outputs carry over).
// If nothing happens for about 1s (no One ROM, plugin missing, bad slot),
// the loop ends in LJMP 0000, which restarts this firmware.
//
// Requirements on the firmware that calls it:
// - no code or tables at 0x3F00-0x3FFF or 0x7F00-0x7FFF (the reserved page as
//   seen by a 16KB and a 32KB chip), so normal running never reads the page;
// - call it from the main loop, not an interrupt (a pending RETI would leave
//   the CPU's interrupt logic blocked in the new firmware).
#include <stdint.h>

// The frame key, "FCB1010".
static const __code uint8_t fcb_key_8051[7] = { 0x46, 0x43, 0x42, 0x31, 0x30, 0x31, 0x30 };

// The frame is built in internal RAM at 0x30-0x3E: safe because the stack
// is reset first and nothing returns from here.
void fcb_onerom_switch(uint8_t slot) __naked
{
    (void)slot;

    __asm
    ; Reset values: interrupts off (IE, IP), timers (TCON, TMOD, T2CON),
    ; UART (SCON), PCON, register bank 0 (PSW), stack, ports.
    mov  0xA8, #0x00
    mov  0xB8, #0x00
    mov  0x88, #0x00
    mov  0x89, #0x00
    mov  0xC8, #0x00
    mov  0x98, #0x00
    mov  0x87, #0x00
    mov  0xD0, #0x00
    mov  0x81, #0x07
    mov  0x90, #0xFF
    mov  0xB0, #0xFF

    ; r2 = slot (0-63), from the first parameter in DPL.
    mov  a, dpl
    anl  a, #0x3F
    mov  r2, a

    ; Key, 7 bytes.
    mov  r0, #0x30
    mov  dptr, #_fcb_key_8051
    mov  r1, #7
00001$:
    clr  a
    movc a, @a+dptr
    mov  @r0, a
    inc  r0
    inc  dptr
    djnz r1, 00001$

    ; Command 1 (switch), slot, ~slot.
    mov  @r0, #0xC1
    inc  r0
    mov  a, r2
    mov  @r0, a
    inc  r0
    cpl  a
    mov  @r0, a
    inc  r0

    ; The address X of the spin loop, one nibble per byte, tagged 0x50-0x80.
    mov  dptr, #00010$
    mov  a, dph
    swap a
    anl  a, #0x0F
    orl  a, #0x50
    mov  @r0, a
    inc  r0
    mov  a, dph
    anl  a, #0x0F
    orl  a, #0x60
    mov  @r0, a
    inc  r0
    mov  a, dpl
    swap a
    anl  a, #0x0F
    orl  a, #0x70
    mov  @r0, a
    inc  r0
    mov  a, dpl
    anl  a, #0x0F
    orl  a, #0x80
    mov  @r0, a
    inc  r0

    ; Check: 0x90 | low nibble of t ^ (t >> 4), t = 1 (the command) ^ slot ^
    ; X high ^ X low.
    mov  a, r2
    xrl  a, #0x01
    xrl  a, dph
    xrl  a, dpl
    mov  r3, a
    swap a
    xrl  a, r3
    anl  a, #0x0F
    orl  a, #0x90
    mov  @r0, a

    ; Send the frame three times: each MOVC reads ROM address 0x7F00 + byte.
    mov  dptr, #0x7F00
    mov  r3, #3
00002$:
    mov  r0, #0x30
    mov  r1, #15
00003$:
    mov  a, @r0
    movc a, @a+dptr
    inc  r0
    djnz r1, 00003$
    djnz r3, 00002$

    ; Spin about 1s (8 x 256 x 256 DJNZ at 2us), then restart this firmware.
    ; These 9 bytes (DF FE DE FC DD FA 02 00 00) are what the plugin replaces
    ; in the new image with 00 00 00 00 00 00 02 00 00. Do not change their
    ; layout without changing FCB_SPIN_SPAN / fcb_patch in fcb_frame.h.
    mov  r5, #8
    mov  r6, #0
    mov  r7, #0
00010$:
    djnz r7, 00010$
    djnz r6, 00010$
    djnz r5, 00010$
    ljmp 0x0000
    __endasm;
}
