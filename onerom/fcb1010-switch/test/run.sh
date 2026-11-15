#!/bin/sh
# Tests for the One ROM slot-switch draft, run from onerom/fcb1010-switch:
# 1. assembles host_8051.c with SDCC and runs it in uCsim (s51 on PATH) until
#    it spins, reads back the frame it built and where it spins;
# 2. checks the spin loop bytes are the ones the plugin patch assumes;
# 3. runs the host decoder test on that frame and on generated ones.
set -e
B=${BUILD:-build/test}
mkdir -p "$B"
sdcc -mmcs51 -c host_8051.c -o "$B/host_8051.rel"
sdcc -mmcs51 -c test/sim_main.c -o "$B/sim_main.rel"
sdcc -mmcs51 "$B/sim_main.rel" "$B/host_8051.rel" -o "$B/t.ihx"

grep -q "DF FE .*djnz	r7, 00010" "$B/host_8051.rst"
grep -q "DE FC .*djnz	r6, 00010" "$B/host_8051.rst"
grep -q "DD FA .*djnz	r5, 00010" "$B/host_8051.rst"
grep -q "02 00 00 .*ljmp	0x0000" "$B/host_8051.rst"

printf 'step 2000\ndump iram 0x30 0x3e\nstate\nquit\n' > "$B/cmd"
s51 -t 80C32 -X 12M "$B/t.ihx" < "$B/cmd" > "$B/sim.out" 2>&1 || true

# The dump lines are "0x30 46 43 ..."; keep the hex byte columns.
bytes=$(grep -E '^0x3[0-9a-e] ' "$B/sim.out" | sed -E 's/^0x[0-9a-f]+ +//; s/  +.*$//' | tr '\n' ' ')
pc=$(sed -nE 's/.*PC= 0x0*([0-9a-f]+).*/\1/p' "$B/sim.out")

if [ -z "$pc" ] || [ "$(echo $bytes | wc -w)" -ne 15 ]; then
    echo "simulator output not understood:"; cat "$B/sim.out"; exit 1
fi

echo "8051 frame: $bytes, spinning at 0x$pc"
cc -O2 -Wall -Wextra -Werror -I. test/test_frame.c -o "$B/test_frame"
"$B/test_frame" $bytes "$pc" 5
