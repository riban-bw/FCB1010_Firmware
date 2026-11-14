# Build hook: fill the README's numbers that come from src/config.h (timings, hold times,
# CC numbers, channel, counts), so the README always describes the firmware that was
# built. Usage: readme_config.py <config.h> <README.md>
# Each value is found by the words around it (the RULES below); only the number itself is
# rewritten. A rule whose words are not found prints a note (the README was reworded) and
# changes nothing for that value. The file is only written when something changes.
import re
import sys

config_h, readme = sys.argv[1], sys.argv[2]
cfg_text = open(config_h).read()


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


stomp_cc = num_list("STOMP_CC_LIST")
pedal_cc = num_list("PEDAL_CC_LIST")
chord_cc = num("CHORD_CC")
channel = num("MIDI_CHANNEL")
hold = num("MOMENTARY_HOLD_MS")
config_hold = num("CONFIG_HOLD_MS")
hold_real = (hold // 12) * 12 + num("DEBOUNCE_SAMPLES") * 12
modes = ["stomp", "stomp with momentary", "momentary", "inverted momentary"]
show = num("VERSION_SHOW_MS")

RULES = [
    (r"Continuous CC (\d+) \(volume\)", [pedal_cc[0]]),
    (r"Continuous CC (\d+) \(expression\)", [pedal_cc[1]]),
    (r"Held: after (\d+) ms the number auto-repeats at (\d+) Hz",
     [num("PROGRAM_REPEAT_DELAY_MS"), num("PROGRAM_REPEAT_HZ")]),
    (r"speeding up to about (\d+) Hz", [num("PROGRAM_REPEAT_FAST_HZ")]),
    (r"`PROGRAM_REPEAT_FAST_AFTER_MS` \(([\d.]+) s\)", [secs(num("PROGRAM_REPEAT_FAST_AFTER_MS"))]),
    (r"A CC on channel (\d+) with", [channel]),
    (r"on the same channel \(default (\d+)\)", [channel]),
    (r"CC (\d+) sets the UP \+ DOWN toggle", [chord_cc]),
    (r"Toggles CC (\d+) \(127 / 0\)", [chord_cc]),
    (r"the CC (\d+) toggle \(UP \+ DOWN\)", [chord_cc]),
    (r"the CC (\d+) trigger while CC (\d+) is on", [chord_cc, chord_cc]),
    (r"CC (\d+) \(the UP \+ DOWN toggle\)", [chord_cc]),
    (r"to follow CC (\d+) \(DIRECT SELECT", [chord_cc]),
    (r"shows the CC (\d+) state", [chord_cc]),
    (r"For example, CC (\d+) value 127 sets stomp 1 on", [stomp_cc[0]]),
    (r"and CC (\d+) value 0 turns it off", [stomp_cc[0]]),
    (r"If CC (\d+) turned switch 1 on, pressing switch 1 sends CC (\d+) value 0",
     [stomp_cc[0], stomp_cc[0]]),
    (r"UP \+ DOWN held ([\d.]+) s ", [secs(config_hold)]),
    (r"Held ([\d.]+) s again in configuration mode", [secs(config_hold)]),
    (r"Hold UP and DOWN together for ([\d.]+ seconds?)", [sec_words(config_hold)]),
    (r"or UP \+ DOWN together for ([\d.]+ seconds?)", [sec_words(config_hold)]),
    (r"a hold of more than ([\d.]+) s undoes it", [secs(hold)]),
    (r"Released within ([\d.]+ seconds?)", [sec_words(hold)]),
    (r"Released after more than ([\d.]+ seconds?)", [sec_words(hold)]),
    (r"If you let go within (a second|[\d.]+ seconds?) it stays",
     ["a second" if hold == 1000 else sec_words(hold)]),
    (r"`MOMENTARY_HOLD_MS` in `src/config\.h` \((?:default|currently) (\d+)\)", [hold]),
    (r"so the real threshold is about ([\d.]+) s", ["%.2f" % (hold_real / 1000.0)]),
    (r"Hold UP \(ENTER\) for ([\d.]+ seconds?)", [sec_words(hold)]),
    (r"hold DOWN \(ESCAPE\) for ([\d.]+ seconds?)", [sec_words(hold)]),
    (r"The ([\d.]+) second long-press threshold", [secs(hold)]),
    (r"Switch 10 flashes `0` for ([\d.]+ seconds?)", [sec_words(num("CHANNEL_DIGIT_TIMEOUT_MS"))]),
    (r"After ([\d.]+ seconds?), or on a page change", [sec_words(num("CHANNEL_DIGIT_TIMEOUT_MS"))]),
    (r"moved less than (\d+) counts between the heel and toe", [num("CAL_MIN_SPAN")]),
    (r"Every LED blinks (\d+) times", [num("LAMP_TEST_BLINKS")]),
    (r"shows in hex, one part (per second|every [\d.]+ seconds?)",
     ["per second" if show == 1000 else "every " + sec_words(show)]),
    (r"\(default: ([a-z ]+), `DEFAULT_PEDAL_MODE`\)", [modes[num("DEFAULT_PEDAL_MODE") & 3]]),
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

if new != text:
    open(readme, "w").write(new)
    print("updated the config.h values in " + readme)
