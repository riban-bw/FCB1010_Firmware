#ifndef VERSION_H
#define VERSION_H

#include "platform.h"

// Firmware version: major and minor in decimal, 0-199 (the build number is shown in hex).
#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 7

#if FW_VERSION_MAJOR > 127 || FW_VERSION_MINOR > 127
#error FW_VERSION_MAJOR and FW_VERSION_MINOR must be 0-127
#endif

// The build number is in src/version_build.inc, which `make` increments each time it
// links the firmware image. It is compiled only into version.c (built at link time),
// so bumping it does not make the other objects out of date.
// fw_version[] = { major, minor, build, their bit 7s in bits 0-2 }.
extern const CODE uint8_t fw_version[4];

#endif
