/**
 * @file pinled_profiling.cpp
 * @brief Observation-window arithmetic (FR-PROF-5).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "pinled_profiling.h"

#include <cmath>

namespace ooe::pinled
{
    size_t profile_window_frames(uint32_t window_ms, float fs_hz)
    {
        // A failed Fs measurement must not divide by zero or propagate a NaN
        // into a loop bound. std::isfinite catches the inf/NaN a degenerate
        // measurement can produce; the sign test catches the rest.
        if (!std::isfinite(fs_hz) || fs_hz <= 0.0f || window_ms == 0)
            return kMinWindowFrames;

        const double frames = static_cast<double>(fs_hz) * (static_cast<double>(window_ms) / 1000.0);

        // Compared as a double before narrowing: at a high Fs and a long
        // window the product exceeds what a size_t rounds to predictably on
        // every target, and clamping after the cast would clamp a number that
        // had already wrapped.
        if (frames >= static_cast<double>(kMaxWindowFrames))
            return kMaxWindowFrames;
        const size_t n = static_cast<size_t>(frames + 0.5);
        return n < kMinWindowFrames ? kMinWindowFrames : n;
    }
} // namespace ooe::pinled
