#ifndef VERSION_H
#define VERSION_H

#include "platform.h"

// Firmware version
#define FW_VERSION_MAJOR 0x00
#define FW_VERSION_MINOR 0x02

// The build number is in src/version_build.inc, which `make` increments each time it
// links the firmware image. It is compiled only into version.c (built at link time),
// so bumping it does not make the other objects out of date.
// fw_version[] = { major, minor, build }.
extern const CODE uint8_t fw_version[3];

#endif
