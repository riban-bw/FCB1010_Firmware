#!/bin/bash
# Runs the real 8051 image in the uCsim simulator and checks the MIDI bytes.
# The simulator cannot model the key-column latches, so pressing the key-row
# line shows all six keys on that row (switches 1-5 and DOWN) as pressed at once.
# Pedals and the settings EEPROM are not simulated; see README for what is untested.
# Usage: test/sim_smoke.sh   (run from the project root after `make`)

set -u

IHX=build/fcb1010.ihx
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

run_sim() {
    # $1 = command file, $2 = output file for UART bytes
    s51 -t 80C32 -X 12M -S out="$2" "$IHX" < "$1" > "$TMP/console.txt" 2>&1
}

hex_of() {
    od -An -v -tx1 "$1" | tr -d ' \n'
}

fail=0

check() {
    if [ "$2" = "$3" ]; then
        echo "  ok   $1"
    else
        echo "  FAIL $1"
        echo "       expected: $3"
        echo "       got:      $2"
        fail=1
    fi
}

# Sorted list of MIDI messages (CC = 3 bytes, program change = 2 bytes).
messages() {
    python3 -I - "$1" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
out, i = [], 0
while i < len(d):
    s = d[i]
    n = 2 if (s & 0xF0) == 0xC0 else 3
    out.append(d[i:i+n].hex())
    i += n
print(' '.join(sorted(out)))
PY
}

echo "scenario 1: idle, then press row 1 (switches 1-5 and DOWN)"
cat > "$TMP/s1.txt" <<'EOF'
set hardware port[1] 0x9f
step 800000
set hardware port[1] 0xdf
step 300000
set hardware port[1] 0x9f
step 400000
quit
EOF
run_sim "$TMP/s1.txt" "$TMP/s1.out"
check "five stomp CCs at 127 and program change 127" \
      "$(messages "$TMP/s1.out")" \
      "b0147f b0157f b0167f b0177f b0187f c07f"

echo "scenario 2: press, release, press again (toggle off, program steps down)"
cat > "$TMP/s2.txt" <<'EOF'
set hardware port[1] 0x9f
step 800000
set hardware port[1] 0xdf
step 300000
set hardware port[1] 0x9f
step 800000
set hardware port[1] 0xdf
step 300000
set hardware port[1] 0x9f
step 400000
quit
EOF
run_sim "$TMP/s2.txt" "$TMP/s2.out"
check "second press toggles stomps off and sends program 126" \
      "$(messages "$TMP/s2.out")" \
      "b01400 b0147f b01500 b0157f b01600 b0167f b01700 b0177f b01800 b0187f c07e c07f"

echo "scenario 3: nothing pressed sends nothing"
cat > "$TMP/s3.txt" <<'EOF'
set hardware port[1] 0x9f
step 1500000
quit
EOF
run_sim "$TMP/s3.txt" "$TMP/s3.out"
check "no MIDI output while idle" "$(hex_of "$TMP/s3.out")" ""

echo "scenario 4: UP + DOWN held opens config mode, a second hold closes it"
# 0xff presses both key rows, so UP, DOWN and switches 1-10 are all down together. The
# chord opens config mode after about 1s; the stomp presses sent their CCs at the start.
# The config state is read back from internal RAM: mode 1 = config, 0 = run.
MODE_ADDR=$((16#$(awk '/_mode:/ {print $1}' build/ctl.rst | head -1)))
cat > "$TMP/s4.txt" <<EOF4
set hardware port[1] 0x9f
step 800000
set hardware port[1] 0xff
step 900000
dump iram $MODE_ADDR $MODE_ADDR 1
set hardware port[1] 0x9f
step 300000
dump iram $MODE_ADDR $MODE_ADDR 1
set hardware port[1] 0xff
step 900000
dump iram $MODE_ADDR $MODE_ADDR 1
set hardware port[1] 0x9f
step 300000
dump iram $MODE_ADDR $MODE_ADDR 1
state
quit
EOF4
s51 -t 80C32 -X 12M -S out="$TMP/s4.out" "$IHX" < "$TMP/s4.txt" > "$TMP/s4.console" 2>&1
MODES=$(grep -E "^0x$(printf '%02x' "$MODE_ADDR") " "$TMP/s4.console" | awk '{print $2}' | tr '\n' ' ')
check "mode after chord / release / chord / release (config, config, run, run)" \
      "$MODES" "01 01 00 00 "
check "no program change was sent by the chord" \
      "$(messages "$TMP/s4.out" | tr ' ' '\n' | grep -c '^c0')" "0"
MAXSP4=$(grep -E "Max value of stack pointer" "$TMP/s4.console" | tail -1 | sed -E 's/.*= *0x0*([0-9a-fA-F]+),.*/\1/')
if [ -n "$MAXSP4" ] && [ "$((16#$MAXSP4))" -le 240 ]; then
    echo "  ok   config enter and exit (including the failed EEPROM save) keep the stack at 0x$MAXSP4"
else
    echo "  FAIL stack pointer 0x$MAXSP4 too high during config"
    fail=1
fi

echo "scenario 5: receive interrupts are queued and drained, stack stays within RAM"
# The simulator cannot deliver real MIDI IN bytes (its UART input did not respond), so
# this injects three receive-complete interrupts with an unknown SBUF value. It checks
# the RX queue fills and empties and that key presses still work during the load.
# The byte-to-state path is covered by the host tests instead.
RX_HEAD=$(awk '/_rx_head:/ {print $1}' build/hw.rst | head -1)
RX_TAIL=$(awk '/_rx_tail:/ {print $1}' build/hw.rst | head -1)
RX_HEAD_DEC=$((16#$RX_HEAD))
RX_TAIL_DEC=$((16#$RX_TAIL))
cat > "$TMP/s5.txt" <<EOF2
set hardware port[1] 0x9f
step 800000
set hardware port[1] 0xdf
set memory sfr 0x98 0x51
step 5000
set memory sfr 0x98 0x51
step 5000
set memory sfr 0x98 0x51
step 300000
set hardware port[1] 0x9f
step 400000
dump iram $RX_HEAD_DEC $RX_HEAD_DEC 1
dump iram $RX_TAIL_DEC $RX_TAIL_DEC 1
state
quit
EOF2
s51 -t 80C32 -X 12M -S out="$TMP/s5.out" "$IHX" < "$TMP/s5.txt" > "$TMP/s5.console" 2>&1
check "key presses still produce the same MIDI under receive load" \
      "$(messages "$TMP/s5.out")" \
      "b0147f b0157f b0167f b0177f b0187f c07f"
HEAD_VAL=$(grep -E "^0x$(printf '%02x' "$RX_HEAD_DEC") " "$TMP/s5.console" | awk '{print $2}')
TAIL_VAL=$(grep -E "^0x$(printf '%02x' "$RX_TAIL_DEC") " "$TMP/s5.console" | awk '{print $2}' | tail -1)
check "three receive interrupts were queued (head = 3)" "$HEAD_VAL" "03"
check "and all three were drained (tail = 3)" "$TAIL_VAL" "03"
MAXSP=$(grep -E "Max value of stack pointer" "$TMP/s5.console" | tail -1 | sed -E 's/.*= *0x0*([0-9a-fA-F]+),.*/\1/')
MAXSP_DEC=$((16#${MAXSP:-0}))
if [ -n "$MAXSP" ] && [ "$MAXSP_DEC" -le 240 ]; then
    echo "  ok   maximum stack pointer 0x$MAXSP is within internal RAM (limit 0xF0)"
else
    echo "  FAIL maximum stack pointer 0x$MAXSP is too close to the end of RAM"
    fail=1
fi

# About 595,000 simulator steps equal one second. The hold threshold fires about 1.03s
# after a press, so 500,000 steps (0.84s) is under it and 715,000 steps (1.20s) is over it.

echo "scenario 6: held 0.84 s (under the 1s threshold) stays latched; DOWN repeated once more"
cat > "$TMP/s6.txt" <<'EOF'
set hardware port[1] 0x9f
step 800000
set hardware port[1] 0xdf
step 500000
set hardware port[1] 0x9f
step 800000
quit
EOF
run_sim "$TMP/s6.txt" "$TMP/s6.out"
check "no revert; one program change for the repeated number" \
      "$(messages "$TMP/s6.out")" \
      "b0147f b0157f b0167f b0177f b0187f c07e"

echo "scenario 7: held 1.2s (over the threshold) reverts on release"
cat > "$TMP/s7.txt" <<'EOF'
set hardware port[1] 0x9f
step 800000
set hardware port[1] 0xdf
step 715000
set hardware port[1] 0x9f
step 800000
quit
EOF
run_sim "$TMP/s7.txt" "$TMP/s7.out"
check "stomps revert to off; one program change for the repeated number" \
      "$(messages "$TMP/s7.out")" \
      "b01400 b0147f b01500 b0157f b01600 b0167f b01700 b0177f b01800 b0187f c07d"

echo "scenario 8: stomps ON, long press turns OFF, release puts them back ON"
cat > "$TMP/s8.txt" <<'EOF'
set hardware port[1] 0x9f
step 800000
set hardware port[1] 0xdf
step 300000
set hardware port[1] 0x9f
step 800000
set hardware port[1] 0xdf
step 715000
set hardware port[1] 0x9f
step 800000
quit
EOF
run_sim "$TMP/s8.txt" "$TMP/s8.out"
check "on, off at press, back on at release" \
      "$(messages "$TMP/s8.out")" \
      "b01400 b0147f b0147f b01500 b0157f b0157f b01600 b0167f b0167f b01700 b0177f b0177f b01800 b0187f b0187f c07c c07f"

if [ "$fail" -eq 0 ]; then
    echo "simulator smoke tests passed"
else
    echo "simulator smoke tests FAILED"
fi

exit "$fail"
