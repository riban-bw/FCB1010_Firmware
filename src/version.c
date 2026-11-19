#include "platform.h"
#include "version.h"
#include "version_build.inc"

// Major, minor and build version numbers, then their bit 7s in bits 0-2 (as SysEx sends
// them: each number's bits 0-6, then this byte).
const CODE uint8_t fw_version[4] = { FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_BUILD,
                                     (FW_VERSION_MAJOR >> 7) | ((FW_VERSION_MINOR >> 6) & 2) | ((FW_VERSION_BUILD >> 5) & 4) };
