# Build hook: check the firmware code size against the ROM size and record it in the
# documentation. Usage: check_size.py <raw image .bin> <rom size in bytes> <docs/building.md>
# Fails (exit 1) if the code does not fit. Rewrites the size sentence ("The code
# is about N KB (N bytes at the last build), so it needs a N KB or larger chip") only when
# the numbers change, and warns if that sentence is missing.
import os
import re
import sys

image, rom_size, readme = sys.argv[1], int(sys.argv[2]), sys.argv[3]
size = os.path.getsize(image)

if size > rom_size:
    sys.exit("firmware size: %d bytes is over the %d byte ROM by %d" % (size, rom_size, size - rom_size))

# Smallest standard EPROM that holds it (8 KB 2764 upwards).
chip = 8192
while chip < size:
    chip *= 2

print("firmware size: %d bytes (%.1f KB), %d free of %d; fits a %d KB or larger chip"
      % (size, size / 1024, rom_size - size, rom_size, chip // 1024))

# The One ROM image-switch plugin (onerom/fcb1010-switch) reserves the last 256 bytes
# below 16KB and 32KB (0x3F00-0x3FFF, 0x7F00-0x7FFF): code there would be read as frames.
for top in (0x4000, 0x8000):
    if top - 256 < size <= top:
        print("note: the code reaches 0x%04X, in the page 0x%04X-0x%04X that the One ROM "
              "switch plugin reserves" % (size - 1, top - 256, top - 1))

# No space before the unit (documentation style); an older spaced sentence is still found.
pattern = re.compile(r"The code is about [0-9.]+ ?KB \([0-9]+ bytes at the last build\), "
                     r"so it needs an? [0-9]+ ?KB or larger chip")
new = ("The code is about %.1fKB (%d bytes at the last build), so it needs a %dKB or larger chip"
       % (size / 1024, size, chip // 1024))

text = open(readme).read()
found = pattern.search(text)

if not found:
    print("note: %s has no 'The code is about ... larger chip' sentence; size not recorded" % readme)
elif found.group(0) != new:
    open(readme, "w").write(text[:found.start()] + new + text[found.end():])
    print("updated the size in " + readme)
