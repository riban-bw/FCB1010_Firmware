#!/usr/bin/env python3
"""Write the definitions of ctl.c's CTL_NEAR variables (8051 build only).

ctl.c declares its most used variables CTL_NEAR: on the 8051 they are directly addressed
(NEAR, the lower 128 bytes of internal RAM), a byte less at every use than IDATA. ctl.c's
own directly addressed data has to fit the gap below the bit area, so these are defined
in a module of their own, linked after the others so its data goes above the bits. On the
host and in the editor CTL_NEAR is a plain static and this file is not used.

Usage: ctl_ram.py src/ctl.c build/ctl_ram.c (the output is only rewritten when it changes,
so an unchanged list does not relink the image).
"""

import re
import sys

DECL = re.compile(r"^CTL_NEAR\s+(uint8_t|uint16_t)\s+(\w+)\s*(\[\s*\d+\s*\])?\s*;", re.M)


def main():
    src, out = sys.argv[1], sys.argv[2]
    found = DECL.findall(open(src, encoding="utf-8").read())
    if not found:
        sys.exit("ctl_ram.py: no CTL_NEAR variables in " + src)
    lines = ["// Generated from src/ctl.c by tools/ctl_ram.py: do not edit.",
             "// ctl.c's CTL_NEAR variables, in directly addressed RAM (see ctl_ram.py).",
             '#include "platform.h"', ""]
    lines += ["NEAR %s %s%s;" % (t, n, a.replace(" ", "")) for t, n, a in found]
    text = "\n".join(lines) + "\n"
    try:
        if open(out, encoding="utf-8").read() == text:
            return
    except OSError:
        pass
    with open(out, "w", encoding="utf-8") as f:
        f.write(text)


if __name__ == "__main__":
    main()
