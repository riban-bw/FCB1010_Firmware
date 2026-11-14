# Add one to FW_VERSION_BUILD in the given file (default src/version_build.inc), two hex
# digits, wrapping from 0xFF to 0x00. `make` runs this each time it links the firmware.
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "src/version_build.inc"
text = open(path).read()
pattern = re.compile(r"^(#define FW_VERSION_BUILD )0x([0-9A-Fa-f]+)", re.M)
found = pattern.search(text)

if not found:
    sys.exit(path + ": no '#define FW_VERSION_BUILD 0x..' line")

build = (int(found.group(2), 16) + 1) & 0xFF
text = text[:found.start()] + "%s0x%02X" % (found.group(1), build) + text[found.end():]
open(path, "w").write(text)
print("firmware build 0x%02X" % build)
