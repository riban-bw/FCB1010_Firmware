# Build hook: make the ROM image for one chip size from the linked code.
# Usage: rom_image.py <raw .bin from makebin> <link size in bytes> <chip size in bytes> <out .bin>
# The code is padded with 0xFF to the smaller of the link size (ROM_SIZE, the address
# range the code is linked for) and the chip size, then repeated to fill the chip. A
# 27C512 (64KB) thus holds the 32KB image twice, so it runs whether the socket drives the
# chip's top address line high or low; a 27C128 (16KB) holds it once, which works because
# the CPU's higher address lines are not connected to that chip (addresses repeat every
# 16KB). Fails if the code does not fit the chip.
import sys

raw_path, link_size, chip_size, out_path = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
code = open(raw_path, "rb").read()
block = min(link_size, chip_size)

if len(code) > block:
    sys.exit("%s: the code (%d bytes) does not fit a %dKB chip" % (out_path, len(code), chip_size // 1024))

image = (code + b"\xff" * (block - len(code))) * (chip_size // block)
open(out_path, "wb").write(image)
