#pragma once

/**
 * @file pinled_profiling.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief Observation-window arithmetic and the profiler's public state.
 * @version 0.8.0
 * @date 2026-08-13
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * @par Why the window is milliseconds and not frames
 * FR-PROF-5. The classifier is looking for periodic structure, and every
 * period it cares about is a wall-clock one: a 60 Hz mains half-cycle is
 * 8.3 ms, a matrix column strobe is 0.5–5 ms. A window counted in frames means
 * the *duration* it observes changes with the sample rate and the channel
 * count — the first cut observed 512 frames, which at the boot free-run rate
 * is about 14 ms, or under two AC half-cycles. It classified confidently and
 * had seen almost nothing.
 *
 * So the window is declared in milliseconds and the frame count is derived
 * from the **measured** Fs (`Main::measure_and_clamp_fs`), never the
 * configured one — same rule as every other time constant in the firmware.
 *
 * This lives in `pinled_schema` rather than in the profiler because it is
 * ordinary arithmetic with an interesting failure mode, and this is the layer
 * that builds on the host.
 */

#include <cstddef>
#include <cstdint>

namespace ooe::pinled
{
    /// Bounds on a derived window, in frames.
    ///
    /// The floor exists because `Profiler::classify()` refuses a window of
    /// zero frames, and a zero would otherwise be reachable from ordinary
    /// arithmetic — a 50 ms window at a clamped-to-crawl Fs, say. Returning a
    /// tiny-but-usable window makes the failure a bad classification someone
    /// can see rather than an error path nothing ever exercises.
    ///
    /// The ceiling bounds how long a re-arm can hold the profiler armed. At
    /// 10 kHz it is five seconds, which is far past any window worth asking
    /// for and well short of "the request appeared to do nothing".
    inline constexpr size_t kMinWindowFrames = 8;
    inline constexpr size_t kMaxWindowFrames = 50000;

    /**
     * @brief Frames to observe for a window of @p window_ms at @p fs_hz.
     *
     * @param window_ms desired observation window, milliseconds
     * @param fs_hz     the MEASURED per-channel sample rate
     * @return frame count, clamped to [kMinWindowFrames, kMaxWindowFrames]
     *
     * A non-positive or non-finite @p fs_hz yields the floor rather than a
     * division by zero: Fs is measured at boot and a measurement can fail.
     */
    size_t profile_window_frames(uint32_t window_ms, float fs_hz);

    /// What the profiler is doing, as reported over the API.
    enum class ProfilerState : uint8_t
    {
        IDLE = 0,        ///< holding the last classification
        OBSERVING = 1,   ///< accumulating a window
        CLASSIFYING = 2, ///< window complete, deciding
        /// No scan task, so nothing can feed it — the bring-up scan modes own
        /// the chain themselves. Distinct from IDLE on purpose: "nothing is
        /// running" and "nothing can run" are different answers and collapsing
        /// them would leave someone re-arming a device that cannot profile.
        UNAVAILABLE = 3,
    };

    /// A consistent-enough view of the profiler for a status response.
    ///
    /// Sampled without a lock, so `frames_observed` may be a frame or two
    /// stale against `state`. That is the correct trade for a progress
    /// readout: the alternative is taking a lock in the path of a 10 kHz task
    /// to make a number in a browser marginally fresher.
    struct ProfilerStatus
    {
        ProfilerState state{ProfilerState::IDLE};
        uint32_t window_ms{0};       ///< configured window
        uint32_t window_frames{0};   ///< derived from the measured Fs
        uint32_t frames_observed{0}; ///< progress through the current window
        uint32_t channels{0};
        uint32_t passes{0}; ///< completed classifications since boot, boot included
    };
} // namespace ooe::pinled
