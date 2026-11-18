#!/bin/bash
# Runs the real 8051 image in the uCsim simulator and checks the MIDI bytes.
# The simulator cannot model the key-column latches, so pressing the key-row
# line shows all six keys on that row (switches 1-5 and DOWN) as pressed at once.
# Pedals and the settings EEPROM are not simulated; see docs/testing.md for what is untested.
# Usage: test/sim_smoke.sh   (run from the project root after `make`)

set -u

IHX=build/fcb1010.ihx
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

run_sim() {
    # $1 = command file, $2 = output file for UART bytes. The SysEx identity sent at
    # power-on (the first bytes, F0 to F7) is moved to $2.id, so the scenarios see only
    # what their keys send; scenarios 3 and 12 check it.
    s51 -t 80C32 -X 12M -S out="$2" "$IHX" < "$1" > "$TMP/console.txt" 2>&1
    strip_id "$2"
}

# Move a leading SysEx (the power-on identity) from UART output file $1 to $1.id.
strip_id() {
    python3 -I - "$1" <<'PY'
import sys
p = sys.argv[1]
d = open(p, 'rb').read()
n = d.find(b'\xf7') + 1 if d[:1] == b'\xf0' else 0
open(p + '.id', 'wb').write(d[:n])
open(p, 'wb').write(d[n:])
PY
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
# DEFAULT_PEDAL_MODE (the EEPROM is not simulated, so every switch has the default mode).
read -r CC1 CC2 CC3 CC4 CC5 CC6 CC7 CC8 CC9 CC10 DEF WRAP < <(python3 -I -c '
import re, sys
t = open("src/config.h").read()
cc = [int(x) for x in re.search(r"#define STOMP_CC_LIST ([0-9, ]+)", t).group(1).split(",")]
d = re.search(r"#define DEFAULT_PEDAL_MODE (\d)", t).group(1)
w = re.search(r"#define PROGRAM_WRAP (\d)", t).group(1)
print(" ".join("%02x" % c for c in cc[:10]), d, w)')

# What n DOWN steps send at power-on: nothing. The pedal starts in bank mode (DIRECT
# SELECT dark), where DOWN chooses a footswitch bank (from bank 1, stopping there or, with
# PROGRAM_WRAP 1, wrapping to the last), and a bank change sends nothing here (no bank
# has a program change: the EEPROM is not simulated).
pc_down() {
    :
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

# Hold times under and over the long-press time, from config.h: each is placed as far
# as possible from the hold threshold and from the DOWN auto-repeat steps (DOWN is in
# the same row, and the simulator's steps only roughly track time), with the number of
# program steps each hold gives. About 595 simulator steps per ms.
# Steps per ms vary with what the firmware is doing (about 550-800 while keys are held),
# so each step count is chosen for that whole band: UNDER stays under the long-press time,
# OVER over it, TAP under both it and the auto-repeat delay. The DOWN program steps a hold
# can give are a range (N_*_LO..N_*_HI) over the band.
read -r UNDER_STEPS OVER_STEPS TAP_STEPS UNDER_LO UNDER_HI OVER_LO OVER_HI N_UNDER_LO N_UNDER_HI N_OVER_LO N_OVER_HI < <(python3 -I -c '
import re, sys
t = open("src/config.h").read()
n = lambda k: int(re.search(r"#define " + k + r" (\d+)", t).group(1))
hold = (n("MOMENTARY_HOLD_MS") // 12) * 12
d, p = n("PROGRAM_REPEAT_DELAY_MS"), 1000 // n("PROGRAM_REPEAT_HZ")
lo, hi = 550, 800
steps = lambda ms: 1 if ms < d else int((ms - d) // p) + 1
under = int((hold - 50) * lo)
over = int((hold + 100) * hi)
tap = int((min(hold, d) - 50) * lo)
if under < 60 * hi or tap < 60 * hi:
    sys.exit("sim_smoke: MOMENTARY_HOLD_MS %d is too short for these scenarios" % hold)
r = lambda st: (st // hi, st // lo)
(ul, uh), (ol, oh) = r(under), r(over)
print(under, over, tap, ul, uh, ol, oh, steps(ul), steps(uh), steps(ol), steps(oh))')

# check_any label got expected... : passes if got matches any of the expected lists
# (each given with "_" for the spaces between messages).
check_any() {
    local label="$1" got="$2" e
    shift 2
    for e in "$@"; do
        if [ "$got" = "${e//_/ }" ]; then check "$label" "$got" "$got"; return; fi
    done
    check "$label" "$got" "one of: $*"
}

# Per scenario, what each switch sends in each mode (stomp, stomp with momentary,
# momentary, inverted momentary): tap; tap tap; hold under; hold over; tap then hold over.
case "$DEF" in
    0) V1="7f"; V2="7f 00"; V6="7f"; V7="7f"; V8="7f 00" ;;
    1) V1="7f"; V2="7f 00"; V6="7f"; V7="7f 00"; V8="7f 00 7f" ;;
    2) V1="7f 00"; V2="7f 00 7f 00"; V6="7f 00"; V7="7f 00"; V8="7f 00 7f 00" ;;
    # Inverted momentary: 0 pressed, 127 released (the lists are compared sorted).
    *) V1="00 7f"; V2="00 7f 00 7f"; V6="00 7f"; V7="00 7f"; V8="00 7f 00 7f" ;;
esac
echo "config.h: switch 1-5 CCs 0x$CC1..0x$CC5, default pedal mode $DEF"

echo "scenario 1: idle, then press row 1 (switches 1-5 and DOWN)"
cat > "$TMP/s1.txt" <<EOF
set hardware port[1] 0x9f
step 800000
set hardware port[1] 0xdf
step $TAP_STEPS
set hardware port[1] 0x9f
step 400000
quit
EOF
run_sim "$TMP/s1.txt" "$TMP/s1.out"
check "five stomp CCs and the program change for one DOWN from 0 (none without wrap)" \
      "$(messages "$TMP/s1.out")" \
      "$(expect "$V1" $(pc_down 1))"

echo "scenario 2: press, release, press again (toggle off; DOWN changes bank, sending nothing)"
cat > "$TMP/s2.txt" <<EOF
set hardware port[1] 0x9f
step 800000
set hardware port[1] 0xdf
step $TAP_STEPS
set hardware port[1] 0x9f
step 800000
set hardware port[1] 0xdf
step $TAP_STEPS
set hardware port[1] 0x9f
step 400000
quit
EOF
run_sim "$TMP/s2.txt" "$TMP/s2.out"
check "second press toggles stomps off; no program change for DOWN (bank mode)" \
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
# The identity at power-on: F0 7E <channel - 1> 06 02 7D "FC10", the version (bits 0-6),
# their bit 7s, F7.
ID_EXPECT=$(python3 -I -c '
import re
t = open("src/version.h").read() + open("src/version_build.inc").read()
v = [int(re.search(r"#define FW_VERSION_" + k + r" (0x[0-9A-Fa-f]+|[0-9]+)", t).group(1), 0) for k in ("MAJOR", "MINOR", "BUILD")]
c = int(re.search(r"#define MIDI_CHANNEL (\d+)", open("src/config.h").read()).group(1))
b = [0xF0, 0x7E, c - 1, 0x06, 0x02, 0x7D, 0x46, 0x43, 0x31, 0x30] + [x & 0x7F for x in v]
b += [sum(((x >> 7) & 1) << i for i, x in enumerate(v)), 0xF7]
print("".join("%02x" % x for x in b))')
check "the SysEx identity sent once at power-on" "$(hex_of "$TMP/s3.out.id")" "$ID_EXPECT"

echo "scenario 4: UP + DOWN held opens config mode; a long UP asks for a bank and a short UP saves and leaves, a long DOWN cancels, a long UP + DOWN saves and leaves"
# 0xff presses both key rows, so UP, DOWN and switches 1-10 are all down together. The
# chord opens config mode after about 1s; the stomp presses sent their CCs at the start.
# 0xbf presses row 2 only (switches 6-10 and UP): held over 1s in config mode that is
# a long UP (the switches type channel digits first), which opens the save prompt, still
# in config mode; a short tap of row 2 then saves to the bank shown (switches 6-10 type
# no bank digit while UP is down) and leaves. 0xdf presses
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
set hardware port[1] 0xbf
step 60000
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
strip_id "$TMP/s4.out"
MODES=$(grep -E "^0x$(printf '%02x' "$MODE_ADDR") " "$TMP/s4.console" | awk '{print $2}' | tr '\n' ' ')
check "mode: chord, release, long UP, release, short UP, chord, long DOWN, release, chord, release, chord, release (1 1 1 1 0 1 0 0 1 1 0 0)" \
      "$MODES" "01 01 01 01 00 01 00 00 01 01 00 00 "
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
step $TAP_STEPS
set hardware port[1] 0x9f
step 400000
dump iram $RX_HEAD_DEC $RX_HEAD_DEC 1
dump iram $RX_TAIL_DEC $RX_TAIL_DEC 1
state
quit
EOF2
s51 -t 80C32 -X 12M -S out="$TMP/s5.out" "$IHX" < "$TMP/s5.txt" > "$TMP/s5.console" 2>&1
strip_id "$TMP/s5.out"
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

# About 595,000 simulator steps equal one second. UNDER_MS and OVER_MS (from config.h,
# above) are under and over the long-press time.

echo "scenario 6: held about $UNDER_LO-$UNDER_HI ms (under the long-press time) stays latched; DOWN repeats"
printf 'set hardware port[1] 0x9f\nstep 800000\nset hardware port[1] 0xdf\nstep %s\nset hardware port[1] 0x9f\nstep 800000\nquit\n' \
    "$UNDER_STEPS" > "$TMP/s6.txt"
run_sim "$TMP/s6.txt" "$TMP/s6.out"
check_any "no revert; no program change for the repeated DOWN (bank mode)" "$(messages "$TMP/s6.out")" \
      $(for k in $(seq "$N_UNDER_LO" "$N_UNDER_HI"); do printf '%s\n' "$(expect "$V6" $(pc_down "$k"))" | tr ' ' '_'; done)

echo "scenario 7: held about $OVER_LO-$OVER_HI ms (over the long-press time) reverts on release"
printf 'set hardware port[1] 0x9f\nstep 800000\nset hardware port[1] 0xdf\nstep %s\nset hardware port[1] 0x9f\nstep 800000\nquit\n' \
    "$OVER_STEPS" > "$TMP/s7.txt"
run_sim "$TMP/s7.txt" "$TMP/s7.out"
check_any "over the threshold (reverts only in stomp with momentary); no program change" "$(messages "$TMP/s7.out")" \
      $(for k in $(seq "$N_OVER_LO" "$N_OVER_HI"); do printf '%s\n' "$(expect "$V7" $(pc_down "$k"))" | tr ' ' '_'; done)

echo "scenario 8: stomps ON, long press turns OFF, release puts them back ON"
printf 'set hardware port[1] 0x9f\nstep 800000\nset hardware port[1] 0xdf\nstep %s\nset hardware port[1] 0x9f\nstep 800000\nset hardware port[1] 0xdf\nstep %s\nset hardware port[1] 0x9f\nstep 800000\nquit\n' \
    "$TAP_STEPS" "$OVER_STEPS" > "$TMP/s8.txt"
run_sim "$TMP/s8.txt" "$TMP/s8.out"
check_any "on, off at press, back on at release only in stomp with momentary" "$(messages "$TMP/s8.out")" \
      $(for k in $(seq "$N_OVER_LO" "$N_OVER_HI"); do printf '%s\n' "$(expect "$V8" $(pc_down $((k + 1))) $(pc_down 1))" | tr ' ' '_'; done)

echo "scenario 9: a short UP + DOWN press in run mode toggles program mode, sending nothing"
# 0xff presses every key, so the ten footswitches are tapped too. Held briefly (under
# the 1s config hold) and let go: only the ten taps, no program change.
cat > "$TMP/s9.txt" <<EOF
set hardware port[1] 0x9f
step 800000
set hardware port[1] 0xff
step $TAP_STEPS
set hardware port[1] 0x9f
step 400000
quit
EOF
run_sim "$TMP/s9.txt" "$TMP/s9.out"
check "ten stomp taps and nothing else, no program change" \
      "$(messages "$TMP/s9.out")" \
      "$(SWITCHES="$CC1 $CC2 $CC3 $CC4 $CC5 $CC6 $CC7 $CC8 $CC9 $CC10" expect "$V1")"

echo "scenario 10: config page 9 lights every LED, scrolls riban, then shows the major version"
# Into config mode, then eight taps of row 2 (UP, plus switches 6-10, which only edit
# settings on the way; on pages 3, 4 and 8, EXP A, EXP B and misc, they do nothing, as
# UP is down when they are let go) reach page 9.
# About 0.1 s in, led_map (units, tens, left) holds the scroll's lead-in: digits blank,
# the units' dot lit; about 3.5 s in (after the ten scroll frames, 3 s with the default
# SCROLL_FRAME_MS) the major version with the dot after the units.
LED_MAP=$((16#$(awk '/_led_map:/ {print $1}' build/hw.rst | head -1)))
VER_EXPECT=$(python3 -I -c '
import re
t = open("src/version.h").read()
v = int(re.search(r"#define FW_VERSION_MAJOR (0x[0-9A-Fa-f]+|[0-9]+)", t).group(1), 0)
seg = [0xFC, 0x60, 0xDA, 0xF2, 0x66, 0xB6, 0xBE, 0xE0, 0xFE, 0xF6, 0xEE, 0x3E, 0x9C, 0x7A, 0x9E, 0x8E]
left = 0xFF if v < 100 else ((~seg[1]) & 0xFE) | 1   # the leftmost "1" from 100
v = v % 100
units = (~seg[v % 10]) & 0xFE
tens = ((~seg[v // 10]) & 0xFE) | 1
print("%02x %02x %02x" % (units, tens, left))')
# The lead-in: units blank with its dot (bit 0, lit low), tens and left blank.
SCROLL_EXPECT="fe ff ff"
# Dump times from config.h (about 595 steps per ms), counted from the last tap's release
# (about 36ms to debounce): mid first lead-in frame, mid " r" (2 frames on), and mid the
# major version (it starts 10 frames in).
read -r S10_LEAD S10_R S10_MAJOR < <(python3 -I -c '
import re
t = open("src/config.h").read()
n = lambda k: int(re.search(r"#define " + k + r"\s+(\d+)", t).group(1))
f, v = n("SCROLL_FRAME_MS"), n("VERSION_SHOW_MS")
print((36 + f // 2) * 595, 2 * f * 595, (15 * f // 2 + v // 2) * 595)')
{
    echo "set hardware port[1] 0x9f"; echo "step 800000"
    echo "set hardware port[1] 0xff"; echo "step 900000"
    echo "set hardware port[1] 0x9f"; echo "step 300000"
    for i in 1 2 3 4 5 6 7 8; do
        echo "set hardware port[1] 0xbf"; echo "step 60000"
        echo "set hardware port[1] 0x9f"; echo "step 60000"
    done
    echo "step $((S10_LEAD - 60000))"
    # One byte per dump, so a row boundary between them cannot split the output.
    for a in 0 1 2; do echo "dump iram $((LED_MAP + a)) $((LED_MAP + a)) 1"; done
    # " r" (after the two lead-in frames), its dot after the left position's "1".
    echo "step $S10_R"
    for a in 0 1 2; do echo "dump iram $((LED_MAP + a)) $((LED_MAP + a)) 1"; done
    echo "step $S10_MAJOR"
    for a in 0 1 2; do echo "dump iram $((LED_MAP + a)) $((LED_MAP + a)) 1"; done
    echo "quit"
} > "$TMP/s10.txt"
s51 -t 80C32 -X 12M -S out="$TMP/s10.out" "$IHX" < "$TMP/s10.txt" > "$TMP/s10.console" 2>&1
strip_id "$TMP/s10.out"
# Each address is dumped three times: the lead-in, " r", and the major version.
check "at once the display shows the scroll's lead-in dot (units, tens, left: $SCROLL_EXPECT)" \
      "$(for a in 0 1 2; do grep -E "^0x$(printf '%02x' $((LED_MAP + a))) " "$TMP/s10.console" | head -1 | awk '{printf "%s ", $2}'; done | sed 's/ $//')" "$SCROLL_EXPECT"
# " r": units the scroll's r (A B E F = 0xCC, its dot off), tens blank, left blank with
# its dot lit (bit 0 low).
R_EXPECT="$(printf '%02x' $(( (~0xCC & 0xFE) | 1 ))) ff fe"
check "then the first riban frame, the scroll's r (A B E F) with the dot after the \"1\" (units, tens, left: $R_EXPECT)" \
      "$(for a in 0 1 2; do grep -E "^0x$(printf '%02x' $((LED_MAP + a))) " "$TMP/s10.console" | sed -n 2p | awk '{printf "%s ", $2}'; done | sed 's/ $//')" "$R_EXPECT"
check "after the scroll the display shows the major version (units, tens, left: $VER_EXPECT)" \
      "$(for a in 0 1 2; do grep -E "^0x$(printf '%02x' $((LED_MAP + a))) " "$TMP/s10.console" | tail -1 | awk '{printf "%s ", $2}'; done | sed 's/ $//')" "$VER_EXPECT"
# The channel page taps changed the channel, so a change is unsaved. By now (past the
# LAMP_TEST_MS lamp test) page 9 shows the chase: one panel LED lit at a time (row 3's
# SWITCH 1 / 2 and the MIDI / EXP bits, row 5's SWITCH, SELECT to VALUE 2 and DIRECT
# SELECT), and a footswitch LED lit at the start of each segment (switches 4 and 5 are
# row 3's 0x80 / 0x40, the other eight all of row 4). Lit is a low bit. So no row shows
# more than one chase LED and one footswitch LED; exactly one of EXP 1 / EXP 2 (row 3's
# 0x08 / 0x04) is lit, the other dark; row 5's CONFIG bit (0x40) flashes, so
# both levels are seen; and the chase is seen in rows 3 and 5, a footswitch LED lit.
# A second run to the same point, then about 1 s of latch samples (commons at 0x1FF8,
# segments at 0x1FF9). s51 echoes queued input into its output, so the dump lines are
# picked out by pattern, and kept apart from the checks above.
{
    sed '/^dump\|^quit/d' "$TMP/s10.txt"
    for i in $(seq 300); do echo "step 2000"; echo "dump xram 0x1ff8 0x1ff9 2"; done
    echo "quit"
} > "$TMP/s10b.txt"
s51 -t 80C32 -X 12M -S out="$TMP/s10b.out" "$IHX" < "$TMP/s10b.txt" > "$TMP/s10b.console" 2>&1
strip_id "$TMP/s10b.out"
# s51 prints a dump either as one line (0x1ff8, both bytes) or one line per byte; both
# are paired up here. Too few samples fails rather than passing on nothing.
LAMP10=$(grep -aoE '0x1ff[89] {4,}[0-9a-f]{2}( [0-9a-f]{2})?' "$TMP/s10b.console" | awk '
    BEGIN { for (i = 0; i < 16; i++) hexv[substr("0123456789abcdef", i + 1, 1)] = i }
    # The lit (low) bits of v among mask m, counted.
    function lit(v, m,   x, k, n) {
        x = hexv[substr(v, 1, 1)] * 16 + hexv[substr(v, 2, 1)]
        n = 0
        for (k = 1; k < 256; k *= 2) if (int(m / k) % 2 == 1 && int(x / k) % 2 == 0) n++
        return n
    }
    # 0xff is the ISR blanking the row before driving it (sampled mid-ISR), or a row with
    # nothing lit: skipped (so about 60% of the samples count).
    function sample(c, v,   fs, ch) {
        if (v == "ff") return
        total++
        if (c == "08") { fs = lit(v, 192); ch = lit(v, 51); if (fs > 1 || ch > 1 || lit(v, 12) != 1) bad = 1; if (ch) chase3 = 1; if (fs) blink = 1 }
        if (c == "10") { fs = lit(v, 255); if (fs > 1) bad = 1; if (fs) blink = 1 }
        if (c == "20") { n++; ch = lit(v, 191); if (ch > 1) bad = 1; if (ch) chase5 = 1; if (lit(v, 64)) on = 1; else off = 1 }
    }
    $1 == "0x1ff8" && NF == 3 { sample($2, $3); com = ""; next }
    $1 == "0x1ff8" { com = $2; next }
    $1 == "0x1ff9" && com != "" { sample(com, $2); com = "" }
    END { print (bad ? "bad" : "ok"), (total >= 150 ? "enough" : "few:" total + 0), (n >= 20 ? "many" : n + 0), on + 0, off + 0, chase3 + 0, chase5 + 0, blink + 0 }')
check "page 9 with an unsaved change: the chase, CONFIG flashing, a footswitch LED lit (state, samples, row-5 samples, CONFIG lit, CONFIG dark, chase row 3, chase row 5, footswitch lit)" \
      "$LAMP10" "ok enough many 1 1 1 1 1"
# The entry chord taps all ten stomps in run mode (their CCs); page changes send no program.
check "no program change was sent" "$(messages "$TMP/s10.out" | tr ' ' '\n' | grep -c '^c')" "0"

echo "scenario 11: UP held about 5 s repeats slowly, then fast after PROGRAM_REPEAT_FAST_AFTER_MS; one bank change"
# The pedal starts in bank mode, so UP steps the footswitch bank (the same repeat as for
# programs). Program mode cannot be held here: the simulator presses switches 6-10 with
# UP, and in program mode switch 10 starts typing a number, which stops UP repeating.
# Row 2 (0xbf) is UP plus switches 6-10 (their CCs are ignored here). Slow steps: one at
# 0.4 s, then every 0.25 s to 16 at about 4.15 s; then one every 50 ms, so about 30 at
# 5 s (instruction steps only roughly track time; the host tests check exact timing).
# Without the fast repeat it would be about 19. The bank stops at the last one (or wraps
# with PROGRAM_WRAP), so the fast repeat shows if BANK_COUNT is over the slow count. The
# bank (hal_bank) is read back from RAM; nothing but the footswitches' CCs is sent.
BANK_ADDR=$((16#$(awk '/_hal_bank:/ {print $1}' build/hw.rst | head -1)))
cat > "$TMP/s11.txt" <<EOF11
set hardware port[1] 0x9f
step 800000
set hardware port[1] 0xbf
step 2975000
set hardware port[1] 0x9f
step 400000
dump iram $BANK_ADDR $BANK_ADDR 1
quit
EOF11
s51 -t 80C32 -X 12M -S out="$TMP/s11.out" "$IHX" < "$TMP/s11.txt" > "$TMP/s11.console" 2>&1
strip_id "$TMP/s11.out"
B11=$(grep -E "^0x$(printf '%02x' "$BANK_ADDR") " "$TMP/s11.console" | awk '{print $2}')
N11=$((16#${B11:-ff}))
PC11=$(messages "$TMP/s11.out" | tr ' ' '\n' | grep -c '^c')
# The banks a hold of 4.4 to 5.0 s can reach (the simulator's steps only roughly track
# time), from the timings in config.h, and the bank with no fast repeat.
read -r OK11 SLOW11 < <(python3 -I -c '
import math, re
t = open("src/config.h").read()
n = lambda k: int(re.search(r"#define " + k + r" (\d+)", t).group(1))
d, p = n("PROGRAM_REPEAT_DELAY_MS"), 1000 / n("PROGRAM_REPEAT_HZ")
f, a = 1000 / n("PROGRAM_REPEAT_FAST_HZ"), n("PROGRAM_REPEAT_FAST_AFTER_MS")
banks, wrap = n("BANK_COUNT"), n("PROGRAM_WRAP")
fs = math.ceil((a - d) / p) + 1
at = d + (fs - 1) * p
steps = lambda ms: int((ms - d) // p) + 1 if ms < at else fs + int((ms - at) // f)
bank = lambda k: k % banks if wrap else min(k, banks - 1)
ok = sorted(set(bank(k) for k in range(steps(4400) - 1, steps(5000) + 2)))
print(",".join(map(str, ok)), bank(int((5000 - d) // p) + 1))')
if [ "$PC11" = "0" ] && echo ",$OK11," | grep -q ",$N11,"; then
    echo "  ok   no program change; bank index $N11 (one of $OK11 expected from config.h, $SLOW11 without the fast repeat)"
else
    echo "  FAIL after a 5 s UP hold: bank index $N11 and $PC11 program changes, expected one of $OK11 and none"
    fail=1
fi

echo "scenario 12: power-on: every LED lit while riban scrolls, then run mode on its own"
# No keys. About 0.5 s in (during the scroll) LED row 3 (common
# 0x08: MIDI CHAN, MIDI FUNCTION, EXP and SWITCH LEDs) is fully lit (0x00); well after the
# animation (about 10 s) it is back to the normal LEDs, none of which are lit in run mode
# here, so it is never 0x00 (dark is 0xff). During the animation 0xff samples are the ISR
# blanking the row, so they are skipped there.
{
    # 1300 steps is just over one 2 ms scan tick, so the samples walk through the rows.
    echo "set hardware port[1] 0x9f"; echo "step 300000"
    for i in $(seq 60); do echo "step 1300"; echo "dump xram 0x1ff8 0x1ff9 2"; done
    echo "step 6700000"
    for i in $(seq 60); do echo "step 1300"; echo "dump xram 0x1ff8 0x1ff9 2"; done
    echo "quit"
} > "$TMP/s12.txt"
s51 -t 80C32 -X 12M -S out="$TMP/s12.out" "$IHX" < "$TMP/s12.txt" > "$TMP/s12.console" 2>&1
strip_id "$TMP/s12.out"
# Samples in order; the first 60 dumps are during the animation, the rest after it.
BOOT12=$(grep -aoE '0x1ff[89] {4,}[0-9a-f]{2}( [0-9a-f]{2})?' "$TMP/s12.console" | awk '
    function sample(c, v) {
        k++
        if (c != "08") return
        if (k <= 60) { if (v == "ff") return; during++; if (v != "00") dbad = 1 } else { after++; if (v == "00") abad = 1 }
    }
    $1 == "0x1ff8" && NF == 3 { sample($2, $3); com = ""; next }
    $1 == "0x1ff8" { com = $2; next }
    $1 == "0x1ff9" && com != "" { sample(com, $2); com = "" }
    END { print (during >= 3 ? "seen" : "few:" during + 0), (dbad ? "not-lit" : "lit"), (after >= 3 ? "seen" : "few:" after + 0), (abad ? "still-lit" : "normal") }')
check "LED row 3 lit during the animation, normal afterwards (samples, state, samples, state)" "$BOOT12" "seen lit seen normal"
check "nothing sent at power-on but the identity" "$(messages "$TMP/s12.out") $(hex_of "$TMP/s12.out.id")" " $ID_EXPECT"

echo "scenario 13: by default (DIRECT SELECT dark) UP and DOWN change footswitch bank, not program"
# Row 2 (0xbf, UP with switches 6-10) three times and row 1 (0xdf, DOWN with switches
# 1-5) once, each a tap. Each bank change loads the new bank (the EEPROM is not
# simulated, so the defaults, whose CCs keep their states) and sends nothing. The bank
# (hal_bank) is read back from RAM.
{
    echo "set hardware port[1] 0x9f"; echo "step 800000"
    for row in 0xbf 0xbf 0xbf 0xdf; do
        echo "set hardware port[1] $row"; echo "step $TAP_STEPS"
        echo "set hardware port[1] 0x9f"; echo "step 400000"
        echo "dump iram $BANK_ADDR $BANK_ADDR 1"
    done
    echo "state"; echo "quit"
} > "$TMP/s13.txt"
s51 -t 80C32 -X 12M -S out="$TMP/s13.out" "$IHX" < "$TMP/s13.txt" > "$TMP/s13.console" 2>&1
strip_id "$TMP/s13.out"
BANKS13=$(grep -E "^0x$(printf '%02x' "$BANK_ADDR") " "$TMP/s13.console" | awk '{print $2}' | tr '\n' ' ')
check "bank after UP, UP, UP, DOWN (2 3 4 3, stored from 0)" "$BANKS13" "01 02 03 02 "
check "no program change was sent" "$(messages "$TMP/s13.out" | tr ' ' '\n' | grep -c '^c')" "0"
MAXSP13=$(grep -E "Max value of stack pointer" "$TMP/s13.console" | tail -1 | sed -E 's/.*= *0x0*([0-9a-fA-F]+),.*/\1/')
if [ -n "$MAXSP13" ] && [ "$((16#$MAXSP13))" -le 240 ]; then
    echo "  ok   bank changes (EEPROM reads) keep the stack at 0x$MAXSP13"
else
    echo "  FAIL stack pointer 0x$MAXSP13 too high during bank changes"
    fail=1
fi

echo "scenario 14: a bank change keeps each CC's state (nothing is sent)"
# Stomp modes only (0, 1): the taps must latch. A momentary switch sends on press and
# release and is off once let go, so the expected states and count do not apply; the host
# tests (test_banks, test_cc_memory) cover the carry-over in every mode.
if [ "$DEF" = "0" ] || [ "$DEF" = "1" ]; then
# Each tap of row 2 (0xbf, UP with switches 6-10) or row 1 (0xdf, DOWN with switches 1-5)
# toggles those five and changes bank (bank mode is the default). Every bank has the
# default CCs (no EEPROM), so the states carry over: stomp_state and hal_bank are read
# back from RAM after each step.
SS_ADDR=$((16#$(awk '/_stomp_state:/ {print $1}' build/ctl.rst | head -1)))
{
    echo "set hardware port[1] 0x9f"; echo "step 800000"
    for row in 0xbf 0xdf 0xbf 0xdf; do
        echo "set hardware port[1] $row"; echo "step $TAP_STEPS"
        echo "set hardware port[1] 0x9f"; echo "step 400000"
        echo "dump iram $SS_ADDR $((SS_ADDR + 1)) 2"
        echo "dump iram $BANK_ADDR $BANK_ADDR 1"
    done
    echo "quit"
} > "$TMP/s14.txt"
s51 -t 80C32 -X 12M -S out="$TMP/s14.out" "$IHX" < "$TMP/s14.txt" > "$TMP/s14.console" 2>&1
strip_id "$TMP/s14.out"
STATES14=$(grep -E "^0x($(printf '%02x' "$SS_ADDR")|$(printf '%02x' "$BANK_ADDR")) " "$TMP/s14.console" | awk '{printf "%s ", ($3 == "" || $3 == ".") ? $2 : $2 $3}')
check "states and bank after UP row, DOWN row, UP row, DOWN row (6-10 on in 2; all in 1; 1-5 in 2; none in 1)" \
      "$STATES14" "e003 01 ff03 00 1f00 01 0000 00 "
check "the bank changes sent only the footswitch taps' own CCs (20 messages)" \
      "$(messages "$TMP/s14.out" | wc -w)" "20"
else
    echo "  skip (default pedal mode $DEF is momentary)"
fi

if [ "$fail" -eq 0 ]; then
    echo "simulator smoke tests passed"
else
    echo "simulator smoke tests FAILED"
fi

exit "$fail"
