/**
 * @file test_profiling.cpp
 * @brief The observation window: milliseconds -> frames (FR-PROF-5).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * Small arithmetic with an expensive failure mode. A window that derives to
 * zero frames makes `Profiler::classify()` return ESP_ERR_INVALID_STATE and
 * the classification silently never updates — which is exactly the bug the
 * re-arm exists to fix, reintroduced one layer down. So the degenerate inputs
 * are the point of this suite, not an afterthought to it.
 */

#include <cmath>
#include <limits>

#include "harness.h"
#include "pinled_profiling.h"

using namespace ooe::pinled;

TEST(window_is_fs_times_seconds)
{
    // The ordinary case: the bench runs 10 kHz, and 750 ms of it is 7500
    // frames — a bit over 90 mains half-cycles, which is what the window is
    // sized for.
    CHECK_EQ(profile_window_frames(750, 10000.0f), 7500u);
    CHECK_EQ(profile_window_frames(1000, 10000.0f), 10000u);
    CHECK_EQ(profile_window_frames(500, 4000.0f), 2000u);
}

TEST(window_scales_with_the_measured_rate_not_the_configured_one)
{
    // The same window in milliseconds must observe the same DURATION however
    // fast the chain turns out to be. That is the whole reason this is not a
    // frame count: a 128-channel machine samples slower than a 16-channel one
    // and must still see several AC cycles.
    const size_t fast = profile_window_frames(750, 20000.0f);
    const size_t slow = profile_window_frames(750, 5000.0f);
    CHECK_EQ(fast, 15000u);
    CHECK_EQ(slow, 3750u);
    CHECK_EQ(fast, slow * 4);
}

TEST(rounds_to_nearest_rather_than_truncating)
{
    // 3333.33 frames. Truncation would be fine here and is wrong in general:
    // a rate that lands just under an integer should not lose a frame every
    // time, and the assertion pins the intent.
    CHECK_EQ(profile_window_frames(1000, 3333.33f), 3333u);
    CHECK_EQ(profile_window_frames(1000, 3333.66f), 3334u);
}

TEST(a_failed_fs_measurement_yields_a_usable_floor)
{
    // Fs is measured at boot and the measurement can fail. None of these may
    // produce zero: classify() refuses a zero-frame window, so a zero here
    // would turn a bad measurement into a profiler that never runs again.
    CHECK_EQ(profile_window_frames(750, 0.0f), kMinWindowFrames);
    CHECK_EQ(profile_window_frames(750, -1.0f), kMinWindowFrames);
    CHECK_EQ(profile_window_frames(750, std::numeric_limits<float>::quiet_NaN()),
             kMinWindowFrames);
    CHECK_EQ(profile_window_frames(750, std::numeric_limits<float>::infinity()),
             kMinWindowFrames);
}

TEST(a_zero_window_yields_the_floor_not_zero_frames)
{
    CHECK_EQ(profile_window_frames(0, 10000.0f), kMinWindowFrames);
}

TEST(a_tiny_window_is_floored_rather_than_rounded_away)
{
    // 1 ms at 1 kHz is one frame, which classify() would accept and learn
    // nothing from. The floor is not a correctness guarantee — it is a
    // guarantee that the failure is visible as a bad classification instead
    // of an error nothing surfaces.
    CHECK_EQ(profile_window_frames(1, 1000.0f), kMinWindowFrames);
}

TEST(an_absurd_window_is_capped)
{
    // Nothing should be able to hold the profiler armed indefinitely: an
    // over-large window makes a re-arm look like it did nothing at all.
    CHECK_EQ(profile_window_frames(5000, 40000.0f), kMaxWindowFrames);
    CHECK_EQ(profile_window_frames(4000000, 40000.0f), kMaxWindowFrames);
    // Large enough to overflow a 32-bit size_t if the clamp happened after
    // the narrowing cast rather than before it.
    CHECK_EQ(profile_window_frames(4000000000u, 1e9f), kMaxWindowFrames);
}

TEST(the_clamp_bounds_are_ordered_and_usable)
{
    CHECK(kMinWindowFrames > 0);
    CHECK(kMinWindowFrames < kMaxWindowFrames);
}

TEST(status_defaults_are_the_quiet_ones)
{
    // A default-constructed status must not claim a pass is running: it is
    // what a device with no scan task reports, and OBSERVING there would put
    // a spinner on screen that never stops.
    ProfilerStatus s{};
    CHECK(s.state == ProfilerState::IDLE);
    CHECK_EQ(s.frames_observed, 0u);
    CHECK_EQ(s.passes, 0u);
}

int main() { return ooe::test::run_all(); }
