# Build hook: put the firmware version that was just linked into the documentation's
# "8. Lamp test and version" table row. Usage:
#   readme_version.py <version.h> <version_build.inc> <docs/configuration.md>
# In that row it rewrites the three display examples: major with a trailing dot (`MM.`),
# minor with dots both sides (`.mm.`) and build with a leading dot (`.bb`), in hex as
# the display shows them. The file is only written when something changes; if the row or
# one of the examples is missing it prints a note and changes nothing.
import re
import sys

version_h, build_inc, readme = sys.argv[1], sys.argv[2], sys.argv[3]


def number(path, name):
    found = re.search(r"^#define " + name + r" (0x[0-9A-Fa-f]+|[0-9]+)", open(path).read(), re.M)

    if not found:
        sys.exit("%s: no '#define %s' line" % (path, name))

    return int(found.group(1), 0) & 0xFF


major = number(version_h, "FW_VERSION_MAJOR")
minor = number(version_h, "FW_VERSION_MINOR")
build = number(build_inc, "FW_VERSION_BUILD")

text = open(readme).read()
row = re.search(r"^\| *8\. Lamp test and version *\|.*$", text, re.M)

if not row:
    print("note: %s has no '| 8. Lamp test and version |' table row; version not recorded" % readme)
    sys.exit(0)

line = row.group(0)
# Each example in backticks: `MM.` (no dot before), `.mm.`, `.bb` (no dot after).
examples = [
    (r"`[0-9A-Fa-f]{2}\.`", "`%02X.`" % major),
    (r"`\.[0-9A-Fa-f]{2}\.`", "`.%02X.`" % minor),
    (r"`\.[0-9A-Fa-f]{2}`", "`.%02X`" % build),
]

for pattern, value in examples:
    if len(re.findall(pattern, line)) != 1:
        print("note: the version row needs exactly one %s example; version not recorded" % value)
        sys.exit(0)

    line = re.sub(pattern, value, line)

if line != row.group(0):
    open(readme, "w").write(text[:row.start()] + line + text[row.end():])
    print("updated the version in %s to %02X.%02X.%02X" % (readme, major, minor, build))
