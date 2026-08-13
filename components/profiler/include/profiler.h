#pragma once

/**
 * @file profiler.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief Per-channel lamp drive-scheme classifier (auto-profiling).
 * @version 0.2.0
 * @date 2026-07-16
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * Because the analog front end preserves the raw pulse pattern, the firmware
 * can watch each channel's transitions over a short window and infer which
 * drive scheme the machine is using, then pick integrator gain/attack/decay
 * automatically instead of shipping a hand-tuned table per game.
 *
 * STATUS: first-cut skeleton. The classification algorithm is documented below
 * and partially implemented (duty + edge counting). Period estimation / robust
 * AC-dimming detection is a v1 item.
 */

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

#include "filament.h"          // FilamentParams
#include "pinled_drive_class.h" // DriveClass — moved out so the config layer,
                                // which must build off-target, can reach it
#include "pinled_profiling.h"   // profile_window_frames — the window in ms

namespace ooe::pinled
{
    struct ChannelProfile
    {
        DriveClass klass{DriveClass::UNKNOWN};
        int32_t duty_q16{0};     ///< observed duty (0..65536)
        float period_est_ms{0};  ///< dominant period estimate, 0 if none
        uint8_t confidence{0};   ///< 0..255
        bool locked{false};      ///< true = set by machine profile, don't auto-change
    };

    /**
     * @brief Streaming per-channel classifier.
     *
     * Feed it the same boolean samples the scan loop produces (observe()), let
     * it run for a window of frames, then call classify() to fill a profile per
     * channel and derive FilamentParams. Algorithm:
     *
     *   duty ~= 1, edges ~= 0                      -> STEADY   (gain 1)
     *   periodic bursts, f in [200Hz,2kHz], low duty -> MATRIX (gain 1/duty)
     *   period ~= 8.3/10 ms, duty ~= 50%           -> AC_STEADY(gain 1)
     *   period ~= 8.3/10 ms, duty variable          -> AC_DIMMED(gain 1, track angle)
     *   no edges, duty ~= 0                         -> OFF
     */
    class Profiler
    {
    public:
        /**
         * @param num_channels   channels to classify
         * @param sample_rate_hz the MEASURED Fs, not the configured one
         * @param window_ms      observation window (FR-PROF-5); the frame
         *                       count is derived from @p sample_rate_hz
         */
        esp_err_t init(size_t num_channels, float sample_rate_hz, uint32_t window_ms);
        void deinit();

        /**
         * @brief Reset accumulators and begin a fresh observation window.
         * @param window_frames frames to accumulate before the window is
         *        complete; 0 means "the window set at init()".
         *
         * Called only from whichever context feeds observe() — the two are a
         * pair and splitting them across tasks is what would need a lock.
         */
        void arm(size_t window_frames = 0);

        /**
         * @brief Accumulate one frame of booleans (length >= num_channels).
         *
         * Frames past the window are **ignored**, deliberately: that is what
         * lets a second task call classify() without a lock. Once
         * window_complete() reads true the accumulators have stopped moving,
         * so the reader sees a stable set without the writer having to be
         * stopped or told anything.
         */
        void observe(const bool *samples, size_t n);

        /// True once the window has been filled. See observe().
        bool window_complete() const { return frames_ >= window_frames_; }

        /// Frames accumulated in the current window, for progress reporting.
        uint32_t frames() const { return frames_; }
        /// Frames this window will accumulate before it is complete.
        uint32_t window_frames() const { return window_frames_; }

        /// Finalize: fill profiles[] and map each to FilamentParams params[].
        esp_err_t classify(ChannelProfile *profiles, FilamentParams *params, size_t n);

        /// Map a single profile to integrator params (pure, testable on host).
        static FilamentParams params_for(const ChannelProfile &p);

        size_t channels() const { return num_channels_; }
        float sample_rate_hz() const { return sample_rate_hz_; }

    private:
        size_t num_channels_{0};
        float sample_rate_hz_{0};
        uint32_t frames_{0};
        uint32_t window_frames_{0}; ///< derived from the window in ms (FR-PROF-5)

        uint32_t *high_count_{nullptr}; ///< samples seen high, per channel
        uint32_t *edges_{nullptr};      ///< transitions, per channel
        uint8_t *last_{nullptr};        ///< last sample, per channel
    };
} // namespace ooe::pinled
