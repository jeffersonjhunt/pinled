#pragma once

/**
 * @file pinled_drive_class.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief How a machine drives a lamp — the vocabulary shared by the profiler,
 *        the config schema and the UI.
 * @version 0.3.0
 * @date 2026-08-10
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * This enum used to live in `profiler.h`, which pulls in `filament.h` and
 * therefore `esp_err.h`. That made it unreachable from anything host-buildable,
 * and the config layer needs it — so it moved here, where it depends on
 * nothing. `profiler.h` includes this and is otherwise unchanged.
 *
 * The values mirror `pinled.proto`'s `DriveClass` exactly, and
 * `pinled_channel_config.cpp` holds `static_assert`s to that effect rather than
 * trusting the two lists to stay in step. `UNKNOWN` and the wire's
 * `UNSPECIFIED` are both zero and both mean the same thing: *the profiler owns
 * this channel*. That is why a per-lamp class is a **lock** rather than a
 * value — an untouched entry leaves classification where it belongs.
 */

#include <cstdint>

namespace ooe::pinled
{
    enum class DriveClass : uint8_t
    {
        UNKNOWN = 0, ///< not classified / not locked — the profiler decides
        OFF,         ///< no activity
        STEADY,      ///< ~100% duty, no edges (EM DC / always-on GI)
        MATRIX,      ///< periodic low-duty bursts, ~200 Hz..2 kHz (strobed matrix)
        AC_STEADY,   ///< 100/120 Hz envelope, ~50% raw duty (on/off GI)
        AC_DIMMED,   ///< 100/120 Hz, variable conduction angle (triac-dimmed GI)
    };

    /// Human-readable name, for logs and the API.
    const char *drive_class_str(DriveClass c);
} // namespace ooe::pinled
