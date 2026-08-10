/**
 * @file pinled_drive_class.cpp
 * @brief Names for the drive classes.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * Separate from `pinled_channel_config.cpp` because that file needs the
 * generated nanopb header and this one must not. `profiler.h` includes
 * `pinled_drive_class.h`, so anything reachable from that header has to link
 * in a firmware build that does not yet carry nanopb.
 */

#include "pinled_drive_class.h"

namespace ooe::pinled
{
    const char *drive_class_str(DriveClass c)
    {
        switch (c)
        {
        case DriveClass::UNKNOWN:   return "UNKNOWN";
        case DriveClass::OFF:       return "OFF";
        case DriveClass::STEADY:    return "STEADY";
        case DriveClass::MATRIX:    return "MATRIX";
        case DriveClass::AC_STEADY: return "AC_STEADY";
        case DriveClass::AC_DIMMED: return "AC_DIMMED";
        }
        return "?";
    }
} // namespace ooe::pinled
