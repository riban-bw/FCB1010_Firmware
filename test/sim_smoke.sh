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

# The expected MIDI follows src/config.h: the CCs of switches 1-5 (STOMP_CC_LIST) and
# DEFAULT_PEDAL_MODE (the EEPROM is not simulated, so every switch has the default mode),
# plus CHORD_CC for the run-mode UP + DOWN tap.
read -r CC1 CC2 CC3 CC4 CC5 CC6 CC7 CC8 CC9 CC10 CHORD DEF WRAP < <(python3 -I -c '
import re, sys
t = open("src/config.h").read()
cc = [int(x) for x in re.search(r"#define STOMP_CC_LIST ([0-9, ]+)", t).group(1).split(",")]
d = re.search(r"#define DEFAULT_PEDAL_MODE (\d)", t).group(1)
ch = int(re.search(r"#define CHORD_CC (\d+)", t).group(1))
w = re.search(r"#define PROGRAM_WRAP (\d)", t).group(1)
print(" ".join("%02x" % c for c in cc[:10]), "%02x" % ch, d, w)')

# The program change after n DOWN steps from 0: wrapping (PROGRAM_WRAP 1), or none at all
# (stopped at 0, so the number did not change and nothing is sent).
pc_down() {
    if [ "$WRAP" = "1" ]; then printf 'c0%02x' $(( (128 - $1) & 127 )); fi
}

# expect "<values each of switches 1-5 sends>" [other messages...] -> sorted list.
# With SWITCHES set to the CCs of other switches (for example all ten), uses those.
expect() {
    local vals="$1" out=() c v
    shift
    for c in ${SWITCHES:-$CC1 $CC2 $CC3 $CC4 $CC5}; do
        for v in $vals; do
            out+=("b0$c$v")
        done
    done
    printf '%s\n' "${out[@]}" "$@" | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//'
}

# Per scenario, what each switch sends in each mode (stomp, stomp with momentary,
# momentary, inverted momentary): tap; tap tap; hold 0.84 s; hold 1.2 s; tap then hold 1.2 s.
case "$DEF" in
    0) V1="7f"; V2="7f 00"; V6="7f"; V7="7f"; V8="7f 00" ;;
    1) V1="7f"; V2="7f 00"; V6="7f"; V7="7f 00"; V8="7f 00 7f" ;;
    2) V1="7f 00"; V2="7f 00 7f 00"; V6="7f 00"; V7="7f 00"; V8="7f 00 7f 00" ;;
    # Inverted momentary: 0 pressed, 127 released (the lists are compared sorted).
    *) V1="00 7f"; V2="00 7f 00 7f"; V6="00 7f"; V7="00 7f"; V8="00 7f 00 7f" ;;
esac
echo "config.h: switch 1-5 CCs 0x$CC1..0x$CC5, default pedal mode $DEF"

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
check "five stomp CCs and the program change for one DOWN from 0 (none without wrap)" \
      "$(messages "$TMP/s1.out")" \
      "$(expect "$V1" $(pc_down 1))"

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
check "second press toggles stomps off; program changes for DOWN from 0 twice (none without wrap)" \
      "$(messages "$TMP/s2.out")" \
      "$(expect "$V2" $(pc_down 2) $(pc_down 1))"

echo "scenario 3: nothing pressed sends nothing"
cat > "$TMP/s3.txt" <<'EOF'
set hardware port[1] 0x9f
step 1500000
quit
EOF
run_sim "$TMP/s3.txt" "$TMP/s3.out"
check "no MIDI output while idle" "$(hex_of "$TMP/s3.out")" ""

echo "scenario 4: UP + DOWN held opens config mode; a long UP saves and leaves, a long DOWN cancels, a long UP + DOWN saves and leaves"
# 0xff presses both key rows, so UP, DOWN and switches 1-10 are all down together. The
# chord opens config mode after about 1s; the stomp presses sent their CCs at the start.
# 0xbf presses row 2 only (switches 6-10 and UP): held over 1s in config mode that is
# a long UP (save and leave; the switches type channel digits first). 0xdf presses
# row 1 only (switches 1-5 and DOWN): a long DOWN (cancel, which reads the EEPROM).
# Last, 0xff held again opens config mode, and held a second time (after letting go)
# saves and leaves.
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
set hardware port[1] 0xbf
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
set hardware port[1] 0xdf
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
check "mode: chord, release, long UP, release, chord, long DOWN, release, chord, release, chord, release (1 1 0 0 1 0 0 1 1 0 0)" \
      "$MODES" "01 01 00 00 01 00 00 01 01 00 00 "
check "no program change was sent by the chords or the long UP / DOWN" \
      "$(messages "$TMP/s4.out" | tr ' ' '\n' | grep -c '^c')" "0"
MAXSP4=$(grep -E "Max value of stack pointer" "$TMP/s4.console" | tail -1 | sed -E 's/.*= *0x0*([0-9a-fA-F]+),.*/\1/')
if [ -n "$MAXSP4" ] && [ "$((16#$MAXSP4))" -le 240 ]; then
    echo "  ok   config enter, save (failed EEPROM write) and cancel (EEPROM read) keep the stack at 0x$MAXSP4"
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
      "$(expect "$V1" $(pc_down 1))"
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
      "$(expect "$V6" $(pc_down 2))"

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
check "over the threshold (reverts only in stomp with momentary); one program change" \
      "$(messages "$TMP/s7.out")" \
      "$(expect "$V7" $(pc_down 3))"

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
check "on, off at press, back on at release only in stomp with momentary" \
      "$(messages "$TMP/s8.out")" \
      "$(expect "$V8" $(pc_down 4) $(pc_down 1))"

echo "scenario 9: a short UP + DOWN press in run mode toggles CHORD_CC"
# 0xff presses every key, so the ten footswitches are tapped too. Held 0.5 s (under
# the 1s config hold) and let go: one CHORD_CC 127, no program change.
cat > "$TMP/s9.txt" <<'EOF'
set hardware port[1] 0x9f
step 800000
set hardware port[1] 0xff
step 300000
set hardware port[1] 0x9f
step 400000
quit
EOF
run_sim "$TMP/s9.txt" "$TMP/s9.out"
check "ten stomp taps and CC 0x$CHORD at 127, no program change" \
      "$(messages "$TMP/s9.out")" \
      "$(SWITCHES="$CC1 $CC2 $CC3 $CC4 $CC5 $CC6 $CC7 $CC8 $CC9 $CC10" expect "$V1" "b0${CHORD}7f")"

echo "scenario 10: config page 7 blinks every LED and segment, then shows the major version"
# Into config mode, then six taps of row 2 (UP, plus switches 6-10, which only edit
# settings on the way) reach page 7. 50 ms in, the lamp test has the segment latch
# (0x1FF9) at 0x00 (all lit); about 2 s in, after the blinks, led_map (units, tens,
# left) holds the major version with the dot after the units.
LED_MAP=$((16#$(awk '/_led_map:/ {print $1}' build/hw.rst | head -1)))
VER_EXPECT=$(python3 -I -c '
import re
t = open("src/version.h").read()
v = int(re.search(r"#define FW_VERSION_MAJOR (0x[0-9A-Fa-f]+)", t).group(1), 16)
seg = [0xFC, 0x60, 0xDA, 0xF2, 0x66, 0xB6, 0xBE, 0xE0, 0xFE, 0xF6, 0xEE, 0x3E, 0x9C, 0x7A, 0x9E, 0x8E]
units = (~seg[v & 15]) & 0xFE
tens = ((~seg[v >> 4]) & 0xFE) | 1
print("%02x %02x ff" % (units, tens))')
{
    echo "set hardware port[1] 0x9f"; echo "step 800000"
    echo "set hardware port[1] 0xff"; echo "step 900000"
    echo "set hardware port[1] 0x9f"; echo "step 300000"
    for i in 1 2 3 4 5 6; do
        echo "set hardware port[1] 0xbf"; echo "step 60000"
        echo "set hardware port[1] 0x9f"; echo "step 60000"
    done
    echo "step 30000"
    echo "dump xram 0x1ff9 0x1ff9 1"
    echo "step 1150000"
    echo "dump iram $LED_MAP $((LED_MAP + 2)) 3"
    echo "quit"
} > "$TMP/s10.txt"
s51 -t 80C32 -X 12M -S out="$TMP/s10.out" "$IHX" < "$TMP/s10.txt" > "$TMP/s10.console" 2>&1
check "during the lamp test the segment latch is 0x00 (everything lit)" \
      "$(grep -E '^0x1ff9 ' "$TMP/s10.console" | awk '{print $2}')" "00"
check "after the blinks the display shows the major version (units, tens, left: $VER_EXPECT)" \
      "$(grep -E "^0x$(printf '%02x' "$LED_MAP") " "$TMP/s10.console" | awk '{print $2, $3, $4}')" "$VER_EXPECT"
# The entry chord taps all ten stomps in run mode (their CCs); page changes send no program.
check "no program change was sent" "$(messages "$TMP/s10.out" | tr ' ' '\n' | grep -c '^c')" "0"

echo "scenario 11: UP held about 5 s repeats slowly, then fast after PROGRAM_REPEAT_FAST_AFTER_MS; one program change"
# Row 2 (0xbf) is UP plus switches 6-10 (their CCs are ignored here). Slow steps: one at
# 0.4 s, then every 0.25 s to 16 at about 4.15 s; then one every 50 ms, so about 30 at
# 5 s (instruction steps only roughly track time; the host tests check exact timing).
# Without the fast repeat it would be about 19. Only the program change is checked.
cat > "$TMP/s11.txt" <<'EOF11'
set hardware port[1] 0x9f
step 800000
set hardware port[1] 0xbf
step 2975000
set hardware port[1] 0x9f
step 400000
quit
EOF11
run_sim "$TMP/s11.txt" "$TMP/s11.out"
PC11=$(messages "$TMP/s11.out" | tr ' ' '\n' | grep '^c0')
N11=$((16#${PC11#c0}))
# The expected range from the timings in config.h, for a hold of 4.4 to 5.0 s (the
# simulator's steps only roughly track time), and the count with no fast repeat.
read -r LO11 HI11 SLOW11 < <(python3 -I -c '
import math, re
t = open("src/config.h").read()
n = lambda k: int(re.search(r"#define " + k + r" (\d+)", t).group(1))
d, p = n("PROGRAM_REPEAT_DELAY_MS"), 1000 / n("PROGRAM_REPEAT_HZ")
f, a = 1000 / n("PROGRAM_REPEAT_FAST_HZ"), n("PROGRAM_REPEAT_FAST_AFTER_MS")
fs = math.ceil((a - d) / p) + 1
at = d + (fs - 1) * p
steps = lambda ms: int((ms - d) // p) + 1 if ms < at else fs + int((ms - at) // f)
print(min(127, steps(4400) - 1), min(127, steps(5000) + 1), int((5000 - d) // p) + 1)')
if [ "$(echo "$PC11" | wc -w)" = "1" ] && [ "$N11" -ge "$LO11" ] && [ "$N11" -le "$HI11" ]; then
    echo "  ok   one program change, $N11 ($LO11-$HI11 expected from config.h, about $SLOW11 without the fast repeat)"
else
    echo "  FAIL program change after a 5 s UP hold: got '$PC11', expected one in $LO11-$HI11"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "simulator smoke tests passed"
else
    echo "simulator smoke tests FAILED"
fi

exit "$fail"
