# Build hook: fill the README's numbers that come from src/config.h (timings, hold times,
# CC numbers, channel, counts), so the README always describes the firmware that was
# built. Usage: readme_config.py <config.h> <README.md>
# Two kinds of update:
# - The "Compile-time configuration" table: every row whose first cell is a `NAME`
#   defined in config.h gets that definition in its second cell (a missing NAME prints a
#   note).
# - Numbers in the text: each is found by the words around it (the RULES below); only
#   the number itself is rewritten, in every place the words match. A rule whose words are
#   not found prints a note (the README was reworded) and changes nothing for that value.
# The file is only written when something changes.
import re
import sys

config_h, readme = sys.argv[1], sys.argv[2]
cfg_text = open(config_h).read()


def define(name):
    found = re.search(r"^#define " + name + r"\s+(\S.*?)\s*$", cfg_text, re.M)

    return found.group(1) if found else None


def num(name):
    found = re.search(r"^#define " + name + r"\s+(\d+)\s*$", cfg_text, re.M)

    if not found:
        sys.exit("%s: no '#define %s <number>' line" % (config_h, name))

    return int(found.group(1))


def num_list(name):
    found = re.search(r"^#define " + name + r"\s+([\d,\s]+?)\s*$", cfg_text, re.M)

    if not found:
        sys.exit("%s: no '#define %s <list>' line" % (config_h, name))

    return [int(v) for v in found.group(1).split(",")]


def secs(ms):
    return "%g" % (ms / 1000.0)


def sec_words(ms):
    return "1 second" if ms == 1000 else secs(ms) + " seconds"


# "CC 21 to 30" for a run of consecutive numbers, else "CC 21, 25, 22, ...".
def cc_words(ccs):
    if all(ccs[i + 1] == ccs[i] + 1 for i in range(len(ccs) - 1)):
        return "CC %d to %d" % (ccs[0], ccs[-1])

    return "CC " + ", ".join(str(c) for c in ccs)


stomp_cc = num_list("STOMP_CC_LIST")
pedal_cc = num_list("PEDAL_CC_LIST")
chord_cc = num("CHORD_CC")
channel = num("MIDI_CHANNEL")
hold = num("MOMENTARY_HOLD_MS")
config_hold = num("CONFIG_HOLD_MS")
hold_real = (hold // 12) * 12 + num("DEBOUNCE_SAMPLES") * 12
modes = ["stomp", "stomp with momentary", "momentary", "inverted momentary"]
show = num("VERSION_SHOW_MS")
wrap = num("PROGRAM_WRAP") != 0
WRAP_TEXT = "wrapping from 127 back to 0, and from 0 to 127"
STOP_TEXT = "stopping at 0 and 127; a press that cannot change the number sends nothing"
CC_PATTERN = r"(CC [0-9][0-9, ]*(?: to [0-9]+)?)"

RULES = [
    # Using the pedal
    (r"Volume \(CC (\d+)\)", [pedal_cc[0]]),
    (r"Expression \(CC (\d+)\)", [pedal_cc[1]]),
    (r"An extra on/off switch \(CC (\d+)\)", [chord_cc]),
    (r"UP \+ DOWN held ([\d.]+)s ", [secs(config_hold)]),
    (r"channel (\d+) unless you change it", [channel]),
    (r"Footswitches 1 to 10 send " + CC_PATTERN + ", with value", [cc_words(stomp_cc)]),
    (r"chosen in configuration mode \(default: ([a-z ]+)\)", [modes[num("DEFAULT_PEDAL_MODE") & 3]]),
    (r"a hold of more than ([\d.]+)s undoes it", [secs(hold)]),
    (r"Released within ([\d.]+ seconds?)", [sec_words(hold)]),
    (r"Released after more than ([\d.]+ seconds?)", [sec_words(hold)]),
    (r"If you let go within (a second|[\d.]+ seconds?) it stays",
     ["a second" if hold == 1000 else sec_words(hold)]),
    (r"from 0 to 127 \((" + re.escape(WRAP_TEXT) + "|" + re.escape(STOP_TEXT) + r")\)",
     [WRAP_TEXT if wrap else STOP_TEXT]),
    (r"Held: after (\d+)ms the number auto-repeats at (\d+)Hz",
     [num("PROGRAM_REPEAT_DELAY_MS"), num("PROGRAM_REPEAT_HZ")]),
    (r"speeding up to about (\d+)Hz", [num("PROGRAM_REPEAT_FAST_HZ")]),
    (r"Hz after ([\d.]+)s,", [secs(num("PROGRAM_REPEAT_FAST_AFTER_MS"))]),
    # Configuration
    (r"Hold UP and DOWN together for ([\d.]+ seconds?)", [sec_words(config_hold)]),
    (r"or UP \+ DOWN together for ([\d.]+ seconds?)", [sec_words(config_hold)]),
    (r"[Hh]old UP \(ENTER\) for ([\d.]+ seconds?)", [sec_words(hold)]),
    (r"hold DOWN \(ESCAPE\) for ([\d.]+ seconds?)", [sec_words(hold)]),
    (r"Switch 10 flashes `0` for ([\d.]+ seconds?)", [sec_words(num("CHANNEL_DIGIT_TIMEOUT_MS"))]),
    (r"After ([\d.]+ seconds?), or on a page change", [sec_words(num("CHANNEL_DIGIT_TIMEOUT_MS"))]),
    # Technical reference
    (CC_PATTERN + r" \(`STOMP_CC_LIST`\)", [cc_words(stomp_cc)]),
    (r"CC (\d+) and CC (\d+) \(`PEDAL_CC_LIST`\)", [pedal_cc[0], pedal_cc[1]]),
    (r"A CC on channel (\d+) with", [channel]),
    (r"CC (\d+) sets the UP \+ DOWN toggle", [chord_cc]),
    (r"For example, CC (\d+) value 127 sets stomp 1 on", [stomp_cc[0]]),
    (r"and CC (\d+) value 0 turns it off", [stomp_cc[0]]),
    (r"[Ii]f CC (\d+) turned switch 1 on, pressing switch 1 sends CC (\d+) value 0",
     [stomp_cc[0], stomp_cc[0]]),
    (r"`MOMENTARY_HOLD_MS` in `src/config\.h` \((?:default|currently) (\d+)\)", [hold]),
    (r"so the real threshold is about ([\d.]+)s", ["%.2f" % (hold_real / 1000.0)]),
    (r"`PROGRAM_REPEAT_FAST_AFTER_MS` \(([\d.]+)s\)", [secs(num("PROGRAM_REPEAT_FAST_AFTER_MS"))]),
    (r"[Hh]eld ([\d.]+)s again in configuration mode", [secs(config_hold)]),
    (r"A short UP \+ DOWN is the CC (\d+) toggle", [chord_cc]),
    (r"shows in hex, one part (per second|every [\d.]+ seconds?)",
     ["per second" if show == 1000 else "every " + sec_words(show)]),
    (r"The ([\d.]+) second long-press threshold", [secs(hold)]),
]

text = open(readme).read()
new = text

for pattern, values in RULES:
    regex = re.compile(pattern)

    if not regex.search(new):
        print("note: %s: no match for '%s'; that value is not filled in" % (readme, pattern))
        continue

    def fill(m):
        out = m.group(0)
        start = m.start(0)

        for i in range(len(values), 0, -1):
            a, b = m.start(i) - start, m.end(i) - start
            out = out[:a] + str(values[i - 1]) + out[b:]

        return out

    new = regex.sub(fill, new)


# Units follow the README style: no space between a number and a unit symbol (0.6s,
# 400ms, 4Hz); spelled-out units keep it ("0.6 seconds").
# The option table: "| `NAME` | value | meaning |" rows.
def table_row(m):
    value = define(m.group(1))

    if value is None:
        print("note: %s: `%s` is in the option table but not defined in %s" % (readme, m.group(1), config_h))
        return m.group(0)

    return "| `%s` | %s |" % (m.group(1), value)


new = re.sub(r"^\| `([A-Z][A-Z0-9_]*)` \| ([^|]*?) \|", table_row, new, flags=re.M)

if new != text:
    open(readme, "w").write(new)
    print("updated the config.h values in " + readme)
