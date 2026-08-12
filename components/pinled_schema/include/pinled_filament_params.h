#pragma once

/**
 * @file pinled_filament_params.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief Per-channel integrator tuning — the shape both the profiler and the
 *        stored configuration produce.
 * @version 0.3.0
 * @date 2026-08-10
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * Moved out of `filament.h` for the same reason `DriveClass` was (see
 * `pinled_drive_class.h`): that header declares a class returning `esp_err_t`
 * and so pulls in ESP-IDF, which put this struct out of reach of anything
 * host-buildable. The rule that decides which of two sources wins for a given
 * channel is ordinary logic and deserves ordinary tests, so the type it
 * operates on has to live somewhere testable.
 *
 * Floats are fine here: these are set once when configuration changes, and
 * `Filament` converts them to Q16 coefficients before the hot loop ever sees
 * them (FR-FIL-5, NFR-4).
 */

namespace ooe::pinled
{
    /// Per-channel tuning. Attack/decay are thermal time constants; gain
    /// normalizes duty (e.g. a ~1/8-duty matrix lamp uses gain ~8 to reach full
    /// brightness, while a dimmed GI channel uses gain 1 so brightness tracks
    /// the conduction angle).
    struct FilamentParams
    {
        float attack_ms{30.0f}; ///< rise time constant (ms)
        float decay_ms{40.0f};  ///< fall time constant (ms)
        float gain{1.0f};       ///< duty normalization multiplier at output
    };
} // namespace ooe::pinled
